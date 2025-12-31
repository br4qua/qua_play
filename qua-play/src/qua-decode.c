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
#include <FLAC/stream_decoder.h>
#include <wavpack/wavpack.h>

// FLAC decoder context for in-memory decoding
typedef struct {
  int32_t *samples;
  size_t sample_count;
  size_t capacity;
  unsigned channels;
  unsigned sample_rate;
  unsigned bits_per_sample;
  bool error;
} flac_decoder_data_t;

// WavPack decoder context for in-memory decoding
typedef struct {
  int32_t *samples;
  size_t sample_count;
  size_t capacity;
  unsigned channels;
  unsigned sample_rate;
  unsigned bits_per_sample;
  bool error;
} wavpack_decoder_data_t;

static FLAC__StreamDecoderWriteStatus flac_write_callback(
    const FLAC__StreamDecoder *decoder,
    const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[],
    void *client_data) {
  flac_decoder_data_t *data = (flac_decoder_data_t *)client_data;
  
  if (data->error)
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  
  size_t samples_needed = frame->header.blocksize * frame->header.channels;
  size_t new_capacity = data->sample_count + samples_needed;
  
  if (new_capacity > data->capacity) {
    size_t new_size = new_capacity * sizeof(int32_t);
    int32_t *new_samples = realloc(data->samples, new_size);
    if (!new_samples) {
      data->error = true;
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    data->samples = new_samples;
    data->capacity = new_capacity;
  }
  
  // Interleave samples from buffer into our flat array
  for (size_t i = 0; i < frame->header.blocksize; i++) {
    for (unsigned ch = 0; ch < frame->header.channels; ch++) {
      data->samples[data->sample_count++] = buffer[ch][i];
    }
  }
  
  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void flac_metadata_callback(
    const FLAC__StreamDecoder *decoder,
    const FLAC__StreamMetadata *metadata,
    void *client_data) {
  flac_decoder_data_t *data = (flac_decoder_data_t *)client_data;
  
  if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
    data->channels = metadata->data.stream_info.channels;
    data->sample_rate = metadata->data.stream_info.sample_rate;
    data->bits_per_sample = metadata->data.stream_info.bits_per_sample;
  }
}

static void flac_error_callback(
    const FLAC__StreamDecoder *decoder,
    FLAC__StreamDecoderErrorStatus status,
    void *client_data) {
  flac_decoder_data_t *data = (flac_decoder_data_t *)client_data;
  (void)decoder;
  (void)status;
  data->error = true;
}

// Decode FLAC file to memory using libflac
static bool decode_flac_to_memory(const char *input_path, flac_decoder_data_t *data) {
  FLAC__StreamDecoder *decoder = FLAC__stream_decoder_new();
  if (!decoder)
    return false;
  
  data->samples = NULL;
  data->sample_count = 0;
  data->capacity = 0;
  data->channels = 0;
  data->sample_rate = 0;
  data->bits_per_sample = 0;
  data->error = false;
  
  FLAC__StreamDecoderInitStatus init_status = FLAC__stream_decoder_init_file(
      decoder, input_path, flac_write_callback, flac_metadata_callback,
      flac_error_callback, data);
  
  if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
    FLAC__stream_decoder_delete(decoder);
    return false;
  }
  
  if (!FLAC__stream_decoder_process_until_end_of_stream(decoder)) {
    FLAC__stream_decoder_delete(decoder);
    if (data->samples)
      free(data->samples);
    return false;
  }
  
  bool ok = FLAC__stream_decoder_get_state(decoder) == FLAC__STREAM_DECODER_END_OF_STREAM && !data->error;
  FLAC__stream_decoder_delete(decoder);
  
  if (!ok && data->samples) {
    free(data->samples);
    data->samples = NULL;
  }
  
  return ok;
}

// Decode WavPack file to memory using libwavpack
// WavPack returns samples already interleaved, so we can use them directly
static bool decode_wavpack_to_memory(const char *input_path, wavpack_decoder_data_t *data) {
  char error[80];
  WavpackContext *wpc = WavpackOpenFileInput(input_path, error, OPEN_TAGS | OPEN_WVC | OPEN_DSD_AS_PCM, 0);
  if (!wpc)
    return false;
  
  data->samples = NULL;
  data->sample_count = 0;
  data->capacity = 0;
  data->channels = WavpackGetNumChannels(wpc);
  data->sample_rate = WavpackGetSampleRate(wpc);
  data->bits_per_sample = WavpackGetBitsPerSample(wpc);
  data->error = false;
  
  if (data->channels == 0 || data->sample_rate == 0) {
    WavpackCloseFile(wpc);
    return false;
  }
  
  uint32_t total_samples = WavpackGetNumSamples(wpc);
  if (total_samples == (uint32_t)-1) {
    // Unknown sample count, decode in chunks
    uint32_t chunk_samples = 4096;
    int32_t *chunk_buffer = malloc(chunk_samples * data->channels * sizeof(int32_t));
    if (!chunk_buffer) {
      WavpackCloseFile(wpc);
      return false;
    }
    
    uint32_t samples_unpacked;
    while ((samples_unpacked = WavpackUnpackSamples(wpc, chunk_buffer, chunk_samples)) > 0) {
      size_t samples_needed = samples_unpacked * data->channels;
      size_t new_capacity = data->sample_count + samples_needed;
      
      if (new_capacity > data->capacity) {
        size_t new_size = new_capacity * sizeof(int32_t);
        int32_t *new_samples = realloc(data->samples, new_size);
        if (!new_samples) {
          data->error = true;
          break;
        }
        data->samples = new_samples;
        data->capacity = new_capacity;
      }
      
      // Copy chunk directly (samples are already interleaved from WavPack)
      memcpy(data->samples + data->sample_count, chunk_buffer, samples_needed * sizeof(int32_t));
      data->sample_count += samples_needed;
    }
    
    free(chunk_buffer);
  } else {
    // Known sample count, allocate once and decode directly
    size_t total_samples_needed = total_samples * data->channels;
    data->samples = malloc(total_samples_needed * sizeof(int32_t));
    if (!data->samples) {
      WavpackCloseFile(wpc);
      return false;
    }
    
    uint32_t samples_unpacked = WavpackUnpackSamples(wpc, data->samples, total_samples);
    if (samples_unpacked != total_samples) {
      free(data->samples);
      data->samples = NULL;
      WavpackCloseFile(wpc);
      return false;
    }
    
    data->sample_count = total_samples_needed;
    data->capacity = total_samples_needed;
  }
  
  bool ok = !data->error && data->samples != NULL && data->sample_count > 0;
  WavpackCloseFile(wpc);
  
  if (!ok && data->samples) {
    free(data->samples);
    data->samples = NULL;
  }
  
  return ok;
}

// Helper function to write decoded samples to WAV file
static bool write_samples_to_wav(const char *output_path, int32_t *samples, size_t sample_count,
                                  unsigned channels, unsigned sample_rate, unsigned bits_per_sample) {
  FILE *wav_file = fopen(output_path, "wb");
  if (!wav_file)
    return false;
  
  unsigned bytes_per_sample = bits_per_sample / 8;
  unsigned samples_per_channel = sample_count / channels;
  unsigned data_size = samples_per_channel * channels * bytes_per_sample;
  unsigned file_size = 36 + data_size;
  
  // Helper to write little-endian values
  #define WRITE_LE16(f, v) do { uint16_t _v = (v); fwrite(&_v, 2, 1, f); } while(0)
  #define WRITE_LE32(f, v) do { uint32_t _v = (v); fwrite(&_v, 4, 1, f); } while(0)
  
  fwrite("RIFF", 1, 4, wav_file);
  WRITE_LE32(wav_file, file_size);
  fwrite("WAVE", 1, 4, wav_file);
  fwrite("fmt ", 1, 4, wav_file);
  WRITE_LE32(wav_file, 16); // fmt chunk size
  WRITE_LE16(wav_file, 1);  // audio format (PCM)
  WRITE_LE16(wav_file, channels);
  WRITE_LE32(wav_file, sample_rate);
  WRITE_LE32(wav_file, sample_rate * channels * bytes_per_sample); // byte rate
  WRITE_LE16(wav_file, channels * bytes_per_sample); // block align
  WRITE_LE16(wav_file, bits_per_sample);
  fwrite("data", 1, 4, wav_file);
  WRITE_LE32(wav_file, data_size);
  
  // Write sample data (convert int32_t to appropriate bit depth, little-endian)
  if (bits_per_sample == 16) {
    for (size_t i = 0; i < sample_count; i++) {
      int16_t sample = (int16_t)samples[i];
      WRITE_LE16(wav_file, sample);
    }
  } else if (bits_per_sample == 24) {
    for (size_t i = 0; i < sample_count; i++) {
      int32_t sample = samples[i];
      uint8_t bytes[3];
      bytes[0] = sample & 0xFF;
      bytes[1] = (sample >> 8) & 0xFF;
      bytes[2] = (sample >> 16) & 0xFF;
      fwrite(bytes, 3, 1, wav_file);
    }
  } else { // 32-bit
    for (size_t i = 0; i < sample_count; i++) {
      WRITE_LE32(wav_file, samples[i]);
    }
  }
  
  #undef WRITE_LE16
  #undef WRITE_LE32
  
  fclose(wav_file);
  return true;
}

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
      // Use libflac for in-memory decoding
      flac_decoder_data_t flac_data;
      if (!decode_flac_to_memory(params->input_path, &flac_data)) {
        return NULL;
      }
      
      // Write decoded FLAC data to temp WAV file for sox processing
      unsigned sample_rate = flac_data.sample_rate;
      unsigned channels = flac_data.channels;
      unsigned bits_per_sample = flac_data.bits_per_sample;
      
      if (!write_samples_to_wav(tmp, flac_data.samples, flac_data.sample_count,
                                channels, sample_rate, bits_per_sample)) {
        free(flac_data.samples);
        return NULL;
      }
      
      free(flac_data.samples);
      
      // FLAC decoded to temp file, continue to sox processing
      break;
    }
    goto unsupported;

  case 'w': // .wv or .wav
    if (strcmp(ext, "wv") == 0) {
      // Use libwavpack for in-memory decoding
      wavpack_decoder_data_t wavpack_data;
      if (!decode_wavpack_to_memory(params->input_path, &wavpack_data)) {
        return NULL;
      }
      
      // Write decoded WavPack data to temp WAV file for sox processing
      unsigned sample_rate = wavpack_data.sample_rate;
      unsigned channels = wavpack_data.channels;
      unsigned bits_per_sample = wavpack_data.bits_per_sample;
      
      if (!write_samples_to_wav(tmp, wavpack_data.samples, wavpack_data.sample_count,
                                 channels, sample_rate, bits_per_sample)) {
        free(wavpack_data.samples);
        return NULL;
      }
      
      free(wavpack_data.samples);
      
      // WavPack decoded to temp file, continue to sox processing
      break;
    }
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
