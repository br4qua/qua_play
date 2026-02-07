#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/personality.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <linux/prctl.h>
#include <unistd.h>

#include "qua-launcher.h"

_Noreturn void launcher_exec(int core_id, const char *player,
			     char *const argv[])
{
	/* CPU affinity: pin to specified core */
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(core_id, &cpuset);
	sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);

	/* Real-time priority: SCHED_FIFO at max */
	struct sched_param param = { .sched_priority = 99 };
	sched_setscheduler(0, SCHED_FIFO, &param);

	/* OOM protection */
	int oom_fd = open("/proc/self/oom_score_adj", O_WRONLY);
	if (oom_fd >= 0) {
		write(oom_fd, "-1000", 5);
		close(oom_fd);
	}

	/* Disable ASLR for PGO/BOLT determinism */
	personality(ADDR_NO_RANDOMIZE);

	/* Disable speculative execution mitigations */
	// PR_SPEC_STORE_BYPASS: Disable Spectre v4 protection
	// if (prctl(PR_SET_SPECULATION_CTRL, PR_SPEC_STORE_BYPASS,
	//           PR_SPEC_DISABLE, 0, 0) == 0) {
	// }

	// PR_SPEC_INDIRECT_BRANCH: Disable Spectre v2 protection
	// if (prctl(PR_SET_SPECULATION_CTRL, PR_SPEC_INDIRECT_BRANCH,
	//           PR_SPEC_DISABLE, 0, 0) == 0) {
	// }

	/* Close all file descriptors */
	DIR *dir = opendir("/proc/self/fd");
	if (dir) {
		struct dirent *entry;
		int dir_fd = dirfd(dir);
		while ((entry = readdir(dir)) != NULL) {
			int fd = atoi(entry->d_name);
			if (fd != dir_fd)
				close(fd);
		}
		closedir(dir);
	} else {
		struct rlimit rl;
		if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
			for (int fd = 0; fd < (int)rl.rlim_max; fd++)
				close(fd);
		}
	}

	/* New session */
	setsid();

	execve(player, argv, NULL);
	_exit(1);
}
