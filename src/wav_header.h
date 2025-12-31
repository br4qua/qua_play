#ifndef WAV_HEADER_H
#define WAV_HEADER_H

#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include "config_consts.h"
#include "debug.h"

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


static off_t read_wav_header(int fd, WavHeader *header)
{
  ssize_t bytes_read;
  uint8_t basic_header[12];

  // --- 1. Read Basic RIFF/WAVE Header ---
  bytes_read = read(fd, basic_header, 12);
  CHECK_READ(bytes_read, 12, "Failed to read WAV header");

#ifdef DEBUG
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

#ifdef DEBUG
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

#ifdef DEBUG
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
  return data_offset;
}

#endif // WAV_HEADER_H
