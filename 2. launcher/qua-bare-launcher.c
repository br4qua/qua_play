#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <dirent.h>

#include <sys/personality.h>
#include <sys/prctl.h>
#include <linux/prctl.h>


/**
 * @file bare-launcher.c
 * @brief High-Quality, Low-Latency Execution Environment Launcher.
 *
 * PURPOSE:
 * This program ensures the executed audio player binary runs in a deterministic, 
 * low-jitter environment critical for two goals: 
 * 1. Achieving stable, real-time audio playback performance.
 * 2. Guaranteeing consistent memory layout (Stack Pinning) for advanced 
 * optimizations like PGO and BOLT.
 *
 * KEY ACTIONS:
 * - CPU Affinity: Pins the process to a single core to minimize cache misses.
 * - Real-Time Priority: Sets SCHED_FIFO priority 99 to prevent OS preemption.
 * - Timer Slack: Reduces OS jitter for precise timing.
 * - OOM Protection: Prevents the Out-of-Memory killer from terminating the process.
 * - Fixed Environment: Guarantees a consistent stack pointer address for optimization determinism.
 *
 * INPUT MAPPING:
 * $1 (argv[1]) -> Core ID (integer)
 * $2 (argv[2]) -> Binary Path (e.g., ./player.pgo3)
 * $3+ (argv[3..]) -> Arguments passed directly to the player
 */

int main(int argc, char *argv[]) {
    printf("--- [LAUNCHER DEBUG] Starting Setup ---\n");

    if (argc < 3) {
        fprintf(stderr, "[ERROR] Missing arguments.\n");
        fprintf(stderr, "Usage: bare-launcher <core_id> <player_path> [args...]\n");
        return 1;
    }

    // --- INPUT 1: CORE ID ---
    int target_core = atoi(argv[1]);
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(target_core, &cpuset); 
    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) == 0) {
        printf("[SUCCESS] CPU Affinity pinned to core %d\n", target_core);
    } else {
        fprintf(stderr, "[FAILED] Affinity: %s\n", strerror(errno));
    }

    // --- SCHEDULER SETUP ---
    struct sched_param param;
    param.sched_priority = 99; // Max Real-Time Priority
    if (sched_setscheduler(0, SCHED_FIFO, &param) == 0) {
        printf("[SUCCESS] Scheduler set to SCHED_FIFO (Pri 99)\n");
    } else {
        fprintf(stderr, "[FAILED] SCHED_FIFO: %s (Check setcap or sudo!)\n", strerror(errno));
    }

    // --- TIMER SLACK ---
    // Reduces OS jitter for internal poll/nanosleep calls
    // prctl(PR_SET_TIMERSLACK, 1);
    // printf("[SUCCESS] Timer Slack set to 1ns\n");

    // --- OOM PROTECTION ---
    int oom_fd = open("/proc/self/oom_score_adj", O_WRONLY);
    if (oom_fd >= 0) {
        write(oom_fd, "-1000", 5);
        close(oom_fd);
        printf("[SUCCESS] OOM protection active (-1000)\n");
    }

    // --- FIXED ENVIRONMENT (The "Stack Pin") ---
    // By keeping these strings identical for PGO and Production, 
    // the Stack Pointer ($rsp) stays at the exact same address.
    // char *fixed_env[] = {
    //     "PGO_MODE=1",
    //     "GCOV_PREFIX=.", // Helps profiler find where to write .gcda files
	   //   NULL
    // };
//     printf("[DEBUG] Executing: %s\n", argv[2]);
//     printf("--- [LAUNCHER DEBUG] Handover to Player ---\n");/ 


	if (personality(ADDR_NO_RANDOMIZE) == -1) {
	    fprintf(stderr, "[FAILED] personality: %s\n", strerror(errno));
	} else {
	    printf("[SUCCESS] ASLR disabled\n");
	}
	
	// Disable speculative execution mitigations
	// PR_SPEC_STORE_BYPASS: Disable Spectre v4 protection
	// if (prctl(PR_SET_SPECULATION_CTRL, PR_SPEC_STORE_BYPASS, 
	//           PR_SPEC_DISABLE, 0, 0) == 0) {
	//     printf("[SUCCESS] Spectre v4 mitigation disabled\n");
	// }
	
	// PR_SPEC_INDIRECT_BRANCH: Disable Spectre v2 protection  
	// if (prctl(PR_SET_SPECULATION_CTRL, PR_SPEC_INDIRECT_BRANCH,
	//           PR_SPEC_DISABLE, 0, 0) == 0) {
	//     printf("[SUCCESS] Spectre v2 mitigation disabled\n");
	// }

	DIR *dir = opendir("/proc/self/fd");
	if (dir) {
	  struct dirent *entry;
	  int dir_fd = dirfd(dir);
	  while ((entry = readdir(dir)) != NULL) {
	    int fd = atoi(entry->d_name);
	    if (fd != dir_fd) {
	      close(fd);
	    }
	  }
	  closedir(dir);
	} else {
	  // Fallback: close everything including stdin/stdout/stderr
	  struct rlimit rl;
	  if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
	    for (int fd = 0; fd < (int)rl.rlim_max; fd++) {
	      close(fd);
	    }
	  }
	}
	
	setsid();
	execve(argv[2], &argv[2], NULL);


	// Should be unreachable
    perror("[FATAL] execve failed");
    return 1;
}
