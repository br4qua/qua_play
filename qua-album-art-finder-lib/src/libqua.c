#define _GNU_SOURCE
#include "libqua.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <libgen.h>
#include <limits.h>
#include <dirent.h>
#include <ctype.h>

static inline int is_image_extension(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot || !dot[1]) return 0;

    switch (tolower((unsigned char)dot[1])) {
        case 'j': return (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0);
        case 'p': return (strcasecmp(dot, ".png") == 0);
        case 'w': return (strcasecmp(dot, ".webp") == 0);
        case 'b': return (strcasecmp(dot, ".bmp") == 0);
        case 't': return (strcasecmp(dot, ".tiff") == 0);
        default:  return 0;
    }
}

// Internal helper using d_type to avoid redundant stat() calls
static void scan_dir_for_art(const char *dir_path, char *winner_path, off_t *max_size) {
    DIR *dp = opendir(dir_path);
    if (!dp) return;

    struct dirent *ep;
    struct stat st;

    while ((ep = readdir(dp))) {
        // Skip hidden and parent/current dirs
        if (ep->d_name[0] == '.') continue;

        // Only process regular files
        if (ep->d_type == DT_REG) {
            char first = tolower((unsigned char)ep->d_name[0]);
            // Fast-path: check first char before running expensive strcasestr
            if (first == 'c' || first == 'f' || strcasestr(ep->d_name, "cover") || strcasestr(ep->d_name, "front")) {
                if (is_image_extension(ep->d_name)) {
                    char full_path[PATH_MAX];
                    if (snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ep->d_name) < (int)sizeof(full_path)) {
                        if (stat(full_path, &st) == 0 && st.st_size > *max_size) {
                            *max_size = st.st_size;
                            memcpy(winner_path, full_path, strlen(full_path) + 1);
                        }
                    }
                }
            }
        }
    }
    closedir(dp);
}

__attribute__((visibility("default")))
char* find_biggest_art(const char *input_path) {
    if (!input_path) return NULL;

    char base_dir[PATH_MAX];
    struct stat st;

    if (!realpath(input_path, base_dir)) return NULL;
    
    // Resolve target directory if a file was provided
    if (stat(base_dir, &st) == 0 && S_ISREG(st.st_mode)) {
        char *parent = dirname(base_dir);
        char temp[PATH_MAX];
        snprintf(temp, sizeof(temp), "%s", parent);
        snprintf(base_dir, sizeof(base_dir), "%s", temp);
    }

    char global_winner[PATH_MAX] = "";
    off_t max_size = -1;

    // 1. Scan root folder
    scan_dir_for_art(base_dir, global_winner, &max_size);

    // 2. Scan potential subfolders using d_type logic for efficiency
    const char *subdirs[] = {"scan", "scans", "Scan", "Scans"};
    for (int i = 0; i < 4; i++) {
        char sub_path[PATH_MAX];
        if (snprintf(sub_path, sizeof(sub_path), "%s/%s", base_dir, subdirs[i]) < (int)sizeof(sub_path)) {
            // Check if it's a directory before calling the scan helper
            if (stat(sub_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                scan_dir_for_art(sub_path, global_winner, &max_size);
            }
        }
    }

    return (max_size != -1) ? strdup(global_winner) : NULL;
}
