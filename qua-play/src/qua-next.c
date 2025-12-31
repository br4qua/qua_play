#include "qua-next.h"

#include <dirent.h>
#include <libgen.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static bool is_audio(const char *name) {
  const char *dot = strrchr(name, '.');
  if (!dot || dot == name || *(dot + 1) == '\0')
    return false;

  dot++; // Skip the '.'

  // Fast dispatch on first character
  switch (dot[0]) {
  case 'a':
    return strcmp(dot, "ape") == 0 || strcmp(dot, "aiff") == 0 ||
           strcmp(dot, "aif") == 0;
  case 'f':
    return strcmp(dot, "flac") == 0;
  case 'm':
    return strcmp(dot, "mp3") == 0 || strcmp(dot, "m4a") == 0;
  case 'o':
    return strcmp(dot, "opus") == 0 || strcmp(dot, "ogg") == 0;
  case 'w':
    return strcmp(dot, "wv") == 0 || strcmp(dot, "wav") == 0;
  default:
    return false;
  }
}

static int filter_fn(const struct dirent *entry) { return is_audio(entry->d_name); }

// Navigate to next/previous file in directory based on offset
// Returns 0 on success, 1 on failure
int qua_next(const char *current_path, int offset, char *result, size_t result_size) {
  if (!current_path || current_path[0] == '\0' || !result || result_size == 0)
    return 1;

  char *d_copy = strdup(current_path);
  char *d_name = dirname(d_copy);
  struct dirent **nl;

  // Fast "Dir" scan and Alphabetical Sort
  int n = scandir(d_name, &nl, filter_fn, alphasort);

  if (n <= 0) {
    free(d_copy);
    return 1;
  }

  // Find current file index
  int cur_idx = -1;
  for (int i = 0; i < n; i++) {
    char full[PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s", d_name, nl[i]->d_name);
    if (strcmp(full, current_path) == 0) {
      cur_idx = i;
      break;
    }
  }

  // Wrap-around math: (current + offset) % total
  int next = (cur_idx == -1) ? 0 : (cur_idx + offset) % n;
  if (next < 0)
    next += n; // Handle negative offsets for "Previous"

  // Write result path directly to buffer
  snprintf(result, result_size, "%s/%s", d_name, nl[next]->d_name);

  // Cleanup
  for (int i = 0; i < n; i++)
    free(nl[i]);
  free(nl);
  free(d_copy);

  return 0;
}
