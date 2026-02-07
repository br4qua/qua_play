// qua-convert: Single responsibility - decode audio file to WAV
// Usage: qua-convert <input-file> <output-wav-path>
// Exit codes: 0 = success, 1 = error

#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "qua-decode.h"
#include "qua-config.h"
#include "qua-post-processing.h"


int main(int argc, char *argv[]) {
  // Require both input and output paths
  if (argc < 3) {
    fprintf(stderr, "Usage: %s <input-audio-file> <output-wav-path>\n", argv[0]);
    return 1;
  }

  char input_file[PATH_MAX] = {0};
  const char *output_file = argv[2];

  // Resolve input path
  if (realpath(argv[1], input_file) == NULL) {
    fprintf(stderr, "Error: Cannot resolve path: %s\n", argv[1]);
    return 1;
  }

  // Decode audio to output file
  wav_info_t detected;
  decode_params_t decode_params = {
    .input_path = input_file,
    .output_path = (char *)output_file,
    .output_buffer = NULL,
    .output_size = NULL,
    .detected = &detected
  };

  pthread_t thread_convert;
  pthread_create(&thread_convert, NULL, convert_audio, &decode_params);
  pthread_join(thread_convert, NULL);

  // Verify conversion succeeded
  struct stat st;
  if (stat(output_file, &st) != 0 || !S_ISREG(st.st_mode)) {
    fprintf(stderr, "Error: Conversion failed - output file does not exist: %s\n", output_file);
    return 1;
  }

  // Get target values from configuration
  int target_bd = qua_config_get_target_bit_depth(detected.bit_depth);
  int target_sr = qua_config_get_target_sample_rate(detected.sample_rate);

  // Check if post-processing is needed
  bool needs_post_process = (detected.is_float || detected.channels != 2 ||
                             target_bd != detected.bit_depth || target_sr != detected.sample_rate);

  if (needs_post_process) {
    // Float conversion must go through sox (no fast path for float)
    if (detected.is_float) {
      fprintf(stderr, "Converting float to signed-integer PCM...\n");
      if (qua_post_process(output_file, target_bd, target_sr, detected.channels) != 0) {
        fprintf(stderr, "Error: Float conversion failed\n");
        return 1;
      }
    }
    // FAST PATH: Only bit depth 24->32 conversion is needed
    else if (detected.bit_depth == 24 && target_bd == 32 && detected.channels == 2 && detected.sample_rate == target_sr) {
      fprintf(stderr, "Applying fast 24-bit to 32-bit conversion...\n");
      if (convert_24bit_to_32bit_wav_ultrafast(output_file) != 0) {
        fprintf(stderr, "Error: Fast bit-depth conversion failed\n");
        return 1;
      }
    }
    else if (detected.bit_depth == 16 && target_bd == 32 && detected.channels == 2 && detected.sample_rate == target_sr) {
      fprintf(stderr, "Applying fast 16-bit to 32-bit conversion...\n");
      if (convert_16bit_to_32bit_wav_fast(output_file) != 0) {
        fprintf(stderr, "Error: Fast 16-bit conversion failed\n");
        return 1;
      }
    }
    // SLOW PATH: Channel mixing, resampling, or complex bit depth changes
    else {
      fprintf(stderr, "Applying standard post-processing (resampling/mixing)...\n");
      if (qua_post_process(output_file, target_bd, target_sr, detected.channels) != 0) {
        fprintf(stderr, "Error: Post-processing failed\n");
        return 1;
      }
    }
  }

  return 0;
}
