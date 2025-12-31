#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <unistd.h>

static void run_command_async(const char *cmd, char *const args[]) {
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    execvp(cmd, args);
    exit(1);
  }
}

// Signal handlers
static void handle_sigusr1(int sig) {
  char *args[] = {"qua-play", "-n", "1", NULL};
  run_command_async("qua-play", args);
}

static void handle_sigusr2(int sig) {
  char *args[] = {"qua-play", "-n", "-1", NULL};
  run_command_async("qua-play", args);
}

static void handle_sigcont(int sig) {
  char *args[] = {"qua-play", NULL};
  run_command_async("qua-play", args);
}

int main(void) {
  int lock_fd = open("/tmp/qua-signal-daemon.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0666);
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
  									 // STOP just call qua-stop directly

  fprintf(stderr, "qua signal daemon running (PID: %d)\n", getpid());
  
  while (1) {
    pause();
  }

  // Technically unreachable
  // close(lock_fd);
  // return 0;
}
