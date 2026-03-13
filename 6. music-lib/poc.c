/* poc.c - music library indexer POC
 * Reads FLAC/WavPack metadata natively, loads into SQLite.
 * No forking, no external tools. */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sqlite3.h>

/* ------------------------------------------------------------------ */
/* FLAC native parser                                                  */
/* FLAC spec: fLaC marker, then metadata blocks.                      */
/* Block header: byte0 = last-flag|type(7bits), bytes1-3 = length     */
/* ------------------------------------------------------------------ */

typedef struct {
	uint32_t sample_rate;
	uint8_t  channels;
	uint8_t  bits_per_sample;
	uint64_t total_samples;
} flac_streaminfo_t;

typedef struct {
	char title[256];
	char artist[256];
	char album[256];
	char album_artist[256];
	char date[64];
	char genre[128];
	int  track_num;
	int  disc_num;
} vorbis_tags_t;

static int flac_read_streaminfo(const uint8_t *d, flac_streaminfo_t *si)
{
	/* STREAMINFO is exactly 34 bytes */
	/* sample_rate: bits 80-99 (from start of block data) */
	si->sample_rate     = ((uint32_t)d[10] << 12) |
	                      ((uint32_t)d[11] << 4)  |
	                      ((uint32_t)d[12] >> 4);
	si->channels        = ((d[12] >> 1) & 0x07) + 1;
	si->bits_per_sample = (((d[12] & 0x01) << 4) | (d[13] >> 4)) + 1;
	si->total_samples   = ((uint64_t)(d[13] & 0x0f) << 32) |
	                      ((uint64_t)d[14] << 24) |
	                      ((uint64_t)d[15] << 16) |
	                      ((uint64_t)d[16] << 8)  |
	                       (uint64_t)d[17];
	return 0;
}

static void vorbis_parse_comment(const char *kv, size_t len, vorbis_tags_t *t)
{
	char key[128] = {0};
	const char *eq = memchr(kv, '=', len);
	if (!eq)
		return;

	size_t klen = eq - kv;
	if (klen >= sizeof(key))
		return;
	memcpy(key, kv, klen);
	key[klen] = '\0';
	/* lowercase key for comparison */
	for (char *p = key; *p; p++)
		if (*p >= 'A' && *p <= 'Z') *p |= 0x20;

	const char *val = eq + 1;
	size_t      vlen = len - klen - 1;
	if (vlen == 0)
		return;

#define COPY_TAG(field, name) \
	if (!strcmp(key, name)) { \
		size_t n = vlen < sizeof(t->field) - 1 ? vlen : sizeof(t->field) - 1; \
		memcpy(t->field, val, n); t->field[n] = '\0'; return; \
	}

	COPY_TAG(title,       "title")
	COPY_TAG(artist,      "artist")
	COPY_TAG(album,       "album")
	COPY_TAG(album_artist,"albumartist")
	COPY_TAG(date,        "date")
	COPY_TAG(genre,       "genre")

	if (!strcmp(key, "tracknumber")) {
		t->track_num = atoi(val);
	} else if (!strcmp(key, "discnumber")) {
		t->disc_num = atoi(val);
	}
#undef COPY_TAG
}

static int parse_flac(const char *path,
                      flac_streaminfo_t *si, vorbis_tags_t *tags)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) return -1;

	/* Read enough to cover several metadata blocks */
	uint8_t buf[65536];
	ssize_t n = read(fd, buf, sizeof(buf));
	close(fd);
	if (n < 8) return -1;

	if (memcmp(buf, "fLaC", 4) != 0) return -1;

	size_t pos = 4;
	int got_si = 0;

	while (pos + 4 <= (size_t)n) {
		uint8_t  hdr   = buf[pos];
		int      last  = (hdr >> 7) & 1;
		int      type  = hdr & 0x7f;
		uint32_t blen  = ((uint32_t)buf[pos+1] << 16) |
		                 ((uint32_t)buf[pos+2] << 8)  |
		                  (uint32_t)buf[pos+3];
		pos += 4;

		if (pos + blen > (size_t)n) break;

		if (type == 0 && blen >= 18) { /* STREAMINFO */
			flac_read_streaminfo(buf + pos, si);
			got_si = 1;
		} else if (type == 4) { /* VORBIS_COMMENT */
			const uint8_t *d = buf + pos;
			size_t off = 0;
			/* vendor string length (LE u32) */
			if (off + 4 > blen) goto next;
			uint32_t vend_len = (uint32_t)d[0] | ((uint32_t)d[1]<<8) |
			                    ((uint32_t)d[2]<<16) | ((uint32_t)d[3]<<24);
			off += 4 + vend_len;
			if (off + 4 > blen) goto next;
			uint32_t count = (uint32_t)d[off] | ((uint32_t)d[off+1]<<8) |
			                 ((uint32_t)d[off+2]<<16) | ((uint32_t)d[off+3]<<24);
			off += 4;
			for (uint32_t i = 0; i < count; i++) {
				if (off + 4 > blen) break;
				uint32_t clen = (uint32_t)d[off] | ((uint32_t)d[off+1]<<8) |
				                ((uint32_t)d[off+2]<<16) | ((uint32_t)d[off+3]<<24);
				off += 4;
				if (off + clen > blen) break;
				vorbis_parse_comment((const char *)(d + off), clen, tags);
				off += clen;
			}
		}
next:
		pos += blen;
		if (last) break;
	}

	return got_si ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* WavPack native parser                                               */
/* Unified: reads stream info + APEv2 tags in one fd pass.            */
/* pread() at known offsets — no seek, no malloc.                     */
/* ------------------------------------------------------------------ */

typedef struct {
	uint32_t sample_rate;
	uint8_t  channels;
	uint8_t  bits_per_sample;
	uint64_t total_samples;
} wv_info_t;

static const uint32_t wv_rates[15] = {
	6000,8000,9600,11025,12000,16000,22050,24000,
	32000,44100,48000,64000,88200,96000,192000
};

static inline int match_key(const char *a, size_t alen, const char *b)
{
	return alen == strlen(b) && strcasecmp(a, b) == 0;
}

/* Parse WavPack header + APEv2 tags from an open fd. Scratch must be >=256 bytes. */
static int parse_wv_fd(int fd, wv_info_t *wi, vorbis_tags_t *tags,
                        uint8_t *scratch)
{
	/* --- stream info from 32-byte block header --- */
	if (pread(fd, scratch, 32, 0) != 32) return -1;
	if (memcmp(scratch, "wvpk", 4) != 0) return -1;

	uint32_t low   = (uint32_t)scratch[12] | ((uint32_t)scratch[13]<<8) |
	                 ((uint32_t)scratch[14]<<16) | ((uint32_t)scratch[15]<<24);
	uint8_t  upper = scratch[11];
	wi->total_samples  = (low == 0xFFFFFFFF) ? 0 :
	                     (uint64_t)low + ((uint64_t)upper << 32) - upper;

	uint8_t f24        = scratch[24];
	wi->bits_per_sample= (uint8_t)(((f24 & 3) + 1) << 3);
	wi->channels       = (uint8_t)(2 - ((f24 >> 2) & 1));
	uint32_t sr_idx    = (scratch[26] >> 7) | ((scratch[27] & 0x07) << 1);
	wi->sample_rate    = sr_idx < 15 ? wv_rates[sr_idx] : 0;

	/* --- APEv2 tags at end of file --- */
	struct stat st;
	if (fstat(fd, &st) < 0 || st.st_size < 32) return 0;

	uint8_t footer[32];
	if (pread(fd, footer, 32, st.st_size - 32) != 32) return 0;
	if (memcmp(footer, "APETAGEX", 8) != 0) return 0;

	uint32_t tag_sz = (uint32_t)footer[12] | ((uint32_t)footer[13]<<8) |
	                  ((uint32_t)footer[14]<<16) | ((uint32_t)footer[15]<<24);
	uint32_t items  = (uint32_t)footer[16] | ((uint32_t)footer[17]<<8) |
	                  ((uint32_t)footer[18]<<16) | ((uint32_t)footer[19]<<24);

	off_t pos = st.st_size - (off_t)tag_sz; /* items start here */

	for (uint32_t i = 0; i < items; i++) {
		ssize_t n = pread(fd, scratch, 256, pos);
		if (n < 10) break;

		uint32_t v_len = (uint32_t)scratch[0] | ((uint32_t)scratch[1]<<8) |
		                 ((uint32_t)scratch[2]<<16) | ((uint32_t)scratch[3]<<24);
		uint32_t flags = (uint32_t)scratch[4] | ((uint32_t)scratch[5]<<8) |
		                 ((uint32_t)scratch[6]<<16) | ((uint32_t)scratch[7]<<24);

		char   *key   = (char *)(scratch + 8);
		size_t  k_len = 0;
		while (k_len < 100 && (8 + k_len < (size_t)n) && key[k_len]) k_len++;

		size_t hdr_sz = 8 + k_len + 1;
		off_t  val_pos = pos + (off_t)hdr_sz;
		pos += (off_t)(hdr_sz + v_len);

		/* skip binary/external items */
		if (((flags >> 1) & 3) != 0 || v_len == 0) continue;

		char *target = NULL;
		char  track_tmp[16] = {0};

		switch (key[0] | 32) {
		case 'a':
			if      (match_key(key, k_len, "Artist"))       target = tags->artist;
			else if (match_key(key, k_len, "Album"))        target = tags->album;
			else if (match_key(key, k_len, "Album Artist") ||
			         match_key(key, k_len, "AlbumArtist"))  target = tags->album_artist;
			break;
		case 't':
			if      (match_key(key, k_len, "Title"))        target = tags->title;
			else if (match_key(key, k_len, "Track"))        target = track_tmp;
			break;
		case 'd':
			if      (match_key(key, k_len, "Date"))         target = tags->date;
			else if (match_key(key, k_len, "Disc"))         target = track_tmp; /* reuse for disc */
			break;
		case 'y':
			if      (match_key(key, k_len, "Year"))         target = tags->date;
			break;
		case 'g':
			if      (match_key(key, k_len, "Genre"))        target = tags->genre;
			break;
		}

		if (target && target[0] == '\0') {
			size_t cpy = v_len < 255 ? v_len : 255;
			if (hdr_sz + cpy <= (size_t)n)
				memcpy(target, scratch + hdr_sz, cpy);
			else
				pread(fd, target, cpy, val_pos);
			target[cpy] = '\0';

			/* convert track/disc string to int */
			if (target == track_tmp) {
				if (match_key(key, k_len, "Track"))
					tags->track_num = atoi(track_tmp);
				else
					tags->disc_num  = atoi(track_tmp);
			}
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* SQLite helpers                                                      */
/* ------------------------------------------------------------------ */

static const char *SCHEMA =
	"CREATE TABLE IF NOT EXISTS tracks ("
	"  id          INTEGER PRIMARY KEY,"
	"  path        TEXT NOT NULL UNIQUE,"
	"  format      TEXT,"
	"  sample_rate INTEGER,"
	"  bit_depth   INTEGER,"
	"  channels    INTEGER,"
	"  duration    REAL,"
	"  title       TEXT,"
	"  artist      TEXT,"
	"  album       TEXT,"
	"  album_artist TEXT,"
	"  track_num   INTEGER,"
	"  disc_num    INTEGER,"
	"  date        TEXT,"
	"  genre       TEXT,"
	"  indexed_at  INTEGER"
	");";

static sqlite3_stmt *g_insert_stmt;

static int db_init(sqlite3 *db)
{
	char *err = NULL;
	int rc = sqlite3_exec(db, SCHEMA, NULL, NULL, &err);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "schema error: %s\n", err);
		sqlite3_free(err);
		return -1;
	}

	const char *sql =
		"INSERT OR REPLACE INTO tracks "
		"(path,format,sample_rate,bit_depth,channels,duration,"
		" title,artist,album,album_artist,track_num,disc_num,date,genre,indexed_at)"
		" VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,strftime('%s','now'));";

	rc = sqlite3_prepare_v2(db, sql, -1, &g_insert_stmt, NULL);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "prepare error: %s\n", sqlite3_errmsg(db));
		return -1;
	}
	return 0;
}

static void db_insert_track(sqlite3 *db, const char *path, const char *fmt,
                             uint32_t sr, uint8_t bd, uint8_t ch,
                             double duration, const vorbis_tags_t *t)
{
	sqlite3_stmt *s = g_insert_stmt;
	sqlite3_reset(s);
	sqlite3_bind_text(s, 1, path, -1, SQLITE_STATIC);
	sqlite3_bind_text(s, 2, fmt,  -1, SQLITE_STATIC);
	sqlite3_bind_int (s, 3, (int)sr);
	sqlite3_bind_int (s, 4, (int)bd);
	sqlite3_bind_int (s, 5, (int)ch);
	sqlite3_bind_double(s, 6, duration);
	sqlite3_bind_text(s, 7,  *t->title        ? t->title        : NULL, -1, SQLITE_STATIC);
	sqlite3_bind_text(s, 8,  *t->artist       ? t->artist       : NULL, -1, SQLITE_STATIC);
	sqlite3_bind_text(s, 9,  *t->album        ? t->album        : NULL, -1, SQLITE_STATIC);
	sqlite3_bind_text(s, 10, *t->album_artist ? t->album_artist : NULL, -1, SQLITE_STATIC);
	sqlite3_bind_int (s, 11, t->track_num);
	sqlite3_bind_int (s, 12, t->disc_num);
	sqlite3_bind_text(s, 13, *t->date         ? t->date         : NULL, -1, SQLITE_STATIC);
	sqlite3_bind_text(s, 14, *t->genre        ? t->genre        : NULL, -1, SQLITE_STATIC);

	if (sqlite3_step(s) != SQLITE_DONE)
		fprintf(stderr, "insert error: %s\n", sqlite3_errmsg(db));
}

/* ------------------------------------------------------------------ */
/* Directory scanner                                                   */
/* ------------------------------------------------------------------ */

static int index_file(sqlite3 *db, const char *path)
{
	size_t plen = strlen(path);
	const char *ext = plen > 5 ? path + plen - 5 : path;

	flac_streaminfo_t si = {0};
	wv_info_t         wi = {0};
	vorbis_tags_t     tags = {0};
	const char       *fmt = NULL;
	uint32_t sr; uint8_t bd, ch;
	double   duration;

	if (strcasecmp(ext + 1, "flac") == 0 || strcasecmp(ext, ".flac") == 0) {
		if (parse_flac(path, &si, &tags) != 0) return -1;
		fmt      = "flac";
		sr       = si.sample_rate;
		bd       = si.bits_per_sample;
		ch       = si.channels;
		duration = sr > 0 ? (double)si.total_samples / sr : 0.0;

	} else if (plen > 3 && strcasecmp(path + plen - 3, ".wv") == 0) {

		uint8_t scratch[256];
		int fd = open(path, O_RDONLY);
		if (fd < 0) return -1;
		int r = parse_wv_fd(fd, &wi, &tags, scratch);
		close(fd);
		if (r != 0) return -1;

		fmt      = "wavpack";
		sr       = wi.sample_rate;
		bd       = wi.bits_per_sample;
		ch       = wi.channels;
		duration = sr > 0 ? (double)wi.total_samples / sr : 0.0;

	} else {
		return 0; /* skip */
	}

	db_insert_track(db, path, fmt, sr, bd, ch, duration, &tags);
	printf("  [%s] %s | %s - %s | %uHz %ubit %uch %.1fs\n",
	       fmt, path,
	       *tags.artist ? tags.artist : "?",
	       *tags.title  ? tags.title  : "?",
	       sr, bd, ch, duration);
	return 0;
}

static void scan_dir(sqlite3 *db, const char *dirpath)
{
	DIR *d = opendir(dirpath);
	if (!d) { perror(dirpath); return; }

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (ent->d_name[0] == '.') continue;

		char path[4096];
		snprintf(path, sizeof(path), "%s/%s", dirpath, ent->d_name);

		struct stat st;
		if (stat(path, &st) != 0) continue;

		if (S_ISDIR(st.st_mode)) {
			scan_dir(db, path);
		} else if (S_ISREG(st.st_mode)) {
			index_file(db, path);
		}
	}
	closedir(d);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
	const char *dir    = argc > 1 ? argv[1] : ".";
	const char *dbpath = argc > 2 ? argv[2] : "music.db";

	sqlite3 *db;
	if (sqlite3_open(dbpath, &db) != SQLITE_OK) {
		fprintf(stderr, "cannot open db: %s\n", sqlite3_errmsg(db));
		return 1;
	}

	sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
	sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
	sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);

	if (db_init(db) != 0) return 1;

	printf("Scanning: %s\n", dir);
	scan_dir(db, dir);

	sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
	sqlite3_finalize(g_insert_stmt);
	sqlite3_close(db);

	printf("Done. DB: %s\n", dbpath);
	return 0;
}
