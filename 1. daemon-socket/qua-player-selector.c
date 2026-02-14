#include "qua-player-selector.h"

#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct {
	int bitdepth;
	int samplerate;
	char path[PATH_MAX];
} players[] = {
	{16, 44100},  {16, 48000},  {16, 96000},
	{16, 88200},  {16, 176400}, {16, 192000},
	{32, 44100},  {32, 48000},  {32, 96000},
	{32, 88200},  {32, 176400}, {32, 192000},
};

static const int player_count = sizeof(players) / sizeof(players[0]);

static int find_in_path(const char *path_env, const char *bin,
			char *out, size_t out_size) {
	const char *p = path_env;
	while (*p) {
		const char *colon = strchr(p, ':');
		int len = colon ? (int)(colon - p) : (int)strlen(p);
		char full[PATH_MAX];
		snprintf(full, sizeof(full), "%.*s/%s", len, p, bin);

		if (access(full, X_OK) == 0) {
			if (out)
				snprintf(out, out_size, "%s", full);
			return 1;
		}
		if (colon) p = colon + 1;
		else break;
	}
	return 0;
}

void init_player_paths(void) {
	const char *path_env = getenv("PATH");
	if (!path_env) return;

	for (int i = 0; i < player_count; i++) {
		char pgo_name[64];
		snprintf(pgo_name, sizeof(pgo_name), "qua-player-%d-%d" PGO_SUFFIX,
			 players[i].bitdepth, players[i].samplerate);

		if (find_in_path(path_env, pgo_name,
				 players[i].path, sizeof(players[i].path))) {
			fprintf(stderr, "[init] %d-%d -> %s\n",
				players[i].bitdepth, players[i].samplerate,
				players[i].path);
		} else {
			char name[64];
			snprintf(name, sizeof(name), "qua-player-%d-%d",
				 players[i].bitdepth, players[i].samplerate);
			if (find_in_path(path_env, name,
					 players[i].path, sizeof(players[i].path)))
				fprintf(stderr, "[init] %d-%d -> %s\n",
					players[i].bitdepth,
					players[i].samplerate,
					players[i].path);
			else
				fprintf(stderr, "[init] %d-%d -> NOT FOUND\n",
					players[i].bitdepth,
					players[i].samplerate);
		}
	}
}

int parse_wav_header(const char *filepath, int *bits_per_sample, int *sample_rate, int *channels) {
	int fd = open(filepath, O_RDONLY);
	if (fd == -1) return -1;

	uint8_t header[36];
	ssize_t bytes_read = read(fd, header, 36);
	if (bytes_read != 36) {
		close(fd);
		return -1;
	}

	if (strncmp((char *)header, "RIFF", 4) != 0 ||
	    strncmp((char *)header + 8, "WAVE", 4) != 0 ||
	    strncmp((char *)header + 12, "fmt ", 4) != 0) {
		close(fd);
		return -1;
	}

	uint16_t num_channels = 0;
	uint32_t sample_rate_val = 0;
	uint16_t bits_per_sample_val = 0;

	memcpy(&num_channels, header + 22, 2);
	memcpy(&sample_rate_val, header + 24, 4);
	memcpy(&bits_per_sample_val, header + 34, 2);

	close(fd);

	if (bits_per_sample) *bits_per_sample = bits_per_sample_val;
	if (sample_rate) *sample_rate = sample_rate_val;
	if (channels) *channels = num_channels;

	return 0;
}

int select_player(const char *wav_file, char *player_out, size_t player_size) {
	int bd, sr, ch;
	if (parse_wav_header(wav_file, &bd, &sr, &ch) != 0)
		return -1;

	for (int i = 0; i < player_count; i++) {
		if (players[i].bitdepth == bd &&
		    players[i].samplerate == sr) {
			if (players[i].path[0] == '\0')
				return -1;
			snprintf(player_out, player_size, "%s",
				 players[i].path);
			return 0;
		}
	}

	return -1;
}
