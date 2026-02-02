#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <dirent.h>
#include <libgen.h>
#include <sys/wait.h>
#include <poll.h>

#define QUA_CONVERT_CMD "qua-convert"
#define QUA_LAUNCHER_CMD "qua-bare-launcher"
#define SOCKET_PATH "/tmp/qua-socket.sock"
#define LOCK_PATH "/tmp/qua-socket-daemon.lock"
#define BUF_SIZE 4096
#define COALESCE_TIMEOUT_MS 50

// Conversion state for event loop
static struct {
    pid_t pid;
    int fd;
    char target[PATH_MAX];
    int active;
} conv_state = {0};

extern char **environ;

// Debug logging with millisecond timestamps
#define DEBUG_LOG
#ifdef DEBUG_LOG
static void log_ts(const char *fmt, ...) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "[%ld.%03ld] ", ts.tv_sec % 1000, ts.tv_nsec / 1000000);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}
#else
#define log_ts(...) ((void)0)
#endif

static posix_spawnattr_t global_attr;

static void run_hook(const char *hook, const char *audio_path) {
    if (access(hook, X_OK) != 0) return;

    pid_t pid;
    char *args[] = {(char *)hook, (char *)audio_path, NULL};
    if (posix_spawn(&pid, hook, NULL, NULL, args, environ) == 0) {
        waitpid(pid, NULL, 0);
    }
}

static void run_hook_async(const char *hook, const char *audio_path) {
    if (access(hook, X_OK) != 0) return;

    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            execlp(hook, hook, audio_path, NULL);
            _exit(1);
        }
        _exit(0);
    } else if (pid > 0) {
        waitpid(pid, NULL, 0);
    }
}
static char history_path[PATH_MAX];
static char hook_prelaunch[PATH_MAX];
static char hook_teardown[PATH_MAX];
static char last_played[PATH_MAX];

static int is_audio(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || *(dot + 1) == '\0')
        return 0;
    dot++;
    switch (dot[0]) {
    case 'a': return strcmp(dot, "ape") == 0 || strcmp(dot, "aiff") == 0 || strcmp(dot, "aif") == 0;
    case 'f': return strcmp(dot, "flac") == 0;
    case 'm': return strcmp(dot, "mp3") == 0 || strcmp(dot, "m4a") == 0;
    case 'o': return strcmp(dot, "opus") == 0 || strcmp(dot, "ogg") == 0;
    case 'w': return strcmp(dot, "wv") == 0 || strcmp(dot, "wav") == 0;
    default:  return 0;
    }
}

static int filter_audio(const struct dirent *e) { return is_audio(e->d_name); }

static int get_next(const char *current, int offset, char *result, size_t size) {
    if (!current || !current[0]) return 1;

    char *copy = strdup(current);
    char *dir = dirname(copy);
    struct dirent **list;

    int n = scandir(dir, &list, filter_audio, alphasort);
    if (n <= 0) { free(copy); return 1; }

    int cur = -1;
    for (int i = 0; i < n; i++) {
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir, list[i]->d_name);
        if (strcmp(full, current) == 0) { cur = i; break; }
    }

    int next = (cur == -1) ? 0 : (cur + offset) % n;
    if (next < 0) next += n;

    snprintf(result, size, "%s/%s", dir, list[next]->d_name);

    for (int i = 0; i < n; i++) free(list[i]);
    free(list);
    free(copy);
    return 0;
}

// TODO: Add logging for loaded paths and errors (missing XDG_CONFIG_HOME, etc.)
static void init_paths(void) {
    const char *config = getenv("XDG_CONFIG_HOME");
    if (!config) return;

    snprintf(history_path, sizeof(history_path), "%s/qua-player/history", config);
    snprintf(hook_prelaunch, sizeof(hook_prelaunch), "%s/qua-player/hooks/prelaunch", config);
    snprintf(hook_teardown, sizeof(hook_teardown), "%s/qua-player/hooks/teardown", config);
}

static int find_playable_from_history(char *result, size_t size) {
    if (!history_path[0]) return 1;

    FILE *f = fopen(history_path, "r");
    if (!f) return 1;

    char line[PATH_MAX + 32];
    int found = 0;

    while (fgets(line, sizeof(line), f)) {
        // Skip unix timestamp
        char *path = strchr(line, ' ');
        if (!path) continue;
        path++;
        // Remove newline
        char *nl = strchr(path, '\n');
        if (nl) *nl = '\0';
        // Keep last valid file
        if (*path && access(path, F_OK) == 0) {
            snprintf(result, size, "%s", path);
            found = 1;
        }
    }
    fclose(f);
    return found ? 0 : 1;
}

static void log_play(const char *path) {
    if (!history_path[0]) return;

    // Ensure directory exists
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%.*s", (int)(strrchr(history_path, '/') - history_path), history_path);
    mkdir(dir, 0755);

    FILE *f = fopen(history_path, "a");
    if (!f) return;

    fprintf(f, "%ld %s\n", time(NULL), path);
    fclose(f);
}

// Native /proc-based player kill - no fork, direct syscalls
static int kill_players_async(pid_t *pids, int max_pids) {
    DIR *proc = opendir("/proc");
    if (!proc) return 0;

    struct dirent *ent;
    int count = 0;

    while ((ent = readdir(proc)) && count < max_pids) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;

        char path[64];
        snprintf(path, sizeof(path), "/proc/%s/comm", ent->d_name);

        FILE *f = fopen(path, "r");
        if (!f) continue;

        char comm[32];
        if (fgets(comm, sizeof(comm), f) && strncmp(comm, "qua-player", 10) == 0) {
            pid_t pid = atoi(ent->d_name);
            kill(pid, SIGKILL);
            pids[count++] = pid;
        }
        fclose(f);
    }
    closedir(proc);
    return count;
}

// Wait for all killed players to die
static void kill_players_wait(pid_t *pids, int count) {
    for (int i = 0; i < count; i++) {
        waitpid(pids[i], NULL, 0);
    }
}

// Convenience: blocking kill (for stop command)
static void kill_player(void) {
    pid_t pids[16];
    int count = kill_players_async(pids, 16);
    kill_players_wait(pids, count);
}

// Start qua-convert, returns read fd for stdout (-1 on error)
static int start_convert(const char *path, pid_t *pid_out) {
    log_ts("start_convert: input path=%s", path);

    int pipefd[2];
    if (pipe(pipefd) == -1) {
        log_ts("start_convert: pipe() failed");
        return -1;
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    posix_spawn_file_actions_addclose(&fa, pipefd[1]);

    char *args[] = {QUA_CONVERT_CMD, (char *)path, NULL};
    int err = posix_spawnp(pid_out, QUA_CONVERT_CMD, &fa, NULL, args, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pipefd[1]);

    if (err != 0) {
        log_ts("start_convert: posix_spawnp failed err=%d", err);
        close(pipefd[0]);
        return -1;
    }
    log_ts("start_convert: output pid=%d fd=%d", *pid_out, pipefd[0]);
    return pipefd[0];
}

// Wait for convert and read output, returns 0 on success
static int finish_convert(pid_t pid, int fd, char *player, char *wav, size_t size) {
    log_ts("finish_convert: input pid=%d fd=%d", pid, fd);

    int status;
    waitpid(pid, &status, 0);
    log_ts("finish_convert: waitpid done, status=0x%x", status);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        log_ts("finish_convert: bad exit, WIFEXITED=%d WEXITSTATUS=%d", WIFEXITED(status), WEXITSTATUS(status));
        close(fd);
        return -1;
    }

    char buf[PATH_MAX * 2 + 16];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    log_ts("finish_convert: read %zd bytes", n);

    if (n <= 0) {
        log_ts("finish_convert: read failed or empty");
        return -1;
    }
    buf[n] = '\0';
    log_ts("finish_convert: raw output=[%s]", buf);

    char *nl = strchr(buf, '\n');
    if (!nl) {
        log_ts("finish_convert: no newline found");
        return -1;
    }
    *nl = '\0';

    char *line2 = nl + 1;
    nl = strchr(line2, '\n');
    if (nl) *nl = '\0';

    if (!buf[0] || !line2[0]) {
        log_ts("finish_convert: empty line1 or line2");
        return -1;
    }

    snprintf(player, size, "%s", buf);
    snprintf(wav, size, "%s", line2);
    log_ts("finish_convert: output player=%s wav=%s", player, wav);
    return 0;
}

// Forward declarations
static void launch_player(const char *player, const char *wav);

// Cancel current conversion if active
static void cancel_conversion(void) {
    if (!conv_state.active) return;

    log_ts("EVENT: canceling conversion pid=%d target=%s", conv_state.pid, conv_state.target);
    kill(conv_state.pid, SIGKILL);
    waitpid(conv_state.pid, NULL, 0);
    close(conv_state.fd);
    conv_state.active = 0;
    conv_state.pid = 0;
    conv_state.fd = -1;
    conv_state.target[0] = '\0';
}

// Start async conversion, returns 0 on success
static int start_async_conversion(const char *path) {
    cancel_conversion();  // Cancel any existing

    pid_t pid;
    int fd = start_convert(path, &pid);
    if (fd < 0) return -1;

    conv_state.pid = pid;
    conv_state.fd = fd;
    snprintf(conv_state.target, sizeof(conv_state.target), "%s", path);
    conv_state.active = 1;

    log_ts("EVENT: started conversion pid=%d target=%s", pid, path);
    return 0;
}

// Handle conversion completion (called when conv_state.fd is readable)
static void handle_conversion_complete(void) {
    if (!conv_state.active) return;

    log_ts("EVENT: conversion complete, processing result");

    // Kill old players + run prelaunch hook
    pid_t killed_pids[16];
    int killed_count = kill_players_async(killed_pids, 16);
    run_hook(hook_prelaunch, conv_state.target);
    kill_players_wait(killed_pids, killed_count);

    // Finish conversion
    char player_path[PATH_MAX], wav_path[PATH_MAX];
    if (finish_convert(conv_state.pid, conv_state.fd, player_path, wav_path, PATH_MAX) == 0) {
        launch_player(player_path, wav_path);
        log_play(conv_state.target);
        snprintf(last_played, sizeof(last_played), "%s", conv_state.target);
        log_ts("EVENT: player launched for %s", conv_state.target);
    } else {
        log_ts("EVENT: conversion failed for %s", conv_state.target);
    }

    conv_state.active = 0;
    conv_state.pid = 0;
    conv_state.fd = -1;
    conv_state.target[0] = '\0';
}

// Launch player binary with wav file (double-fork so init reaps it)
static void launch_player(const char *player, const char *wav) {
    log_ts("launch_player: input player=%s wav=%s", player, wav);

    pid_t pid = fork();
    if (pid == 0) {
        // Intermediate child: fork again and exit
        if (fork() == 0) {
            // Grandchild: exec the launcher
            int fd = open("/dev/null", O_WRONLY);
            if (fd != -1) { dup2(fd, STDOUT_FILENO); close(fd); }
            execlp(QUA_LAUNCHER_CMD, QUA_LAUNCHER_CMD, "4", player, wav, "hw:0,0", NULL);
            _exit(1);
        }
        _exit(0);  // Intermediate exits immediately
    } else if (pid > 0) {
        // Parent: reap intermediate child (instant)
        waitpid(pid, NULL, 0);
        log_ts("launch_player: launched via double-fork");
    }
}

// Coalesce rapid next/prev commands, returns net offset
static int coalesce_navigation(int server_fd, int initial_offset,
                               int first_client_fd, int *last_client_fd) {
    int offset = initial_offset;
    int coalesced_count = 0;
    *last_client_fd = first_client_fd;

    while (1) {
        struct pollfd pfd = { .fd = server_fd, .events = POLLIN };
        int ret = poll(&pfd, 1, COALESCE_TIMEOUT_MS);

        if (ret <= 0) break;  // timeout or error

        int new_fd = accept(server_fd, NULL, NULL);
        if (new_fd == -1) break;

        char buf[BUF_SIZE];
        ssize_t n = read(new_fd, buf, sizeof(buf) - 1);
        if (n <= 0) { close(new_fd); continue; }
        buf[n] = '\0';

        if (strcmp(buf, "play-next") == 0) {
            offset++;
            coalesced_count++;
            dprintf(*last_client_fd, "Queued (+%d)\n", offset);
            close(*last_client_fd);
            *last_client_fd = new_fd;
        } else if (strcmp(buf, "play-prev") == 0) {
            offset--;
            coalesced_count++;
            dprintf(*last_client_fd, "Queued (%d)\n", offset);
            close(*last_client_fd);
            *last_client_fd = new_fd;
        } else {
            // Non-navigation command - drop during coalesce, user retries
            close(new_fd);
            break;
        }
    }

    if (coalesced_count > 0) {
        log_ts("coalesce: merged %d commands, final offset=%+d", coalesced_count, offset);
    }

    return offset;
}

static void handle_command(int server_fd, int client_fd) {
    char buf[BUF_SIZE];
    ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';

    // Parse null-terminated: action\0data\0
    char *action = buf;
    char *data = action + strlen(action) + 1;

    if (strcmp(action, "play") == 0) {
        const char *path = NULL;
        char history_fallback[PATH_MAX];

        if (*data) {
            path = data;
        } else if (*last_played && access(last_played, F_OK) == 0) {
            path = last_played;
        } else if (find_playable_from_history(history_fallback, sizeof(history_fallback)) == 0) {
            path = history_fallback;
        }

        if (path) {
            if (start_async_conversion(path) == 0) {
                dprintf(client_fd, "Playing: %s\n", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
            } else {
                dprintf(client_fd, "Conversion failed\n");
            }
        } else {
            dprintf(client_fd, "Nothing playable\n");
        }
    } else if (strcmp(action, "play-next") == 0 || strcmp(action, "play-prev") == 0) {
        int initial_offset = (strcmp(action, "play-next") == 0) ? 1 : -1;
        int last_fd;
        int offset = coalesce_navigation(server_fd, initial_offset, client_fd, &last_fd);

        // If conversion is in progress, compute offset from current conversion target
        const char *base = conv_state.active ? conv_state.target : last_played;
        log_ts("EVENT: nav offset=%+d from base=%s (conv_active=%d)", offset, base, conv_state.active);

        char target[PATH_MAX];
        if (get_next(base, offset, target, sizeof(target)) == 0) {
            if (start_async_conversion(target) == 0) {
                const char *basename = strrchr(target, '/');
                basename = basename ? basename + 1 : target;
                if (offset == 1) {
                    dprintf(last_fd, "Next: %s\n", basename);
                } else if (offset == -1) {
                    dprintf(last_fd, "Prev: %s\n", basename);
                } else {
                    dprintf(last_fd, "Skipped %+d: %s\n", offset, basename);
                }
            }
        }
        if (last_fd != client_fd) {
            close(last_fd);
        }
        return;  // client_fd already handled or will be closed by caller
    } else if (strcmp(action, "stop") == 0) {
        cancel_conversion();  // Cancel any pending conversion
        kill_player();
        run_hook_async(hook_teardown, last_played);
        dprintf(client_fd, "Stopped\n");
    } else if (strcmp(action, "show") == 0) {
        if (last_played[0]) {
            write(client_fd, last_played, strlen(last_played));
        }
    }
}

int main(void) {
    init_paths();
    find_playable_from_history(last_played, sizeof(last_played));

    // Single instance check
    int lock_fd = open(LOCK_PATH, O_CREAT | O_RDWR | O_CLOEXEC, 0666);
    if (lock_fd == -1 || flock(lock_fd, LOCK_EX | LOCK_NB) == -1) {
        fprintf(stderr, "[qua-socket] Already running or lock error.\n");
        return 1;
    }

    // Remove stale socket
    unlink(SOCKET_PATH);

    // Create socket
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
        return 1;
    }

    if (listen(server_fd, 5) == -1) {
        perror("listen");
        return 1;
    }

    // Initialize spawn attributes
    posix_spawnattr_init(&global_attr);
    short flags = POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
    posix_spawnattr_setflags(&global_attr, flags);

    sigset_t all_signals, no_signals;
    sigfillset(&all_signals);
    sigemptyset(&no_signals);
    posix_spawnattr_setsigdefault(&global_attr, &all_signals);
    posix_spawnattr_setsigmask(&global_attr, &no_signals);

    // No SIGCHLD handler - we use explicit waitpid for all children
    // Fire-and-forget children use double-fork so init reaps them

    // Ignore SIGPIPE (broken pipe when client disconnects)
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "[qua-socket] Listening on %s\n", SOCKET_PATH);

    // Event loop
    while (1) {
        struct pollfd pfds[2];
        int nfds = 1;

        // Always poll server socket
        pfds[0].fd = server_fd;
        pfds[0].events = POLLIN;

        // Optionally poll conversion pipe
        if (conv_state.active) {
            pfds[1].fd = conv_state.fd;
            pfds[1].events = POLLIN;
            nfds = 2;
        }

        log_ts("EVENT: poll nfds=%d conv_active=%d", nfds, conv_state.active);
        int ret = poll(pfds, nfds, -1);  // Block indefinitely
        if (ret <= 0) continue;

        // Check conversion completion first (higher priority)
        if (nfds == 2 && (pfds[1].revents & (POLLIN | POLLHUP))) {
            log_ts("EVENT: conversion fd ready, revents=0x%x", pfds[1].revents);
            handle_conversion_complete();
        }

        // Check for new client connections
        if (pfds[0].revents & POLLIN) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd != -1) {
                log_ts("EVENT: new client connection");
                handle_command(server_fd, client_fd);
                close(client_fd);
            }
        }
    }

    posix_spawnattr_destroy(&global_attr);
    return 0;
}
