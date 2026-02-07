#include "qua-post-processing.h"

/* Standard C Headers */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* System / POSIX Headers */
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* CPU Intrinsics */
#include <immintrin.h>

// Post-process audio file: mix channels and resample
// Returns 0 on success, 1 on failure
int qua_post_process(const char *file_path, int bit_depth, int sample_rate,
                     int channels) {
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
             "-t", "wav", "-r", sr_str, temp_path, "channels", "2", "rate",
             "-v", NULL);
    } else if (channels == 6) {
      // 5.1 -> Stereo downmix
      fprintf(stderr, "DEBUG: Executing 5.1->stereo downmix\n");
      execlp("sox", "sox", file_path, "-b", bd_str, "-e", "signed-integer",
             "-t", "wav", "-r", sr_str, temp_path, "remix", "1,3v0.707,5v0.707",
             "2,3v0.707,6v0.707", "rate", "-v", NULL);
    } else {
      // Other channels -> just resample
      fprintf(stderr, "DEBUG: Executing resample or bit-depth\n");
      execlp("sox", "sox", file_path, "-b", bd_str, "-e", "signed-integer",
             "-t", "wav", "-r", sr_str, temp_path, "rate", "-v", NULL);
    }
    _exit(1);
  }

  if (pid > 0) {
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
      fprintf(stderr, "DEBUG: sox exited with status %d\n",
              WEXITSTATUS(status));
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

int convert_24bit_to_32bit_wav(const char *input_path) {
  char tmp_path[PATH_MAX];
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", input_path);

  int fd_in = open(input_path, O_RDONLY);
  if (fd_in == -1) {
    return -1;
  }

  // Read full header
  uint8_t header[44];
  if (read(fd_in, header, 44) != 44) {
    close(fd_in);
    return -1;
  }

  // Get parameters we need
  uint16_t channels = 0;
  uint32_t sample_rate = 0;
  uint32_t data_size_24bit = 0;

  memcpy(&channels, header + 22, 2);
  memcpy(&sample_rate, header + 24, 4);
  memcpy(&data_size_24bit, header + 40, 4);

  // Calculate new sizes
  uint32_t num_samples = data_size_24bit / 3;
  uint32_t data_size_32bit = num_samples * 4;
  uint32_t file_size_32bit = 36 + data_size_32bit;

  // Open output file
  int fd_out = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd_out == -1) {
    close(fd_in);
    return -1;
  }

  // Modify header for 32-bit
  uint32_t byte_rate = sample_rate * channels * 4;
  uint16_t block_align = channels * 4;
  uint16_t bits_32 = 32;

  memcpy(header + 4, &file_size_32bit, 4);
  memcpy(header + 28, &byte_rate, 4);
  memcpy(header + 32, &block_align, 2);
  memcpy(header + 34, &bits_32, 2);
  memcpy(header + 40, &data_size_32bit, 4);

  // Write modified header
  if (write(fd_out, header, 44) != 44) {
    close(fd_in);
    close(fd_out);
    unlink(tmp_path);
    return -1;
  }

  // Convert sample data
  uint8_t in_buf[65536 * 3];
  uint8_t out_buf[65536 * 4];

  ssize_t bytes_read;
  while ((bytes_read = read(fd_in, in_buf, sizeof(in_buf))) > 0) {
    size_t samples_read = bytes_read / 3;

    // Convert 24-bit to 32-bit (zero-pad on LSB)
    for (size_t i = 0; i < samples_read; i++) {
      out_buf[i * 4 + 0] = 0;
      out_buf[i * 4 + 1] = in_buf[i * 3 + 0];
      out_buf[i * 4 + 2] = in_buf[i * 3 + 1];
      out_buf[i * 4 + 3] = in_buf[i * 3 + 2];
    }

    size_t bytes_to_write = samples_read * 4;
    if (write(fd_out, out_buf, bytes_to_write) != (ssize_t)bytes_to_write) {
      close(fd_in);
      close(fd_out);
      unlink(tmp_path);
      return -1;
    }
  }

  close(fd_in);
  close(fd_out);

  // Atomically replace original with converted version
  if (rename(tmp_path, input_path) != 0) {
    unlink(tmp_path);
    return -1;
  }

  return 0;
}

//  AVX2
int convert_24bit_to_32bit_wav_fast(const char *input_path) {
  char tmp_path[PATH_MAX];
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", input_path);

  int fd_in = open(input_path, O_RDONLY);
  if (fd_in == -1)
    return -1;
  posix_fadvise(fd_in, 0, 0, POSIX_FADV_SEQUENTIAL);

  uint16_t channels = 0;
  uint32_t sample_rate = 0;
  uint8_t buf[4];
  lseek(fd_in, 12, SEEK_SET);
  while (read(fd_in, buf, 4) == 4) {
    uint32_t sz;
    if (read(fd_in, &sz, 4) != 4)
      break;
    if (memcmp(buf, "fmt ", 4) == 0) {
      lseek(fd_in, 2, SEEK_CUR);
      read(fd_in, &channels, 2);
      read(fd_in, &sample_rate, 4);
      lseek(fd_in, sz - 8, SEEK_CUR);
    } else if (memcmp(buf, "data", 4) == 0)
      break;
    else
      lseek(fd_in, sz, SEEK_CUR);
  }

  if (channels == 0 || sample_rate == 0) {
    close(fd_in);
    return -1;
  }

  int fd_out = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd_out == -1) {
    close(fd_in);
    return -1;
  }

  uint8_t h[68] = {0};
  memcpy(h, "RIFF", 4);
  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4);
  *(uint32_t *)(h + 16) = 40;
  *(uint16_t *)(h + 20) = 0xFFFE;
  *(uint16_t *)(h + 22) = channels;
  *(uint32_t *)(h + 24) = sample_rate;
  *(uint32_t *)(h + 28) = sample_rate * channels * 4;
  *(uint16_t *)(h + 32) = channels * 4;
  *(uint16_t *)(h + 34) = 32;
  *(uint16_t *)(h + 36) = 22;
  *(uint16_t *)(h + 38) = 32;
  *(uint32_t *)(h + 40) = (channels == 2) ? 3 : 4;
  uint8_t guid[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                      0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
  memcpy(h + 44, guid, 16);
  memcpy(h + 60, "data", 4);
  write(fd_out, h, 68);

  const size_t batch = 65536;
  uint8_t *in = _mm_malloc(batch * 3, 32);
  uint8_t *out = _mm_malloc(batch * 4, 32);

  __m128i m128 =
      _mm_setr_epi8(-1, 0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11);

  ssize_t n;
  while ((n = read(fd_in, in, batch * 3)) > 0) {
    size_t samples = n / 3;
    size_t i = 0;

    for (; i + 7 < samples; i += 8) {
      __m128i low_in = _mm_loadu_si128((__m128i *)&in[i * 3]);
      __m128i high_in = _mm_loadu_si128((__m128i *)&in[i * 3 + 12]);

      __m256i combined = _mm256_set_m128i(_mm_shuffle_epi8(high_in, m128),
                                          _mm_shuffle_epi8(low_in, m128));

      _mm256_stream_si256((__m256i *)&out[i * 4], combined);
    }

    for (; i < samples; i++) {
      out[i * 4 + 0] = 0;
      out[i * 4 + 1] = in[i * 3 + 0];
      out[i * 4 + 2] = in[i * 3 + 1];
      out[i * 4 + 3] = in[i * 3 + 2];
    }

    write(fd_out, out, samples * 4);
    if (n % 3 > 0)
      lseek(fd_in, -(n % 3), SEEK_CUR);
  }

  uint32_t final = lseek(fd_out, 0, SEEK_CUR);
  lseek(fd_out, 4, SEEK_SET);
  uint32_t r_sz = final - 8;
  write(fd_out, &r_sz, 4);
  lseek(fd_out, 64, SEEK_SET);
  uint32_t d_sz = final - 68;
  write(fd_out, &d_sz, 4);

  _mm_free(in);
  _mm_free(out);
  close(fd_in);
  close(fd_out);
  rename(tmp_path, input_path);
  return 0;
}

// AVX2: High-performance 16-bit to 32-bit PCM conversion
int convert_16bit_to_32bit_wav_fast(const char *input_path) {
  char tmp_path[PATH_MAX];
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", input_path);

  int fd_in = open(input_path, O_RDONLY);
  if (fd_in == -1)
    return -1;

  struct stat st;
  if (fstat(fd_in, &st) != 0) {
    close(fd_in);
    return -1;
  }
  size_t file_size = st.st_size;

  // 1. Map Input (RAM-to-CPU path)
  uint8_t *src = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd_in, 0);
  if (src == MAP_FAILED) {
    close(fd_in);
    return -1;
  }

  // Parse existing header
  uint16_t channels = 0;
  uint32_t sample_rate = 0;
  uint32_t data_offset = 0;
  for (uint32_t i = 12; i < 1024 && i < file_size;) {
    if (memcmp(src + i, "fmt ", 4) == 0) {
      channels = *(uint16_t *)(src + i + 10);
      sample_rate = *(uint32_t *)(src + i + 12);
      i += 8 + *(uint32_t *)(src + i + 4);
    } else if (memcmp(src + i, "data", 4) == 0) {
      data_offset = i + 8;
      break;
    } else
      i += 8 + *(uint32_t *)(src + i + 4);
  }

  if (channels == 0 || data_offset == 0) {
    munmap(src, file_size);
    close(fd_in);
    return -1;
  }

  // 2. Prepare Output in /dev/shm
  size_t total_samples = (file_size - data_offset) / 2;
  size_t output_data_len = total_samples * 4;
  size_t total_out_size = 68 + output_data_len;

  int fd_out = open(tmp_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd_out == -1) {
    munmap(src, file_size);
    close(fd_in);
    return -1;
  }

  // Allocate space in RAM for the output
  if (ftruncate(fd_out, total_out_size) != 0) {
    munmap(src, file_size);
    close(fd_in);
    close(fd_out);
    return -1;
  }

  // 3. Map Output (CPU-to-RAM path)
  uint8_t *dst =
      mmap(NULL, total_out_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_out, 0);
  if (dst == MAP_FAILED) {
    munmap(src, file_size);
    close(fd_in);
    close(fd_out);
    return -1;
  }

  // Write WaveFormatExtensible Header directly to memory
  memcpy(dst, "RIFF", 4);
  memcpy(dst + 8, "WAVE", 4);
  memcpy(dst + 12, "fmt ", 4);
  *(uint32_t *)(dst + 4) = (uint32_t)(total_out_size - 8);
  *(uint32_t *)(dst + 16) = 40;
  *(uint16_t *)(dst + 20) = 0xFFFE;
  *(uint16_t *)(dst + 22) = channels;
  *(uint32_t *)(dst + 24) = sample_rate;
  *(uint32_t *)(dst + 28) = sample_rate * channels * 4;
  *(uint16_t *)(dst + 32) = (uint16_t)(channels * 4);
  *(uint16_t *)(dst + 34) = 32;
  *(uint16_t *)(dst + 36) = 22;
  *(uint16_t *)(dst + 38) = 32;
  *(uint32_t *)(dst + 40) = (channels == 2) ? 3 : 4;
  uint8_t guid[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                      0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
  memcpy(dst + 44, guid, 16);
  memcpy(dst + 60, "data", 4);
  *(uint32_t *)(dst + 64) = (uint32_t)output_data_len;

  // 4. SIMD "Memory-to-Memory" Burst
  int16_t *in_ptr = (int16_t *)(src + data_offset);
  int32_t *out_ptr = (int32_t *)(dst + 68);
  size_t i = 0;

  // Unrolled 32-sample burst (128 bytes in -> 256 bytes out)
  for (; i + 31 < total_samples; i += 32) {
    __m256i in0 = _mm256_loadu_si256((__m256i *)&in_ptr[i]);
    __m256i in1 = _mm256_loadu_si256((__m256i *)&in_ptr[i + 16]);

    // Sign extend + Left Shift (Bit-Perfect PCM)
    __m256i r0 = _mm256_slli_epi32(
        _mm256_cvtepi16_epi32(_mm256_castsi256_si128(in0)), 16);
    __m256i r1 = _mm256_slli_epi32(
        _mm256_cvtepi16_epi32(_mm256_extracti128_si256(in0, 1)), 16);
    __m256i r2 = _mm256_slli_epi32(
        _mm256_cvtepi16_epi32(_mm256_castsi256_si128(in1)), 16);
    __m256i r3 = _mm256_slli_epi32(
        _mm256_cvtepi16_epi32(_mm256_extracti128_si256(in1, 1)), 16);

    _mm256_storeu_si256((__m256i *)&out_ptr[i], r0);
    _mm256_storeu_si256((__m256i *)&out_ptr[i + 8], r1);
    _mm256_storeu_si256((__m256i *)&out_ptr[i + 16], r2);
    _mm256_storeu_si256((__m256i *)&out_ptr[i + 24], r3);
  }

  // Exact tail handling
  for (; i < total_samples; i++) {
    out_ptr[i] = (int32_t)in_ptr[i] << 16;
  }

  // Clean up and Atomic swap
  munmap(src, file_size);
  munmap(dst, total_out_size);
  close(fd_in);
  close(fd_out);
  rename(tmp_path, input_path);
  return 0;
}

int convert_24bit_to_32bit_wav_ultrafast(const char *input_path) {
    char tmp_path[PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", input_path);

    int fd_in = open(input_path, O_RDONLY);
    if (fd_in == -1) return -1;

    struct stat st;
    fstat(fd_in, &st);
    size_t file_size = st.st_size;

    // Map input file directly from /dev/shm
    uint8_t *src = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd_in, 0);
    if (src == MAP_FAILED) {
        close(fd_in);
        return -1;
    }

    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint32_t data_offset = 0;
    for (uint32_t i = 12; i < 1024 && i < file_size - 8;) {
        if (memcmp(src + i, "fmt ", 4) == 0) {
            channels = *(uint16_t *)(src + i + 10);
            sample_rate = *(uint32_t *)(src + i + 12);
            i += 8 + *(uint32_t *)(src + i + 4);
        } else if (memcmp(src + i, "data", 4) == 0) {
            data_offset = i + 8;
            break;
        } else {
            i += 8 + *(uint32_t *)(src + i + 4);
        }
    }

    if (channels == 0 || data_offset == 0) {
        munmap(src, file_size);
        close(fd_in);
        return -1;
    }

    size_t total_samples = (file_size - data_offset) / 3;
    size_t output_data_len = total_samples * 4;
    size_t total_out_size = 68 + output_data_len;

    int fd_out = open(tmp_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd_out == -1) {
        munmap(src, file_size);
        close(fd_in);
        return -1;
    }
    ftruncate(fd_out, total_out_size);

    // Map output file directly back to /dev/shm
    uint8_t *dst = mmap(NULL, total_out_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_out, 0);
    if (dst == MAP_FAILED) {
        munmap(src, file_size);
        close(fd_in);
        close(fd_out);
        return -1;
    }

    // Header logic (Standard RIFF/WAVE 32-bit PCM)
    memcpy(dst, "RIFF", 4);
    memcpy(dst + 8, "WAVE", 4);
    memcpy(dst + 12, "fmt ", 4);
    *(uint32_t *)(dst + 4) = (uint32_t)(total_out_size - 8);
    *(uint32_t *)(dst + 16) = 40;
    *(uint16_t *)(dst + 20) = 0xFFFE;
    *(uint16_t *)(dst + 22) = channels;
    *(uint32_t *)(dst + 24) = sample_rate;
    *(uint32_t *)(dst + 28) = sample_rate * channels * 4;
    *(uint16_t *)(dst + 32) = (uint16_t)(channels * 4);
    *(uint16_t *)(dst + 34) = 32;
    *(uint16_t *)(dst + 36) = 22;
    *(uint16_t *)(dst + 38) = 32;
    *(uint32_t *)(dst + 40) = (channels == 2) ? 3 : 4;
    uint8_t guid[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                        0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    memcpy(dst + 44, guid, 16);
    memcpy(dst + 60, "data", 4);
    *(uint32_t *)(dst + 64) = (uint32_t)output_data_len;

    uint8_t *in_ptr = src + data_offset;
    uint8_t *out_ptr = dst + 68;

    // Mask to convert 3-byte samples to 4-byte lanes (Zero-pad LSB)
    __m128i mask = _mm_setr_epi8(-1, 0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11);

    size_t i = 0;
    // UNROLLED 32-sample burst: Zero write() overhead, Zero intermediate buffers
    for (; i + 31 < total_samples; i += 32) {
        __m128i s0 = _mm_loadu_si128((__m128i *)&in_ptr[(i + 0) * 3]);
        __m128i s1 = _mm_loadu_si128((__m128i *)&in_ptr[(i + 4) * 3]);
        __m128i s2 = _mm_loadu_si128((__m128i *)&in_ptr[(i + 8) * 3]);
        __m128i s3 = _mm_loadu_si128((__m128i *)&in_ptr[(i + 12) * 3]);
        __m128i s4 = _mm_loadu_si128((__m128i *)&in_ptr[(i + 16) * 3]);
        __m128i s5 = _mm_loadu_si128((__m128i *)&in_ptr[(i + 20) * 3]);
        __m128i s6 = _mm_loadu_si128((__m128i *)&in_ptr[(i + 24) * 3]);
        __m128i s7 = _mm_loadu_si128((__m128i *)&in_ptr[(i + 28) * 3]);

        _mm256_storeu_si256((__m256i *)&out_ptr[i * 4 + 0],  _mm256_set_m128i(_mm_shuffle_epi8(s1, mask), _mm_shuffle_epi8(s0, mask)));
        _mm256_storeu_si256((__m256i *)&out_ptr[i * 4 + 32], _mm256_set_m128i(_mm_shuffle_epi8(s3, mask), _mm_shuffle_epi8(s2, mask)));
        _mm256_storeu_si256((__m256i *)&out_ptr[i * 4 + 64], _mm256_set_m128i(_mm_shuffle_epi8(s5, mask), _mm_shuffle_epi8(s4, mask)));
        _mm256_storeu_si256((__m256i *)&out_ptr[i * 4 + 96], _mm256_set_m128i(_mm_shuffle_epi8(s7, mask), _mm_shuffle_epi8(s6, mask)));
    }

    // Tail: Byte-by-byte conversion for remainder
    for (; i < total_samples; i++) {
        out_ptr[i * 4 + 0] = 0;
        out_ptr[i * 4 + 1] = in_ptr[i * 3 + 0];
        out_ptr[i * 4 + 2] = in_ptr[i * 3 + 1];
        out_ptr[i * 4 + 3] = in_ptr[i * 3 + 2];
    }

    munmap(src, file_size);
    munmap(dst, total_out_size);
    close(fd_in);
    close(fd_out);
    rename(tmp_path, input_path);
    return 0;
}