#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <unistd.h>

#define QUA_PLAY_CMD "qua-play"

extern char **environ;

// 1. GLOBAL ARGS: Pre-allocated in the data segment (no stack setup time)
static char *const ARGS_NEXT[] = {QUA_PLAY_CMD, "-n", "1", NULL};
static char *const ARGS_PREV[] = {QUA_PLAY_CMD, "-p", "1", NULL};
static char *const ARGS_PLAY[] = {QUA_PLAY_CMD, NULL};

// 2. GLOBAL ATTR: Pre-configured detachment rules
static posix_spawnattr_t global_attr;

static void handle_signal(char *const args[]) {
    pid_t pid;
    // Fires the spawn immediately using pre-configured session detachment
    posix_spawnp(&pid, QUA_PLAY_CMD, NULL, &global_attr, args, environ);
}

// Handler functions (effectively instantaneous)
static void handle_sigusr1(int sig) { handle_signal(ARGS_NEXT); }
static void handle_sigusr2(int sig) { handle_signal(ARGS_PREV); }
static void handle_sigcont(int sig) { handle_signal(ARGS_PLAY); }

int main(void) {
    // Single instance check
    int lock_fd = open("/tmp/qua-signal-daemon.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0666);
    if (lock_fd == -1 || flock(lock_fd, LOCK_EX | LOCK_NB) == -1) {
    	fprintf(stderr, "[qua-signals] Already running or lock error.\n");
        return 1;
    }

    // Initialize global spawn attributes once at startup
    posix_spawnattr_init(&global_attr);

    // POSIX_SPAWN_SETSID is the "magic" that detaches the process.
    // SETSIGDEF and SETSIGMASK ensure the child starts with a clean slate.
    short flags = POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
    posix_spawnattr_setflags(&global_attr, flags);

    sigset_t all_signals, no_signals;
    sigfillset(&all_signals);
    sigemptyset(&no_signals);
    posix_spawnattr_setsigdefault(&global_attr, &all_signals);
    posix_spawnattr_setsigmask(&global_attr, &no_signals);

    // Ignore SIGCHLD so the kernel reaps qua-play instantly (no zombies)
    struct sigaction sa_ign = {.sa_handler = SIG_IGN, .sa_flags = SA_NOCLDWAIT};
    sigaction(SIGCHLD, &sa_ign, NULL);

    // Bind signals
    signal(SIGUSR1, handle_sigusr1);
    signal(SIGUSR2, handle_sigusr2);
    signal(SIGCONT, handle_sigcont);

    fprintf(stderr, "[qua-signals] Ready. Triggering %s via signals.\n", QUA_PLAY_CMD);

    while (1) {
        pause();
    }
    
    // Cleanup (though we never reach here)
    posix_spawnattr_destroy(&global_attr);
    return 0;
}
