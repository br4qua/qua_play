/* poc.c - music library indexer
 * Reads FLAC/WavPack metadata natively, loads into SQLite.
 * No forking, no external tools.
 *
 * Usage:
 *   qua-music-lib <dir> [db]     scan and index directory
 *   qua-music-lib -a <dir> [db]  same (add/update mode)
 *   qua-music-lib -c [db]        cleanup stale entries
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <pthread.h>
#include <sqlite3.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/* ------------------------------------------------------------------ */
/* Shared types                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
	char title[256];
	char artist[256];
	char album[256];
	char album_artist[256];
	char date[64];
	char genre[128];
	int  track_num;
	int  disc_num;
} tags_t;

typedef struct {
	uint32_t sample_rate;
	uint8_t  channels;
	uint8_t  bits_per_sample;
	uint64_t total_samples;
} stream_info_t;

/* ------------------------------------------------------------------ */
/* FLAC native parser                                                  */
/* FLAC spec: fLaC marker, then metadata blocks.                      */
/* Block header: byte0 = last-flag|type(7bits), bytes1-3 = length     */
/* ------------------------------------------------------------------ */

static int flac_read_streaminfo(const uint8_t *d, stream_info_t *si)
{
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

static void flac_parse_vorbis_block(const uint8_t *d, uint32_t blen,
                                     tags_t *tags)
{
	size_t off = 0;

	/* vendor string length (LE u32) + skip vendor */
	if (off + 4 > blen) return;
	uint32_t vend_len = d[0] | (d[1] << 8) | (d[2] << 16) | (d[3] << 24);
	off += 4 + vend_len;

	/* comment count */
	if (off + 4 > blen) return;
	uint32_t count = d[off] | (d[off+1] << 8) |
	                 (d[off+2] << 16) | (d[off+3] << 24);
	off += 4;

	for (uint32_t i = 0; i < count; i++) {
		if (off + 4 > blen) break;
		uint32_t clen = d[off] | (d[off+1] << 8) |
		                (d[off+2] << 16) | (d[off+3] << 24);
		off += 4;
		if (off + clen > blen) break;

		const char *tag = (const char *)(d + off);
		const char *sep = memchr(tag, '=', clen);
		off += clen;
		if (!sep) continue;

		size_t key_len = sep - tag;
		const char *val = sep + 1;
		size_t val_len = clen - key_len - 1;
		if (val_len == 0) continue;

		char *target = NULL;
		size_t max_copy = 0;

		switch (tag[0] | 32) {
		case 't':
			if (key_len == 5 &&
			    strncasecmp(tag, "TITLE", 5) == 0) {
				target = tags->title;
				max_copy = sizeof(tags->title) - 1;
			} else if (key_len == 11 &&
			           strncasecmp(tag, "TRACKNUMBER", 11) == 0) {
				tags->track_num = atoi(val);
			}
			break;
		case 'a':
			if (key_len == 6 &&
			    strncasecmp(tag, "ARTIST", 6) == 0) {
				target = tags->artist;
				max_copy = sizeof(tags->artist) - 1;
			} else if (key_len == 5 &&
			           strncasecmp(tag, "ALBUM", 5) == 0) {
				target = tags->album;
				max_copy = sizeof(tags->album) - 1;
			} else if (key_len == 11 &&
			           strncasecmp(tag, "ALBUMARTIST", 11) == 0) {
				target = tags->album_artist;
				max_copy = sizeof(tags->album_artist) - 1;
			}
			break;
		case 'd':
			if (key_len == 4 &&
			    strncasecmp(tag, "DATE", 4) == 0) {
				target = tags->date;
				max_copy = sizeof(tags->date) - 1;
			} else if (key_len == 10 &&
			           strncasecmp(tag, "DISCNUMBER", 10) == 0) {
				tags->disc_num = atoi(val);
			}
			break;
		case 'g':
			if (key_len == 5 &&
			    strncasecmp(tag, "GENRE", 5) == 0) {
				target = tags->genre;
				max_copy = sizeof(tags->genre) - 1;
			}
			break;
		}

		if (target && target[0] == '\0') {
			size_t cpy = val_len < max_copy ? val_len : max_copy;
			memcpy(target, val, cpy);
			target[cpy] = '\0';
		}
	}
}

static int parse_flac(const char *path,
                      stream_info_t *si, tags_t *tags)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) return -1;

	uint8_t buf[65536];
	ssize_t n = read(fd, buf, sizeof(buf));
	if (n < 8 || memcmp(buf, "fLaC", 4) != 0) {
		close(fd);
		return -1;
	}

	off_t pos = 4;
	int got_si = 0;

	for (;;) {
		/* Read block header — from buffer or pread */
		uint8_t bh[4];
		if (pos + 4 <= n) {
			memcpy(bh, buf + pos, 4);
		} else if (pread(fd, bh, 4, pos) != 4) {
			break;
		}

		int      last = (bh[0] >> 7) & 1;
		int      type = bh[0] & 0x7f;
		uint32_t blen = ((uint32_t)bh[1] << 16) |
		                ((uint32_t)bh[2] << 8)  |
		                 (uint32_t)bh[3];
		pos += 4;

		if (type == 0 && blen >= 18) {
			/* STREAMINFO — always small, try buffer first */
			if (pos + 18 <= n) {
				flac_read_streaminfo(buf + pos, si);
			} else {
				uint8_t si_buf[34];
				if (pread(fd, si_buf, 18, pos) == 18)
					flac_read_streaminfo(si_buf, si);
			}
			got_si = 1;
		} else if (type == 4 && blen > 8) {
			/* VORBIS_COMMENT — read from buffer or pread */
			const uint8_t *d;
			uint8_t vc_stack[8192];
			uint8_t *vc_heap = NULL;

			if (pos + blen <= (off_t)n) {
				d = buf + pos;
			} else {
				uint8_t *vc = vc_stack;
				if (blen > sizeof(vc_stack)) {
					vc_heap = malloc(blen);
					if (!vc_heap) goto skip;
					vc = vc_heap;
				}
				if (pread(fd, vc, blen, pos) != (ssize_t)blen) {
					free(vc_heap);
					goto skip;
				}
				d = vc;
			}

			flac_parse_vorbis_block(d, blen, tags);
			free(vc_heap);
		}
skip:
		pos += blen;
		if (last) break;
	}

	close(fd);
	return got_si ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* WavPack native parser                                               */
/* Reads stream info + APEv2 tags in one fd pass.                     */
/* pread() at known offsets — no seek, no malloc.                     */
/* ------------------------------------------------------------------ */

static const uint32_t wv_rates[15] = {
	6000,8000,9600,11025,12000,16000,22050,24000,
	32000,44100,48000,64000,88200,96000,192000
};

static inline int match_key(const char *a, size_t alen, const char *b)
{
	return alen == strlen(b) && strcasecmp(a, b) == 0;
}

static int parse_wv_fd(int fd, stream_info_t *si, tags_t *tags,
                        uint8_t *scratch)
{
	if (pread(fd, scratch, 32, 0) != 32) return -1;
	if (memcmp(scratch, "wvpk", 4) != 0) return -1;

	uint32_t low   = (uint32_t)scratch[12] | ((uint32_t)scratch[13]<<8) |
	                 ((uint32_t)scratch[14]<<16) | ((uint32_t)scratch[15]<<24);
	uint8_t  upper = scratch[11];
	si->total_samples  = (low == 0xFFFFFFFF) ? 0 :
	                     (uint64_t)low + ((uint64_t)upper << 32) - upper;

	uint8_t f24        = scratch[24];
	si->bits_per_sample= (uint8_t)(((f24 & 3) + 1) << 3);
	si->channels       = (uint8_t)(2 - ((f24 >> 2) & 1));
	uint32_t sr_idx    = (scratch[26] >> 7) | ((scratch[27] & 0x07) << 1);
	si->sample_rate    = sr_idx < 15 ? wv_rates[sr_idx] : 0;

	struct stat st;
	if (fstat(fd, &st) < 0 || st.st_size < 32) return 0;

	uint8_t footer[32];
	if (pread(fd, footer, 32, st.st_size - 32) != 32) return 0;
	if (memcmp(footer, "APETAGEX", 8) != 0) return 0;

	uint32_t tag_sz = (uint32_t)footer[12] | ((uint32_t)footer[13]<<8) |
	                  ((uint32_t)footer[14]<<16) | ((uint32_t)footer[15]<<24);
	uint32_t items  = (uint32_t)footer[16] | ((uint32_t)footer[17]<<8) |
	                  ((uint32_t)footer[18]<<16) | ((uint32_t)footer[19]<<24);

	off_t pos = st.st_size - (off_t)tag_sz;

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
			else if (match_key(key, k_len, "Disc"))         target = track_tmp;
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
/* Hashmap: (dev, ino) -> {mtime, rowid, path}                        */
/* Open-addressing, power-of-2 capacity, linear probing.              */
/* ------------------------------------------------------------------ */

typedef struct {
	uint64_t dev;
	uint64_t ino;
	int64_t  mtime;
	int64_t  rowid;
	char    *path;
	uint8_t  occupied;
	uint8_t  visited;
} hmap_slot_t;

typedef struct {
	hmap_slot_t *slots;
	uint32_t     cap;
	uint32_t     count;
} hmap_t;

static inline uint64_t hmap_hash(uint64_t dev, uint64_t ino)
{
	uint64_t h = 14695981039346656037ULL;
	h ^= dev; h *= 1099511628211ULL;
	h ^= ino; h *= 1099511628211ULL;
	return h;
}

static uint32_t next_pow2(uint32_t n)
{
	n--;
	n |= n >> 1;  n |= n >> 2;
	n |= n >> 4;  n |= n >> 8;
	n |= n >> 16;
	return n + 1;
}

static int hmap_init(hmap_t *m, uint32_t expected)
{
	uint32_t min = expected < 32 ? 64 : expected * 2;
	m->cap   = next_pow2(min);
	m->count = 0;
	m->slots = calloc(m->cap, sizeof(hmap_slot_t));
	return m->slots ? 0 : -1;
}

static void hmap_destroy(hmap_t *m)
{
	for (uint32_t i = 0; i < m->cap; i++)
		free(m->slots[i].path);
	free(m->slots);
	m->slots = NULL;
	m->cap = m->count = 0;
}

static hmap_slot_t *hmap_find(hmap_t *m, uint64_t dev, uint64_t ino)
{
	uint32_t mask = m->cap - 1;
	for (uint32_t i = (uint32_t)(hmap_hash(dev, ino) & mask); ; i = (i + 1) & mask) {
		hmap_slot_t *s = &m->slots[i];
		if (!s->occupied)
			return NULL;
		if (s->dev == dev && s->ino == ino)
			return s;
	}
}

static void hmap_insert(hmap_t *m, uint64_t dev, uint64_t ino,
                         int64_t mtime, int64_t rowid, const char *path)
{
	uint32_t mask = m->cap - 1;
	for (uint32_t i = (uint32_t)(hmap_hash(dev, ino) & mask); ; i = (i + 1) & mask) {
		hmap_slot_t *s = &m->slots[i];
		if (!s->occupied) {
			s->dev      = dev;
			s->ino      = ino;
			s->mtime    = mtime;
			s->rowid    = rowid;
			s->path     = path ? strdup(path) : NULL;
			s->occupied = 1;
			s->visited  = 0;
			m->count++;
			return;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Database                                                            */
/* ------------------------------------------------------------------ */

static const char *SCHEMA =
	"CREATE TABLE IF NOT EXISTS tracks ("
	"  id           INTEGER PRIMARY KEY,"
	"  dev          INTEGER,"
	"  ino          INTEGER,"
	"  mtime        INTEGER,"
	"  path         TEXT,"
	"  format       TEXT,"
	"  sample_rate  INTEGER,"
	"  bit_depth    INTEGER,"
	"  channels     INTEGER,"
	"  duration     REAL,"
	"  title        TEXT,"
	"  artist       TEXT,"
	"  album        TEXT,"
	"  album_artist TEXT,"
	"  track_num    INTEGER,"
	"  disc_num     INTEGER,"
	"  date         TEXT,"
	"  genre        TEXT,"
	"  audiomd5     TEXT,"
	"  cover        TEXT DEFAULT ''"
	");"
	"CREATE UNIQUE INDEX IF NOT EXISTS idx_dev_ino ON tracks(dev, ino);";

static sqlite3_stmt *g_upsert_stmt;
static sqlite3_stmt *g_update_path_stmt;
static sqlite3_stmt *g_delete_stmt;

/* Detect and handle old schema (missing dev column) */
static void db_migrate(sqlite3 *db)
{
	sqlite3_stmt *probe;
	int rc = sqlite3_prepare_v2(db,
		"SELECT dev FROM tracks LIMIT 0;", -1, &probe, NULL);
	if (rc == SQLITE_OK) {
		sqlite3_finalize(probe);
		/* add cover column if missing */
		rc = sqlite3_prepare_v2(db,
			"SELECT cover FROM tracks LIMIT 0;", -1, &probe, NULL);
		if (rc == SQLITE_OK)
			sqlite3_finalize(probe);
		else
			sqlite3_exec(db,
				"ALTER TABLE tracks ADD COLUMN cover TEXT DEFAULT '';",
				NULL, NULL, NULL);
		return;
	}

	/* Check if table exists at all */
	rc = sqlite3_prepare_v2(db,
		"SELECT 1 FROM tracks LIMIT 0;", -1, &probe, NULL);
	if (rc == SQLITE_OK) {
		sqlite3_finalize(probe);
		fprintf(stderr, "Migrating: dropping old schema (re-scan required)\n");
		sqlite3_exec(db, "DROP TABLE IF EXISTS tracks;", NULL, NULL, NULL);
	}
}

static int db_init(sqlite3 *db)
{
	db_migrate(db);

	char *err = NULL;
	if (sqlite3_exec(db, SCHEMA, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "schema error: %s\n", err);
		sqlite3_free(err);
		return -1;
	}

	const char *upsert_sql =
		"INSERT INTO tracks "
		"(dev,ino,mtime,path,format,sample_rate,bit_depth,channels,"
		" duration,title,artist,album,album_artist,track_num,disc_num,"
		" date,genre)"
		" VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
		" ON CONFLICT(dev,ino) DO UPDATE SET"
		"  mtime=excluded.mtime, path=excluded.path,"
		"  format=excluded.format, sample_rate=excluded.sample_rate,"
		"  bit_depth=excluded.bit_depth, channels=excluded.channels,"
		"  duration=excluded.duration, title=excluded.title,"
		"  artist=excluded.artist, album=excluded.album,"
		"  album_artist=excluded.album_artist, track_num=excluded.track_num,"
		"  disc_num=excluded.disc_num, date=excluded.date,"
		"  genre=excluded.genre;";

	const char *path_sql = "UPDATE tracks SET path=? WHERE id=?;";
	const char *del_sql  = "DELETE FROM tracks WHERE id=?;";

	int rc = 0;
	rc |= sqlite3_prepare_v2(db, upsert_sql, -1, &g_upsert_stmt, NULL);
	rc |= sqlite3_prepare_v2(db, path_sql, -1, &g_update_path_stmt, NULL);
	rc |= sqlite3_prepare_v2(db, del_sql, -1, &g_delete_stmt, NULL);
	if (rc) {
		fprintf(stderr, "prepare error: %s\n", sqlite3_errmsg(db));
		return -1;
	}
	return 0;
}

static void db_upsert_track(const char *path, uint64_t dev, uint64_t ino,
                             int64_t mtime, const char *fmt,
                             uint32_t sr, uint8_t bd, uint8_t ch,
                             double duration, const tags_t *t)
{
	sqlite3_stmt *s = g_upsert_stmt;
	sqlite3_reset(s);
	sqlite3_bind_int64(s, 1, (int64_t)dev);
	sqlite3_bind_int64(s, 2, (int64_t)ino);
	sqlite3_bind_int64(s, 3, mtime);
	sqlite3_bind_text (s, 4, path, -1, SQLITE_STATIC);
	sqlite3_bind_text (s, 5, fmt,  -1, SQLITE_STATIC);
	sqlite3_bind_int  (s, 6, (int)sr);
	sqlite3_bind_int  (s, 7, (int)bd);
	sqlite3_bind_int  (s, 8, (int)ch);
	sqlite3_bind_double(s, 9, duration);
	sqlite3_bind_text (s, 10, *t->title        ? t->title        : NULL, -1, SQLITE_STATIC);
	sqlite3_bind_text (s, 11, *t->artist       ? t->artist       : NULL, -1, SQLITE_STATIC);
	sqlite3_bind_text (s, 12, *t->album        ? t->album        : NULL, -1, SQLITE_STATIC);
	sqlite3_bind_text (s, 13, *t->album_artist ? t->album_artist : NULL, -1, SQLITE_STATIC);
	sqlite3_bind_int  (s, 14, t->track_num);
	sqlite3_bind_int  (s, 15, t->disc_num);
	sqlite3_bind_text (s, 16, *t->date         ? t->date         : NULL, -1, SQLITE_STATIC);
	sqlite3_bind_text (s, 17, *t->genre        ? t->genre        : NULL, -1, SQLITE_STATIC);

	if (sqlite3_step(s) != SQLITE_DONE)
		fprintf(stderr, "upsert error: %s\n",
		        sqlite3_errmsg(sqlite3_db_handle(s)));
}

static void db_update_path(int64_t rowid, const char *path)
{
	sqlite3_stmt *s = g_update_path_stmt;
	sqlite3_reset(s);
	sqlite3_bind_text (s, 1, path, -1, SQLITE_STATIC);
	sqlite3_bind_int64(s, 2, rowid);
	sqlite3_step(s);
}

static void db_delete(int64_t rowid)
{
	sqlite3_stmt *s = g_delete_stmt;
	sqlite3_reset(s);
	sqlite3_bind_int64(s, 1, rowid);
	sqlite3_step(s);
}

/* Load existing entries into hashmap for skip/update logic */
static int db_load_existing(sqlite3 *db, hmap_t *m)
{
	sqlite3_stmt *st;

	int rc = sqlite3_prepare_v2(db,
		"SELECT COUNT(*) FROM tracks;", -1, &st, NULL);
	if (rc != SQLITE_OK) return -1;
	uint32_t count = 0;
	if (sqlite3_step(st) == SQLITE_ROW)
		count = (uint32_t)sqlite3_column_int(st, 0);
	sqlite3_finalize(st);

	if (hmap_init(m, count) != 0)
		return -1;

	if (count == 0)
		return 0;

	rc = sqlite3_prepare_v2(db,
		"SELECT id, dev, ino, mtime, path FROM tracks;", -1, &st, NULL);
	if (rc != SQLITE_OK) return -1;

	while (sqlite3_step(st) == SQLITE_ROW) {
		int64_t     rowid = sqlite3_column_int64(st, 0);
		uint64_t    dev   = (uint64_t)sqlite3_column_int64(st, 1);
		uint64_t    ino   = (uint64_t)sqlite3_column_int64(st, 2);
		int64_t     mtime = sqlite3_column_int64(st, 3);
		const char *path  = (const char *)sqlite3_column_text(st, 4);
		hmap_insert(m, dev, ino, mtime, rowid, path);
	}
	sqlite3_finalize(st);
	return 0;
}

/* ------------------------------------------------------------------ */
/* File parser dispatch                                                */
/* ------------------------------------------------------------------ */

static const char *check_ext(const char *name, size_t len)
{
	if (len > 5 && strcasecmp(name + len - 5, ".flac") == 0)
		return "flac";
	if (len > 3 && strcasecmp(name + len - 3, ".wv") == 0)
		return "wavpack";
	return NULL;
}

static int parse_file(const char *path, const char *fmt,
                       stream_info_t *si, tags_t *tags)
{
	if (fmt[0] == 'f')
		return parse_flac(path, si, tags);

	uint8_t scratch[256];
	int fd = open(path, O_RDONLY);
	if (fd < 0) return -1;
	int r = parse_wv_fd(fd, si, tags, scratch);
	close(fd);
	return r;
}

/* ------------------------------------------------------------------ */
/* Directory scanner                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
	uint32_t scanned;
	uint32_t skipped;
	uint32_t updated;
	uint32_t added;
	uint32_t errors;
	uint32_t moved;
} scan_stats_t;

static int g_debug;

static void scan_dir(sqlite3 *db, hmap_t *m, const char *dirpath,
                      scan_stats_t *stats)
{
	DIR *d = opendir(dirpath);
	if (!d) { perror(dirpath); return; }

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (ent->d_name[0] == '.') continue;

		char path[4096];
		int plen = snprintf(path, sizeof(path), "%s/%s",
		                     dirpath, ent->d_name);
		if (plen < 0 || (size_t)plen >= sizeof(path)) continue;

		/* Fast path: known directory from d_type */
		if (ent->d_type == DT_DIR) {
			scan_dir(db, m, path, stats);
			continue;
		}

		/* Check extension before stat */
		size_t nlen = strlen(ent->d_name);
		const char *fmt = check_ext(ent->d_name, nlen);
		if (!fmt) {
			/* Could be DT_UNKNOWN directory */
			if (ent->d_type == DT_UNKNOWN) {
				struct stat st;
				if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
					scan_dir(db, m, path, stats);
			}
			continue;
		}

		/* stat for dev/ino/mtime */
		struct stat st;
		if (stat(path, &st) != 0) continue;
		if (!S_ISREG(st.st_mode)) continue;

		stats->scanned++;
		uint64_t dev   = (uint64_t)st.st_dev;
		uint64_t ino   = (uint64_t)st.st_ino;
		int64_t  mtime = (int64_t)st.st_mtim.tv_sec;

		/* Hashmap lookup: skip/update/insert decision */
		hmap_slot_t *slot = hmap_find(m, dev, ino);
		if (slot) {
			slot->visited = 1;
			if (slot->mtime == mtime) {
				/* Same content — check if path changed */
				if (slot->path && strcmp(slot->path, path) != 0) {
					if (g_debug)
						printf("  [moved] %s -> %s\n",
						       slot->path, path);
					db_update_path(slot->rowid, path);
					stats->moved++;
				} else {
					stats->skipped++;
				}
				continue;
			}
			/* mtime changed — re-parse */
			stats->updated++;
		} else {
			if (g_debug)
				printf("  [added] %s\n", path);
			stats->added++;
		}

		/* Parse tags and upsert */
		stream_info_t si = {0};
		tags_t tags = {0};
		if (parse_file(path, fmt, &si, &tags) != 0) {
			if (g_debug)
				printf("  [error] %s\n", path);
			stats->errors++;
			continue;
		}

		double duration = si.sample_rate > 0 ?
			(double)si.total_samples / si.sample_rate : 0.0;

		db_upsert_track(path, dev, ino, mtime, fmt,
		                si.sample_rate, si.bits_per_sample,
		                si.channels, duration, &tags);
	}
	closedir(d);
}

/* ------------------------------------------------------------------ */
/* Cleanup: remove entries whose files no longer exist on disk         */
/* ------------------------------------------------------------------ */

static void cleanup_stale(sqlite3 *db)
{
	sqlite3_stmt *st;
	int rc = sqlite3_prepare_v2(db,
		"SELECT id, path, dev, ino FROM tracks;", -1, &st, NULL);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "cleanup query error: %s\n", sqlite3_errmsg(db));
		return;
	}

	uint32_t removed = 0, checked = 0;

	while (sqlite3_step(st) == SQLITE_ROW) {
		int64_t     rowid = sqlite3_column_int64(st, 0);
		const char *path  = (const char *)sqlite3_column_text(st, 1);
		uint64_t    dev   = (uint64_t)sqlite3_column_int64(st, 2);
		uint64_t    ino   = (uint64_t)sqlite3_column_int64(st, 3);
		checked++;

		struct stat sb;
		if (stat(path, &sb) != 0) {
			db_delete(rowid);
			removed++;
			continue;
		}

		/* File exists but dev:ino mismatch — path reused by different file */
		if ((uint64_t)sb.st_dev != dev || (uint64_t)sb.st_ino != ino) {
			db_delete(rowid);
			removed++;
		}
	}
	sqlite3_finalize(st);

	printf("Cleanup: checked %u, removed %u stale entries\n",
	       checked, removed);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s <dir> [db]      scan and index directory\n"
		"  %s -a <dir> [db]   add/update entries (same as above)\n"
		"  %s -c [db]         remove stale entries (files deleted from disk)\n"
		"  %s -i [db]         build cover art PNG cache\n"
		"  %s -h              show this help\n"
		"\n"
		"Scan mode (default):\n"
		"  Recursively scans <dir> for .flac and .wv files.\n"
		"  New files are parsed and inserted into the database.\n"
		"  Files with unchanged inode+mtime are skipped (fast re-scan).\n"
		"  Files with same inode but changed mtime are re-parsed.\n"
		"  Renamed/moved files are detected via inode and path is updated.\n"
		"\n"
		"Cleanup mode (-c):\n"
		"  Checks every database entry against disk.\n"
		"  Removes entries whose files no longer exist or whose\n"
		"  path now points to a different file (dev:ino mismatch).\n"
		"\n"
		"Database defaults to ~/.config/qua-player/music.db\n"
		"\n"
		"Examples:\n"
		"  %s /mnt/music\n"
		"  %s -a /mnt/more-music library.db\n"
		"  %s -c library.db\n",
		prog, prog, prog, prog, prog, prog, prog, prog);
}

/* ------------------------------------------------------------------ */
/* Cover art cache builder                                             */
/* ------------------------------------------------------------------ */

static int is_image_ext(const char *name)
{
	int len = strlen(name);
	/* skip spectrogram files (*.spec.png) */
	if (len > 9 && strcasecmp(name + len - 9, ".spec.png") == 0)
		return 0;
	const char *dot = strrchr(name, '.');
	if (!dot) return 0;
	if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0 ||
	    strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".webp") == 0 ||
	    strcasecmp(dot, ".bmp") == 0)
		return 1;
	return 0;
}

static int is_cover_image(const char *name)
{
	if (!is_image_ext(name)) return 0;
	char lo = name[0] | 32;
	return lo == 'c' || lo == 'f';
}

/* find best cover in dir: prefer cover/front named, fall back to biggest
 * image, then check one subdirectory level deep. */
static int find_cover(const char *d, char *out, int outmax)
{
	DIR *dp = opendir(d);
	if (!dp) return 0;

	struct dirent *ep;
	char best_named[4096] = "";
	off_t best_named_sz = 0;
	char best_any[4096] = "";
	off_t best_any_sz = 0;
	char subdirs[64][256];
	int nsub = 0;

	while ((ep = readdir(dp))) {
		if (ep->d_name[0] == '.') continue;
		if (ep->d_type == DT_DIR && nsub < 64) {
			strncpy(subdirs[nsub], ep->d_name,
				sizeof(subdirs[0]) - 1);
			subdirs[nsub][sizeof(subdirs[0]) - 1] = '\0';
			nsub++;
			continue;
		}
		if (ep->d_type != DT_REG) continue;
		if (!is_image_ext(ep->d_name)) continue;

		char full[4096];
		snprintf(full, sizeof(full), "%s/%s", d, ep->d_name);
		struct stat ist;
		if (stat(full, &ist) != 0) continue;

		if (is_cover_image(ep->d_name) &&
		    ist.st_size > best_named_sz) {
			best_named_sz = ist.st_size;
			strncpy(best_named, full, sizeof(best_named) - 1);
		}
		if (ist.st_size > best_any_sz) {
			best_any_sz = ist.st_size;
			strncpy(best_any, full, sizeof(best_any) - 1);
		}
	}
	closedir(dp);

	/* prefer named cover/front, then biggest image */
	if (*best_named) {
		strncpy(out, best_named, outmax - 1);
		out[outmax - 1] = '\0';
		return 1;
	}
	if (*best_any) {
		strncpy(out, best_any, outmax - 1);
		out[outmax - 1] = '\0';
		return 1;
	}

	/* check one subdirectory level */
	for (int i = 0; i < nsub; i++) {
		char subpath[4096];
		snprintf(subpath, sizeof(subpath), "%s/%s", d, subdirs[i]);
		dp = opendir(subpath);
		if (!dp) continue;
		while ((ep = readdir(dp))) {
			if (ep->d_name[0] == '.') continue;
			if (ep->d_type != DT_REG) continue;
			if (!is_image_ext(ep->d_name)) continue;

			char full[4096];
			snprintf(full, sizeof(full), "%s/%s",
				 subpath, ep->d_name);
			struct stat ist;
			if (stat(full, &ist) != 0) continue;

			if (is_cover_image(ep->d_name) &&
			    ist.st_size > best_named_sz) {
				best_named_sz = ist.st_size;
				strncpy(best_named, full,
					sizeof(best_named) - 1);
			}
			if (ist.st_size > best_any_sz) {
				best_any_sz = ist.st_size;
				strncpy(best_any, full,
					sizeof(best_any) - 1);
			}
		}
		closedir(dp);
	}

	if (*best_named) {
		strncpy(out, best_named, outmax - 1);
		out[outmax - 1] = '\0';
		return 1;
	}
	if (*best_any) {
		strncpy(out, best_any, outmax - 1);
		out[outmax - 1] = '\0';
		return 1;
	}
	return 0;
}

static unsigned long hash_str(const char *s)
{
	unsigned long h = 5381;
	while (*s) h = h * 33 + (unsigned char)*s++;
	return h;
}

static int cache_cover(const char *src, const char *dst, int max_dim)
{
	int w, h, ch;
	unsigned char *img = stbi_load(src, &w, &h, &ch, 3);
	if (!img) {
		/* stb_image can't decode (webp?) — fallback to magick */
		pid_t pid = fork();
		if (pid == 0) {
			execlp("magick", "magick", src,
			       "-resize", "1200x1200>", dst, NULL);
			_exit(1);
		}
		int status;
		waitpid(pid, &status, 0);
		return (WIFEXITED(status) && WEXITSTATUS(status) == 0)
			? 0 : -1;
	}

	/* compute scaled dimensions */
	int nw = w, nh = h;
	if (w > max_dim || h > max_dim) {
		double s = (double)max_dim / (w > h ? w : h);
		nw = (int)(w * s);
		nh = (int)(h * s);
		if (nw < 1) nw = 1;
		if (nh < 1) nh = 1;
	}

	unsigned char *out;
	if (nw != w || nh != h) {
		out = malloc(nw * nh * 3);
		stbir_resize_uint8_linear(img, w, h, 0,
					  out, nw, nh, 0, STBIR_RGB);
		stbi_image_free(img);
	} else {
		out = img;
	}

	int ok = stbi_write_png(dst, nw, nh, 3, out, nw * 3);
	free(out);
	return ok ? 0 : -1;
}

/* ---- threaded cache_cover worker ---- */

struct cache_job {
	char src[4096];
	char dst[4096];
};

static struct cache_job *g_jobs;
static int               g_njobs;
static _Atomic int       g_next_job;
static _Atomic int       g_done;

static void *cache_worker(void *arg)
{
	(void)arg;
	for (;;) {
		int i = g_next_job++;
		if (i >= g_njobs) break;
		cache_cover(g_jobs[i].src, g_jobs[i].dst, 1200);
		g_done++;
	}
	return NULL;
}

static void build_image_cache(sqlite3 *db, const char *home)
{
	char cache_dir[4096];
	snprintf(cache_dir, sizeof(cache_dir),
		 "%s/.cache/qua-music-tui", home);
	mkdir(cache_dir, 0755);

	sqlite3_stmt *dirs;
	if (sqlite3_prepare_v2(db,
		"SELECT DISTINCT"
		" rtrim(rtrim(path, replace(path, '/', '')), '/')"
		" FROM tracks;", -1, &dirs, NULL) != SQLITE_OK)
		return;

	sqlite3_exec(db,
		"CREATE INDEX IF NOT EXISTS idx_path ON tracks(path);",
		NULL, NULL, NULL);
	sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);

	sqlite3_stmt *upd;
	sqlite3_prepare_v2(db,
		"UPDATE tracks SET cover=?"
		" WHERE path >= ? || '/'"
		" AND path < ? || '0';",  /* '0' is '/' + 1 in ASCII */
		-1, &upd, NULL);

	/* phase 1: scan dirs, set cover on tracks, collect jobs */
	int cap = 4096;
	g_jobs = malloc(cap * sizeof(*g_jobs));
	g_njobs = 0;
	int total = 0;

	while (sqlite3_step(dirs) == SQLITE_ROW) {
		const char *d = (const char *)sqlite3_column_text(dirs, 0);
		if (!d || !*d) continue;
		total++;

		char cover[4096] = "";
		if (!find_cover(d, cover, sizeof(cover))) {
			/* no image file — extract embedded art from
			 * first flac/wv into the album directory */
			DIR *adp = opendir(d);
			int extracted = 0;
			if (adp) {
				struct dirent *aep;
				while ((aep = readdir(adp))) {
					if (aep->d_type != DT_REG) continue;
					const char *dot = strrchr(aep->d_name, '.');
					if (!dot) continue;
					int is_flac = strcasecmp(dot, ".flac") == 0;
					int is_wv = strcasecmp(dot, ".wv") == 0;
					if (!is_flac && !is_wv) continue;

					char audio[4096], out[4096];
					snprintf(audio, sizeof(audio),
						 "%s/%s", d, aep->d_name);
					snprintf(out, sizeof(out),
						 "%s/cover.png", d);

					pid_t pid = fork();
					if (pid == 0) {
						int nul = open("/dev/null", O_WRONLY);
						if (nul >= 0) {
							dup2(nul, STDERR_FILENO);
							dup2(nul, STDOUT_FILENO);
							close(nul);
						}
						if (is_flac) {
							execlp("metaflac", "metaflac",
							       "--export-picture-to",
							       out, audio, NULL);
						} else {
							chdir(d);
							execlp("wvtag", "wvtag",
							       "-xx",
							       "Cover Art (Front)=cover.%e",
							       "-y", audio, NULL);
						}
						_exit(1);
					}
					int st;
					waitpid(pid, &st, 0);
					if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
						extracted = 1;
					break; /* only try one file */
				}
				closedir(adp);
				if (extracted)
					find_cover(d, cover, sizeof(cover));
			}
		}

		/* compute cache path, update all tracks in this dir */
		char cpath[4096] = "";
		if (*cover)
			snprintf(cpath, sizeof(cpath), "%s/%016lx.png",
				 cache_dir, hash_str(cover));

		sqlite3_reset(upd);
		sqlite3_bind_text(upd, 1, cpath, -1, SQLITE_STATIC);
		sqlite3_bind_text(upd, 2, d, -1, SQLITE_STATIC);
		sqlite3_bind_text(upd, 3, d, -1, SQLITE_STATIC);
		sqlite3_step(upd);

		/* queue cache job if PNG doesn't exist yet */
		if (*cpath) {
			struct stat cst;
			if (stat(cpath, &cst) != 0) {
				if (g_njobs >= cap) {
					cap *= 2;
					g_jobs = realloc(g_jobs,
						cap * sizeof(*g_jobs));
				}
				strncpy(g_jobs[g_njobs].src, cover, 4095);
				strncpy(g_jobs[g_njobs].dst, cpath, 4095);
				g_njobs++;
			}
		}
	}
	sqlite3_finalize(dirs);
	sqlite3_finalize(upd);
	sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

	printf("Cover art: %d dirs, %d to cache\n", total, g_njobs);

	/* phase 2: fan out cache_cover across threads */
	if (g_njobs > 0) {
		g_next_job = 0;
		g_done = 0;
		int nthreads = sysconf(_SC_NPROCESSORS_ONLN);
		if (nthreads < 1) nthreads = 4;
		if (nthreads > g_njobs) nthreads = g_njobs;
		pthread_t *tids = malloc(nthreads * sizeof(*tids));

		for (int i = 0; i < nthreads; i++)
			pthread_create(&tids[i], NULL, cache_worker, NULL);
		for (int i = 0; i < nthreads; i++)
			pthread_join(tids[i], NULL);

		free(tids);
		printf("Cached %d images (%d threads)\n", g_njobs, nthreads);
	}
	free(g_jobs);
}

int main(int argc, char *argv[])
{
	int cleanup_mode = 0;
	int imgcache_mode = 0;
	const char *dir    = NULL;
	static char dbpath_buf[4096];
	const char *home = getenv("HOME");
	if (!home) { fprintf(stderr, "$HOME not set\n"); return 1; }
	snprintf(dbpath_buf, sizeof(dbpath_buf),
	         "%s/.config/qua-player", home);
	mkdir(dbpath_buf, 0755); /* ensure dir exists, ignore EEXIST */
	snprintf(dbpath_buf, sizeof(dbpath_buf),
	         "%s/.config/qua-player/music.db", home);
	const char *dbpath = dbpath_buf;

	int argi = 1;
	if (argi < argc && (strcmp(argv[argi], "-h") == 0 ||
	                     strcmp(argv[argi], "--help") == 0)) {
		usage(argv[0]);
		return 0;
	}
	if (argi < argc && strcmp(argv[argi], "-d") == 0) {
		g_debug = 1;
		argi++;
	}
	if (argi < argc && strcmp(argv[argi], "-i") == 0) {
		imgcache_mode = 1;
		argi++;
		if (argi < argc) dbpath = argv[argi++];
	} else if (argi < argc && strcmp(argv[argi], "-c") == 0) {
		cleanup_mode = 1;
		argi++;
		if (argi < argc) dbpath = argv[argi++];
	} else {
		if (argi < argc && strcmp(argv[argi], "-a") == 0)
			argi++;
		if (argi < argc) dir = argv[argi++];
		if (argi < argc) dbpath = argv[argi++];
		if (!dir) { usage(argv[0]); return 1; }
		/* resolve to absolute path so DB never stores relative paths */
		char resolved[PATH_MAX];
		if (realpath(dir, resolved))
			dir = resolved;
	}

	sqlite3 *db;
	if (sqlite3_open(dbpath, &db) != SQLITE_OK) {
		fprintf(stderr, "cannot open db: %s\n", sqlite3_errmsg(db));
		return 1;
	}

	sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
	sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

	if (db_init(db) != 0) return 1;

	if (imgcache_mode) {
		build_image_cache(db, home);
		sqlite3_finalize(g_upsert_stmt);
		sqlite3_finalize(g_update_path_stmt);
		sqlite3_finalize(g_delete_stmt);
		sqlite3_close(db);
		return 0;
	} else if (cleanup_mode) {
		sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
		cleanup_stale(db);
		sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
	} else {
		hmap_t m = {0};
		if (db_load_existing(db, &m) != 0) {
			fprintf(stderr, "failed to load existing entries\n");
			return 1;
		}
		printf("Loaded %u existing entries\n", m.count);
		printf("Scanning: %s\n", dir);

		sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);

		scan_stats_t stats = {0};
		scan_dir(db, &m, dir, &stats);

		sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

		printf("Done. scanned=%u added=%u updated=%u "
		       "skipped=%u moved=%u errors=%u\n",
		       stats.scanned, stats.added, stats.updated,
		       stats.skipped, stats.moved, stats.errors);

		hmap_destroy(&m);
	}

	/* (re)build FTS5 trigram index for fast substring search */
	printf("Building search index...\n");
	sqlite3_exec(db,
		"DROP TABLE IF EXISTS tracks_fts;"
		"CREATE VIRTUAL TABLE tracks_fts USING fts5("
		"  title, artist, album, album_artist, date, path,"
		"  content=tracks, content_rowid=rowid,"
		"  tokenize='trigram'"
		");"
		"INSERT INTO tracks_fts(tracks_fts) VALUES('rebuild');",
		NULL, NULL, NULL);
	printf("Search index built.\n");

	sqlite3_finalize(g_upsert_stmt);
	sqlite3_finalize(g_update_path_stmt);
	sqlite3_finalize(g_delete_stmt);
	sqlite3_close(db);
	return 0;
}
