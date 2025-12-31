#include <dirent.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "qua-decode.h"
#include "qua-cache.h"
#include "qua-next.h"
#include "qua-config.h"
#include "qua-post-processing.h"

extern char **environ;


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


void *cleanup_services(void *arg) {
  pid_t pid;
  int status;
  
  // 1. Kill players/compositors immediately - wait for completion
  pid = vfork();
  if (pid == 0) {
    execlp("pkill", "pkill", "-9", "qua-player", NULL);
    _exit(1);
  } else if (pid > 0) {
    waitpid(pid, &status, 0);
  }

  pid = vfork();
  if (pid == 0) {
    execlp("pkill", "pkill", "-9", "picom", NULL);
    _exit(1);
  } else if (pid > 0) {
    waitpid(pid, &status, 0);
  }

  // 2. Parallel stop for Pipewire/Sound infrastructure
  const char *services[] = {"pipewire.socket",        "pipewire.service",
                            "wireplumber.service",    "pipewire-pulse.socket",
                            "pipewire-pulse.service", NULL};

  for (int i = 0; services[i] != NULL; i++) {
    pid = vfork();
    if (pid == 0) {
      execlp("systemctl", "systemctl", "--user", "stop", services[i], NULL);
      _exit(1);
    } else if (pid > 0) {
      waitpid(pid, &status, 0);
    }
  }
  return NULL;
}



// Select and execute the appropriate player binary
static void play_audio(const char *cache_file) {
  // Read WAV header from cache file to get specs for player selection
  int bd, sr, ch;
  if (parse_wav_header(cache_file, &bd, &sr, &ch) != 0) {
    fprintf(stderr, "Error: Failed to read WAV header from cache file: %s\n", cache_file);
    return;
  }
  
  char s_bd[8], s_sr[16];
  snprintf(s_bd, sizeof(s_bd), "%d", bd);
  snprintf(s_sr, sizeof(s_sr), "%d", sr);
  
  // update_mpris(cache_file);
  
  // Binary Selection (PGO with Generic Fallback)
  char p_bin[128], p_full[PATH_MAX] = {0};
  
  // Try specialized .pgo8 first
  snprintf(p_bin, sizeof(p_bin), "qua-player-%s-%s.pgo8", s_bd, s_sr);
  char q_cmd[PATH_MAX + 64];
  snprintf(q_cmd, sizeof(q_cmd), "which %s 2>/dev/null", p_bin);
  FILE *pwh = popen(q_cmd, "r");
  if (pwh) {
    if (fgets(p_full, PATH_MAX, pwh))
      p_full[strcspn(p_full, "\n")] = 0;
    pclose(pwh);
  }
  
  // Fallback to generic qua-player
  if (p_full[0] == '\0') {
    pwh = popen("which qua-player 2>/dev/null", "r");
    if (pwh) {
      if (fgets(p_full, PATH_MAX, pwh))
        p_full[strcspn(p_full, "\n")] = 0;
      pclose(pwh);
    }
  }
  
  // Execute the chosen player with vfork
  if (p_full[0] != '\0') {
    if (vfork() == 0) {
      execlp("qua-bare-launcher", "qua-bare-launcher", "4", p_full,
             cache_file, "hw:0,0", NULL);
      _exit(1);
    }
  } else {
    fprintf(stderr,
            "Error: No player binary (PGO or Generic) found in PATH.\n");
    fprintf(stderr, "Error: %s\n", p_full); // Debugging
    fprintf(stderr, "Error: %s\n", cache_file); // Debugging
    fprintf(stderr, "Error: %s\n", s_bd); // Debugging
    fprintf(stderr, "Error: %s\n", s_sr); // Debugging
    
  }
}


int main(int argc, char *argv[]) {
  char target_file[PATH_MAX] = {0};
  char cache_file[PATH_MAX] = {0};
  int offset = 0;
  char current_song_record[PATH_MAX];
  snprintf(current_song_record, sizeof(current_song_record),
           "%s/.config/qua-player/current-song", getenv("HOME"));

  // 1. Parse Arguments
  int opt;
  while ((opt = getopt(argc, argv, "n:p:")) != -1) {
    if (opt == 'n') {
      offset += atoi(optarg); // Add for forward movement
    }
    if (opt == 'p') {
      offset -= atoi(optarg); // Subtract for backward movement
    }
  }

  // 2. Resolve Target File (Navigation Logic)
  // Command line file argument takes precedence
  if (optind < argc) {
    if (realpath(argv[optind], target_file) == NULL) {
      return 1;
    }
  } else {
    // Read current file from config
    char current_file[PATH_MAX] = {0};
    FILE *f = fopen(current_song_record, "r");
    if (f) {
      if (fgets(current_file, PATH_MAX, f))
        current_file[strcspn(current_file, "\n")] = 0;
      fclose(f);
    }

    // Navigate if we have a current file
    if (current_file[0] != '\0') {
      if (qua_next(current_file, offset, target_file, sizeof(target_file)) != 0) {
        return 1;
      }
    } else {
      return 1; // No current file and no command line arg
    }
  }

  // 3. Generate cache file path
  if (!qua_cache_generate_path(target_file, cache_file, sizeof(cache_file))) {
    return 1;
  }
  
  // 4. Check if cache file exists
  struct stat st;
  bool cache_hit = (stat(cache_file, &st) == 0 && S_ISREG(st.st_mode));
  
  if (cache_hit) {
    // ========== CACHE HIT PATH ==========
    // Notify user about cache hit
    char *file_dup = strdup(target_file);
    char *base = basename(file_dup);
    char notify_cmd[PATH_MAX + 128];
    snprintf(notify_cmd, sizeof(notify_cmd), "notify-send -t 2000 'Cache Hit' 'Using cached version: %s'", base);
    system(notify_cmd);
    free(file_dup);
    
  } else {
    // ========== CACHE MISS PATH ==========
    // Manage cache size before decoding
    qua_cache_manage_size();
    
    // Parallel execution: cleanup services + convert audio
    pthread_t thread_cleanup, thread_convert;
    pthread_create(&thread_cleanup, NULL, cleanup_services, NULL);
    
    int detected_bd, detected_sr, detected_ch;
    decode_params_t decode_params = {
      .input_path = target_file,
      .output_path = cache_file,
      .output_buffer = NULL,
      .output_size = NULL,
      .detected_bit_depth = &detected_bd,
      .detected_sample_rate = &detected_sr,
      .detected_channels = &detected_ch
    };
    
    pthread_create(&thread_convert, NULL, convert_audio, &decode_params);
    
    // Wait for both threads to complete
    pthread_join(thread_cleanup, NULL);
    pthread_join(thread_convert, NULL);
    
    // Verify conversion succeeded
    struct stat st;
    if (stat(cache_file, &st) != 0 || !S_ISREG(st.st_mode)) {
      fprintf(stderr, "Error: Conversion failed - cache file does not exist: %s\n", cache_file);
      return 1;
    }
    
    // Get target values from configuration
    int target_bd = qua_config_get_target_bit_depth(detected_bd);
    int target_sr = qua_config_get_target_sample_rate(detected_sr);
    
    // Check if post-processing is needed
    bool needs_post_process = false;
    if (detected_ch != 2) {
      needs_post_process = true; // Need channel mixing
    } else if (target_bd != detected_bd) {
      needs_post_process = true; // Need bit depth conversion
    } else if (target_sr != detected_sr) {
      needs_post_process = true; // Need resampling
    }
    
    if (needs_post_process) {
      // Post-process: mix channels and resample
      char post_processed[PATH_MAX];
      snprintf(post_processed, sizeof(post_processed), "%s", cache_file);
      
      if (qua_post_process(cache_file, target_bd, target_sr, detected_ch) != 0) {
        fprintf(stderr, "Error: Post-processing failed\n");
        return 1;
      }
      
      // Replace cache file with post-processed version
      if (rename(post_processed, cache_file) != 0) {
        fprintf(stderr, "Error: Failed to replace cache file\n");
        return 1;
      }
    }
  }
  
  // 5. Common playback path (both cache hit and miss converge here)
  // Cleanup services before playback
  pthread_t thread_cleanup;
  pthread_create(&thread_cleanup, NULL, cleanup_services, NULL);
  pthread_join(thread_cleanup, NULL);
  
  // 7. Persist State
  FILE *fs = fopen(current_song_record, "w");
  if (fs) {
    fprintf(fs, "%s\n", target_file);
    fclose(fs);
  }

  // 8. Play audio
  play_audio(cache_file);

  return 0;
}
