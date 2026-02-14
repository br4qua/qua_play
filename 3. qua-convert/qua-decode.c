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


// Parse WAV header directly without using soxi
int parse_wav_header(const char *filepath, wav_info_t *info) {
  if (!info)
    return -1;

  int fd = open(filepath, O_RDONLY);
  if (fd == -1)
    return -1;

  uint8_t header[36]; // RIFF header (12) + fmt chunk header (8) + fmt data (16) = 36

  if (read(fd, header, 36) != 36) {
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

  // Extract values (assumes little-endian system)
  // fmt data starts at offset 20 (12 RIFF + 8 fmt header)
  uint16_t audio_format = 0;
  uint16_t num_channels = 0;
  uint32_t sample_rate_val = 0;
  uint16_t bits_per_sample_val = 0;

  memcpy(&audio_format, header + 20, 2);        // offset 20: audio format (1=PCM, 3=IEEE float)
  memcpy(&num_channels, header + 22, 2);        // offset 22: channels
  memcpy(&sample_rate_val, header + 24, 4);     // offset 24: sample_rate
  memcpy(&bits_per_sample_val, header + 34, 2); // offset 34: bits_per_sample

  close(fd);

  info->bit_depth = bits_per_sample_val;
  info->sample_rate = sample_rate_val;
  info->channels = num_channels;
  info->is_float = (audio_format == 3);

  return 0;
}

void *convert_audio(void *arg) {
  decode_params_t *params = (decode_params_t *)arg;

  if (!params || !params->input_path) {
    return NULL;
  }

  // Determine output path: use provided path, or temp file for in-memory mode
  char output_file[PATH_MAX];
  bool use_temp_output = (params->output_path == NULL || params->output_path[0] == '\0');

  if (use_temp_output) {
    snprintf(output_file, sizeof(output_file), "/dev/shm/qua-decoded-%d.wav", getpid());
  } else {
    strncpy(output_file, params->output_path, sizeof(output_file) - 1);
    output_file[sizeof(output_file) - 1] = '\0';
  }

  char *ext = strrchr(params->input_path, '.');
  pid_t pid = 0;

  ext++; // Skip the '.'

  // Decode directly to output_file
  switch (ext[0]) {
  case 'f': // .flac
    if (strcmp(ext, "flac") == 0) {
      if ((pid = vfork()) == 0) {
        execlp("flac", "flac", "-d", "--decode-through-errors", "-f", params->input_path, "-o", output_file, NULL);
        _exit(1);
      }
      break;
    }
    goto unsupported;

  case 'w': // .wv or .wav
    if (strcmp(ext, "wv") == 0) {
      if ((pid = vfork()) == 0) {
        execlp("wvunpack", "wvunpack", "-y", params->input_path, "-o", output_file, NULL);
        _exit(1);
      }
      break;
    }
    if (strcmp(ext, "wav") == 0) {
      if ((pid = vfork()) == 0) {
        execlp("ffmpeg", "ffmpeg", "-v", "quiet", "-y", "-i", params->input_path,
               "-f", "wav", output_file, NULL);
        _exit(1);
      }
      break;
    }
    goto unsupported;

  case 'a': // .ape or .aiff or .aif
    if (strcmp(ext, "ape") == 0) {
      if ((pid = vfork()) == 0) {
        execlp("mac", "mac", params->input_path, output_file, "-d", NULL);
        _exit(1);
      }
      break;
    }
    if (strcmp(ext, "aiff") == 0 || strcmp(ext, "aif") == 0) {
      if ((pid = vfork()) == 0) {
        execlp("ffmpeg", "ffmpeg", "-v", "quiet", "-y", "-i", params->input_path,
               "-f", "wav", output_file, NULL);
        _exit(1);
      }
      break;
    }
    goto unsupported;

  case 'm': // .mp3 or .m4a
    if (strcmp(ext, "mp3") == 0) {
      if ((pid = vfork()) == 0) {
        execlp("mpg123", "mpg123", "-w", output_file, params->input_path, NULL);
        _exit(1);
      }
      break;
    }
    if (strcmp(ext, "m4a") == 0) {
      if ((pid = vfork()) == 0) {
        execlp("ffmpeg", "ffmpeg", "-v", "quiet", "-y", "-i", params->input_path,
               "-f", "wav", output_file, NULL);
        _exit(1);
      }
      break;
    }
    goto unsupported;

  case 'o': // .opus or .ogg
    if (strcmp(ext, "opus") == 0) {
      if ((pid = vfork()) == 0) {
        execlp("opusdec", "opusdec", "--force-wav", params->input_path, output_file, NULL);
        _exit(1);
      }
      break;
    }
    if (strcmp(ext, "ogg") == 0) {
      if ((pid = vfork()) == 0) {
        execlp("oggdec", "oggdec", params->input_path, "-o", output_file, NULL);
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

  // Wait for decoder to finish
  if (pid > 0) {
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
      return NULL;
    }
  }

  // Verify output file exists
  struct stat st;
  if (stat(output_file, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0) {
    return NULL;
  }

  // Read audio properties
  wav_info_t info = {.bit_depth = 32, .sample_rate = 96000, .channels = 2, .is_float = false};
  if (parse_wav_header(output_file, &info) != 0) {
    info.bit_depth = 32;
    info.sample_rate = 96000;
    info.channels = 2;
    info.is_float = false;
  }
  *params->detected = info;

  // If in-memory mode, read file into buffer and clean up
  if (use_temp_output && params->output_buffer && params->output_size) {
    FILE *f = fopen(output_file, "rb");
    if (f) {
      *params->output_size = st.st_size;
      *params->output_buffer = malloc(st.st_size);
      if (*params->output_buffer) {
        size_t bytes_read = fread(*params->output_buffer, 1, st.st_size, f);
        if (bytes_read != st.st_size) {
          free(*params->output_buffer);
          *params->output_buffer = NULL;
          *params->output_size = 0;
        }
      }
      fclose(f);
      unlink(output_file);
    }
  }

  return NULL;
}

