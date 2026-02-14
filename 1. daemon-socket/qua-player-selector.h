#ifndef QUA_PLAYER_SELECTOR_H
#define QUA_PLAYER_SELECTOR_H

#include <stddef.h>

#define PGO_SUFFIX	".pgo9994x"

void init_player_paths(void);
int parse_wav_header(const char *filepath, int *bits_per_sample, int *sample_rate, int *channels);
int select_player(const char *wav_file, char *player_out, size_t player_size);

#endif
