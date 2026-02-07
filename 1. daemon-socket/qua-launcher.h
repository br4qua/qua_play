#ifndef QUA_LAUNCHER_H
#define QUA_LAUNCHER_H

/*
 * Prepare execution environment and exec the audio player.
 * Sets CPU affinity, real-time scheduling, OOM protection,
 * disables ASLR, closes all FDs, creates a new session,
 * then execve()s the player. Does not return on success.
 *
 * Must be called in a forked child.
 */
_Noreturn void launcher_exec(int core_id, const char *player,
			     char *const argv[]);

#endif
