#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdbool.h>
#include <limits.h>
#include <libgen.h>
#include <pthread.h>
#include <fcntl.h>

extern char **environ;

#define CACHE_DIR "/dev/shm/qua-cache"
#define CACHE_MAX_SIZE (2ULL * 1024 * 1024 * 1024)

typedef struct {
    char target_file[PATH_MAX];
    char cache_file[PATH_MAX];
    char pattern[128];
    bool conversion_needed;
} task_data_t;

const char *extensions[] = {".flac", ".mp3", ".wv", ".ape", ".opus", ".ogg", ".m4a", ".mp4", ".wav", ".aiff", ".aif", NULL};

bool is_audio(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    for (int i = 0; extensions[i]; i++) if (strcasecmp(dot, extensions[i]) == 0) return true;
    return false;
}

int filter_fn(const struct dirent *entry) { return is_audio(entry->d_name); }

void update_mpris(const char *filepath) {
    // char *file_dup = strdup(filepath);
    // char *base = basename(file_dup);
    // char *dir_dup = strdup(filepath);
    // char *folder = basename(dirname(dir_dup));
    // 
    // if (vfork() == 0) {
    //     char dest[] = "org.mpris.MediaPlayer2.qua";
    //     char path[] = "/org/mpris/MediaPlayer2";
    //     char method[] = "org.mpris.MediaPlayer2.Player.UpdateMetadata";
    //     execlp("dbus-send", "dbus-send", "--session", "--type=method_call",
    //            "--dest=" "org.mpris.MediaPlayer2.qua",
    //            "/org/mpris/MediaPlayer2",
    //            "org.mpris.MediaPlayer2.Player.UpdateMetadata",
    //            base, folder, NULL);
    //     _exit(1);
    // }
    // 
    // free(file_dup); 
    // free(dir_dup);
}

void manage_cache_size() {
    struct dirent **namelist;
    int n = scandir(CACHE_DIR, &namelist, NULL, alphasort);
    if (n < 0) return;
    long long total = 0;
    for (int i = 0; i < n; i++) {
        char p[PATH_MAX]; snprintf(p, sizeof(p), "%s/%s", CACHE_DIR, namelist[i]->d_name);
        struct stat s; if (stat(p, &s) == 0 && S_ISREG(s.st_mode)) total += s.st_size;
    }
    if (total > CACHE_MAX_SIZE) {
        for (int i = 0; i < n && total > (CACHE_MAX_SIZE * 0.7); i++) {
            if (namelist[i]->d_name[0] == '.') continue;
            char p[PATH_MAX]; snprintf(p, sizeof(p), "%s/%s", CACHE_DIR, namelist[i]->d_name);
            struct stat s; if (stat(p, &s) == 0) { total -= s.st_size; unlink(p); }
        }
    }
    for (int i = 0; i < n; i++) free(namelist[i]); free(namelist);
}

void* cleanup_services(void* arg) {
    // 1. Kill players/compositors immediately
    if (vfork() == 0) {
        execlp("pkill", "pkill", "-9", "qua-player", NULL);
        _exit(1);
    }
    
    if (vfork() == 0) {
        execlp("pkill", "pkill", "-9", "picom", NULL);
        _exit(1);
    }
    
    // 2. Parallel stop for Pipewire/Sound infrastructure
    const char *services[] = {
        "pipewire.socket", "pipewire.service", 
        "wireplumber.service", "pipewire-pulse.socket", 
        "pipewire-pulse.service", NULL
    };

    for (int i = 0; services[i] != NULL; i++) {
        if (vfork() == 0) {
            execlp("systemctl", "systemctl", "--user", "stop", services[i], NULL);
            _exit(1);
        }
    }
    return NULL;
}

void* convert_audio(void* arg) {
    task_data_t* data = (task_data_t*)arg;
    if (!data->conversion_needed) return NULL;

    manage_cache_size();
    
    int fd = open(data->target_file, O_RDONLY);
    if (fd != -1) {
        posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
        close(fd);
    }

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "/dev/shm/raw-%d.wav", getpid());

    char *ext = strrchr(data->target_file, '.');
    
    // Optimized native decoder selection with direct execution
    pid_t pid;
    if (ext && strcasecmp(ext, ".flac") == 0) {
        if ((pid = vfork()) == 0) {
            execlp("flac", "flac", "-d", "-s", "-f", data->target_file, "-o", tmp, NULL);
            _exit(1);
        }
    } 
    else if (ext && strcasecmp(ext, ".wv") == 0) {
        if ((pid = vfork()) == 0) {
            execlp("wvunpack", "wvunpack", "-q", "-y", data->target_file, "-o", tmp, NULL);
            _exit(1);
        }
    } 
    else if (ext && strcasecmp(ext, ".ape") == 0) {
        if ((pid = vfork()) == 0) {
            execlp("mac", "mac", data->target_file, tmp, "-d", NULL);
            _exit(1);
        }
    }
    else if (ext && (strcasecmp(ext, ".opus") == 0 || strcasecmp(ext, ".ogg") == 0)) {
        if ((pid = vfork()) == 0) {
            execlp("opusdec", "opusdec", "--quiet", data->target_file, tmp, NULL);
            _exit(1);
        }
    }
    else if (ext && (strcasecmp(ext, ".aiff") == 0 || strcasecmp(ext, ".aif") == 0)) {
        if ((pid = vfork()) == 0) {
            execlp("sox", "sox", data->target_file, tmp, NULL);
            _exit(1);
        }
    }
    else {
        if ((pid = vfork()) == 0) {
            execlp("ffmpeg", "ffmpeg", "-v", "quiet", "-i", data->target_file, 
                   "-f", "wav", "-y", tmp, NULL);
            _exit(1);
        }
    }
    
    if (pid > 0) waitpid(pid, NULL, 0);

    // Query audio properties
    char buf[32];
    int bd = 32, sr = 96000, ch = 2;
    
    FILE *fp;
    if ((pid = vfork()) == 0) {
        int pipefd[2];
        pipe(pipefd);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execlp("soxi", "soxi", "-p", tmp, NULL);
        _exit(1);
    }
    fp = popen("echo", "r");  // Placeholder - needs proper pipe handling
    if (fp) { if (fgets(buf, 32, fp)) bd = atoi(buf); pclose(fp); }
    
    // Simplified: use system() for soxi queries (complex pipe handling)
    char cmd[PATH_MAX * 2];
    snprintf(cmd, sizeof(cmd), "soxi -p \"%s\"", tmp);
    fp = popen(cmd, "r"); if (fp) { fgets(buf, 32, fp); pclose(fp); bd = atoi(buf); }
    
    snprintf(cmd, sizeof(cmd), "soxi -r \"%s\"", tmp);
    fp = popen(cmd, "r"); if (fp) { fgets(buf, 32, fp); pclose(fp); sr = atoi(buf); }
    
    snprintf(cmd, sizeof(cmd), "soxi -c \"%s\"", tmp);
    fp = popen(cmd, "r"); if (fp) { fgets(buf, 32, fp); pclose(fp); ch = atoi(buf); }

    if (bd != 16 && bd != 32) bd = 32;
    if (sr < 44100) sr = 96000;

    snprintf(data->cache_file, PATH_MAX, "%s/%s-%d-%d.wav", CACHE_DIR, data->pattern, bd, sr);

    // Build sox arguments
    char bd_str[8], sr_str[16];
    snprintf(bd_str, sizeof(bd_str), "%d", bd);
    snprintf(sr_str, sizeof(sr_str), "%d", sr);
    
    if ((pid = vfork()) == 0) {
        if (ch == 1) {
            execlp("sox", "sox", tmp, "-b", bd_str, "-e", "signed-integer", 
                   data->cache_file, "channels", "2", "rate", "-v", sr_str, NULL);
        } else if (ch == 6) {
            execlp("sox", "sox", tmp, "-b", bd_str, "-e", "signed-integer",
                   data->cache_file, "remix", "1,3v0.707,5v0.707", "2,3v0.707,6v0.707",
                   "rate", "-v", sr_str, NULL);
        } else {
            execlp("sox", "sox", tmp, "-b", bd_str, "-e", "signed-integer",
                   data->cache_file, "rate", "-v", sr_str, NULL);
        }
        _exit(1);
    }
    
    if (pid > 0) waitpid(pid, NULL, 0);
    unlink(tmp);
    return NULL;
}

int main(int argc, char *argv[]) {
    task_data_t data = {0};
    int offset = 0;
    char config_path[PATH_MAX];
    snprintf(config_path, sizeof(config_path), "%s/.config/qua-player/current-song", getenv("HOME"));

    // 1. Parse Arguments
    int opt;
    while ((opt = getopt(argc, argv, "n:")) != -1) {
        if (opt == 'n') offset = atoi(optarg);
    }

    // 2. Resolve Target File (Navigation Logic)
    if (offset != 0 || optind >= argc) {
        char current[PATH_MAX] = {0};
        FILE *f = fopen(config_path, "r");
        if (f) {
            if (fgets(current, PATH_MAX, f)) current[strcspn(current, "\n")] = 0;
            fclose(f);
        }
        
        if (offset != 0 && current[0] != '\0') {
            char *d_copy = strdup(current);
            char *d_name = dirname(d_copy);
            struct dirent **nl;
            
            // Fast "Dir" scan and Alphabetical Sort
            int n = scandir(d_name, &nl, filter_fn, alphasort);
            
            if (n > 0) {
                int cur_idx = -1;
                for (int i = 0; i < n; i++) {
                    char full[PATH_MAX];
                    snprintf(full, sizeof(full), "%s/%s", d_name, nl[i]->d_name);
                    if (strcmp(full, current) == 0) { cur_idx = i; break; }
                }

                // Wrap-around math: (current + offset) % total
                int next = (cur_idx == -1) ? 0 : (cur_idx + offset) % n;
                if (next < 0) next += n; // Handle negative offsets for "Previous"
                
                snprintf(data.target_file, sizeof(data.target_file), "%s/%s", d_name, nl[next]->d_name);
                
                for(int i = 0; i < n; i++) free(nl[i]);
                free(nl);
            }
            free(d_copy);
        } else if (current[0] != '\0') {
            strcpy(data.target_file, current); // Just replay current
        }
    } 
    
    // Command line file argument always takes highest precedence
    if (optind < argc) {
        realpath(argv[optind], data.target_file);
    }

    if (data.target_file[0] == '\0') return 1;

    // 3. Fingerprinting (The Inode/MTIME Trick)
    struct stat st;
    if (stat(data.target_file, &st) != 0) return 1;
    snprintf(data.pattern, sizeof(data.pattern), "qua-%lx-%lx", 
             (unsigned long)st.st_ino, (unsigned long)st.st_mtime);
    
    mkdir(CACHE_DIR, 0755);
    data.conversion_needed = true;

    // Check if valid cache already exists
    DIR *dirp = opendir(CACHE_DIR);
    if (dirp) {
        struct dirent *dp;
        while ((dp = readdir(dirp))) {
            if (strncmp(dp->d_name, data.pattern, strlen(data.pattern)) == 0) {
                snprintf(data.cache_file, sizeof(data.cache_file), "%s/%s", CACHE_DIR, dp->d_name);
                data.conversion_needed = false;
                break;
            }
        }
        closedir(dirp);
    }

    // 4. Parallel Execution (Services + Conversion)
    pthread_t thread_cleanup, thread_convert;
    pthread_create(&thread_cleanup, NULL, cleanup_services, NULL);
    pthread_create(&thread_convert, NULL, convert_audio, &data);

    pthread_join(thread_cleanup, NULL);
    pthread_join(thread_convert, NULL);

    // 5. Playback Preparation (Spec Detection)
    char s_bd[8] = "32", s_sr[16] = "96000", q_cmd[PATH_MAX+64];
    
    snprintf(q_cmd, sizeof(q_cmd), "soxi -p \"%s\" 2>/dev/null", data.cache_file);
    FILE *fbd = popen(q_cmd, "r"); 
    if(fbd) { if(fgets(s_bd, 8, fbd)) s_bd[strcspn(s_bd, "\n")] = 0; pclose(fbd); }
    
    snprintf(q_cmd, sizeof(q_cmd), "soxi -r \"%s\" 2>/dev/null", data.cache_file);
    FILE *fsr = popen(q_cmd, "r"); 
    if(fsr) { if(fgets(s_sr, 16, fsr)) s_sr[strcspn(s_sr, "\n")] = 0; pclose(fsr); }

    update_mpris(data.target_file);

    // 6. Binary Selection (PGO with Generic Fallback)
    char p_bin[128], p_full[PATH_MAX] = {0};
    
    // Try specialized .pgo8 first
    snprintf(p_bin, sizeof(p_bin), "qua-player-%s-%s.pgo8", s_bd, s_sr);
    snprintf(q_cmd, sizeof(q_cmd), "which %s 2>/dev/null", p_bin);
    FILE *pwh = popen(q_cmd, "r");
    if (pwh) {
        if (fgets(p_full, PATH_MAX, pwh)) p_full[strcspn(p_full, "\n")] = 0;
        pclose(pwh);
    }

    // Fallback to generic qua-player
    if (p_full[0] == '\0') {
        pwh = popen("which qua-player 2>/dev/null", "r");
        if (pwh) {
            if (fgets(p_full, PATH_MAX, pwh)) p_full[strcspn(p_full, "\n")] = 0;
            pclose(pwh);
        }
    }

    // Execute the chosen player with vfork
    if (p_full[0] != '\0') {
        if (vfork() == 0) {
            execlp("qua-bare-launcher", "qua-bare-launcher", "4", p_full, 
                   data.cache_file, "hw:0,0", NULL);
            _exit(1);
        }
    } else {
        fprintf(stderr, "Error: No player binary (PGO or Generic) found in PATH.\n");
    }

    // 7. Persist State
    FILE *fs = fopen(config_path, "w");
    if (fs) { fprintf(fs, "%s\n", data.target_file); fclose(fs); }

    return 0;
}
