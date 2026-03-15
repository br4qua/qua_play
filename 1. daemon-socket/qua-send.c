#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <signal.h>
#include <dirent.h>
#include <limits.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <poll.h>

#define SOCK_PATH "/tmp/qua-socket.sock"
#define LOCK_PATH "/tmp/qua-socket-daemon.lock"
#define CACHE_DIR "/dev/shm/qua-cache"

/* ── Socket I/O ──────────────────────────────────────────────── */

/*
 * Connect to daemon, send buf, read response.
 * If resp is NULL, copies response to stdout.
 * Returns 0 on success, -1 on failure.
 */
static int sock_exchange(const char *buf, size_t len,
			 char *resp, size_t resp_sz)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	memcpy(addr.sun_path, SOCK_PATH, sizeof(SOCK_PATH));

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}

	const char *p = buf;
	size_t rem = len;
	while (rem > 0) {
		ssize_t n = write(fd, p, rem);
		if (n <= 0) {
			close(fd);
			return -1;
		}
		p += n;
		rem -= n;
	}
	shutdown(fd, SHUT_WR);

	if (resp && resp_sz > 0) {
		size_t total = 0;
		while (total < resp_sz - 1) {
			ssize_t n = read(fd, resp + total,
					 resp_sz - 1 - total);
			if (n <= 0)
				break;
			total += n;
		}
		resp[total] = '\0';
	} else {
		char tmp[4096];
		ssize_t n;
		while ((n = read(fd, tmp, sizeof(tmp))) > 0)
			fwrite(tmp, 1, n, stdout);
	}

	close(fd);
	return 0;
}

/* ── Message building ────────────────────────────────────────── */

/*
 * Build null-terminated message: "cmd\0path1\0path2\0..."
 * argv[0] = command, argv[1..] = file args (resolved with realpath).
 * Returns malloc'd buffer; caller frees. Sets *out_len.
 */
static char *build_play_msg(int argc, char **argv, size_t *out_len)
{
	char resolved[PATH_MAX];

	/* First pass: compute total size */
	size_t total = strlen(argv[0]) + 1;
	for (int i = 1; i < argc; i++) {
		if (!realpath(argv[i], resolved)) {
			fprintf(stderr, "%s: %s\n", argv[i],
				strerror(errno));
			return NULL;
		}
		total += strlen(resolved) + 1;
	}

	char *buf = malloc(total);
	if (!buf)
		return NULL;

	/* Second pass: fill buffer */
	size_t off = 0;
	size_t slen = strlen(argv[0]) + 1;
	memcpy(buf + off, argv[0], slen);
	off += slen;

	for (int i = 1; i < argc; i++) {
		realpath(argv[i], resolved);
		slen = strlen(resolved) + 1;
		memcpy(buf + off, resolved, slen);
		off += slen;
	}

	*out_len = off;
	return buf;
}

/* ── Process management ──────────────────────────────────────── */

static int find_pids(const char *name, pid_t *pids, int max)
{
	DIR *proc = opendir("/proc");
	if (!proc)
		return 0;

	int count = 0;
	struct dirent *ent;
	while ((ent = readdir(proc)) && count < max) {
		char *end;
		long pid = strtol(ent->d_name, &end, 10);
		if (*end || pid <= 0)
			continue;

		char path[64];
		snprintf(path, sizeof(path), "/proc/%ld/comm", pid);

		FILE *f = fopen(path, "r");
		if (!f)
			continue;

		char comm[256];
		if (fgets(comm, sizeof(comm), f)) {
			comm[strcspn(comm, "\n")] = '\0';
			if (strcmp(comm, name) == 0)
				pids[count++] = (pid_t)pid;
		}
		fclose(f);
	}
	closedir(proc);
	return count;
}

static int is_running(const char *name)
{
	pid_t pid;
	return find_pids(name, &pid, 1) > 0;
}

static void killall(const char *name)
{
	pid_t pids[64];
	int n = find_pids(name, pids, 64);
	for (int i = 0; i < n; i++)
		kill(pids[i], SIGKILL);
}

/* ── Daemon lifecycle ────────────────────────────────────────── */

static int wait_for_socket(void)
{
	if (access(SOCK_PATH, F_OK) == 0)
		return 0;

	int ifd = inotify_init1(IN_CLOEXEC);
	if (ifd < 0)
		return -1;

	int wd = inotify_add_watch(ifd, "/tmp", IN_CREATE);
	if (wd < 0) {
		close(ifd);
		return -1;
	}

	/* Re-check after watch to avoid race */
	if (access(SOCK_PATH, F_OK) == 0) {
		close(ifd);
		return 0;
	}

	char buf[4096]
		__attribute__((aligned(__alignof__(struct inotify_event))));

	struct pollfd pfd = { .fd = ifd, .events = POLLIN };
	for (;;) {
		if (poll(&pfd, 1, 5000) <= 0) {
			close(ifd);
			return -1;
		}

		ssize_t n = read(ifd, buf, sizeof(buf));
		if (n <= 0)
			break;

		char *ptr = buf;
		while (ptr < buf + n) {
			struct inotify_event *ev =
				(struct inotify_event *)ptr;
			if (ev->len > 0 &&
			    strcmp(ev->name, "qua-socket.sock") == 0) {
				close(ifd);
				return 0;
			}
			ptr += sizeof(*ev) + ev->len;
		}
	}

	close(ifd);
	return -1;
}

static pid_t start_daemon(void)
{
	if (is_running("qua-socket")) {
		puts("already running");
		return 0;
	}

	pid_t pid = fork();
	if (pid < 0)
		return -1;

	if (pid == 0) {
		setsid();
		const char *home = getenv("HOME");
		if (home)
			chdir(home);

		int null = open("/dev/null", O_RDWR);
		if (null >= 0) {
			dup2(null, STDIN_FILENO);
			dup2(null, STDOUT_FILENO);
			dup2(null, STDERR_FILENO);
			if (null > 2)
				close(null);
		}
		execlp("qua-socket", "qua-socket", NULL);
		_exit(127);
	}

	if (wait_for_socket() < 0) {
		fprintf(stderr, "timed out waiting for socket\n");
		return -1;
	}

	printf("(%d) qua-socket\n", pid);
	return pid;
}

static void kill_daemon(void)
{
	killall("qua-socket");
	killall("qua-player");
	killall("qua-convert");

	struct timespec ts = { .tv_nsec = 10000000 }; /* 10ms */
	while (is_running("qua-socket"))
		nanosleep(&ts, NULL);

	unlink(SOCK_PATH);
	unlink(LOCK_PATH);
}

/* ── Cache clearing ──────────────────────────────────────────── */

static void rm_rf_contents(const char *dir)
{
	DIR *d = opendir(dir);
	if (!d)
		return;

	struct dirent *ent;
	while ((ent = readdir(d))) {
		if (ent->d_name[0] == '.' &&
		    (ent->d_name[1] == '\0' ||
		     (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
			continue;

		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

		struct stat st;
		if (lstat(path, &st) < 0)
			continue;

		if (S_ISDIR(st.st_mode)) {
			rm_rf_contents(path);
			rmdir(path);
		} else {
			unlink(path);
		}
	}
	closedir(d);
}

static void clear_cache(void)
{
	DIR *d = opendir("/dev/shm");
	if (d) {
		struct dirent *ent;
		while ((ent = readdir(d))) {
			if (strncmp(ent->d_name, "raw-", 4) == 0) {
				char path[PATH_MAX];
				snprintf(path, sizeof(path),
					 "/dev/shm/%s", ent->d_name);
				unlink(path);
			}
		}
		closedir(d);
	}

	rm_rf_contents(CACHE_DIR);
	puts("Cache cleared");
}

/* ── History ─────────────────────────────────────────────────── */

struct hist_entry {
	time_t ts;
	char *path;
};

static int read_history(struct hist_entry **out, int *out_count)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	char hist_path[PATH_MAX];

	if (xdg)
		snprintf(hist_path, sizeof(hist_path),
			 "%s/qua-player/history", xdg);
	else if (home)
		snprintf(hist_path, sizeof(hist_path),
			 "%s/.config/qua-player/history", home);
	else
		return -1;

	FILE *f = fopen(hist_path, "r");
	if (!f)
		return -1;

	/* Read all lines */
	struct hist_entry *all = NULL;
	int total = 0, alloc = 0;
	char line[PATH_MAX + 32];

	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
		if (!line[0])
			continue;

		char *space = strchr(line, ' ');
		if (!space)
			continue;

		if (total >= alloc) {
			alloc = alloc ? alloc * 2 : 256;
			all = realloc(all, alloc * sizeof(*all));
		}
		all[total].ts = strtol(line, NULL, 10);
		all[total].path = strdup(space + 1);
		total++;
	}
	fclose(f);

	/* Reverse iterate, deduplicate */
	struct hist_entry *dedup = NULL;
	int count = 0, dalloc = 0;

	for (int i = total - 1; i >= 0; i--) {
		int dup = 0;
		for (int j = 0; j < count; j++) {
			if (strcmp(dedup[j].path, all[i].path) == 0) {
				dup = 1;
				break;
			}
		}
		if (!dup) {
			if (count >= dalloc) {
				dalloc = dalloc ? dalloc * 2 : 256;
				dedup = realloc(dedup,
						dalloc * sizeof(*dedup));
			}
			dedup[count].ts = all[i].ts;
			dedup[count].path = all[i].path;
			count++;
		} else {
			free(all[i].path);
		}
	}
	free(all);

	*out = dedup;
	*out_count = count;
	return 0;
}

static void free_history(struct hist_entry *entries, int count)
{
	for (int i = 0; i < count; i++)
		free(entries[i].path);
	free(entries);
}

static void send_play_path(const char *path)
{
	char buf[PATH_MAX + 8];
	size_t off = 0;

	memcpy(buf, "play", 5);
	off += 5;
	size_t plen = strlen(path) + 1;
	memcpy(buf + off, path, plen);
	off += plen;

	sock_exchange(buf, off, NULL, 0);
}

static int cmd_hist_fzf(void)
{
	struct hist_entry *entries;
	int count;

	if (read_history(&entries, &count) < 0) {
		fprintf(stderr, "No history file found\n");
		return 1;
	}

	int pipe_in[2], pipe_out[2];
	if (pipe(pipe_in) || pipe(pipe_out)) {
		free_history(entries, count);
		return 1;
	}

	pid_t pid = fork();
	if (pid < 0) {
		free_history(entries, count);
		return 1;
	}

	if (pid == 0) {
		close(pipe_in[1]);
		close(pipe_out[0]);
		dup2(pipe_in[0], STDIN_FILENO);
		dup2(pipe_out[1], STDOUT_FILENO);
		close(pipe_in[0]);
		close(pipe_out[1]);

		execlp("fzf", "fzf",
		       "--with-nth=1,3", "--delimiter=\t", "--nth=..",
		       "--height=10", "--layout=reverse", "--border=sharp",
		       "--info=inline", "--no-bold", NULL);
		_exit(127);
	}

	close(pipe_in[0]);
	close(pipe_out[1]);

	/* Feed formatted entries to fzf: "date\tpath\tbasename" */
	FILE *wf = fdopen(pipe_in[1], "w");
	for (int i = 0; i < count; i++) {
		struct tm tm;
		localtime_r(&entries[i].ts, &tm);
		char ts[32];
		strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", &tm);

		const char *bn = strrchr(entries[i].path, '/');
		bn = bn ? bn + 1 : entries[i].path;

		fprintf(wf, "%s\t%s\t%s\n", ts, entries[i].path, bn);
	}
	fclose(wf);

	/* Read selection */
	char sel[PATH_MAX * 2];
	FILE *rf = fdopen(pipe_out[0], "r");
	char *line = fgets(sel, sizeof(sel), rf);
	fclose(rf);

	waitpid(pid, NULL, 0);

	if (line) {
		sel[strcspn(sel, "\n")] = '\0';
		char *tab1 = strchr(sel, '\t');
		if (tab1) {
			char *path = tab1 + 1;
			char *tab2 = strchr(path, '\t');
			if (tab2)
				*tab2 = '\0';
			if (*path)
				send_play_path(path);
		}
	}

	free_history(entries, count);
	return 0;
}

static int cmd_hist_rofi(void)
{
	struct hist_entry *entries;
	int count;

	if (read_history(&entries, &count) < 0)
		return 1;

	int pipe_in[2], pipe_out[2];
	if (pipe(pipe_in) || pipe(pipe_out)) {
		free_history(entries, count);
		return 1;
	}

	pid_t pid = fork();
	if (pid < 0) {
		free_history(entries, count);
		return 1;
	}

	if (pid == 0) {
		close(pipe_in[1]);
		close(pipe_out[0]);
		dup2(pipe_in[0], STDIN_FILENO);
		dup2(pipe_out[1], STDOUT_FILENO);
		close(pipe_in[0]);
		close(pipe_out[1]);

		execlp("rofi", "rofi",
		       "-dmenu", "-i", "-p", "History", "-format", "s",
		       "-display-column-separator", "\t",
		       "-display-columns", "1",
		       "-theme-str", "window {width: 25%;}",
		       "-theme-str", "* {font: \"JetBrains Mono 12\";}",
		       NULL);
		_exit(127);
	}

	close(pipe_in[0]);
	close(pipe_out[1]);

	/* Feed formatted entries to rofi: "date basename\tpath" */
	FILE *wf = fdopen(pipe_in[1], "w");
	for (int i = 0; i < count; i++) {
		struct tm tm;
		localtime_r(&entries[i].ts, &tm);
		char ts[32];
		strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", &tm);

		const char *bn = strrchr(entries[i].path, '/');
		bn = bn ? bn + 1 : entries[i].path;

		fprintf(wf, "%s %s\t%s\n", ts, bn, entries[i].path);
	}
	fclose(wf);

	/* Read selection */
	char sel[PATH_MAX * 2];
	FILE *rf = fdopen(pipe_out[0], "r");
	char *line = fgets(sel, sizeof(sel), rf);
	fclose(rf);

	waitpid(pid, NULL, 0);

	if (line) {
		sel[strcspn(sel, "\n")] = '\0';
		char *tab = strchr(sel, '\t');
		if (tab) {
			char *path = tab + 1;
			if (*path)
				send_play_path(path);
		}
	}

	free_history(entries, count);
	return 0;
}

/* ── Help ────────────────────────────────────────────────────── */

static void show_help(void)
{
	puts(
"qua-send - control qua-player daemon\n"
"\n"
"Usage: qua-send <action> [files...]\n"
"\n"
"Actions:\n"
"  play [file]     Play file, or resume last played\n"
"  next            Play next track in directory\n"
"  prev            Play previous track in directory\n"
"  stop            Stop playback\n"
"  info            Show current track info\n"
"  status          Show playback state (PLAYING/STOPPED)\n"
"  hist            Pick from history with fzf\n"
"  hist-rofi       Pick from history with rofi\n"
"  restart         Kill qua-socket, qua-player, qua-convert\n"
"  cc              Clear cache (/dev/shm/raw-* and /dev/shm/qua-cache)\n"
"\n"
"Examples:\n"
"  qua-send play song.flac\n"
"  qua-send play *.wv\n"
"  qua-send next\n"
"  qua-send stop\n"
"  qua-send history");
}

/* ── Main ────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);

	if (argc < 2 || strcmp(argv[1], "-h") == 0 ||
	    strcmp(argv[1], "--help") == 0) {
		show_help();
		return 0;
	}

	const char *action = argv[1];

	/* next / prev */
	if (strcmp(action, "next") == 0)
		return sock_exchange("play-next", 10, NULL, 0) < 0;
	if (strcmp(action, "prev") == 0)
		return sock_exchange("play-prev", 10, NULL, 0) < 0;

	/* status */
	if (strcmp(action, "status") == 0)
		return sock_exchange("status", 7, NULL, 0) < 0;

	/* play: try connect, auto-start daemon on failure, retry */
	if (strcmp(action, "play") == 0) {
		char *args[argc - 1];
		args[0] = "play";
		for (int i = 2; i < argc; i++)
			args[i - 1] = argv[i];
		int nargs = argc - 1;

		size_t len;
		char *msg = build_play_msg(nargs, args, &len);
		if (!msg)
			return 1;

		if (sock_exchange(msg, len, NULL, 0) == 0) {
			free(msg);
			return 0;
		}

		if (start_daemon() < 0) {
			free(msg);
			return 1;
		}

		int ret = sock_exchange(msg, len, NULL, 0) < 0;
		free(msg);
		return ret;
	}

	/* info: get path from daemon, print it, exec qua-info */
	if (strcmp(action, "info") == 0) {
		char path[PATH_MAX];
		if (sock_exchange("info", 5, path, sizeof(path)) < 0)
			return 1;

		puts(path);
		if (path[0]) {
			execlp("qua-info", "qua-info", path, NULL);
			perror("qua-info");
			return 1;
		}
		return 0;
	}

	/* history pickers */
	if (strcmp(action, "hist") == 0)
		return cmd_hist_fzf();
	if (strcmp(action, "hist-rofi") == 0)
		return cmd_hist_rofi();

	/* daemon lifecycle */
	if (strcmp(action, "start") == 0) {
		start_daemon();
		return 0;
	}
	if (strcmp(action, "kill") == 0) {
		kill_daemon();
		puts("qua-socket killed");
		return 0;
	}
	if (strcmp(action, "restart") == 0) {
		kill_daemon();
		start_daemon();
		return 0;
	}

	/* cache clear */
	if (strcmp(action, "cc") == 0) {
		clear_cache();
		return 0;
	}

	/* fallthrough: send action + resolved file args */
	{
		char *args[argc - 1];
		args[0] = (char *)action;
		for (int i = 2; i < argc; i++)
			args[i - 1] = argv[i];
		int nargs = argc - 1;

		size_t len;
		char *msg = build_play_msg(nargs, args, &len);
		if (!msg)
			return 1;

		int ret = sock_exchange(msg, len, NULL, 0) < 0;
		free(msg);
		return ret;
	}
}
