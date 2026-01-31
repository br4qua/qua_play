#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
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

#define QUA_PLAY_CMD "qua-play"
#define SOCKET_PATH "/tmp/qua-socket.sock"
#define LOCK_PATH "/tmp/qua-socket-daemon.lock"
#define BUF_SIZE 4096

extern char **environ;

static posix_spawnattr_t global_attr;

static void sigchld_handler(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

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

    pid_t pid;
    char *args[] = {(char *)hook, (char *)audio_path, NULL};
    posix_spawn(&pid, hook, NULL, NULL, args, environ);
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
        while (kill(pids[i], 0) == 0) {
            usleep(1000);  // 1ms poll
        }
    }
}

// Convenience: blocking kill (for stop command)
static void kill_player(void) {
    pid_t pids[16];
    int count = kill_players_async(pids, 16);
    kill_players_wait(pids, count);
}

static void spawn_play(const char *path) {
    // Fork: kill players (instant SIGKILL, no wait yet)
    pid_t killed_pids[16];
    int killed_count = kill_players_async(killed_pids, 16);

    // Run prelaunch hook in parallel (blocking)
    run_hook(hook_prelaunch, path);

    // Join: ensure players are dead before spawning new one
    kill_players_wait(killed_pids, killed_count);

    // Spawn new player
    pid_t pid;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    char *args[] = {QUA_PLAY_CMD, (char *)path, NULL};
    posix_spawnp(&pid, QUA_PLAY_CMD, &fa, &global_attr, args, environ);
    posix_spawn_file_actions_destroy(&fa);
}

static void handle_command(int client_fd) {
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
        } else {
            dprintf(client_fd, "Nothing playable\n");
        }
    } else if (strcmp(action, "play-next") == 0) {
        char next[PATH_MAX];
        if (get_next(last_played, 1, next, sizeof(next)) == 0) {
            spawn_play(next);
            log_play(next);
            snprintf(last_played, sizeof(last_played), "%s", next);
            dprintf(client_fd, "Next: %s\n", strrchr(next, '/') ? strrchr(next, '/') + 1 : next);
        }
    } else if (strcmp(action, "play-prev") == 0) {
        char prev[PATH_MAX];
        if (get_next(last_played, -1, prev, sizeof(prev)) == 0) {
            spawn_play(prev);
            log_play(prev);
            snprintf(last_played, sizeof(last_played), "%s", prev);
            dprintf(client_fd, "Prev: %s\n", strrchr(prev, '/') ? strrchr(prev, '/') + 1 : prev);
        }
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

    // Reap children to prevent zombies
    struct sigaction sa = {.sa_handler = sigchld_handler, .sa_flags = SA_RESTART};
    sigaction(SIGCHLD, &sa, NULL);

    // Ignore SIGPIPE (broken pipe when client disconnects)
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "[qua-socket] Listening on %s\n", SOCKET_PATH);

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd == -1) continue;
        handle_command(client_fd);
        close(client_fd);
    }

    posix_spawnattr_destroy(&global_attr);
    return 0;
}
