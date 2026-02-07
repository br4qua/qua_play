#ifndef QUA_CACHE_H
#define QUA_CACHE_H

#include <stddef.h>

#define CACHE_DIR "/dev/shm/qua-cache"
#define CACHE_MAX_SIZE (2ULL * 1024 * 1024 * 1024)

// Initialize cache directory (create if needed)
void cache_init(void);

// Generate cache path from source file (inode + mtime)
// Returns 0 on success, -1 on failure
int cache_generate_path(const char *filepath, char *cache_path, size_t size);

// Check if cache file exists
int cache_exists(const char *cache_path);

// Manage cache size - delete old entries if over limit
void cache_manage_size(void);

#endif
