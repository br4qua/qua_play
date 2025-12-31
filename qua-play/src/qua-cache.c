#include "qua-cache.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
  char name[256];
  time_t atime;
  off_t size;
} cache_entry_t;

static int compare_atime(const void *a, const void *b) {
  const cache_entry_t *ea = (const cache_entry_t *)a;
  const cache_entry_t *eb = (const cache_entry_t *)b;
  if (ea->atime < eb->atime) return -1;
  if (ea->atime > eb->atime) return 1;
  return 0;
}

bool qua_cache_generate_path(const char *filepath, char *cache_path, size_t cache_path_size) {
  struct stat st;
  if (stat(filepath, &st) != 0)
    return false;
  
  // Ensure cache directory exists
  qua_cache_init();
  
  // Generate cache path from file stats (inode + mtime)
  snprintf(cache_path, cache_path_size, "%s/qua-%lx-%lx.wav", CACHE_DIR,
           (unsigned long)st.st_ino, (unsigned long)st.st_mtime);
  return true;
}

void qua_cache_init(void) {
  mkdir(CACHE_DIR, 0755);
}

void qua_cache_manage_size(void) {
  struct dirent **namelist;
  int n = scandir(CACHE_DIR, &namelist, NULL, NULL);
  if (n < 0)
    return;
  
  // Collect file info
  cache_entry_t *entries = malloc(n * sizeof(cache_entry_t));
  int count = 0;
  long long total = 0;
  
  for (int i = 0; i < n; i++) {
    if (namelist[i]->d_name[0] == '.')
      continue;
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", CACHE_DIR, namelist[i]->d_name);
    struct stat s;
    if (stat(p, &s) == 0 && S_ISREG(s.st_mode)) {
      strncpy(entries[count].name, namelist[i]->d_name, sizeof(entries[count].name) - 1);
      entries[count].name[sizeof(entries[count].name) - 1] = '\0';
      entries[count].atime = s.st_atime;
      entries[count].size = s.st_size;
      total += s.st_size;
      count++;
    }
  }
  
  // Sort by atime (least recently accessed first) and delete until under threshold
  if (total > CACHE_MAX_SIZE) {
    qsort(entries, count, sizeof(cache_entry_t), compare_atime);
    for (int i = 0; i < count && total > (CACHE_MAX_SIZE * 0.7); i++) {
      char p[PATH_MAX];
      snprintf(p, sizeof(p), "%s/%s", CACHE_DIR, entries[i].name);
      unlink(p);
      total -= entries[i].size;
    }
  }
  
  free(entries);
  for (int i = 0; i < n; i++)
    free(namelist[i]);
  free(namelist);
}
