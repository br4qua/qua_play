#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h> // Provides CHAR_BIT
#include <alsa/asoundlib.h>
#include <fcntl.h>
#include <immintrin.h> // Provides INTRINSICS
#include <sched.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "debug.h"

// Branch prediction hints
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

// System Specific Consts
#define PAGE_SIZE 4096
// --- HUGE PAGE CONFIGURATION ---
// IMPORTANT: Adjust this value to your system's Huge Page size (e.g., 2MB)
#define HUGE_PAGE_SIZE 0x40000000 // 2MB

// Audio Configuration
#define NUM_OF_CHANNELS 2
#define BIT_DEPTH 32
#define BYTES_PER_CHANNEL sizeof(int32_t) // 4 bytes (for 32-bit audio)
#define BYTES_PER_AUDIO_FRAME (NUM_OF_CHANNELS * BYTES_PER_CHANNEL)

// --- Primary Configuration (Fixed Constants) ---
// Total Ring Buffer (This is the 2^15 size)
#define TOTAL_BUFFER_SIZE_BYTES 32768 * 16
// The number of periods (chunks) the total buffer is divided into.
#define NUM_OF_PERIODS 2

// --- Derived Constants (Calculated from Fixed Values) ---
// Size of one period/chunk in bytes
#define PERIOD_SIZE_BYTES (TOTAL_BUFFER_SIZE_BYTES / NUM_OF_PERIODS)

// --- Derived ALSA Frame Counts (Required for ALSA API) ---
// Total frames
#define ALSA_BUFFER_FRAMES_COUNT (TOTAL_BUFFER_SIZE_BYTES / BYTES_PER_AUDIO_FRAME)

// The total count of audio frames that fit into one period's memory size.
// (Calculated by: PERIOD_SIZE_BYTES / BYTES_PER_AUDIO_FRAME)
#define ALSA_PERIOD_FRAME_COUNT (PERIOD_SIZE_BYTES / BYTES_PER_AUDIO_FRAME)

static const snd_pcm_uframes_t alsa_buffer_frames_count =
    ALSA_BUFFER_FRAMES_COUNT;
static const snd_pcm_uframes_t alsa_period_frame_count = ALSA_PERIOD_FRAME_COUNT;

// Custom Implementation of memcpy - AVX2 intrinsics, 4Kb aligned
static inline void memcpy_avx_stream_4k(int32_t *dst, const int32_t *src)
{
  for (size_t i = 0; i < ALSA_PERIOD_FRAME_COUNT * 2; i += 32)
  {
    // Load 4x 256-bit vectors from source
    __m256i data0 = _mm256_load_si256((__m256i *)(src + i));
    __m256i data1 = _mm256_load_si256((__m256i *)(src + i + 8));
    __m256i data2 = _mm256_load_si256((__m256i *)(src + i + 16));
    __m256i data3 = _mm256_load_si256((__m256i *)(src + i + 24));
    // Store 4x 256-bit vectors to destination using non-temporal writes
    _mm256_stream_si256((__m256i *)(dst + i), data0);
    _mm256_stream_si256((__m256i *)(dst + i + 8), data1);
    _mm256_stream_si256((__m256i *)(dst + i + 16), data2);
    _mm256_stream_si256((__m256i *)(dst + i + 24), data3);
  }
  // UNSAFE - _mm_sfense prevents race condition.
  // _mm_sfense();
}

// Type definitions for RIFF
typedef struct
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

// Function declarations
static const char *mmap_audio_base = NULL;
static int setup_alsa(snd_pcm_t **handle, const char *device, const int channels, int const rate);
static off_t read_wav_header(int fd, WavHeader *header);

static int setup_alsa(snd_pcm_t **handle, const char *device, const int channels, int const rate)
{
  // Local Declarations (Needed for compilation, fixing previous errors)
  int err;
  snd_pcm_hw_params_t *hw_params;

  // 1. Open audio device
  err = snd_pcm_open(handle, device, SND_PCM_STREAM_PLAYBACK, 0);

  // 2. Allocate hardware parameters
  err = snd_pcm_hw_params_malloc(&hw_params);
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
    snd_pcm_hw_params_free(hw_params);
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
    snd_pcm_hw_params_free(hw_params);
    return -1;
  }
#endif

  // 5. Set sample format
  err = snd_pcm_hw_params_set_format(*handle, hw_params,
                                     SND_PCM_FORMAT_S32_LE);
#ifdef DEBUG_ERROR
  if (err < 0)
  {
    fprintf(stderr, "Cannot set sample format: %s\n", snd_strerror(err));
    snd_pcm_hw_params_free(hw_params);
    return -1;
  }
#endif

  // 6. Set sample rate
  err = snd_pcm_hw_params_set_rate(*handle, hw_params, rate, 0);
#ifdef DEBUG_ERROR
  if (err < 0)
  {
    fprintf(stderr, "Cannot set sample rate: %s\n", snd_strerror(err));
    snd_pcm_hw_params_free(hw_params);
    return -1;
  }
#endif

  // 7. Set channel count
  err = snd_pcm_hw_params_set_channels(*handle, hw_params, channels);
#ifdef DEBUG_ERROR
  if (err < 0)
  {
    fprintf(stderr, "Cannot set channel count: %s\n", snd_strerror(err));
    snd_pcm_hw_params_free(hw_params);
    return -1;
  }
#endif

  // 8. Set buffer size (EXACT)
  // NOTE: This call will fail if the exact frame count is not supported by hardware.
  err = snd_pcm_hw_params_set_buffer_size(*handle, hw_params,
                                          alsa_buffer_frames_count);
#ifdef DEBUG_ERROR
  if (err < 0)
  {
    fprintf(stderr, "Cannot set EXACT buffer size: %s\n", snd_strerror(err));
    fprintf(stderr, "Requested size: %lu frames. Hardware may require 'near' mode.\n", (long unsigned int)alsa_buffer_frames_count);
    snd_pcm_hw_params_free(hw_params);
    return -1;
  }
#endif

  // 9. Set period size (EXACT)

  err = snd_pcm_hw_params_set_period_size(*handle, hw_params,
                                          alsa_period_frame_count, 0);
#ifdef DEBUG_ERROR
  if (err < 0)
  {
    fprintf(stderr, "Cannot set EXACT period size: %s\n", snd_strerror(err));
    fprintf(stderr, "Requested size: %lu frames. Hardware may require 'near' mode.\n", (long unsigned int)alsa_period_frame_count);
    snd_pcm_hw_params_free(hw_params);
    return -1;
  }
#endif

  // 10. Apply hardware parameters
  err = snd_pcm_hw_params(*handle, hw_params);
#ifdef DEBUG_ERROR
  if (err < 0)
  {
    fprintf(stderr, "Cannot set hardware parameters: %s\n", snd_strerror(err));
    snd_pcm_hw_params_free(hw_params);
    return -1;
  }
#endif

  // 11. Clean up hardware parameters structure (after successful application)
  snd_pcm_hw_params_free(hw_params);

  // 12. Prepare the PCM device
  return snd_pcm_prepare(*handle);
}

// static off_t read_wav_header(int fd, WavHeader *header)
// {
//   uint8_t basic_header[12];

//   if (unlikely(read(fd, basic_header, 12) != 12))
//   {
//     fprintf(stderr, "Failed to read WAV header\n");
//     return -1;
//   }

//   if (unlikely(strncmp((char *)basic_header, "RIFF", 4) != 0 ||
//                strncmp((char *)basic_header + 8, "WAVE", 4) != 0))
//   {
//     fprintf(stderr, "Invalid WAV file format\n");
//     return -1;
//   }

//   memcpy(header->riff_header, basic_header, 4);
//   memcpy(&header->wav_size, basic_header + 4, 4);
//   memcpy(header->wave_header, basic_header + 8, 4);

//   int fmt_chunk_found = 0;
//   int data_chunk_found = 0;
//   off_t data_offset = 0;

//   while (!data_chunk_found)
//   {
//     char chunk_id[4];
//     uint32_t chunk_size;

//     if (unlikely(read(fd, chunk_id, 4) != 4 || read(fd, &chunk_size, 4) != 4))
//     {
//       fprintf(stderr, "Couldn't find required chunks\n");
//       return -1;
//     }

//     if (strncmp(chunk_id, "fmt ", 4) == 0)
//     {
//       memcpy(header->fmt_header, chunk_id, 4);
//       header->fmt_chunk_size = chunk_size;

//       if (unlikely(chunk_size < 16))
//       {
//         fprintf(stderr, "Format chunk too small\n");
//         return -1;
//       }

//       uint8_t fmt_data[16];
//       if (unlikely(read(fd, fmt_data, 16) != 16))
//       {
//         fprintf(stderr, "Failed to read format chunk data\n");
//         return -1;
//       }

//       memcpy(&header->audio_format, fmt_data, 2);
//       memcpy(&header->num_channels, fmt_data + 2, 2);
//       memcpy(&header->sample_rate, fmt_data + 4, 4);
//       memcpy(&header->byte_rate, fmt_data + 8, 4);
//       memcpy(&header->sample_alignment, fmt_data + 12, 2);
//       memcpy(&header->bit_depth, fmt_data + 14, 2);

//       if (chunk_size > 16)
//       {
//         lseek(fd, chunk_size - 16, SEEK_CUR);
//       }

//       if (unlikely(header->audio_format != 1 && header->audio_format != 65534))
//       {
//         fprintf(stderr, "Only PCM WAV files are supported\n");
//         return -1;
//       }

//       if (unlikely(header->bit_depth != BIT_DEPTH))
//       {
//         fprintf(stderr, "Only 32-bit audio is supported\n");
//         return -1;
//       }

//       fmt_chunk_found = 1;
//     }
//     else if (strncmp(chunk_id, "data", 4) == 0)
//     {
//       memcpy(header->data_header, chunk_id, 4);
//       header->data_bytes = chunk_size;
//       data_chunk_found = 1;
//       data_offset = lseek(fd, 0, SEEK_CUR);
//       break;
//     }
//     else
//     {
//       lseek(fd, chunk_size, SEEK_CUR);
//       if (chunk_size % 2 != 0)
//       {
//         lseek(fd, 1, SEEK_CUR);
//       }
//     }
//   }

//   if (unlikely(!fmt_chunk_found))
//   {
//     fprintf(stderr, "No format chunk found\n");
//     return -1;
//   }

//   return data_offset;
// }
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

off_t read_wav_header(int fd, WavHeader *header)
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

int main(int argc, char *argv[])
{
WavHeader header = {0};
// Extreme - close fds on the programs (omitted for brevity, assume it's here)
#ifndef DEBUG
  extern char **environ;
  environ = NULL;

  struct rlimit rl;
  getrlimit(RLIMIT_NOFILE, &rl);
  for (rlim_t fd = 0; fd < rl.rlim_cur; fd++)
  {
    close(fd);
  }
  umask(0);
  // chdir("/");
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
  const char *device_name;

  // --- HUGE PAGE ALSA DEVICE CONFIGURATION ---
  if (argc == 2)
  {
    device_name = "hw:0,0";
    printf("No device specified. Using default: %s (Check ALSA config for Huge Pages)\n", device_name);
  }
  else
  {
    device_name = argv[2];
  }

  DEBUG_PRINT("ALSA Optimized Mmap Player - Huge Page Attempt\n");
  DEBUG_PRINT("File: %s, Device: %s\n", filename, device_name);

  // WavHeader header;
  int fd = open(filename, O_RDONLY);

#ifdef DEBUG_BUILD
  if (unlikely(fd < 0))
  {
    perror("Error opening WAV file");
    return -1;
  }
#endif

  off_t data_offset = read_wav_header(fd, &header);
#ifdef DEBUG_BUILD
  if (unlikely(data_offset < 0))
  {
    close(fd);
    return -1;
  }
#endif

  snd_pcm_t *pcm_handle = NULL;
  int err = setup_alsa(&pcm_handle, device_name, NUM_OF_CHANNELS, header.sample_rate);
#ifdef DEBUG_BUILD
  if (unlikely(err < 0))
  {
    close(fd);
    return -1;
  }
#endif

  int32_t *audio_data;
  // Define a fixed 2 GB size for Huge Page allocation

  const size_t huge_page_aligned_size = 0x80000000; // 2 GB = 2 * 1024 * 1024 * 1024 = 0x80000000 bytes

  DEBUG_PRINT("Attempting fixed 2 GB Huge Page allocation for %zu bytes\n",
              huge_page_aligned_size);

  // --- HUGE PAGE ALLOCATION FOR SOURCE BUFFER (Fixed 2 GB) ---
  audio_data = (int32_t *)mmap(NULL,
                               huge_page_aligned_size,
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE | MAP_HUGE_1GB,
                               -1,
                               0);
#ifdef DEBUG_BUILD
  if (unlikely(audio_data == MAP_FAILED))
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
    ssize_t bytes_read = read(fd, (char *)audio_data + total_read,
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
  // memset((char *)audio_data + header.data_bytes, 0, mem_padding_bytes);
  memset((char *)audio_data + header.data_bytes, 0, huge_page_aligned_size - header.data_bytes);
  err = mprotect(audio_data, huge_page_aligned_size, PROT_READ);
#ifdef DEBUG_BUILD
  if (unlikely(err == -1))
  {
    perror("FATAL: mprotect failed to set PROT_READ");
    // Decide if failure is tolerable or requires cleanup/exit
  }
#endif

  // Lock memory for real-time performance
  err = mlockall(MCL_CURRENT | MCL_FUTURE);
#ifdef DEBUG_BUILD
  if (unlikely(result == -1))
  {
    perror("mlockall failed");
  }
#endif
  madvise(audio_data, huge_page_aligned_size, MADV_SEQUENTIAL);

  // Initialize global state (omitted for brevity, assume it's here)
  const size_t total_frames = header.data_bytes / (2 * sizeof(int32_t));

  // PHASE 1: Setup source pointers directly (no pre-calculation array)
  int32_t *current_src = audio_data;
  int32_t *const end_src_boundary = audio_data + (total_frames * NUM_OF_CHANNELS);

  DEBUG_PRINT("Phase 1: Set up source pointers - start=%p, end=%p\n",
              current_src, end_src_boundary);

  // Initialize destination offset for ring buffer
  uint32_t dst_offset = 0;

  const snd_pcm_uframes_t single_period_frames = alsa_period_frame_count;

  // --- 1. Attempt to get buffer area for the full two periods ---
  const snd_pcm_channel_area_t *areas;
  const snd_pcm_uframes_t commit_frames = 2 * single_period_frames; // Request 2 periods
  const snd_pcm_uframes_t commit_offset;
  err = snd_pcm_mmap_begin(pcm_handle, &areas,
                           &commit_offset,
                           &commit_frames);

// --- 2. Check for Error First ---
#ifdef DEBUG_BUILD
  if (unlikely(err < 0 || commit_frames < (2 * single_period_frames)))
  {
    // Fail if we can't get the full two periods
    DEBUG_PRINT("Failed to get full pre-fill buffer area. Requested: %u frames, Got: %u frames. Error: %d\n",
                (unsigned int)(2 * single_period_frames), (unsigned int)commit_frames, err);
    return -1;
  }
#endif

  // --- 3. Main Success Logic (Purely Sequential) ---

  // Direct Initialization (as discussed, assuming guaranteed NULL)
  mmap_audio_base = (char *)areas[0].addr + (areas[0].first / CHAR_BIT);
  DEBUG_PRINT("Set mmap base address: %p\n", mmap_audio_base);

  // --- FILL PERIOD 1 ---
  int32_t *dst_p1 = (int32_t *)(mmap_audio_base + dst_offset);
  memcpy_avx_stream_4k(dst_p1, current_src);
  current_src += single_period_frames * NUM_OF_CHANNELS;
  dst_offset = (dst_offset + PERIOD_SIZE_BYTES) & (TOTAL_BUFFER_SIZE_BYTES - 1);

  // --- FILL PERIOD 2 ---
  int32_t *dst_p2 = (int32_t *)(mmap_audio_base + dst_offset);
  memcpy_avx_stream_4k(dst_p2, current_src);
  current_src += single_period_frames * NUM_OF_CHANNELS;
  dst_offset = (dst_offset + PERIOD_SIZE_BYTES) & (TOTAL_BUFFER_SIZE_BYTES - 1);

  // Commit the two-period buffer (total frames committed)
  snd_pcm_mmap_commit(pcm_handle,
                      commit_offset,
                      2 * single_period_frames);

  DEBUG_PRINT("Pre-filled two periods sequentially.\n");
  DEBUG_PRINT("Starting snd_pcm_start");
  err = snd_pcm_start(pcm_handle);
  #ifdef DEBUG_BUILD
  if (unlikely(err < 0))
  {
    DEBUG_PRINT("Failed to start playback: %s\n", snd_strerror(err));
    // Check for underrun/xrun here if necessary, though it's likely a config issue
    return -1;
  }
  #endif

  // Main Buffer Filling Loop (omitted for brevity, assume it's here)
  const char *mmap_audio_base_cached = mmap_audio_base;
  register int32_t *src = current_src;
  register int32_t *const end_src = end_src_boundary;
  register int32_t *dst = (int32_t *)(mmap_audio_base_cached + dst_offset);
  register snd_pcm_t *const pcm_handle_cached = pcm_handle;
  register snd_pcm_uframes_t const frames = alsa_period_frame_count;
  register int32_t iterations = (end_src - src) / (alsa_period_frame_count * 2);

  // Declarations required for inlined poll() synchronization
  struct pollfd pfd[2];
  int npfds = snd_pcm_poll_descriptors_count(pcm_handle_cached);
  snd_pcm_poll_descriptors(pcm_handle_cached, pfd, npfds);

  if (likely(iterations > 0)) // Check if there is data left to process
  {
    do
    {
      // Wait for ALSA buffer space to be available (snd_pcm_wait)
      poll(pfd, npfds, -1);

      // Custom memcpy: Source and Destination backed by Huge Pages
      memcpy_avx_stream_4k(dst, src);

      // Still need to commit
      snd_pcm_mmap_commit(pcm_handle_cached,
                          0,
                          frames);

      src += alsa_period_frame_count * 2;

      dst_offset = (dst_offset + PERIOD_SIZE_BYTES) & (TOTAL_BUFFER_SIZE_BYTES - 1);
      dst = (int32_t *)(mmap_audio_base_cached + dst_offset);

    } while (unlikely(--iterations > 0)); // Highly optimized pre-decrement check
  }

  snd_pcm_drain(pcm_handle_cached);
  DEBUG_PRINT("\nPlayback completed!\n");

  // --- HUGE PAGE CLEANUP ---
  munmap(audio_data, huge_page_aligned_size);

  return 0;
}
