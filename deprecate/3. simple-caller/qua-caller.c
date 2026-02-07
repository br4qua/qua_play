#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PIDFILE "/tmp/qua-signals.pid"

int main(int argc, char *argv[]) {
    if (argc < 2) return 1;

    const char *home = getenv("HOME");
    if (!home) return 1;

    // Resolve to absolute path
    char fullpath[PATH_MAX];
    if (!realpath(argv[1], fullpath)) return 1;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.config/qua-player/current-song", home);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) return 1;
    write(fd, fullpath, strlen(fullpath));
    write(fd, "\n", 1);
    close(fd);

    fd = open(PIDFILE, O_RDONLY);
    if (fd == -1) return 1;
    char buf[16];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 1;
    buf[n] = '\0';

    kill(atoi(buf), SIGCONT);
    return 0;
}
