#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// Helper to read 32-bit Little Endian (used inside Vorbis Comments)
uint32_t read_le32(const uint8_t* buf) {
    return buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
}

// Helper to read 24-bit Big Endian (used for FLAC block sizes)
uint32_t read_be24(const uint8_t* buf) {
    return (buf[0] << 16) | (buf[1] << 8) | buf[2];
}

void probe_flac(const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) {
        perror("Error opening file");
        return;
    }

    uint8_t buf[4];
    if (fread(buf, 1, 4, f) != 4) { fclose(f); return; }

    // --- PHASE 1: THE PEELER (Handle ID3 at start) ---
    if (memcmp(buf, "ID3", 3) == 0) {
        fseek(f, 6, SEEK_SET); // Skip to ID3 size field
        uint8_t s[4];
        fread(s, 1, 4, f);
        // Synchsafe integer conversion
        uint32_t id3_size = ((s[0] & 0x7F) << 21) | ((s[1] & 0x7F) << 14) | 
                            ((s[2] & 0x7F) << 7) | (s[3] & 0x7F);
        fseek(f, id3_size + 10, SEEK_SET); // Skip header + body
        fread(buf, 1, 4, f); // Read the actual FLAC magic
    }

    if (memcmp(buf, "fLaC", 4) != 0) {
        printf("Not a valid FLAC file (found %c%c%c%c)\n", buf[0], buf[1], buf[2], buf[3]);
        fclose(f);
        return;
    }

    // --- PHASE 2: THE BLOCK WALKER ---
    int is_last = 0;
    while (!is_last) {
        uint8_t bh[4];
        if (fread(bh, 1, 4, f) != 4) break;

        is_last = (bh[0] & 0x80);
        uint8_t type = (bh[0] & 0x7F);
        uint32_t size = read_be24(&bh[1]);

        if (type == 0) { // STREAMINFO
            uint8_t si[34];
            fread(si, 1, 34, f);
            uint32_t rate = (si[10] << 12) | (si[11] << 4) | (si[12] >> 4);
            uint32_t chan = ((si[12] >> 1) & 0x07) + 1;
            uint32_t bits = (((si[12] & 0x01) << 4) | (si[13] >> 4)) + 1;
            printf("--- Technical ---\nRate: %u Hz | Bits: %u | Channels: %u\n", rate, bits, chan);
        } 
        else if (type == 4) { // VORBIS_COMMENT
            uint8_t* cbuf = malloc(size);
            fread(cbuf, 1, size, f);
            
            uint8_t* ptr = cbuf;
            uint32_t vendor_len = read_le32(ptr);
            ptr += 4 + vendor_len;
            
            uint32_t num_tags = read_le32(ptr);
            ptr += 4;

            printf("\n--- Tags (%u) ---\n", num_tags);
            for (uint32_t i = 0; i < num_tags; i++) {
                uint32_t tag_len = read_le32(ptr);
                ptr += 4;
                printf("  %.*s\n", (int)tag_len, ptr);
                ptr += tag_len;
            }
            free(cbuf);
        } 
        else {
            // Skip other blocks (Padding, Pictures, etc.)
            fseek(f, size, SEEK_CUR);
        }
    }

    fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: ./flac_probe <file.flac>\n");
        return 1;
    }
    probe_flac(argv[1]);
    return 0;
}
