#include "qua-post-processing.h"

#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

// Post-process audio file: mix channels and resample
// Returns 0 on success, 1 on failure
int qua_post_process(const char *file_path,
                     int bit_depth, int sample_rate, int channels) {
  char bd_str[8], sr_str[16];
  snprintf(bd_str, sizeof(bd_str), "%d", bit_depth);
  snprintf(sr_str, sizeof(sr_str), "%d", sample_rate);
  
  // Create temporary output file
  char temp_path[PATH_MAX];
  snprintf(temp_path, sizeof(temp_path), "%s.tmp", file_path);
  
  // Debug output
  fprintf(stderr, "DEBUG: qua_post_process called:\n");
  fprintf(stderr, "  file_path:    %s\n", file_path);
  fprintf(stderr, "  temp_path:    %s\n", temp_path);
  fprintf(stderr, "  bit_depth:    %d\n", bit_depth);
  fprintf(stderr, "  sample_rate:  %d\n", sample_rate);
  fprintf(stderr, "  channels:     %d\n", channels);
  
  pid_t pid = vfork();
  if (pid == 0) {
    if (channels == 1) {
      // Mono -> Stereo
      fprintf(stderr, "DEBUG: Executing mono->stereo conversion\n");
      execlp("sox", "sox", file_path, "-b", bd_str, "-e", "signed-integer",
             "-t", "wav", "-r", sr_str, temp_path, "channels", "2", "rate", "-v", NULL);
    } else if (channels == 6) {
      // 5.1 -> Stereo downmix
      fprintf(stderr, "DEBUG: Executing 5.1->stereo downmix\n");
      execlp("sox", "sox", file_path, "-b", bd_str, "-e", "signed-integer",
             "-t", "wav", "-r", sr_str, temp_path, "remix", "1,3v0.707,5v0.707",
             "2,3v0.707,6v0.707", "rate", "-v", NULL);
    } else {
      // Other channels -> just resample
      fprintf(stderr, "DEBUG: Executing resample only\n");
      execlp("sox", "sox", file_path, "-b", bd_str, "-e", "signed-integer",
             "-t", "wav", "-r", sr_str, temp_path, "rate", "-v", NULL);
    }
    _exit(1);
  }
  
  if (pid > 0) {
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
      fprintf(stderr, "DEBUG: sox exited with status %d\n", WEXITSTATUS(status));
      unlink(temp_path); // Clean up temp file on error
      return 1;
    }
    
    // Replace original with processed version
    if (rename(temp_path, file_path) != 0) {
      fprintf(stderr, "Error: Failed to replace original file\n");
      unlink(temp_path);
      return 1;
    }
  }
  return 0;
}