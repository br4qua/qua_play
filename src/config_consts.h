#define _GNU_SOURCE
#include <stdint.h>

// Branch prediction hints
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define CHAR_BIT 8

// --- SYSTEM CONFIGURATION - HUGE PAGE ---
// IMPORTANT: Adjust this value to your system's Huge Page size
#define HUGE_PAGE_SIZE 0x80000000 // 2GB
#define ALIGN_4K 4096

// Audio Configuration General
#define NUM_OF_CHANNELS 2
#define SAMPLES_PER_FRAME NUM_OF_CHANNELS

// Fallback Definition
#ifndef TARGET_BITDEPTH
#define TARGET_BITDEPTH 32
#endif
#ifndef TARGET_SAMPLE_RATE
#define TARGET_SAMPLE_RATE 48000
#endif

// Audio Configuration
#define BIT_DEPTH TARGET_BITDEPTH
#if TARGET_BITDEPTH == 16
    // GUARANTEES 16 bits (2 bytes). Critical for S16_LE format.
    typedef uint16_t sample_t;
    #define BYTES_PER_SAMPLE sizeof(sample_t)
    #define PCM_FORMAT SND_PCM_FORMAT_S16_LE
#elif TARGET_BITDEPTH == 32
    // GUARANTEES 32 bits (4 bytes). Critical for S32_LE format.
    typedef int32_t sample_t;
    #define BYTES_PER_SAMPLE sizeof(sample_t)
    #define PCM_FORMAT SND_PCM_FORMAT_S32_LE
#else
    #error "Unsupported TARGET_BITDEPTH. Only 16 or 32 are supported."
#endif

#if TARGET_BITDEPTH == 32
#define FACTOR_MULTIPLE 1
#elif TARGET_BITDEPTH == 16
#define FACTOR_MULTIPLE 2
#endif

// --- Total Buffer Size ---
#if TARGET_SAMPLE_RATE >= 88000                             // High Sample rates allow bigger buffer
#define BYTES_PER_BUFFER (32768 * 16 * 2 / FACTOR_MULTIPLE) // Doubled for high sample rates
#else
#define BYTES_PER_BUFFER (32768 * 16 / FACTOR_MULTIPLE)
#endif

// The number of periods.
#define PERIODS_PER_BUFFER 2
// --- Derived Constants (Calculated from Fixed Values) ---
// Size of one period/chunk in bytes
#define BYTES_PER_PERIOD (BYTES_PER_BUFFER / PERIODS_PER_BUFFER)
#define BYTES_PER_AUDIO_FRAME (SAMPLES_PER_FRAME * BYTES_PER_SAMPLE)

// --- Derived ALSA Frame Counts (Required for ALSA API) ---
#define FRAMES_PER_BUFFER (BYTES_PER_BUFFER / BYTES_PER_AUDIO_FRAME)
#define FRAMES_PER_PERIOD (FRAMES_PER_BUFFER / PERIODS_PER_BUFFER)


