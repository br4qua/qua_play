/*
 * PROGRAM: qua-album-art-finder
 * ----------------------------
 * A high-performance C utility for finding album artwork.
 * * LOGIC:
 * 1. Resolves input target once to get a base absolute path.
 * 2. Scans for "cover" or "front" keywords with image extensions.
 * 3. Recursively checks "scans" folders.
 * 4. Sorts results by file size (largest first) as a quality proxy.
 * 5. Respects symlinks by not calling realpath on individual matches.
 */

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

typedef struct {
    char path[PATH_MAX];
    off_t size;
} FileResult;

// Global state for results array
FileResult *results = NULL;
int results_count = 0;
int results_capacity = 20;
int limit = INT_MAX;

// High-speed extension check using a Jump Table (switch)
int is_image_extension(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot || !dot[1]) return 0;

    // Switch on the first character after the dot for O(1) branching
    switch (tolower((unsigned char)dot[1])) {
        case 'j': return (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0);
        case 'p': return (strcasecmp(dot, ".png") == 0);
        case 'w': return (strcasecmp(dot, ".webp") == 0);
        case 'b': return (strcasecmp(dot, ".bmp") == 0);
        case 't': return (strcasecmp(dot, ".tiff") == 0);
        default:  return 0;
    }
}

void add_result(const char *path, off_t size) {
    if (results_count >= results_capacity) {
        results_capacity *= 2;
        results = realloc(results, sizeof(FileResult) * results_capacity);
    }
    strncpy(results[results_count].path, path, PATH_MAX);
    results[results_count].size = size;
    results_count++;
}

// Comparator for qsort: descending order (largest files first)
int compare_files(const void *a, const void *b) {
    off_t size_a = ((FileResult*)a)->size;
    off_t size_b = ((FileResult*)b)->size;
    return (size_b > size_a) - (size_b < size_a);
}

void search_directory(const char *dir_path, int depth) {
    if (depth > 1) return; // Only allow one level of recursion (e.g., for "scans/")
    
    DIR *dp = opendir(dir_path);
    if (!dp) return;

    struct dirent *ep;
    struct stat st;
    char full_path[PATH_MAX];

    while ((ep = readdir(dp))) {
        if (ep->d_name[0] == '.') continue;
        
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ep->d_name);
        
        // Logical check: Keywords AND Extension
        if (strcasestr(ep->d_name, "cover") || strcasestr(ep->d_name, "front")) {
            if (is_image_extension(ep->d_name)) {
                if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode)) {
                    add_result(full_path, st.st_size);
                }
            }
        } 
        // Recursive check for scan folders
        else if (depth == 0) {
            if (strcasecmp(ep->d_name, "scans") == 0 || strcasecmp(ep->d_name, "scan") == 0) {
                if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                    search_directory(full_path, depth + 1);
                }
            }
        }
    }
    closedir(dp);
}

int main(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "n:")) != -1) {
        if (opt == 'n') {
            limit = atoi(optarg);
            if (limit <= 0) limit = 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Usage: %s [-n limit] <directory|file>\n", argv[0]);
        return 1;
    }

    results = malloc(sizeof(FileResult) * results_capacity);
    
    char base_target[PATH_MAX];
    struct stat ps;

    // Resolve absolute path and normalize input
    if (realpath(argv[optind], base_target) && stat(base_target, &ps) == 0) {
        // If it's a file, we flatten the path to its parent directory
        if (S_ISREG(ps.st_mode)) {
            char *parent = dirname(base_target);
            memmove(base_target, parent, strlen(parent) + 1);
        }
        search_directory(base_target, 0);
    }

    // Sort winners by file size
    if (results_count > 1) {
        qsort(results, results_count, sizeof(FileResult), compare_files);
    }

    // Print final results up to the -n limit
    for (int i = 0; i < results_count && i < limit; i++) {
        puts(results[i].path);
    }

    free(results);
    return 0;
}
