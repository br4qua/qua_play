#ifndef QUA_POST_PROCESSING_H
#define QUA_POST_PROCESSING_H
#include <linux/limits.h>

// Post-process audio file: mix channels and resample
// input_path: path to input WAV file
// output_path: path to output WAV file
// bit_depth: target bit depth (16 or 32)
// sample_rate: target sample rate
// channels: input channel count (1 = mono, 6 = 5.1, etc.)
// Returns 0 on success, 1 on failure
int qua_post_process(const char *input_path, int bit_depth, int sample_rate, int channels);
int convert_24bit_to_32bit_wav(const char *input_path);
int convert_24bit_to_32bit_wav_fast(const char *input_path);
#endif
