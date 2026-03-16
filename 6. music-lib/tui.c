/* tui.c — ncurses music library browser
 *
 * Architecture:
 *   tracks[]     — flat array of all track_t, loaded once from mem_db
 *   col_defs[]   — column definitions: label, db_col, width/weight, visible, getter
 *   view[]       — display rows: VIEW_HEADER (synthetic) | VIEW_TRACK (index into tracks[])
 *                  rebuilt on every query_buf or sort change via SQL query
 *   query_buf[]     — fzf-style live query_buf: any printable key appends, rebuild on change
 *   sort_col     — index into col_defs[], drives ORDER BY + grouping
 *   sort_asc     — 1=ASC 0=DESC
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <locale.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <libgen.h>
#include <ctype.h>
#include <sqlite3.h>
#include <ncurses.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define DB_PATH_FMT   "%s/.config/qua-player/music.db"
#define MAX_TRACKS    524288
#define MAX_VIEW      (MAX_TRACKS * 2)  /* tracks + headers */
#define QUERY_MAX     256

/* ------------------------------------------------------------------ */
/* Data layer                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
	char   path[512];
	char   format[8];
	int    sample_rate, bit_depth;
	double duration;
	char   title[128];
	char   artist[128];
	char   album[128];
	char   album_artist[128];
	int    track_num, disc_num;
	char   date[16];
	char   genre[64];
	char   cover[512];
	int    cover_w, cover_h;
} track_t;

static track_t *tracks;
static int      ntracks;

/* ------------------------------------------------------------------ */
/* Column definitions                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
	const char *label;
	const char *db_col;  /* NULL = not sortable */
	int         min_w;   /* fixed width when weight=0 */
	int         weight;  /* 0=fixed, >0=proportional share */
	int         visible;
	int         group;   /* 1 = insert headers when sorting by this col */
	int         ralign;  /* 1 = right-align content */
	void      (*get)(const track_t *, char *buf, int n);
} col_def_t;

static void g_tracknum  (const track_t *t, char *b, int n) { if (t->track_num > 0) snprintf(b, n, "%d", t->track_num); else b[0]='\0'; }
static void g_title     (const track_t *t, char *b, int n) { strncpy(b, t->title,        n-1); b[n-1]='\0'; }
static void g_artist    (const track_t *t, char *b, int n) { strncpy(b, t->artist,       n-1); b[n-1]='\0'; }
static void g_album     (const track_t *t, char *b, int n) { strncpy(b, t->album,        n-1); b[n-1]='\0'; }
static void g_albumart  (const track_t *t, char *b, int n) { strncpy(b, t->album_artist, n-1); b[n-1]='\0'; }
static void g_duration  (const track_t *t, char *b, int n) { int s=(int)t->duration; snprintf(b, n, "%d:%02d", s/60, s%60); }
static void g_format    (const track_t *t, char *b, int n) { strncpy(b, strcmp(t->format,"wavpack")==0?"wvpk":t->format, n-1); b[n-1]='\0'; }
static void g_samplerate(const track_t *t, char *b, int n) { snprintf(b, n, "%d", t->sample_rate); }
static void g_bitdepth  (const track_t *t, char *b, int n) { snprintf(b, n, "%d", t->bit_depth); }
static void g_date      (const track_t *t, char *b, int n) { strncpy(b, t->date,         n-1); b[n-1]='\0'; }
static void g_genre     (const track_t *t, char *b, int n) { strncpy(b, t->genre,        n-1); b[n-1]='\0'; }

/* col_defs[] — edit visible/weight here to configure layout */
static col_def_t col_defs[] = {
	/* label       db_col         min_w  wt  vis  grp  ralign  getter       */
	{ "#",        "track_num",      3,  0,  1,   0,   1,     g_tracknum   },
	{ "Title",    "title",          0,  3,  1,   0,   0,     g_title      },
	{ "Artist",   "artist",         0,  2,  1,   1,   0,     g_artist     },
	{ "Album",    "album",          0,  2,  0,   1,   0,     g_album      },
	{ "AlbumArt", "album_artist",   0,  1,  0,   1,   0,     g_albumart   },
	{ "Dur",      "duration",       5,  0,  1,   0,   1,     g_duration   },
	{ "Fmt",      "format",         4,  0,  0,   1,   0,     g_format     },
	{ "Hz",       "sample_rate",    6,  0,  0,   0,   1,     g_samplerate },
	{ "Bd",       "bit_depth",      2,  0,  0,   0,   1,     g_bitdepth   },
	{ "Date",     "date",          10,  0,  0,   1,   0,     g_date       },
	{ "Genre",    "genre",          0,  1,  0,   1,   0,     g_genre      },
};
#define NCOLS ((int)(sizeof(col_defs)/sizeof(col_defs[0])))

/* ------------------------------------------------------------------ */
/* View layer                                                          */
/* ------------------------------------------------------------------ */

typedef enum { VIEW_HEADER, VIEW_TRACK } view_type_t;

typedef struct {
	view_type_t type;
	char        header[256];  /* VIEW_HEADER: group label */
	int         track_idx;    /* VIEW_TRACK:  index into tracks[] */
} view_row_t;

static view_row_t *view;
static int         nview;
static char        query_buf[QUERY_MAX] = "";
static int         search_mode;    /* 0=normal, 1=typing search */
static int         search_cur;     /* cursor position in query_buf */
static int         sort_col = 3;   /* default: sort by album, -1=unsorted */
static int         sort_asc = 1;   /* 1=ASC, 0=DESC */
static char        filter_col[32];    /* db column to filter, empty=off */
static char        filter_val[256];  /* exact value to match */
/* info panel right-click targets (y range → db column + value) */
#define INFO_CLICK_MAX 5
static struct {
	const char *db_col;
	char        val[256];
	int         y0, y1; /* screen row range [y0, y1) */
} info_click[INFO_CLICK_MAX];
static int n_info_click;
static sqlite3    *mem_db;
static sqlite3    *disk_db;
static char        cur_art_path[PATH_MAX];
static char        cache_dir[PATH_MAX];
static const char  no_art_path[] = "/home/free2/code/musl2gcc/6. music-lib/no-art.png";

/* ------------------------------------------------------------------ */
/* DB                                                                  */
/* ------------------------------------------------------------------ */

static sqlite3 *open_memdb(const char *path)
{
	sqlite3 *disk, *mem;
	if (sqlite3_open_v2(path, &disk, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
		fprintf(stderr, "cannot open %s\n", path);
		return NULL;
	}
	sqlite3_open(":memory:", &mem);
	sqlite3_backup *bk = sqlite3_backup_init(mem, "main", disk, "main");
	sqlite3_backup_step(bk, -1);
	sqlite3_backup_finish(bk);
	sqlite3_close(disk);
	return mem;
}

static int load_tracks(void)
{
	tracks = malloc(MAX_TRACKS * sizeof(track_t));
	if (!tracks) return -1;

	/* add cover columns if missing (pre-image-cache DB) */
	sqlite3_exec(mem_db,
		"ALTER TABLE tracks ADD COLUMN cover TEXT DEFAULT '';",
		NULL, NULL, NULL);
	sqlite3_exec(mem_db,
		"ALTER TABLE tracks ADD COLUMN cover_w INTEGER DEFAULT 0;",
		NULL, NULL, NULL);
	sqlite3_exec(mem_db,
		"ALTER TABLE tracks ADD COLUMN cover_h INTEGER DEFAULT 0;",
		NULL, NULL, NULL);

	sqlite3_stmt *st;
	const char *sql =
		"SELECT path,format,sample_rate,bit_depth,duration,"
		"       title,artist,album,album_artist,track_num,disc_num,date,genre,cover,"
		"       cover_w,cover_h"
		" FROM tracks ORDER BY id;";
	if (sqlite3_prepare_v2(mem_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;

	while (sqlite3_step(st) == SQLITE_ROW && ntracks < MAX_TRACKS) {
		track_t *t = &tracks[ntracks++];
		memset(t, 0, sizeof(*t));
#define S(field, col) do { \
	const char *v = (const char *)sqlite3_column_text(st, col); \
	if (v) strncpy(t->field, v, sizeof(t->field)-1); \
} while(0)
		S(path,         0); S(format,       1);
		t->sample_rate  = sqlite3_column_int(st, 2);
		t->bit_depth    = sqlite3_column_int(st, 3);
		t->duration     = sqlite3_column_double(st, 4);
		S(title,        5); S(artist,       6);
		S(album,        7); S(album_artist, 8);
		t->track_num    = sqlite3_column_int(st, 9);
		t->disc_num     = sqlite3_column_int(st, 10);
		S(date,        11); S(genre,        12);
		S(cover,       13);
		t->cover_w      = sqlite3_column_int(st, 14);
		t->cover_h      = sqlite3_column_int(st, 15);
#undef S
		/* fallback: empty title → basename, empty artist → "__" */
		if (!*t->title) {
			const char *sl = strrchr(t->path, '/');
			const char *base = sl ? sl + 1 : t->path;
			strncpy(t->title, base, sizeof(t->title) - 1);
			/* strip extension */
			char *dot = strrchr(t->title, '.');
			if (dot) *dot = '\0';
		}
		if (!*t->artist)
			strncpy(t->artist, "__", sizeof(t->artist) - 1);
	}
	sqlite3_finalize(st);
	return 0;
}

/* ------------------------------------------------------------------ */
/* View rebuild                                                        */
/* rebuild_view() queries mem_db with current query_buf+sort,            */
/* maps rowids back to tracks[] indices, inserts synthetic headers.   */
/* ------------------------------------------------------------------ */

static void rebuild_view(void)
{
	nview = 0;

	/* ORDER BY — prefixed version for FTS5 JOIN, plain for mem_db */
	char order[256], order_pfx[256];
	if (sort_col >= 0) {
		const char *col = col_defs[sort_col].db_col;
		if (!col) col = "album";
		snprintf(order, sizeof(order),
		         " ORDER BY COALESCE(NULLIF(%s,''),'~') %s,"
		         " COALESCE(NULLIF(album,''),'~'), path, disc_num, track_num, title",
		         col, sort_asc ? "ASC" : "DESC");
		snprintf(order_pfx, sizeof(order_pfx),
		         " ORDER BY COALESCE(NULLIF(t.%s,''),'~') %s,"
		         " COALESCE(NULLIF(t.album,''),'~'), t.path, t.disc_num, t.track_num, t.title",
		         col, sort_asc ? "ASC" : "DESC");
	} else {
		/* default: album_artist, date, album, disc, track */
		snprintf(order, sizeof(order),
		         " ORDER BY COALESCE(NULLIF(album_artist,''),NULLIF(artist,''),'~'),"
		         " COALESCE(NULLIF(date,''),'~'),"
		         " COALESCE(NULLIF(album,''),'~'), path, disc_num, track_num, title");
		snprintf(order_pfx, sizeof(order_pfx),
		         " ORDER BY COALESCE(NULLIF(t.album_artist,''),NULLIF(t.artist,''),'~'),"
		         " COALESCE(NULLIF(t.date,''),'~'),"
		         " COALESCE(NULLIF(t.album,''),'~'), t.path, t.disc_num, t.track_num, t.title");
	}

	/* build FTS5 match expression from query_buf words:
	 * "word1" "word2" — trigram tokenizer does substring AND */
	char match[QUERY_MAX * 4] = "";
	int has_query = 0;
	if (query_buf[0]) {
		char fcopy[QUERY_MAX];
		strncpy(fcopy, query_buf, sizeof(fcopy)-1);
		fcopy[sizeof(fcopy)-1] = '\0';
		char *p = match;
		for (char *tok = strtok(fcopy, " "); tok; tok = strtok(NULL, " ")) {
			if (p > match) *p++ = ' ';
			*p++ = '"';
			for (const char *s = tok; *s; s++) {
				if (*s == '"') *p++ = '"';
				*p++ = *s;
			}
			*p++ = '"';
			has_query = 1;
		}
		*p = '\0';
	}

	char sql[2048];
	sqlite3_stmt *st;
	if (has_query && disk_db) {
		/* FTS5 search: groups ranked by best match,
		 * tracks sorted normally within each group.
		 * bm25 weights: title=10, artist=5, album=10,
		 *   album_artist=5, date=1, path=0.5 */
		snprintf(sql, sizeof(sql),
			 "SELECT t.rowid FROM tracks t"
			 " JOIN tracks_fts f ON t.rowid = f.rowid"
			 " WHERE tracks_fts MATCH ?"
			 " ORDER BY MIN(bm25(tracks_fts, 10.0, 5.0, 10.0,"
			 "   5.0, 1.0, 0.5))"
			 "   OVER (PARTITION BY t.album, t.album_artist),"
			 "   t.album, t.path, t.disc_num, t.track_num, t.title;");
		if (sqlite3_prepare_v2(disk_db, sql, -1, &st, NULL) != SQLITE_OK) {
			fprintf(stderr, "FTS5: %s\n", sqlite3_errmsg(disk_db));
			return;
		}
		sqlite3_bind_text(st, 1, match, -1, SQLITE_TRANSIENT);
	} else if (filter_col[0]) {
		/* column filter: LIKE for path (contains %), = otherwise */
		const char *op = strchr(filter_val, '%') ? "LIKE" : "=";
		snprintf(sql, sizeof(sql),
			 "SELECT rowid FROM tracks"
			 " WHERE %s %s ?%s;", filter_col, op, order);
		if (sqlite3_prepare_v2(mem_db, sql, -1, &st, NULL) != SQLITE_OK)
			return;
		sqlite3_bind_text(st, 1, filter_val, -1, SQLITE_STATIC);
	} else {
		snprintf(sql, sizeof(sql),
			 "SELECT rowid FROM tracks%s;", order);
		if (sqlite3_prepare_v2(mem_db, sql, -1, &st, NULL) != SQLITE_OK)
			return;
	}

	/* walk results, insert headers when group value changes */
	char cur_group[256] = "";
	char gbuf[256];

	while (sqlite3_step(st) == SQLITE_ROW && nview + 2 < MAX_VIEW) {
		int rowid   = sqlite3_column_int(st, 0);
		int idx     = rowid - 1;  /* tracks[] is 0-based, rowid is 1-based */
		if (idx < 0 || idx >= ntracks) continue;
		track_t *t  = &tracks[idx];

		/* group header: "album_artist - date album" */
		int do_group = sort_col < 0 || col_defs[sort_col].group;
		if (do_group) {
			const char *slash = strrchr(t->path, '/');
			int dirlen = slash ? (int)(slash - t->path) : 0;
			if (sort_col >= 0) {
				char colbuf[128];
				col_defs[sort_col].get(t, colbuf, sizeof(colbuf));
				snprintf(gbuf, sizeof(gbuf), "%s\x1f%.*s", colbuf, dirlen, t->path);
			} else {
				/* default grouping key: album_artist + album + parent dir */
				const char *aa = *t->album_artist ? t->album_artist : t->artist;
				snprintf(gbuf, sizeof(gbuf), "%s\x1f%s\x1f%.*s", aa, t->album, dirlen, t->path);
			}
			if (strcmp(gbuf, cur_group) != 0) {
				view_row_t *vr = &view[nview++];
				vr->type = VIEW_HEADER;
				const char *aa = *t->album_artist ? t->album_artist : t->artist;
				const char *dt = t->date;
				const char *al = t->album;
				if (*aa || *dt || *al)
					snprintf(vr->header, sizeof(vr->header),
						 "%s - %s%s%s",
						 *aa ? aa : "?",
						 *dt ? dt : "",
						 *dt && *al ? " - " : "",
						 *al ? al : "?");
				else
					strncpy(vr->header, "(untagged)",
						sizeof(vr->header)-1);
				strncpy(cur_group, gbuf, sizeof(cur_group)-1);
			}
		}

		view_row_t *vr = &view[nview++];
		vr->type      = VIEW_TRACK;
		vr->track_idx = idx;
	}
	sqlite3_finalize(st);
}

/* ------------------------------------------------------------------ */
/* Navigation helpers                                                  */
/* ------------------------------------------------------------------ */

static int next_track(int from)
{
	for (int i = from + 1; i < nview; i++)
		if (view[i].type == VIEW_TRACK) return i;
	return from;
}

static int prev_track(int from)
{
	for (int i = from - 1; i >= 0; i--)
		if (view[i].type == VIEW_TRACK) return i;
	return from;
}

static int first_track(void)
{
	for (int i = 0; i < nview; i++)
		if (view[i].type == VIEW_TRACK) return i;
	return 0;
}

static int next_group(int from, int page_size)
{
	/* find the next header after current position, return its first track */
	for (int i = from + 1; i < nview; i++)
		if (view[i].type == VIEW_HEADER)
			return next_track(i);
	/* no header found — fall back to page jump */
	int r = from;
	for (int n = page_size; n > 0; n--) r = next_track(r);
	return r;
}

static int prev_group(int from, int page_size)
{
	/* find header of current group, then go to the one before it */
	int cur_hdr = -1;
	for (int i = from; i >= 0; i--)
		if (view[i].type == VIEW_HEADER) { cur_hdr = i; break; }
	if (cur_hdr >= 0) {
		for (int i = cur_hdr - 1; i >= 0; i--)
			if (view[i].type == VIEW_HEADER)
				return next_track(i);
	}
	/* no header found — fall back to page jump */
	int r = from;
	for (int n = page_size; n > 0; n--) r = prev_track(r);
	return r;
}

static int last_track(void)
{
	for (int i = nview - 1; i >= 0; i--)
		if (view[i].type == VIEW_TRACK) return i;
	return 0;
}

static void play_track(int view_idx)
{
	if (view_idx < 0 || view_idx >= nview) return;
	if (view[view_idx].type != VIEW_TRACK) return;
	const char *path = tracks[view[view_idx].track_idx].path;
	pid_t pid = fork();
	if (pid == 0) { execlp("qua-play-this", "qua-play-this", path, NULL); _exit(1); }
}

/* ------------------------------------------------------------------ */
/* Session state                                                       */
/* ------------------------------------------------------------------ */

static void save_state(int selected)
{
	if (!disk_db) return;
	const char *path = "";
	if (selected >= 0 && selected < nview &&
	    view[selected].type == VIEW_TRACK)
		path = tracks[view[selected].track_idx].path;

	char sc[8], sa[8];
	snprintf(sc, sizeof(sc), "%d", sort_col);
	snprintf(sa, sizeof(sa), "%d", sort_asc);

	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(disk_db,
	    "INSERT OR REPLACE INTO state(key,val) VALUES(?,?)",
	    -1, &st, NULL) != SQLITE_OK) return;

	const char *kv[][2] = {
		{"sort_col",  sc},
		{"sort_asc",  sa},
		{"query",     query_buf},
		{"sel_path",  path},
	};
	for (int i = 0; i < 4; i++) {
		sqlite3_bind_text(st, 1, kv[i][0], -1, SQLITE_STATIC);
		sqlite3_bind_text(st, 2, kv[i][1], -1, SQLITE_STATIC);
		sqlite3_step(st);
		sqlite3_reset(st);
	}
	sqlite3_finalize(st);
}

static int load_state(void)
{
	if (!disk_db) return 0;
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(disk_db,
	    "SELECT key,val FROM state", -1, &st, NULL) != SQLITE_OK)
		return 0;

	char sel_path[512] = "";
	while (sqlite3_step(st) == SQLITE_ROW) {
		const char *k = (const char *)sqlite3_column_text(st, 0);
		const char *v = (const char *)sqlite3_column_text(st, 1);
		if (!k || !v) continue;
		if (strcmp(k, "sort_col") == 0) {
			int c = atoi(v);
			if (c >= 0 && c < NCOLS) sort_col = c;
		} else if (strcmp(k, "sort_asc") == 0) {
			sort_asc = atoi(v) ? 1 : 0;
		} else if (strcmp(k, "query") == 0) {
			strncpy(query_buf, v, QUERY_MAX - 1);
		} else if (strcmp(k, "sel_path") == 0) {
			strncpy(sel_path, v, sizeof(sel_path) - 1);
		}
	}
	sqlite3_finalize(st);

	/* find the saved track in the view */
	if (*sel_path) {
		for (int i = 0; i < nview; i++) {
			if (view[i].type == VIEW_TRACK &&
			    strcmp(tracks[view[i].track_idx].path, sel_path) == 0)
				return i;
		}
	}
	return first_track();
}

static void copy_to_clipboard(const char *text)
{
	size_t len = strlen(text);
	const char *cmds[] = {
		"xclip -selection clipboard",
		"xclip -selection primary",
	};
	const char *wl_cmds[] = {
		"wl-copy",
		"wl-copy --primary",
	};
	int nwl = getenv("WAYLAND_DISPLAY") ? 2 : 0;

	for (int i = 0; i < 2 + nwl; i++) {
		const char *cmd = i < 2 ? cmds[i] : wl_cmds[i - 2];
		FILE *p = popen(cmd, "w");
		if (p) {
			fwrite(text, 1, len, p);
			pclose(p);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

/* UTF-8 aware: output exactly `width` terminal columns */
static void print_field(const char *s, int width)
{
	if (width <= 0) return;
	int disp = 0;
	mbstate_t mbs = {0};
	for (const char *p = s; *p;) {
		wchar_t wc;
		size_t n = mbrtowc(&wc, p, MB_CUR_MAX, &mbs);
		if (n == 0 || n == (size_t)-1 || n == (size_t)-2) break;
		int cw = wcwidth(wc); if (cw < 0) cw = 1;
		if (*(p+n) && disp + cw >= width) { addch('~'); disp++; break; }
		if (disp + cw > width) break;
		for (size_t i = 0; i < n; i++) addch((unsigned char)p[i]);
		disp += cw; p += n;
	}
	while (disp < width) { addch(' '); disp++; }
}

/* compute widths for visible columns given total available pixels `avail` */
static void calc_widths(int avail, int *widths)
{
	int total_fixed = 0, total_weight = 0;
	/* count separators (1 space between each pair of visible cols) */
	int nvis = 0;
	for (int c = 0; c < NCOLS; c++) if (col_defs[c].visible) nvis++;
	int sep = nvis > 1 ? nvis - 1 : 0;

	for (int c = 0; c < NCOLS; c++) {
		widths[c] = 0;
		if (!col_defs[c].visible) continue;
		if (col_defs[c].weight == 0) total_fixed += col_defs[c].min_w;
		else                         total_weight += col_defs[c].weight;
	}

	int rem = avail - total_fixed - sep;
	if (rem < 0) rem = 0;

	int flex_used = 0;
	int last_flex = -1;
	for (int c = 0; c < NCOLS; c++) {
		if (!col_defs[c].visible) continue;
		if (col_defs[c].weight == 0)
			widths[c] = col_defs[c].min_w;
		else {
			widths[c] = total_weight > 0 ? (rem * col_defs[c].weight) / total_weight : 0;
			flex_used += widths[c];
			last_flex = c;
		}
	}
	/* give rounding remainder to last flex column */
	if (last_flex >= 0 && rem > flex_used)
		widths[last_flex] += rem - flex_used;
}

/* map x position in header row to col_defs index, -1 if none */
static int col_at_x(int x, int lw)
{
	int widths[NCOLS];
	calc_widths(lw - 1, widths);
	int pos = 1; /* leading space */
	int first = 1;
	for (int c = 0; c < NCOLS; c++) {
		if (!col_defs[c].visible) continue;
		if (!first) pos++; /* separator */
		first = 0;
		if (x >= pos && x < pos + widths[c])
			return c;
		pos += widths[c];
	}
	return -1;
}

static void render_left(int top, int selected, int lw, int rows_h)
{
	int widths[NCOLS];
	calc_widths(lw - 1, widths); /* -1 for leading space */

	/* column header row */
	attron(COLOR_PAIR(3) | A_BOLD);
	move(0, 0);
	addch(' ');
	int first = 1;
	for (int c = 0; c < NCOLS; c++) {
		if (!col_defs[c].visible) continue;
		if (!first) addch(' ');
		first = 0;
		if (c == sort_col) {
			/* show arrow + label */
			const char *arrow = sort_asc ? "\xe2\x96\xb2" : "\xe2\x96\xbc"; /* ▲ ▼ */
			char hdr[64];
			snprintf(hdr, sizeof(hdr), "%s%s", col_defs[c].label, arrow);
			attron(A_UNDERLINE);
			if (col_defs[c].ralign)
				printw("%*s", widths[c], hdr);
			else
				print_field(hdr, widths[c]);
			attroff(A_UNDERLINE);
		} else {
			if (col_defs[c].ralign)
				printw("%*s", widths[c], col_defs[c].label);
			else
				print_field(col_defs[c].label, widths[c]);
		}
	}
	for (int x = getcurx(stdscr); x < lw; x++) addch(' ');
	attroff(COLOR_PAIR(3) | A_BOLD);

	/* find group header for the selected track */
	int sel_hdr = -1;
	if (selected >= 0 && selected < nview) {
		for (int i = selected; i >= 0; i--) {
			if (view[i].type == VIEW_HEADER) { sel_hdr = i; break; }
		}
	}

	/* sticky group header: if the top row's group header is off-screen,
	 * reserve row 1 for it and shift data down by one */
	int sticky = 0;
	int sticky_hdr = -1;
	if (top > 0 && top < nview && view[top].type == VIEW_TRACK) {
		for (int i = top - 1; i >= 0; i--) {
			if (view[i].type == VIEW_HEADER) {
				sticky_hdr = i;
				sticky = 1;
				break;
			}
		}
	}

	if (sticky) {
		int pair = (sticky_hdr == sel_hdr) ? 4 : 1;
		move(1, 0);
		attron(A_BOLD | COLOR_PAIR(pair));
		addch(' ');
		print_field(view[sticky_hdr].header, lw - 1);
		attroff(A_BOLD | COLOR_PAIR(pair));
	}

	/* track/header rows */
	int y0 = sticky ? 1 : 0;
	for (int y = y0; y < rows_h; y++) {
		int idx = top + y - y0;
		move(y + 1, 0);
		if (idx >= nview) {
			for (int x = 0; x < lw; x++) addch(' ');
			continue;
		}

		view_row_t *vr = &view[idx];

		if (vr->type == VIEW_HEADER) {
			int pair = (idx == sel_hdr) ? 4 : 1;
			attron(A_BOLD | COLOR_PAIR(pair));
			addch(' ');
			print_field(vr->header, lw - 1);
			attroff(A_BOLD | COLOR_PAIR(pair));
		} else {
			if (idx == selected) attron(COLOR_PAIR(2));
			addch(' ');
			track_t *t = &tracks[vr->track_idx];
			char buf[256];
			first = 1;
			for (int c = 0; c < NCOLS; c++) {
				if (!col_defs[c].visible) continue;
				if (!first) addch(' ');
				first = 0;
				buf[0] = '\0';
				col_defs[c].get(t, buf, sizeof(buf));
				if (col_defs[c].ralign)
					printw("%*s", widths[c], buf);
				else
					print_field(buf, widths[c]);
			}
			for (int x = getcurx(stdscr); x < lw; x++) addch(' ');
			if (idx == selected) attroff(COLOR_PAIR(2));
		}
	}
}

/* print one info line, return number of rows consumed (wraps long values) */
static int info_line(int y, int x, int w, const char *label, const char *val)
{
	if (y >= LINES - 1 || !val || !*val || w <= 0) return 0;
	int lbl_w = 9;
	if (lbl_w > w) lbl_w = w;
	int rem = w - lbl_w;
	if (rem <= 0) return 0;

	move(y, x);
	attron(A_BOLD);
	print_field(label, lbl_w);
	attroff(A_BOLD);

	/* first line of value */
	const char *p = val;
	int cols = 0;
	/* count columns for first line */
	const char *s = p;
	while (*s && cols < rem) {
		int cw = 1;
		if ((unsigned char)*s >= 0x80) {
			wchar_t wc;
			int mb = mbtowc(&wc, s, MB_CUR_MAX);
			if (mb > 1) { cw = wcwidth(wc); if (cw < 0) cw = 1; s += mb; }
			else s++;
		} else s++;
		cols += cw;
	}
	int first_bytes = s - p;
	printw("%.*s", first_bytes, p);
	p += first_bytes;

	int rows = 1;
	while (*p && y + rows < LINES - 1) {
		move(y + rows, x + lbl_w);
		cols = 0;
		s = p;
		while (*s && cols < rem) {
			int cw = 1;
			if ((unsigned char)*s >= 0x80) {
				wchar_t wc;
				int mb = mbtowc(&wc, s, MB_CUR_MAX);
				if (mb > 1) { cw = wcwidth(wc); if (cw < 0) cw = 1; s += mb; }
				else s++;
			} else s++;
			cols += cw;
		}
		int nbytes = s - p;
		printw("%.*s", nbytes, p);
		p += nbytes;
		rows++;
	}
	return rows;
}

static void get_cell_size(int *cw, int *ch)
{
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
	    ws.ws_xpixel > 0 && ws.ws_col > 0) {
		*cw = ws.ws_xpixel / ws.ws_col;
		*ch = ws.ws_ypixel / ws.ws_row;
	} else {
		*cw = 8; *ch = 16;
	}
}

static int art_split(int rw, int rows_h)
{
	int cw, ch;
	get_cell_size(&cw, &ch);
	int s = (rw * cw) / ch;  /* rows for a square */
	if (s > rows_h - 2) s = rows_h - 2;
	if (s < 1) s = 1;
	return s;
}

static void render_right(int selected, int rx, int rw, int rows_h)
{
	int split = art_split(rw, rows_h);

	/* -- Album Art header (top) -- */
	attron(COLOR_PAIR(3) | A_BOLD);
	mvprintw(0, rx, "%-*s", rw, " Album Art");
	attroff(COLOR_PAIR(3) | A_BOLD);

	/* only blank art area when no image loaded */
	if (!*cur_art_path)
		for (int y = 1; y <= split; y++)
			mvprintw(y, rx, "%-*s", rw, "");

	/* -- Info divider -- */
	int div_y = split + 1;
	attron(COLOR_PAIR(3) | A_BOLD);
	mvprintw(div_y, rx, "%-*s", rw, " Info");
	attroff(COLOR_PAIR(3) | A_BOLD);

	/* -- Info panel (bottom) -- */
	for (int y = div_y + 1; y <= rows_h; y++) mvprintw(y, rx, "%-*s", rw, "");

	if (selected >= 0 && selected < nview &&
	    view[selected].type == VIEW_TRACK) {
		track_t *t = &tracks[view[selected].track_idx];
		char dur[16], hz[16], bd[16];
		int s = (int)t->duration;
		snprintf(dur, sizeof(dur), "%d:%02d", s/60, s%60);
		snprintf(hz,  sizeof(hz),  "%d Hz",  t->sample_rate);
		snprintf(bd,  sizeof(bd),  "%d bit", t->bit_depth);
		const char *fmt = strcmp(t->format,"wavpack")==0 ? "WavPack" :
		                  strcmp(t->format,"flac")   ==0 ? "FLAC"    : t->format;

		int y = div_y + 1, h;
		n_info_click = 0;
		y += info_line(y, rx+1, rw-1, "Title",     *t->title ? t->title : "(no title)");
		h = info_line(y, rx+1, rw-1, "Artist",    t->artist);
		if (h > 0) { info_click[n_info_click] = (typeof(info_click[0])){"artist", "", y, y+h}; strncpy(info_click[n_info_click].val, t->artist, 255); n_info_click++; }
		y += h;
		h = info_line(y, rx+1, rw-1, "Album",     t->album);
		if (h > 0) { info_click[n_info_click] = (typeof(info_click[0])){"album", "", y, y+h}; strncpy(info_click[n_info_click].val, t->album, 255); n_info_click++; }
		y += h;
		h = info_line(y, rx+1, rw-1, "AlbumArt",  t->album_artist);
		if (h > 0) { info_click[n_info_click] = (typeof(info_click[0])){"album_artist", "", y, y+h}; strncpy(info_click[n_info_click].val, t->album_artist, 255); n_info_click++; }
		y += h;
		if (t->track_num > 0) {
			char tn[8]; snprintf(tn, sizeof(tn), "%d", t->track_num);
			y += info_line(y, rx+1, rw-1, "Track",   tn);
		}
		y += info_line(y, rx+1, rw-1, "Duration",  dur);
		y += info_line(y, rx+1, rw-1, "Format",    fmt);
		y += info_line(y, rx+1, rw-1, "Rate",      hz);
		y += info_line(y, rx+1, rw-1, "Depth",     bd);
		y += info_line(y, rx+1, rw-1, "Date",      t->date);
		h = info_line(y, rx+1, rw-1, "Genre",     t->genre);
		if (h > 0) { info_click[n_info_click] = (typeof(info_click[0])){"genre", "", y, y+h}; strncpy(info_click[n_info_click].val, t->genre, 255); n_info_click++; }
		y += h;
		h = info_line(y, rx+1, rw-1, "Path",      t->path);
		if (h > 0) { info_click[n_info_click] = (typeof(info_click[0])){"path", "", y, y+h}; strncpy(info_click[n_info_click].val, t->path, sizeof(info_click[0].val)-1); n_info_click++; }
		y += h;
	}
}

/* ------------------------------------------------------------------ */
/* Cover art finder                                                    */
/* ------------------------------------------------------------------ */

static int is_image_ext(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!dot || !dot[1]) return 0;
	switch (tolower((unsigned char)dot[1])) {
	case 'j': return strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0;
	case 'p': return strcasecmp(dot, ".png") == 0;
	case 'w': return strcasecmp(dot, ".webp") == 0;
	case 'b': return strcasecmp(dot, ".bmp") == 0;
	default:  return 0;
	}
}

/* find best cover in dir: prefer cover/front named, fall back to biggest
 * image, then check one subdirectory level deep. */
static int is_cover_name(const char *name)
{
	if (!is_image_ext(name)) return 0;
	char lo = name[0] | 32;
	return lo == 'c' || lo == 'f';
}

static int find_cover_art(const char *dir, char *out, int outmax)
{
	DIR *dp = opendir(dir);
	if (!dp) return 0;

	struct dirent *ep;
	char best_named[PATH_MAX] = "";
	off_t best_named_sz = 0;
	char best_any[PATH_MAX] = "";
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

		char full[PATH_MAX];
		snprintf(full, sizeof(full), "%s/%s", dir, ep->d_name);
		struct stat ist;
		if (stat(full, &ist) != 0) continue;

		if (is_cover_name(ep->d_name) &&
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

	if (*best_named) { strncpy(out, best_named, outmax - 1); out[outmax - 1] = '\0'; return 1; }
	if (*best_any)   { strncpy(out, best_any, outmax - 1);   out[outmax - 1] = '\0'; return 1; }

	/* check one subdirectory level */
	for (int i = 0; i < nsub; i++) {
		char subpath[PATH_MAX];
		snprintf(subpath, sizeof(subpath), "%s/%s", dir, subdirs[i]);
		dp = opendir(subpath);
		if (!dp) continue;
		while ((ep = readdir(dp))) {
			if (ep->d_name[0] == '.') continue;
			if (ep->d_type != DT_REG) continue;
			if (!is_image_ext(ep->d_name)) continue;

			char full[PATH_MAX];
			snprintf(full, sizeof(full), "%s/%s",
				 subpath, ep->d_name);
			struct stat ist;
			if (stat(full, &ist) != 0) continue;

			if (is_cover_name(ep->d_name) &&
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

	if (*best_named) { strncpy(out, best_named, outmax - 1); out[outmax - 1] = '\0'; return 1; }
	if (*best_any)   { strncpy(out, best_any, outmax - 1);   out[outmax - 1] = '\0'; return 1; }
	return 0;
}

/* get parent directory of a file path into buf */
static void path_dir(const char *filepath, char *buf, int n)
{
	strncpy(buf, filepath, n - 1);
	buf[n - 1] = '\0';
	char *slash = strrchr(buf, '/');
	if (slash) *slash = '\0';
	else buf[0] = '\0';
}

/* ------------------------------------------------------------------ */
/* Image caching helpers                                               */
/* ------------------------------------------------------------------ */

static unsigned long hash_str(const char *s)
{
	unsigned long h = 5381;
	while (*s)
		h = h * 33 + (unsigned char)*s++;
	return h;
}

static void get_cache_path(const char *src, char *out, int n)
{
	unsigned long h = hash_str(src);
	snprintf(out, n, "%s/%016lx.png", cache_dir, h);
}

/* read width/height from PNG header (IHDR at bytes 16-23) */
static int img_dimensions(const char *path, int *w, int *h)
{
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	unsigned char buf[24];
	if (fread(buf, 1, 24, f) != 24) { fclose(f); return -1; }
	fclose(f);
	*w = (buf[16] << 24) | (buf[17] << 16) | (buf[18] << 8) | buf[19];
	*h = (buf[20] << 24) | (buf[21] << 16) | (buf[22] << 8) | buf[23];
	return 0;
}

/* resize source image to max 1200x1200 (preserving aspect), save to dst */
static int magick_cache(const char *src, const char *dst)
{
	int max_dim = 1200;
	int w, h, ch;
	unsigned char *img = stbi_load(src, &w, &h, &ch, 3);
	if (!img) {
		/* webp fallback: fork magick */
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

/* ------------------------------------------------------------------ */
/* Kitty graphics protocol (direct placement)                          */
/* ------------------------------------------------------------------ */

static const char b64tab[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const unsigned char *in, int len, char *out, int outmax)
{
	int i, j = 0;
	for (i = 0; i + 2 < len && j + 4 < outmax; i += 3) {
		unsigned a = in[i], b = in[i+1], c = in[i+2];
		out[j++] = b64tab[a >> 2];
		out[j++] = b64tab[((a & 3) << 4) | (b >> 4)];
		out[j++] = b64tab[((b & 0xf) << 2) | (c >> 6)];
		out[j++] = b64tab[c & 0x3f];
	}
	if (i < len) {
		unsigned a = in[i];
		out[j++] = b64tab[a >> 2];
		if (i + 1 < len) {
			unsigned b = in[i+1];
			out[j++] = b64tab[((a & 3) << 4) | (b >> 4)];
			out[j++] = b64tab[((b & 0xf) << 2)];
		} else {
			out[j++] = b64tab[(a & 3) << 4];
			out[j++] = '=';
		}
		out[j++] = '=';
	}
	out[j] = '\0';
	return j;
}

static unsigned int kitty_img_id;
static unsigned int kitty_id_counter;

static void kitty_delete(unsigned int id)
{
	char cmd[64];
	int n = snprintf(cmd, sizeof(cmd),
		"\033_Gq=2,a=d,d=i,i=%u\033\\", id);
	write(STDOUT_FILENO, cmd, n);
}

static void kitty_delete_all(void)
{
	const char cmd[] = "\033_Gq=2,a=d,d=A\033\\";
	write(STDOUT_FILENO, cmd, sizeof(cmd) - 1);
}

/* transmit PNG file via kitty t=f (store only, no display) */
static int kitty_transmit_png(const char *path)
{
	kitty_img_id = ++kitty_id_counter;
	int pathlen = strlen(path);
	char b64path[PATH_MAX * 2];
	b64_encode((unsigned char *)path, pathlen, b64path, sizeof(b64path));
	char cmd[PATH_MAX * 2 + 128];
	int n = snprintf(cmd, sizeof(cmd),
		"\033_Gq=2,a=t,f=100,t=f,i=%u;%s\033\\",
		kitty_img_id, b64path);
	write(STDOUT_FILENO, cmd, n);
	return 0;
}

/* place image at screen position via direct placement */
static void kitty_place(int y, int x, int cols, int rows)
{
	char cmd[256];
	int n = snprintf(cmd, sizeof(cmd),
		"\033[%d;%dH\033_Gq=2,a=p,i=%u,c=%d,r=%d,C=1\033\\",
		y + 1, x + 1, kitty_img_id, cols, rows);
	write(STDOUT_FILENO, cmd, n);
}

static void render(int top, int selected)
{
	int rows_h = LINES - 2;
	int lw     = (COLS * 62) / 100;
	int rw     = COLS - lw;
	int rx     = lw;

	render_left(top, selected, lw, rows_h);
	render_right(selected, rx, rw, rows_h);


	/* status / query_buf bar */
	static const char keys[] = "/: search  enter/p: play  s: stop  o: open  q: quit";
	attron(COLOR_PAIR(3) | A_BOLD);
	int ntracks_in_view = 0;
	for (int i = 0; i < nview; i++)
		if (view[i].type == VIEW_TRACK) ntracks_in_view++;
	move(LINES-1, 0);
	if (search_mode) {
		printw(" / ");
		for (int i = 0; query_buf[i]; i++) {
			if (i == search_cur) attron(A_UNDERLINE);
			addch(query_buf[i]);
			if (i == search_cur) attroff(A_UNDERLINE);
		}
		if (search_cur >= (int)strlen(query_buf)) {
			attron(A_UNDERLINE);
			addch(' ');
			attroff(A_UNDERLINE);
		}
		printw("  [%d results]  |  %s", ntracks_in_view, keys);
	}
	else if (*query_buf)
		printw(" [filter:%s] %d results  |  %s", query_buf, ntracks_in_view, keys);
	else
		printw(" %d tracks  |  %s", ntracks_in_view, keys);
	for (int x = getcurx(stdscr); x < COLS; x++) addch(' ');
	attroff(COLOR_PAIR(3) | A_BOLD);

	refresh();

	/* kitty: album art after ncurses flush */
	static int  art_cols;     /* actual cols image occupies */
	static int  art_rows;     /* actual rows image occupies */
	int split = art_split(rw, rows_h);
	int art_y = 1;           /* right below "Album Art" header */
	int art_h = split;

	if (art_h > 0) {
		/* save cursor so kitty writes don't corrupt ncurses */
		write(STDOUT_FILENO, "\0337", 2);  /* ESC 7 = save cursor */

		/* get cover art — prefer pre-cached from track data */
		static char last_art_dir[PATH_MAX];
		static char last_art_val[PATH_MAX];
		char dir[PATH_MAX] = "";
		char resolved[PATH_MAX] = "";
		int db_cw = 0, db_ch = 0;
		if (selected >= 0 && selected < nview &&
		    view[selected].type == VIEW_TRACK) {
			track_t *t = &tracks[view[selected].track_idx];
			if (*t->cover) {
				strncpy(resolved, t->cover,
					sizeof(resolved) - 1);
				db_cw = t->cover_w;
				db_ch = t->cover_h;
			} else {
				/* fallback: scan + cache on the fly */
				path_dir(t->path, dir, sizeof(dir));
				if (*dir && strcmp(dir, last_art_dir) != 0) {
					char src[PATH_MAX] = "";
					find_cover_art(dir, src, sizeof(src));
					if (*src) {
						get_cache_path(src, resolved,
							sizeof(resolved));
						struct stat cst;
						if (stat(resolved, &cst) != 0)
							magick_cache(src,
								resolved);
					} else {
						strncpy(resolved, no_art_path,
							sizeof(resolved) - 1);
					}
					strncpy(last_art_val, resolved,
						sizeof(last_art_val) - 1);
					last_art_val[sizeof(last_art_val) - 1] = '\0';
				} else if (*dir) {
					strncpy(resolved, last_art_val,
						sizeof(resolved) - 1);
				}
				strncpy(last_art_dir, dir,
					sizeof(last_art_dir) - 1);
			}
		}

		if (strcmp(resolved, cur_art_path) != 0) {
			unsigned int old_id = kitty_img_id;
			int had_old = *cur_art_path;

			strncpy(cur_art_path, resolved,
				sizeof(cur_art_path) - 1);
			cur_art_path[sizeof(cur_art_path) - 1] = '\0';

			/* transmit new image (old still visible) */
			if (*cur_art_path) {
				int iw = db_cw, ih = db_ch;
				art_cols = rw;
				art_rows = art_h;
				if ((iw <= 0 || ih <= 0) &&
				    img_dimensions(cur_art_path, &iw, &ih) != 0) {
					iw = 0; ih = 0;
				}
				if (iw > 0 && ih > 0) {
					int cw, ch;
					get_cell_size(&cw, &ch);
					int area_pw = rw * cw;
					int area_ph = art_h * ch;
					double s = (double)area_pw / iw;
					if ((double)area_ph / ih < s)
						s = (double)area_ph / ih;
					int disp_pw = (int)(iw * s);
					int disp_ph = (int)(ih * s);
					art_cols = disp_pw / cw;
					art_rows = disp_ph / ch;
					if (art_cols < 1) art_cols = 1;
					if (art_cols > rw) art_cols = rw;
					if (art_rows < 1) art_rows = 1;
					if (art_rows > art_h) art_rows = art_h;
					kitty_transmit_png(cur_art_path);
				}
				/* place new on top, then delete old */
				kitty_place(art_y, rx, art_cols, art_rows);
				if (had_old)
					kitty_delete(old_id);
			} else {
				/* no new art — just delete old */
				if (had_old)
					kitty_delete(old_id);
			}
		}

		/* re-place every frame for stability */
		if (*cur_art_path)
			kitty_place(art_y, rx, art_cols, art_rows);

		/* restore cursor for ncurses */
		write(STDOUT_FILENO, "\0338", 2);
	}
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
	static char db_default[4096];
	const char *home = getenv("HOME");
	if (!home) { fprintf(stderr, "$HOME not set\n"); return 1; }
	snprintf(db_default, sizeof(db_default), DB_PATH_FMT, home);
	const char *dbpath = argc > 1 ? argv[1] : db_default;

	mem_db = open_memdb(dbpath);
	if (!mem_db) return 1;

	/* open disk DB read-write for state/FTS */
	if (sqlite3_open_v2(dbpath, &disk_db,
	    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) == SQLITE_OK) {
		sqlite3_exec(disk_db,
			"CREATE TABLE IF NOT EXISTS state ("
			"key TEXT PRIMARY KEY, val TEXT);",
			NULL, NULL, NULL);
	} else {
		disk_db = NULL;
	}

	/* create cache directory for resized cover art */
	snprintf(cache_dir, sizeof(cache_dir),
		 "%s/.cache/qua-music-tui", home);
	mkdir(cache_dir, 0755);

	if (load_tracks() != 0) { fprintf(stderr, "load_tracks failed\n"); return 1; }

	view = malloc(MAX_VIEW * sizeof(view_row_t));
	if (!view) return 1;

	load_state();      /* restore sort/query from last session */
	rebuild_view();

	setlocale(LC_ALL, "");
	initscr(); noecho(); cbreak();
	keypad(stdscr, TRUE); curs_set(0);
	scrollok(stdscr, FALSE);
	idlok(stdscr, FALSE);
	start_color(); use_default_colors();
	init_pair(1, COLOR_BLACK, 132);            /* group headers (dusty rose) */
	init_pair(2, COLOR_GREEN, -1);            /* selection (green) */
	init_pair(3, COLOR_BLACK, COLOR_YELLOW);  /* top/bottom bars */
	init_pair(4, COLOR_GREEN, 132);           /* active group header */
	mousemask(BUTTON1_CLICKED | BUTTON3_CLICKED | BUTTON4_PRESSED | BUTTON5_PRESSED, NULL);
	mouseinterval(0);

	int top      = 0;
	int selected = load_state();
	int rows_h   = LINES - 2;


	render(top, selected);

	for (;;) {
		int ch = getch();
		int prev = selected, old_top = top;
		int dirty = 0;

		/* mouse works in both normal and search mode */
		if (ch == KEY_MOUSE) {
			MEVENT ev;
			if (getmouse(&ev) == OK) {
				if (ev.bstate & BUTTON4_PRESSED)
					selected = prev_track(selected);
				else if (ev.bstate & BUTTON5_PRESSED)
					selected = next_track(selected);
				else if (ev.y == 0 &&
				 (ev.bstate & BUTTON1_CLICKED)) {
					int lw = (COLS * 62) / 100;
					int c = col_at_x(ev.x, lw);
					if (c >= 0 && col_defs[c].db_col) {
						if (c == sort_col) {
							/* 3-way: ASC → DESC → unsorted */
							if (sort_asc)
								sort_asc = 0;
							else
								sort_col = -1;
						} else {
							sort_col = c;
							sort_asc = 1;
						}
						rebuild_view();
						selected = first_track();
						top = 0;
						dirty = 1;
					}
				} else if (ev.y >= 1 &&
					   ev.y < LINES - 1 &&
					   ev.x < (COLS * 62) / 100) {
					int idx = top + (ev.y - 1);
					if (idx >= 0 && idx < nview &&
					    view[idx].type == VIEW_TRACK) {
						if (ev.bstate & BUTTON3_CLICKED) {
							selected = idx;
							play_track(selected);
						} else if (ev.bstate & BUTTON1_CLICKED) {
							selected = idx;
						}
					}
				} else if ((ev.bstate & BUTTON3_CLICKED) &&
					   ev.x >= (COLS * 62) / 100) {
					/* right-click on info field: filter */
					int handled = 0;
					int irx = (COLS * 62) / 100;
					int irw = COLS - irx;
					int lbl_w = 9;
					int val_x = irx + 1 + lbl_w;
					int val_w = irw - 1 - lbl_w;
					for (int i = 0; i < n_info_click; i++) {
						if (ev.y >= info_click[i].y0 &&
						    ev.y < info_click[i].y1 &&
						    info_click[i].val[0]) {
							if (strcmp(info_click[i].db_col, "path") == 0 && val_w > 0) {
								/* map click to char index in path */
								int ci = (ev.y - info_click[i].y0) * val_w
									+ (ev.x - val_x);
								const char *p = info_click[i].val;
								int plen = strlen(p);
								if (ci < 0) ci = 0;
								if (ci >= plen) ci = plen - 1;
								/* find the next '/' at or after click */
								int cut = ci;
								while (cut < plen && p[cut] != '/') cut++;
								if (cut >= plen) cut = plen - 1;
								/* walk back to include this dir */
								while (cut > 0 && p[cut] != '/') cut--;
								if (cut <= 0) break;
								strncpy(filter_col, "path", sizeof(filter_col)-1);
								snprintf(filter_val, sizeof(filter_val),
									 "%.*s/%%", cut, p);
							} else {
								strncpy(filter_col, info_click[i].db_col, sizeof(filter_col)-1);
								strncpy(filter_val, info_click[i].val, sizeof(filter_val)-1);
								/* replace commas with spaces for artist fields */
								if (strcmp(info_click[i].db_col, "artist") == 0 ||
								    strcmp(info_click[i].db_col, "album_artist") == 0) {
									for (char *c = filter_val; *c; c++)
										if (*c == ',') *c = ' ';
								}
							}
							copy_to_clipboard(filter_val);
							query_buf[0] = '\0';
							rebuild_view();
							selected = first_track();
							top = 0;
							dirty = 1;
							handled = 1;
							break;
						}
					}
					int rx = (COLS * 62) / 100;
					int rw = COLS - rx;
					int asplit = art_split(rw, LINES - 2);
					if (!handled && ev.y >= 1 && ev.y <= asplit &&
					    *cur_art_path &&
					    selected >= 0 && selected < nview &&
					    view[selected].type == VIEW_TRACK) {
						/* right-click on art area: open original image */
						track_t *t = &tracks[view[selected].track_idx];
						char adir[PATH_MAX], src[PATH_MAX] = "";
						path_dir(t->path, adir, sizeof(adir));
						find_cover_art(adir, src, sizeof(src));
						if (*src) {
							pid_t pid = fork();
							if (pid == 0) {
								setsid();
								freopen("/dev/null", "w", stdout);
								freopen("/dev/null", "w", stderr);
								freopen("/dev/null", "r", stdin);
								execlp("nsxiv-rifle", "nsxiv-rifle",
								       src, NULL);
								_exit(1);
							}
						}
					}
				}
			}
		} else if (search_mode) {
			/* search mode: keys go to query buffer,
			 * arrows scroll playlist / move cursor */
			switch (ch) {
			case 27: /* ESC — cancel search */
				search_mode = 0;
				query_buf[0] = '\0';
				search_cur = 0;
				filter_col[0] = '\0';
				filter_val[0] = '\0';
				rebuild_view();
				selected = first_track();
				top = 0;
				dirty = 1;
				break;
			case KEY_ENTER: case '\n':
				search_mode = 0;
				dirty = 1;
				break;
			case KEY_UP:
				selected = prev_track(selected);
				dirty = 1;
				break;
			case KEY_DOWN:
				selected = next_track(selected);
				dirty = 1;
				break;
			case KEY_LEFT:
				if (search_cur > 0) search_cur--;
				dirty = 1;
				break;
			case KEY_RIGHT:
				if (search_cur < (int)strlen(query_buf)) search_cur++;
				dirty = 1;
				break;
			case KEY_BACKSPACE: case 127: {
				if (search_cur > 0) {
					int len = strlen(query_buf);
					memmove(&query_buf[search_cur-1],
						&query_buf[search_cur],
						len - search_cur + 1);
					search_cur--;
					if (strlen(query_buf) >= 3) {
						rebuild_view();
						selected = first_track(); top = 0;
					} else {
						char save = query_buf[0];
						query_buf[0] = '\0';
						rebuild_view();
						query_buf[0] = save;
						selected = first_track(); top = 0;
					}
					dirty = 1;
				}
				break;
			}
			case KEY_RESIZE:
				kitty_delete_all();
				clear();
				rows_h = LINES - 2;
				cur_art_path[0] = '\0';
				dirty = 1;
				break;
			default:
				if (ch >= 32 && ch < 127) {
					int len = strlen(query_buf);
					if (len < QUERY_MAX - 1) {
						memmove(&query_buf[search_cur+1],
							&query_buf[search_cur],
							len - search_cur + 1);
						query_buf[search_cur] = (char)ch;
						search_cur++;
						if (strlen(query_buf) >= 3) {
							rebuild_view();
							selected = first_track(); top = 0;
						}
						dirty = 1;
					}
				}
				break;
			}
		} else {
			/* normal mode */
			switch (ch) {
			case 27: /* ESC — clear filters only, q to quit */
				query_buf[0] = '\0';
				filter_col[0] = '\0';
				filter_val[0] = '\0';
				rebuild_view();
				selected = first_track();
				top = 0;
				dirty = 1;
				break;
			case '/':
				search_mode = 1;
				query_buf[0] = '\0';
				search_cur = 0;
				dirty = 1;
				break;
			case KEY_ENTER: case '\n':
				play_track(selected);
				break;
			case KEY_UP:    case 'k': selected = prev_track(selected); break;
			case KEY_DOWN:  case 'j': selected = next_track(selected); break;
			case KEY_PPAGE: case 'u':
				selected = prev_group(selected, rows_h); break;
			case KEY_NPAGE: case 'd':
				selected = next_group(selected, rows_h); break;
			case 'o': /* open track's directory */
				if (selected >= 0 && selected < nview &&
				    view[selected].type == VIEW_TRACK) {
					const char *fpath =
						tracks[view[selected].track_idx].path;
					pid_t pid = fork();
					if (pid == 0) {
						setsid();
						freopen("/dev/null", "w", stdout);
						freopen("/dev/null", "w", stderr);
						freopen("/dev/null", "r", stdin);
						execlp("thunar", "thunar",
						       fpath, NULL);
						_exit(1);
					}
				}
				break;
			case 'a': {
				if (selected >= 0 && selected < nview &&
				    view[selected].type == VIEW_TRACK) {
					track_t *t = &tracks[view[selected].track_idx];
					strncpy(filter_col, "album", sizeof(filter_col)-1);
					strncpy(filter_val, t->album, sizeof(filter_val)-1);
					query_buf[0] = '\0';
					rebuild_view();
					selected = first_track();
					dirty = 1;
				}
				break;
			}
			case 'p':
				play_track(selected);
				break;
			case 's': {
				pid_t pid = fork();
				if (pid == 0) {
					execlp("qua-stop", "qua-stop", NULL);
					_exit(1);
				}
				break;
			}
			case 'q': goto quit;
			case 'c': {
				int ch2 = getch();
				if (ch2 == 'p' && selected >= 0 &&
				    selected < nview &&
				    view[selected].type == VIEW_TRACK) {
					copy_to_clipboard(
						tracks[view[selected].track_idx].path);
					dirty = 1;
				}
				break;
			}
			case KEY_HOME: selected = first_track(); break;
			case KEY_END:  selected = last_track();  break;
			case KEY_RESIZE:
				kitty_delete_all();
				clear();
				rows_h = LINES - 2;
				cur_art_path[0] = '\0';
				dirty = 1;
				break;
			default: break;
			}
		}

		if (selected < top)
			top = selected;
		else if (selected >= top + rows_h)
			top = selected - rows_h + 1;

		/* sticky header steals a row — tighten clamp */
		if (top > 0 && top < nview && view[top].type == VIEW_TRACK &&
		    selected >= top + rows_h - 1) {
			top = selected - rows_h + 2;
			if (top < 0) top = 0;
		}

		if (dirty || selected != prev || top != old_top)
			render(top, selected);
	}
quit:
	kitty_delete_all();
	endwin();
	save_state(selected);
	/* OS reclaims all resources on exit — skip slow cleanup */
	/* if (disk_db) sqlite3_close(disk_db); */
	/* sqlite3_close(mem_db); */
	/* free(tracks); */
	/* free(view); */
	return 0;
}
