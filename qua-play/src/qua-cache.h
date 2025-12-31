#ifndef QUA_CACHE_H
#define QUA_CACHE_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>

#define CACHE_DIR "/dev/shm/qua-cache"
#define CACHE_MAX_SIZE (2ULL * 1024 * 1024 * 1024)

// Generate cache file path from source file (inode + mtime)
// Returns true on success, false on failure
bool qua_cache_generate_path(const char *filepath, char *cache_path, size_t cache_path_size);

// Initialize cache directory (create if needed)
void qua_cache_init(void);

// Manage cache size (delete old entries if over limit)
void qua_cache_manage_size(void);

#endif
