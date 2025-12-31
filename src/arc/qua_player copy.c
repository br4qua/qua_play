#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <alsa/asoundlib.h>
#include <fcntl.h>
#include <immintrin.h> // Provides INTRINSICS
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <stdbool.h>
#include <unistd.h>
#include <poll.h>
#include <unistd.h>
#include <assert.h>
#include "debug.h"

// Branch prediction hints
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define CHAR_BIT 8

// --- SYSTEM CONFIGURATION - HUGE PAGE ---
// IMPORTANT: Adjust this value to your system's Huge Page size
#define HUGE_PAGE_SIZE 0x40000000 // 1GB
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

// Audio Configuration (32-Bit)
#define BIT_DEPTH TARGET_BITDEPTH
#if TARGET_BITDEPTH == 16
#define BYTES_PER_SAMPLE sizeof(int16_t) // 2 bytes (for 16-bit audio)
typedef int16_t sample_t;
#define PCM_FORMAT SND_PCM_FORMAT_S16_LE
#elif TARGET_BITDEPTH == 32
#define BYTES_PER_SAMPLE sizeof(int32_t) // 4 bytes (for 32-bit audio)
typedef int32_t sample_t;
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

#ifdef DEBUG_BUILD
#define CHECK_READ(val, expected, msg)        \
  if (unlikely((val) != (ssize_t)(expected))) \
  {                                           \
    fprintf(stderr, msg "\n");                \
    return -1;                                \
  }
#else
#define CHECK_READ(val, expected, msg) /* No-op in release */
#endif

// Custom Implementation of memcpy - AVX2 intrinsics, 4Kb aligned
// UNSAFE - use _mm_sfense prevents race condition.

typedef struct WavHeader_s
{
  char riff_header[4];
  uint32_t wav_size;
  char wave_header[4];
  char fmt_header[4];
  uint32_t fmt_chunk_size;
  uint16_t audio_format;
  uint16_t num_channels;
  uint32_t sample_rate;
  uint32_t byte_rate;
  uint16_t sample_alignment;
  uint16_t bit_depth;
  char data_header[4];
  uint32_t data_bytes;
} WavHeader;


static off_t read_wav_header(int fd, WavHeader *header);
static int setup_alsa(snd_pcm_t **handle, const char *device);
static inline void memcpy_avx_stream_4k(sample_t *dst, const sample_t *src);

static inline void memcpy_avx_stream_4k(sample_t *dst, const sample_t *src)
{
  dst = (sample_t *)__builtin_assume_aligned(dst, ALIGN_4K);
  src = (const sample_t *)__builtin_assume_aligned(src, ALIGN_4K);
  for (int i = 0; i < SAMPLES_PER_FRAME * FRAMES_PER_PERIOD; i += 32 * FACTOR_MULTIPLE)
  {
    const __m256i data0 = _mm256_load_si256((const __m256i *)(src + i));
    const __m256i data1 = _mm256_load_si256((const __m256i *)(src + i + 8 * FACTOR_MULTIPLE));
    const __m256i data2 = _mm256_load_si256((const __m256i *)(src + i + 16 * FACTOR_MULTIPLE));
    const __m256i data3 = _mm256_load_si256((const __m256i *)(src + i + 24 * FACTOR_MULTIPLE));
    _mm256_stream_si256((__m256i *)(dst + i), data0);
    _mm256_stream_si256((__m256i *)(dst + i + 8 * FACTOR_MULTIPLE), data1);
    _mm256_stream_si256((__m256i *)(dst + i + 16 * FACTOR_MULTIPLE), data2);
    _mm256_stream_si256((__m256i *)(dst + i + 24 * FACTOR_MULTIPLE), data3);
  }
}
// Optimal ioctl for Zen 4
static inline long my_poll(struct pollfd *fds, unsigned long nfds, int timeout) {
    register  long rax __asm__("rax") = __NR_poll;  // const here?
    register  long rdi __asm__("rdi") = (long)fds;
    register  long rsi __asm__("rsi") = nfds;
    register  long rdx __asm__("rdx") = (long)timeout;
    register long ret __asm__("rax");  // No const - this is output
    
    __asm__ __volatile__ (
        "syscall"
        : "=r"(ret)
        : "r"(rax), "r"(rdi), "r"(rsi), "r"(rdx)
        : "rcx", "r11", "memory"
    );
    
    return ret;
}

// Optimal ioctl for Zen 4
static inline long my_ioctl(int fd, unsigned long request, void *arg) {
    register  long rax __asm__("rax") = __NR_ioctl;
    register  long rdi __asm__("rdi") = (long)fd;
    register  long rsi __asm__("rsi") = request;
    register  long rdx __asm__("rdx") = (long)arg;
    register long ret __asm__("rax");
    
    __asm__ __volatile__ (
        "syscall"
        : "=r"(ret)
        : "r"(rax), "r"(rdi), "r"(rsi), "r"(rdx)
        : "rcx", "r11", "memory"
    );
    
    return ret;
}


static off_t read_wav_header(int fd, WavHeader *header)
{
  ssize_t bytes_read;
  uint8_t basic_header[12];

  // --- 1. Read Basic RIFF/WAVE Header ---
  bytes_read = read(fd, basic_header, 12);
  CHECK_READ(bytes_read, 12, "Failed to read WAV header");

#ifdef DEBUG_BUILD
  if (unlikely(strncmp((char *)basic_header, "RIFF", 4) != 0 ||
               strncmp((char *)basic_header + 8, "WAVE", 4) != 0))
  {
    fprintf(stderr, "Invalid WAV file format\n");
    return -1;
  }
#endif

  memcpy(header->riff_header, basic_header, 4);
  memcpy(&header->wav_size, basic_header + 4, 4);
  memcpy(header->wave_header, basic_header + 8, 4);

  // --- 2. Iterate Through Chunks ---
  int fmt_chunk_found = 0;
  int data_chunk_found = 0;
  off_t data_offset = 0;

  while (!data_chunk_found)
  {
    char chunk_id[4];
    uint32_t chunk_size;

    // Read Chunk ID
    bytes_read = read(fd, chunk_id, 4);
    CHECK_READ(bytes_read, 4, "Couldn't read chunk ID");

    // Read Chunk Size
    bytes_read = read(fd, &chunk_size, 4);
    CHECK_READ(bytes_read, 4, "Couldn't read chunk size");

    // --- 'fmt ' Chunk Processing ---
    if (strncmp(chunk_id, "fmt ", 4) == 0)
    {
      memcpy(header->fmt_header, chunk_id, 4);
      header->fmt_chunk_size = chunk_size;

#ifdef DEBUG_BUILD
      if (unlikely(chunk_size < 16))
      {
        fprintf(stderr, "Format chunk too small\n");
        return -1;
      }
#endif

      uint8_t fmt_data[16];
      bytes_read = read(fd, fmt_data, 16);
      CHECK_READ(bytes_read, 16, "Failed to read format chunk data");

      // Populate header fields (I/O success assumed)
      memcpy(&header->audio_format, fmt_data, 2);
      memcpy(&header->num_channels, fmt_data + 2, 2);
      memcpy(&header->sample_rate, fmt_data + 4, 4);
      memcpy(&header->byte_rate, fmt_data + 8, 4);
      memcpy(&header->sample_alignment, fmt_data + 12, 2);
      memcpy(&header->bit_depth, fmt_data + 14, 2);

      // Handle optional extra format data
      if (chunk_size > 16)
      {
        lseek(fd, chunk_size - 16, SEEK_CUR);
      }

#ifdef DEBUG_BUILD
      if (unlikely(header->audio_format != 1 && header->audio_format != 65534))
      {
        fprintf(stderr, "Only PCM or Extensible WAV files are supported\n");
        return -1;
      }

      if (unlikely(header->bit_depth != BIT_DEPTH))
      {
        fprintf(stderr, "Only %d-bit audio is supported\n", BIT_DEPTH);
        return -1;
      }
#endif

      fmt_chunk_found = 1;
    }
    // --- 'data' Chunk Processing ---
    else if (strncmp(chunk_id, "data", 4) == 0)
    {
      memcpy(header->data_header, chunk_id, 4);
      header->data_bytes = chunk_size;
      data_chunk_found = 1;
      data_offset = lseek(fd, 0, SEEK_CUR);
      break;
    }
    // --- Skip Unknown Chunk ---
    else
    {
      lseek(fd, chunk_size, SEEK_CUR);
      if (chunk_size % 2 != 0)
      {
        lseek(fd, 1, SEEK_CUR);
      }
    }
  }

  // --- 3. Final Validation ---
#ifdef DEBUG_BUILD
  if (unlikely(!fmt_chunk_found))
  {
    fprintf(stderr, "No format chunk found\n");
    return -1;
  }
#endif

  return data_offset;
}

static int setup_alsa(snd_pcm_t **handle, const char *device)
{
  snd_pcm_hw_params_t *hw_params;
  snd_pcm_hw_params_alloca(&hw_params);

  int err;
  // 1. Open audio device
  err = snd_pcm_open(handle, device, SND_PCM_STREAM_PLAYBACK, 0);

  // 2. Allocate hardware parameters

  // err = snd_pcm_hw_params_malloc(&hw_params);
#ifdef DEBUG_ERROR
  if (err < 0)
  {
    fprintf(stderr, "Cannot allocate hardware parameters: %s\n",
            snd_strerror(err));
    return -1;
  }
#endif

  // 3. Initialize hardware parameters
  err = snd_pcm_hw_params_any(*handle, hw_params);
#ifdef DEBUG_ERROR
  if (err < 0)
  {
    fprintf(stderr, "Cannot initialize hardware parameters: %s\n",
            snd_strerror(err));

    return -1;
  }
#endif

  // 4. Set mmap access mode
  err = snd_pcm_hw_params_set_access(
      *handle, hw_params, SND_PCM_ACCESS_MMAP_INTERLEAVED);
#ifdef DEBUG_ERROR
  if (err < 0)
  {
    fprintf(stderr, "Cannot set interleaved mmap access type: %s\n",
            snd_strerror(err));

    return -1;
  }
#endif

  // 5. Set sample format
  err = snd_pcm_hw_params_set_format(*handle, hw_params,
                                     PCM_FORMAT);

#ifdef DEBUG_ERROR
  if (err < 0)
  {
    fprintf(stderr, "Cannot set sample format: %s\n", snd_strerror(err));

    return -1;
  }
#endif

  // 6. Set sample rate
  err = snd_pcm_hw_params_set_rate(*handle, hw_params, TARGET_SAMPLE_RATE, 0);
#ifdef DEBUG_ERROR
  if (err < 0)
  {
    fprintf(stderr, "Cannot set sample rate: %s\n", snd_strerror(err));

    return -1;
  }
#endif

  // 7. Set channel count
  err = snd_pcm_hw_params_set_channels(*handle, hw_params, SAMPLES_PER_FRAME);
#ifdef DEBUG_ERROR
  if (err < 0)
  {
    fprintf(stderr, "Cannot set channel count: %s\n", snd_strerror(err));

    return -1;
  }
#endif

  // 8. Set buffer size (EXACT)
  // NOTE: This call will fail if the exact frame count is not supported by hardware.
  err = snd_pcm_hw_params_set_buffer_size(*handle, hw_params,
                                          FRAMES_PER_BUFFER);
#ifdef DEBUG_ERROR
  if (err < 0)
  {
    fprintf(stderr, "Cannot set EXACT buffer size: %s\n", snd_strerror(err));
    fprintf(stderr, "Requested size: %lu frames. Hardware may require 'near' mode.\n", (long unsigned int)FRAMES_PER_BUFFER);

    return -1;
  }
#endif

  // 9. Set period size (EXACT)
  err = snd_pcm_hw_params_set_period_size(*handle, hw_params,
                                          FRAMES_PER_PERIOD, 0);
#ifdef DEBUG_ERROR
  if (err < 0)
  {
    fprintf(stderr, "Cannot set EXACT period size: %s\n", snd_strerror(err));
    fprintf(stderr, "Requested size: %lu frames. Hardware may require 'near' mode.\n", (long unsigned int)FRAMES_PER_PERIOD);
    return -1;
  }
#endif

  // 10. Apply hardware parameters
  err = snd_pcm_hw_params(*handle, hw_params);
#ifdef DEBUG_ERROR
  if (err < 0)
  {
    fprintf(stderr, "Cannot set hardware parameters: %s\n", snd_strerror(err));

    return -1;
  }
#endif

  // 11. Clean up hardware parameters structure (after successful application)
  // snd_pcm_hw_params_free(hw_params);

  // 12. Prepare the PCM device
  return snd_pcm_prepare(*handle);
}

int main(int argc, char *argv[])
{
  // Lock memory for real-time performance
  int err = mlockall(MCL_CURRENT | MCL_FUTURE);
#ifdef DEBUG_BUILD
  if (unlikely(result == -1))
  {
    perror("mlockall failed");
  }
#endif

  WavHeader header = {0};
// Extreme - close fds on the programs (omitted for brevity, assume it's here)
#ifndef DEBUG
  extern char **environ;
  environ = NULL;
  //  struct rlimit rl;
  //  getrlimit(RLIMIT_NOFILE, &rl);
  for (int fd = 0; fd < 100; fd++)
  {
    close(fd);
  }
  umask(0);
  chdir("/");
#endif
#ifdef DEBUG_BUILD
  if (unlikely(argc < 2))
  {
    printf("Usage: %s <wav_file> [device_name] \n Example: qua_player test.wav "
           "hw:0,0\n",
           argv[0]);
    return -1;
  }
#endif

  const char *filename = argv[1];
  const char *device_name = "hw:0,0";

  // --- HUGE PAGE ALSA DEVICE CONFIGURATION ---
  // if (argc == 2)
  // {
  //   device_name = "hw:0,0";
  //   printf("No device specified. Using default: %s (Check ALSA config for Huge Pages)\n", device_name);
  // }
  // else
  // {
  //   device_name = argv[2];
  // }

  DEBUG_PRINT("ALSA Optimized Mmap Player - Huge Page Attempt\n");
  DEBUG_PRINT("File: %s, Device: %s\n", filename, device_name);

  // WavHeader header;
  const int fd = open(filename, O_RDONLY);

#ifdef DEBUG_BUILD
  if (unlikely(fd < 0))
  {
    perror("Error opening WAV file");
    return -1;
  }
#endif

  const off_t data_offset = read_wav_header(fd, &header);
#ifdef DEBUG_BUILD
  if (unlikely(data_offset < 0))
  {
    close(fd);
    return -1;
  }
#endif

  snd_pcm_t *pcm_handle_writable = NULL;
  err = setup_alsa(&pcm_handle_writable, device_name);

#ifdef DEBUG_BUILD
  if (unlikely(err < 0))
  {
    close(fd);
    return -1;
  }
#endif
  snd_pcm_t *const pcm_handle = pcm_handle_writable;
  DEBUG_PRINT("Attempting fixed 1 GB Huge Page allocation for %zu bytes\n",
              HUGE_PAGE_SIZE);

  // --- HUGE PAGE ALLOCATION FOR SOURCE BUFFER
  sample_t *audio_data_writable = (sample_t *)mmap(NULL,
                                                   HUGE_PAGE_SIZE, // 1GB
                                                   PROT_READ | PROT_WRITE,
                                                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE | MAP_HUGE_1GB,
                                                   -1,
                                                   0);
#ifdef DEBUG_BUILD
  if (unlikely(audio_data_writable == MAP_FAILED))
  {
    perror("FATAL: Failed to allocate Huge Pages for audio data (mmap MAP_HUGETLB)");
    fprintf(stderr, "Huge Page allocation is mandatory. Check system configuration (e.g., /proc/sys/vm/nr_hugepages).\n");
    // Cleanup and exit immediately as Huge Pages are required.
    snd_pcm_close(pcm_handle);
    close(fd);
    return -1;
  }
#endif

#ifdef DEBUG_BUILD
  // Read audio data (omitted for brevity, assume it's here)
  if (unlikely(lseek(fd, data_offset, SEEK_SET) != data_offset))
  {
    fprintf(stderr, "Failed to seek to audio data\n");
    munmap(audio_data, huge_page_aligned_size); // Use munmap for cleanup
    close(fd);
    return -1;
  }
#endif

  ssize_t total_read = 0;
  while (total_read < header.data_bytes)
  {
    ssize_t bytes_read = read(fd, (char *)audio_data_writable + total_read,
                              header.data_bytes - total_read);
#ifdef DEBUG_BUILD
    if (unlikely(bytes_read <= 0))
    {
      fprintf(stderr, "Failed to read audio data\n");
      munmap(audio_data, huge_page_aligned_size); // Use munmap for cleanup
      snd_pcm_close(pcm_handle);
      close(fd);
      return -1;
    }
#endif
    total_read += bytes_read;
  }
  close(fd);
  memset((char *)audio_data_writable + header.data_bytes, 0, HUGE_PAGE_SIZE - header.data_bytes);
  err = mprotect((void *)audio_data_writable, HUGE_PAGE_SIZE, PROT_READ);
  const sample_t *const audio_data = (const sample_t *)__builtin_assume_aligned(
      audio_data_writable,
      HUGE_PAGE_SIZE // 1GB alignment guaranteed
  );
#ifdef DEBUG_BUILD
  if (unlikely(err == -1))
  {
    perror("FATAL: mprotect failed to set PROT_READ");
    // Decide if failure is tolerable or requires cleanup/exit
  }
#endif

  // PHASE 1: Setup source pointers directly
  const size_t total_frames = header.data_bytes / (2 * sizeof(sample_t));

  const sample_t *current_src = audio_data;
  // Padds some zero to wait for drain
  const sample_t *const end_src_boundary = audio_data + (total_frames * SAMPLES_PER_FRAME) +
                                           (((FRAMES_PER_PERIOD * SAMPLES_PER_FRAME) - ((total_frames * SAMPLES_PER_FRAME) % (FRAMES_PER_PERIOD * SAMPLES_PER_FRAME))) % (FRAMES_PER_PERIOD * SAMPLES_PER_FRAME)) +
                                           (FRAMES_PER_PERIOD * SAMPLES_PER_FRAME);
  DEBUG_PRINT("Phase 1: Set up source pointers - start=%p, end=%p\n",
              current_src, end_src_boundary);

  // --- 1. Attempt to get buffer area for the full two periods ---
  const snd_pcm_channel_area_t *areas;
  snd_pcm_uframes_t commit_frames = 2 * FRAMES_PER_PERIOD; // Request 2 periods
  snd_pcm_uframes_t commit_offset;
  err = snd_pcm_mmap_begin(pcm_handle, &areas,
                           &commit_offset,
                           &commit_frames);

// --- 2. Check for Error First ---
#ifdef DEBUG_BUILD
  if (unlikely(err < 0 || commit_frames < (2 * FRAMES_PER_PERIOD)))
  {
    // Fail if we can't get the full two periods
    DEBUG_PRINT("Failed to get full pre-fill buffer area. Requested: %u frames, Got: %u frames. Error: %d\n",
                (unsigned int)(2 * FRAMES_PER_PERIOD), (unsigned int)commit_frames, err);
    return -1;
  }
#endif

  // --- 3. Main Success Logic (Purely Sequential) ---

  // Direct Initialization (as discussed, assuming guaranteed NULL)
  // const char *mmap_audio_base = (char *)areas[0].addr + (areas[0].first / CHAR_BIT);
  const char *mmap_audio_base = (char *)__builtin_assume_aligned(
      (char *)areas[0].addr + (areas[0].first / CHAR_BIT),
      ALIGN_4K // Page-aligned
  );
  assert(((uintptr_t)mmap_audio_base & 4095) == 0 && "mmap_audio_base must be 4KB aligned");
  DEBUG_PRINT("Set mmap base address: %p\n", mmap_audio_base);

  madvise((void *)audio_data, HUGE_PAGE_SIZE, MADV_SEQUENTIAL);
  memcpy_avx_stream_4k((sample_t *)__builtin_assume_aligned(mmap_audio_base, ALIGN_4K),
                       (const sample_t *)__builtin_assume_aligned(current_src, ALIGN_4K));

  // --- FILL PERIOD 2 ---
  memcpy_avx_stream_4k((sample_t *)__builtin_assume_aligned((mmap_audio_base + BYTES_PER_PERIOD), ALIGN_4K),
                       (const sample_t *)__builtin_assume_aligned(current_src + FRAMES_PER_PERIOD * SAMPLES_PER_FRAME, ALIGN_4K));

  current_src += FRAMES_PER_PERIOD * SAMPLES_PER_FRAME * 2;
  _mm_sfence();

  // Update pointer and notify
  snd_pcm_t *const pcm_handle_cached = pcm_handle;
  volatile snd_pcm_uframes_t *appl_ptr = snd_pcm_appl_ptr(pcm_handle_cached);
  *appl_ptr += (FRAMES_PER_PERIOD * 2); // Advance by both periods at once
  snd_pcm_notify_hw(pcm_handle);

  // DEBUG_PRINT("Pre-filled two periods sequentially.\n");
  // DEBUG_PRINT("Starting snd_pcm_start");

  err = snd_pcm_start(pcm_handle);
#ifdef DEBUG_BUILD
  if (unlikely(err < 0))
  {
    DEBUG_PRINT("Failed to start playback: %s\n", snd_strerror(err));
    // Check for underrun/xrun here if necessary, though it's likely a config issue
    return -1;
  }
#endif

  // Main Buffer Filling Loop
  const char *mmap_audio_base_cached = mmap_audio_base;
  const sample_t *src = (const sample_t *)__builtin_assume_aligned(
      current_src,
      ALIGN_4K);
  const sample_t *const end_src = end_src_boundary;
  sample_t iterations = (end_src - src) / (FRAMES_PER_PERIOD * SAMPLES_PER_FRAME);

  // Declarations required for inlined poll() synchronization
  struct pollfd pfd[2];
  const int npfds = 1; // const int npfds = snd_pcm_poll_descriptors_count(pcm_handle_cached);
  snd_pcm_poll_descriptors(pcm_handle_cached, pfd, npfds);


  // do
  // {
  //   poll(pfd, npfds, -1);
  //   memcpy_avx_stream_4k((sample_t *)__builtin_assume_aligned(mmap_audio_base_cached, ALIGN_4K),
  //                        (const sample_t *)__builtin_assume_aligned(src, ALIGN_4K));
  //   snd_pcm_mmap_commit(pcm_handle_cached, 0, FRAMES_PER_PERIOD);
  //   poll(pfd, npfds, -1);
  //   memcpy_avx_stream_4k((sample_t *)__builtin_assume_aligned((mmap_audio_base_cached + BYTES_PER_PERIOD), ALIGN_4K),
  //                        (const sample_t *)__builtin_assume_aligned(src + (FRAMES_PER_PERIOD * SAMPLES_PER_FRAME), ALIGN_4K));
  //   snd_pcm_mmap_commit(pcm_handle_cached, 0, FRAMES_PER_PERIOD);
  //   src += (FRAMES_PER_PERIOD * SAMPLES_PER_FRAME * 2);
  //   iterations -= 2;
  // } while (likely(iterations > 0));
  do
  {
    my_poll(pfd, npfds, -1);
    // poll(pfd, npfds, -1);
    // syscall(__NR_poll, pfd, npfds, -1);
    memcpy_avx_stream_4k((sample_t *)__builtin_assume_aligned(mmap_audio_base_cached, ALIGN_4K),
                         (const sample_t *)__builtin_assume_aligned(src, ALIGN_4K));
    *appl_ptr += FRAMES_PER_PERIOD;
    snd_pcm_notify_hw(pcm_handle_cached);
    

    // syscall(__NR_poll, pfd, npfds, -1);
    // my_poll(pfd, npfds, -1);
    poll(pfd, npfds, -1);
    memcpy_avx_stream_4k((sample_t *)__builtin_assume_aligned((mmap_audio_base_cached + BYTES_PER_PERIOD), ALIGN_4K),
                         (const sample_t *)__builtin_assume_aligned(src + (FRAMES_PER_PERIOD * SAMPLES_PER_FRAME), ALIGN_4K));
    *appl_ptr += FRAMES_PER_PERIOD;
    snd_pcm_notify_hw(pcm_handle_cached);
   

    src += (FRAMES_PER_PERIOD * SAMPLES_PER_FRAME * 2);
    iterations -= 2;
  } while (likely(iterations > 0));

  snd_pcm_drain(pcm_handle_cached);
  DEBUG_PRINT("\nPlayback completed!\n");

  // --- HUGE PAGE CLEANUP ---
  // munmap(audio_data, huge_page_aligned_size);

  return 0;
}
