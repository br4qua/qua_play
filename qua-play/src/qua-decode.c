#include "qua-decode.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>


// TODO WRITE A TEST FOR THIS, I WILL PROFILE FILE
// TODO SILENCE PRINTS
// Parse WAV header directly without using soxi - simplified, only reads bits_per_sample and sample_rate
int parse_wav_header(const char *filepath, int *bits_per_sample, int *sample_rate, int *channels) {
  int fd = open(filepath, O_RDONLY);
  if (fd == -1)
    return -1;

  ssize_t bytes_read;
  uint8_t header[36]; // RIFF header (12) + fmt chunk header (8) + fmt data (16) = 36

  // Read RIFF header + fmt chunk header + fmt data
  bytes_read = read(fd, header, 36);
  if (bytes_read != 36) {
    close(fd);
    return -1;
  }

  // Check RIFF/WAVE header and fmt chunk
  if (strncmp((char *)header, "RIFF", 4) != 0 ||
      strncmp((char *)header + 8, "WAVE", 4) != 0 ||
      strncmp((char *)header + 12, "fmt ", 4) != 0) {
    close(fd);
    return -1;
  }

  // Extract values using memcpy (assumes little-endian system)
  // fmt data starts at offset 20 (12 RIFF + 8 fmt header)
  uint16_t num_channels = 0;
  uint32_t sample_rate_val = 0;
  uint16_t bits_per_sample_val = 0;

  memcpy(&num_channels, header + 22, 2);      // offset 22: channels (fmt data + 2)
  memcpy(&sample_rate_val, header + 24, 4);   // offset 24: sample_rate (fmt data + 4)
  memcpy(&bits_per_sample_val, header + 34, 2); // offset 34: bits_per_sample (fmt data + 14)

  close(fd);

  if (bits_per_sample)
    *bits_per_sample = bits_per_sample_val;
  if (sample_rate)
    *sample_rate = sample_rate_val;
  if (channels)
    *channels = num_channels;

  return 0;
}

void *convert_audio(void *arg) {
  decode_params_t *params = (decode_params_t *)arg;
  
  if (!params || !params->input_path) {
    return NULL;
  }

  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "/dev/shm/raw-%d.wav", getpid());

  // Determine output path: use provided path, or temp file if not specified
  char output_file[PATH_MAX];
  bool use_temp_output = (params->output_path == NULL || params->output_path[0] == '\0');
  
  if (use_temp_output) {
    // No output path specified - use temp file (will be read into buffer if output_buffer provided)
    snprintf(output_file, sizeof(output_file), "/dev/shm/qua-decoded-%d.wav", getpid());
  } else {
    strncpy(output_file, params->output_path, sizeof(output_file) - 1);
    output_file[sizeof(output_file) - 1] = '\0';
  }

  char *ext = strrchr(params->input_path, '.');

  // Optimized native decoder selection with direct execution
  pid_t pid = 0;

  ext++; // Skip the '.' (will crash if ext is NULL, as intended)

  switch (ext[0]) {
  case 'f': // .flac
    if (strcmp(ext, "flac") == 0) {
      if ((pid = vfork()) == 0) {
        execlp("flac", "flac", "-d", params->input_path, "-o", tmp, NULL);
        _exit(1);
      }
      break;
    }
  goto unsupported;
// case 'f': // .flac
//     if (strcmp(ext, "flac") == 0) {
//         if ((pid = vfork()) == 0) {
//             execlp("ffmpeg", "ffmpeg", "-hide_banner", "-loglevel", "error", 
//                    "-i", params->input_path, 
//                    "-acodec", "pcm_s32le",  // Explicitly set the 32-bit encoder
//                    "-f", "wav", "-y", tmp, NULL);
//             _exit(1);
//         }
//         break;
//     }
//     goto unsupported;
  case 'w': // .wv or .wav
  if (strcmp(ext, "wv") == 0) {
    if ((pid = vfork()) == 0) {
      execlp("wvunpack", "wvunpack", params->input_path, "-o", tmp, NULL);
      _exit(1);
    }
    break;
  }
  // TODO use straight copy, check if there are any remaining bug in wav header parser
  if (strcmp(ext, "wav") == 0) {
    if ((pid = vfork()) == 0) {
      execlp("ffmpeg", "ffmpeg", "-v", "quiet", "-i", params->input_path, "-f",
             "wav", tmp, NULL);
      _exit(1);
    }
    break;
  }
  goto unsupported;

  case 'a': // .ape or .aiff or .aif
    if (strcmp(ext, "ape") == 0) {
      if ((pid = vfork()) == 0) {
        execlp("mac", "mac", params->input_path, "-d", tmp, NULL);
        _exit(1);
      }
      break;
    }
    if (strcmp(ext, "aiff") == 0 || strcmp(ext, "aif") == 0) {
      if ((pid = vfork()) == 0) {
        execlp("ffmpeg", "ffmpeg", "-v", "quiet", "-i", params->input_path, "-f",
               "wav", tmp, NULL);
        _exit(1);
      }
      break;
    }
    goto unsupported;

  case 'm': // .mp3 or .m4a
    if (strcmp(ext, "mp3") == 0) {
      if ((pid = vfork()) == 0) {
        execlp("mpg123", "mpg123", "-w", tmp, params->input_path, NULL);
        _exit(1);
      }
      break;
    }
    if (strcmp(ext, "m4a") == 0) {
      if ((pid = vfork()) == 0) {
        execlp("ffmpeg", "ffmpeg", "-v", "quiet", "-i", params->input_path, "-f",
               "wav", tmp, NULL);
        _exit(1);
      }
      break;
    }
    goto unsupported;

  case 'o': // .opus or .ogg
    if (strcmp(ext, "opus") == 0) {
      if ((pid = vfork()) == 0) {
        execlp("opusdec", "opusdec", "--force-wav", params->input_path, tmp,
               NULL);
        _exit(1);
      }
      break;
    }
    if (strcmp(ext, "ogg") == 0) {
      if ((pid = vfork()) == 0) {
        execlp("oggdec", "oggdec", params->input_path, "-o", tmp, NULL);
        _exit(1);
      }
      break;
    }
    goto unsupported;

  default:
  unsupported:
    fprintf(stderr, "Unsupported file format: %s\n", ext);
    return NULL;
  }

  // Wait for external decoder to finish (if used)
  if (pid > 0) {
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
      // External decoder failed
      return NULL;
    }
  }

  // Verify temp file exists before reading header
  struct stat tmp_st;
  if (stat(tmp, &tmp_st) != 0 || !S_ISREG(tmp_st.st_mode)) {
    return NULL;
  }

  // Read audio properties from temp WAV file
  int detected_bd = 32, detected_sr = 96000, ch = 2;
  if (parse_wav_header(tmp, &detected_bd, &detected_sr, &ch) != 0) {
    // Fallback to defaults if header parsing fails (decode doesn't know about config)
    detected_bd = 32;
    detected_sr = 96000;
    ch = 2;
  }

  // Store detected values
  *params->detected_bit_depth = detected_bd;
  *params->detected_sample_rate = detected_sr;
  *params->detected_channels = ch;

  // Decode just writes temp file - post-processing decision is made at top level
  // For now, just copy temp to output (post-processing will be handled in qua-play.c)
  FILE *src = fopen(tmp, "rb");
  FILE *dst = fopen(output_file, "wb");
  if (!src || !dst) {
    if (src) fclose(src);
    if (dst) fclose(dst);
    unlink(tmp);
    return NULL;
  }
  
  char buffer[8192];
  size_t bytes;
  while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
    if (fwrite(buffer, 1, bytes, dst) != bytes) {
      fclose(src);
      fclose(dst);
      unlink(tmp);
      return NULL;
    }
  }
  
  fclose(src);
  fclose(dst);
  unlink(tmp);

  // Verify conversion finished successfully
  struct stat st;
  if (stat(output_file, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0) {
    return NULL;
  }

  // If output_buffer provided, read temp file into memory buffer and delete temp file
  if (use_temp_output && params->output_buffer && params->output_size) {
    FILE *f = fopen(output_file, "rb");
    if (f) {
      *params->output_size = st.st_size;
      *params->output_buffer = malloc(st.st_size);
      if (*params->output_buffer) {
        size_t read = fread(*params->output_buffer, 1, st.st_size, f);
        if (read != st.st_size) {
          free(*params->output_buffer);
          *params->output_buffer = NULL;
          *params->output_size = 0;
        }
      }
      fclose(f);
      unlink(output_file); // Clean up temp file
    }
  }

  return NULL;
}

