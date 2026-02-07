#ifndef QUA_PLAYER_SELECTOR_H
#define QUA_PLAYER_SELECTOR_H

#include <stddef.h>

#define PGO_SUFFIX ".pgo9992"

// Parse WAV header and extract audio properties
// Returns 0 on success, -1 on error
int parse_wav_header(const char *filepath, int *bits_per_sample, int *sample_rate, int *channels);

// Select appropriate player binary based on WAV file specs
// Returns 0 on success, -1 on error
int select_player(const char *wav_file, char *player_out, size_t player_size);

#endif
