#ifndef QUA_DECODE_H
#define QUA_DECODE_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

// Decode parameters structure
typedef struct {
  const char *input_path;      // Path to source audio file (required)
  char *output_path;           // Path where decoded file will be written (NULL or empty = use temp file for in-memory mode)
  void **output_buffer;        // Pointer to buffer pointer (filled if output_path is NULL)
  size_t *output_size;         // Pointer to size (filled if output_path is NULL)
  int *detected_bit_depth;     // Output: detected bit depth (required)
  int *detected_sample_rate;   // Output: detected sample rate (required)
  int *detected_channels;      // Output: detected channel count (required)
} decode_params_t;

void *convert_audio(void *arg);

// Parse WAV header and extract audio properties
// Returns 0 on success, -1 on error
// Sets bits_per_sample, sample_rate, and channels if successful
int parse_wav_header(const char *filepath, int *bits_per_sample, int *sample_rate, int *channels);

#endif
