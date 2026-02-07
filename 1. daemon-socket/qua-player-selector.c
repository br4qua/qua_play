#include "qua-player-selector.h"

#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int parse_wav_header(const char *filepath, int *bits_per_sample, int *sample_rate, int *channels) {
    int fd = open(filepath, O_RDONLY);
    if (fd == -1) return -1;

    uint8_t header[36];
    ssize_t bytes_read = read(fd, header, 36);
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
    if (parse_wav_header(wav_file, &bd, &sr, &ch) != 0) {
        return -1;
    }

    char s_bd[8], s_sr[16];
    snprintf(s_bd, sizeof(s_bd), "%d", bd);
    snprintf(s_sr, sizeof(s_sr), "%d", sr);

    player_out[0] = '\0';

    // Try specialized PGO binary first
    char p_bin[128];
    snprintf(p_bin, sizeof(p_bin), "qua-player-%s-%s" PGO_SUFFIX, s_bd, s_sr);
    char q_cmd[PATH_MAX + 64];
    snprintf(q_cmd, sizeof(q_cmd), "which %s 2>/dev/null", p_bin);
    FILE *pwh = popen(q_cmd, "r");
    if (pwh) {
        if (fgets(player_out, player_size, pwh))
            player_out[strcspn(player_out, "\n")] = 0;
        pclose(pwh);
    }

    // Fallback to non-PGO binary
    if (player_out[0] == '\0') {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "which qua-player-%s-%s 2>/dev/null", s_bd, s_sr);
        pwh = popen(cmd, "r");
        if (pwh) {
            if (fgets(player_out, player_size, pwh))
                player_out[strcspn(player_out, "\n")] = 0;
            pclose(pwh);
        }
    }

    if (player_out[0] == '\0') {
        return -1;
    }

    return 0;
}
