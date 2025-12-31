#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>

// Find PID of viewer-gtk3.py or viewer-gtk4.py process
static pid_t find_viewer_pid(void) {
  fprintf(stderr, "[DEBUG] Searching for viewer process...\n");
  DIR *proc = opendir("/proc");
  if (!proc) {
    fprintf(stderr, "[DEBUG] Failed to open /proc\n");
    return -1;
  }
  
  struct dirent *entry;
  pid_t viewer_pid = -1;
  
  while ((entry = readdir(proc)) != NULL) {
    // Skip non-numeric entries
    if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
    
    char cmdline_path[256];
    snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%s/cmdline", entry->d_name);
    
    FILE *f = fopen(cmdline_path, "r");
    if (!f) continue;
    
    char cmdline[1024];
    size_t len = fread(cmdline, 1, sizeof(cmdline) - 1, f);
    fclose(f);
    
    if (len > 0) {
      cmdline[len] = '\0';
      // Replace null bytes with spaces for easier matching
      for (size_t i = 0; i < len; i++) {
        if (cmdline[i] == '\0') cmdline[i] = ' ';
      }
      
      if (strstr(cmdline, "viewer-gtk3.py") || strstr(cmdline, "viewer-gtk4.py")) {
        viewer_pid = atoi(entry->d_name);
        fprintf(stderr, "[DEBUG] Found viewer process: PID=%d, cmdline=%s\n", viewer_pid, cmdline);
        break;
      }
    }
  }
  
  closedir(proc);
  if (viewer_pid == -1) {
    fprintf(stderr, "[DEBUG] No viewer process found\n");
  }
  return viewer_pid;
}

static void run_command_async(const char *cmd, char *const args[]) {
  fprintf(stderr, "[DEBUG] Running command asynchronously: %s", cmd);
  for (int i = 1; args[i] != NULL; i++) {
    fprintf(stderr, " %s", args[i]);
  }
  fprintf(stderr, "\n");
  
  pid_t pid = fork();
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
    exit(1);
  } else if (pid > 0) {
    fprintf(stderr, "[DEBUG] Forked child process: PID=%d\n", pid);
  } else {
    fprintf(stderr, "[DEBUG] Fork failed\n");
  }
}

// Signal handlers
static void handle_sigusr1(int sig) {
  fprintf(stderr, "[DEBUG] Received SIGUSR1\n");
  pid_t viewer_pid = find_viewer_pid();
  if (viewer_pid > 0) {
    fprintf(stderr, "[DEBUG] Forwarding SIGUSR1 to viewer PID %d\n", viewer_pid);
    int result = kill(viewer_pid, SIGUSR1);
    if (result == 0) {
      fprintf(stderr, "[DEBUG] Successfully sent SIGUSR1 to viewer\n");
    } else {
      fprintf(stderr, "[DEBUG] Failed to send SIGUSR1 to viewer: %s\n", strerror(errno));
    }
  } else {
    fprintf(stderr, "[DEBUG] No viewer found, running qua-play -n 1\n");
    char *args[] = {"qua-play", "-n", "1", NULL};
    run_command_async("qua-play", args);
  }
}

static void handle_sigusr2(int sig) {
  fprintf(stderr, "[DEBUG] Received SIGUSR2\n");
  pid_t viewer_pid = find_viewer_pid();
  if (viewer_pid > 0) {
    fprintf(stderr, "[DEBUG] Forwarding SIGUSR2 to viewer PID %d\n", viewer_pid);
    int result = kill(viewer_pid, SIGUSR2);
    if (result == 0) {
      fprintf(stderr, "[DEBUG] Successfully sent SIGUSR2 to viewer\n");
    } else {
      fprintf(stderr, "[DEBUG] Failed to send SIGUSR2 to viewer: %s\n", strerror(errno));
    }
  } else {
    fprintf(stderr, "[DEBUG] No viewer found, running qua-play -n -1\n");
    char *args[] = {"qua-play", "-n", "-1", NULL};
    run_command_async("qua-play", args);
  }
}

static void handle_sigcont(int sig) {
    char *args[] = {"qua-play", NULL};
    run_command_async("qua-play", args);
}

int main(void) {
  // Single instance check
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
  
  fprintf(stderr, "[qua-signals] Signal daemon running (PID: %d)\n", getpid());
  fprintf(stderr, "[DEBUG] Registered handlers for SIGUSR1, SIGUSR2, SIGCONT\n");
  
  while (1) {
    fprintf(stderr, "[DEBUG] Waiting for signals...\n");
    pause();
  }
}
