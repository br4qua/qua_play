#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
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

#include "qua-cache.h"
#include "qua-player-selector.h"

#define QUA_CONVERT_CMD "qua-convert"
#define QUA_LAUNCHER_CMD "qua-bare-launcher"
#define SOCKET_PATH "/tmp/qua-socket.sock"
#define LOCK_PATH "/tmp/qua-socket-daemon.lock"
#define BUF_SIZE 4096
#define COALESCE_TIMEOUT_MS 20

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

// Run qua-convert synchronously, returns 0 on success
static int run_convert(const char *input_path, const char *output_path) {
    log_ts("run_convert: input=%s output=%s", input_path, output_path);

    pid_t pid;
    char *args[] = {QUA_CONVERT_CMD, (char *)input_path, (char *)output_path, NULL};
    int err = posix_spawnp(&pid, QUA_CONVERT_CMD, NULL, NULL, args, environ);

    if (err != 0) {
        log_ts("run_convert: posix_spawnp failed err=%d", err);
        return -1;
    }

    int status;
    waitpid(pid, &status, 0);
    log_ts("run_convert: waitpid done, status=0x%x", status);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        log_ts("run_convert: bad exit, WIFEXITED=%d WEXITSTATUS=%d", WIFEXITED(status), WEXITSTATUS(status));
        return -1;
    }

    // Verify output file exists
    struct stat st;
    if (stat(output_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        log_ts("run_convert: output file not found");
        return -1;
    }

    log_ts("run_convert: success, output size=%ld", (long)st.st_size);
    return 0;
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

static void spawn_play(const char *path) {
    log_ts("spawn_play: START path=%s", path);

    // 1. Generate cache path
    char cache_path[PATH_MAX];
    if (cache_generate_path(path, cache_path, sizeof(cache_path)) != 0) {
        log_ts("spawn_play: cache_generate_path failed");
        return;
    }
    log_ts("spawn_play: cache_path=%s", cache_path);

    // 2. Check if cached
    int is_cached = cache_exists(cache_path);
    log_ts("spawn_play: cache %s", is_cached ? "HIT" : "MISS");

    // 3. Kill old players + prelaunch hook (always, regardless of cache)
    log_ts("spawn_play: killing old players + prelaunch hook");
    pid_t killed_pids[16];
    int killed_count = kill_players_async(killed_pids, 16);
    log_ts("spawn_play: killed %d players async", killed_count);
    run_hook(hook_prelaunch, path);
    log_ts("spawn_play: prelaunch hook done");
    kill_players_wait(killed_pids, killed_count);
    log_ts("spawn_play: players dead");

    // 4. If cache miss, convert the file
    if (!is_cached) {
        cache_manage_size();
        if (run_convert(path, cache_path) != 0) {
            log_ts("spawn_play: run_convert failed, aborting");
            return;
        }
    }

    // 5. Select player based on WAV specs
    char player_path[PATH_MAX];
    if (select_player(cache_path, player_path, sizeof(player_path)) != 0) {
        log_ts("spawn_play: select_player failed, aborting");
        return;
    }

    // 6. Launch player
    launch_player(player_path, cache_path);
    log_ts("spawn_play: END");
}

// Prefetch next track in background (fire and forget)
static void prefetch_next(const char *current_path) {
    char next_path[PATH_MAX];
    if (get_next(current_path, 1, next_path, sizeof(next_path)) != 0) {
        return;  // No next file
    }

    // Check if already cached
    char cache_path[PATH_MAX];
    if (cache_generate_path(next_path, cache_path, sizeof(cache_path)) != 0) {
        return;
    }
    if (cache_exists(cache_path)) {
        log_ts("prefetch: %s already cached", strrchr(next_path, '/') + 1);
        return;
    }

    log_ts("prefetch: starting background convert for %s", strrchr(next_path, '/') + 1);

    // Double-fork to run in background (init reaps)
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            // Grandchild: run qua-convert
            execlp(QUA_CONVERT_CMD, QUA_CONVERT_CMD, next_path, cache_path, NULL);
            _exit(1);
        }
        _exit(0);
    } else if (pid > 0) {
        waitpid(pid, NULL, 0);  // Reap intermediate child (instant)
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
            spawn_play(path);
            log_play(path);
            if (path != last_played)
                snprintf(last_played, sizeof(last_played), "%s", path);
            dprintf(client_fd, "Playing: %s\n", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
            prefetch_next(path);
        } else {
            dprintf(client_fd, "Nothing playable\n");
        }
    } else if (strcmp(action, "play-next") == 0 || strcmp(action, "play-prev") == 0) {
        int initial_offset = (strcmp(action, "play-next") == 0) ? 1 : -1;
        int last_fd;
        int offset = coalesce_navigation(server_fd, initial_offset, client_fd, &last_fd);

        char target[PATH_MAX];
        if (get_next(last_played, offset, target, sizeof(target)) == 0) {
            spawn_play(target);
            log_play(target);
            snprintf(last_played, sizeof(last_played), "%s", target);
            const char *basename = strrchr(target, '/');
            basename = basename ? basename + 1 : target;
            if (offset == 1) {
                dprintf(last_fd, "Next: %s\n", basename);
            } else if (offset == -1) {
                dprintf(last_fd, "Prev: %s\n", basename);
            } else {
                dprintf(last_fd, "Skipped %+d: %s\n", offset, basename);
            }
            prefetch_next(target);
        }
        if (last_fd != client_fd) {
            close(last_fd);
        }
        return;  // client_fd already handled or will be closed by caller
    } else if (strcmp(action, "stop") == 0) {
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
    cache_init();
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

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd == -1) continue;
        handle_command(server_fd, client_fd);
        close(client_fd);
    }

    posix_spawnattr_destroy(&global_attr);
    return 0;
}
