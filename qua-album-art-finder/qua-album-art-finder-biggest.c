#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <libgen.h>
#include <limits.h>
#include <dirent.h>
#include <unistd.h>
#include <ctype.h>

char winner_path[PATH_MAX] = "";
off_t winner_size = 0;

static inline int is_image_extension(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot || !dot[1]) return 0;

    // Fast-path: switch on the first char of the extension
    switch (tolower((unsigned char)dot[1])) {
        case 'j': return (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0);
        case 'p': return (strcasecmp(dot, ".png") == 0);
        case 'w': return (strcasecmp(dot, ".webp") == 0);
        case 'b': return (strcasecmp(dot, ".bmp") == 0);
        case 't': return (strcasecmp(dot, ".tiff") == 0);
        default:  return 0;
    }
}

void search_directory(const char *dir_path, int depth) {
    DIR *dp = opendir(dir_path);
    if (!dp) return;

    struct dirent *ep;
    struct stat st;
    char full_path[PATH_MAX];

    while ((ep = readdir(dp))) {
        // 1. Instant skip for hidden files
        if (ep->d_name[0] == '.') continue;
        
        // 2. Branch by Type (DT_REG = File, DT_DIR = Directory)
        // This avoids calling stat() on every single file
        if (ep->d_type == DT_REG) {
            // Check for keywords 'c'over or 'f'ront first to avoid strcasestr overhead
            char first = tolower((unsigned char)ep->d_name[0]);
            if (first == 'c' || first == 'f' || strcasestr(ep->d_name, "cover") || strcasestr(ep->d_name, "front")) {
                if (is_image_extension(ep->d_name)) {
                    snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ep->d_name);
                    if (stat(full_path, &st) == 0 && st.st_size > winner_size) {
                        winner_size = st.st_size;
                        memcpy(winner_path, full_path, strlen(full_path) + 1);
                    }
                }
            }
        } 
        else if (ep->d_type == DT_DIR && depth == 0) {
            // Only check for "scans" if we are in the root folder
            if (tolower((unsigned char)ep->d_name[0]) == 's') {
                if (strcasecmp(ep->d_name, "scans") == 0 || strcasecmp(ep->d_name, "scan") == 0) {
                    snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ep->d_name);
                    search_directory(full_path, 1);
                }
            }
        }
    }
    closedir(dp);
}

int main(int argc, char *argv[]) {
    if (argc < 2) return 1;

    char base_target[PATH_MAX];
    struct stat ps;

    if (realpath(argv[argc - 1], base_target) && stat(base_target, &ps) == 0) {
        if (S_ISREG(ps.st_mode)) {
            char *parent = dirname(base_target);
            memmove(base_target, parent, strlen(parent) + 1);
        }
        search_directory(base_target, 0);
    }

    if (winner_size > 0) {
        puts(winner_path);
        return 0;
    }
    return 1;
}
