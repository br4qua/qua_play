#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <unistd.h>
#include <dirent.h>

#define QUA_PLAY_CMD "qua-play"

static void run_command_async(const char *cmd, char *const args[]) {
  pid_t pid = vfork();
  if (pid == 0) {
    // Start new session
    setsid();
    
    // Reset signal handlers to defaults
    signal(SIGCHLD, SIG_DFL);
    signal(SIGUSR1, SIG_DFL);
    signal(SIGUSR2, SIG_DFL);
    signal(SIGCONT, SIG_DFL);
    
    // Unblock all signals
    sigset_t set;
    sigemptyset(&set);
    sigprocmask(SIG_SETMASK, &set, NULL);
    
    execvp(cmd, args);
    _exit(1);
  }
}

// Signal handlers
static void handle_sigusr1(int sig) {
  char *args[] = {QUA_PLAY_CMD, "-n", "1", NULL};
  run_command_async(QUA_PLAY_CMD, args);
}

static void handle_sigusr2(int sig) {
  char *args[] = {QUA_PLAY_CMD, "-p", "1", NULL};
  run_command_async(QUA_PLAY_CMD, args);
}

static void handle_sigcont(int sig) {
  char *args[] = {QUA_PLAY_CMD, NULL};
  run_command_async(QUA_PLAY_CMD, args);
}

int main(void) {
  // Single instance check (O_CLOEXEC no longer strictly needed but good practice)
  int lock_fd = open("/tmp/qua-signal-daemon.lock", 
                     O_CREAT | O_RDWR | O_CLOEXEC, 0666);
  if (lock_fd == -1) {
    perror("Cannot open lock file");
    return 1;
  }
  
  if (flock(lock_fd, LOCK_EX | LOCK_NB) == -1) {
    fprintf(stderr, "[qua-signals] Another instance is already running\n");
    close(lock_fd);
    return 1;
  }
  
  // Setup Handlers
  signal(SIGCHLD, SIG_IGN);
  signal(SIGUSR1, handle_sigusr1);   // Next
  signal(SIGUSR2, handle_sigusr2);   // Previous
  signal(SIGCONT, handle_sigcont);   // Play
  
  fprintf(stderr, "qua signal daemon running (PID: %d, command: %s)\n", getpid(), QUA_PLAY_CMD);
  
  while (1) {
    pause();
  }
}
