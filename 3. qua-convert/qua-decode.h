#ifndef QUA_DECODE_H
#define QUA_DECODE_H

#include <limits.h>
#include <linux/limits.h>
#include <stdbool.h>
#include <stddef.h>

// WAV audio properties
typedef struct {
  int bit_depth;
  int sample_rate;
  int channels;
  bool is_float;  // true if IEEE 754 float format (audio_format == 3)
} wav_info_t;

// Decode parameters structure
typedef struct {
  const char *input_path;      // Path to source audio file (required)
  char *output_path;           // Path where decoded file will be written (NULL or empty = use temp file for in-memory mode)
  void **output_buffer;        // Pointer to buffer pointer (filled if output_path is NULL)
  size_t *output_size;         // Pointer to size (filled if output_path is NULL)
  wav_info_t *detected;        // Output: detected audio properties (required)
} decode_params_t;

void *convert_audio(void *arg);

// Parse WAV header and extract audio properties
// Returns 0 on success, -1 on error
int parse_wav_header(const char *filepath, wav_info_t *info);

#endif
