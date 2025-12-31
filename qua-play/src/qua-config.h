#ifndef QUA_CONFIG_H
#define QUA_CONFIG_H

#include <stdbool.h>

// Configuration: Valid bit depths (space-separated)
#define BIT_DEPTH_VALID "16 32"

// Configuration: Force playback at specific bit depth (empty = false, or value like "32")
#define BIT_DEPTH_OVERRIDE ""

// Configuration: Fallback bit depth when detected bit depth is not in BIT_DEPTH_VALID
#define BIT_DEPTH_FALLBACK 32

// Configuration: Valid sample rates (space-separated)
#define SAMPLE_RATE_VALID "44100 48000 88200 96000 176400 192000 352800 384000"

// Configuration: Force playback at specific sample rate (empty = false, or value like "96000")
#define SAMPLE_RATE_OVERRIDE ""

// Configuration: Fallback sample rate when detected sample rate is not in SAMPLE_RATE_VALID
#define SAMPLE_RATE_FALLBACK 96000

// Check if value is in space-separated list
bool qua_config_is_valid(const char *valid_list, int value);

// Get target bit depth: override -> detected (if valid) -> fallback
int qua_config_get_target_bit_depth(int detected_bd);

// Get target sample rate: override -> detected (if valid) -> fallback
int qua_config_get_target_sample_rate(int detected_sr);

#endif
