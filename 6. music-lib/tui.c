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
#include <signal.h>
#include <fcntl.h>
#include <pthread.h>
#include <execinfo.h>
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
	char   audiomd5[33];
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
static char        filter_part_bufs[16][260];  /* artist OR parts — file scope for LTO */
/* info panel right-click targets (y range → db column + value) */
#define INFO_CLICK_MAX 6
#define INFO_PAD_ROWS  64
static struct {
	const char *db_col;
	char        val[256];
	int         y0, y1; /* pad/logical row range [y0, y1) when scrolling */
} info_click[INFO_CLICK_MAX];
static int n_info_click;
static int info_scroll;
static int info_height;
static int info_div_y;
static int info_visible;
static int last_info_selected = -1;
/* info pad: prefresh must run after refresh() or stdscr overwrites it */
static WINDOW *info_pad;
static int info_pad_sminrow, info_pad_smincol, info_pad_smaxrow, info_pad_smaxcol;
static volatile sig_atomic_t got_sigcont;
static void handle_sigcont(int sig) { (void)sig; got_sigcont = 1; }

static void sigchld_handler(int sig)
{
	(void)sig;
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
}

static void crash_handler(int sig)
{
	/* restore terminal before logging */
	endwin();

	void *frames[32];
	int n = backtrace(frames, 32);

	const char *home = getenv("HOME");
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/.config/qua-player/crash.log",
		 home ? home : "/tmp");
	int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd >= 0) {
		dprintf(fd, "--- signal %d ---\n", sig);
		backtrace_symbols_fd(frames, n, fd);
		dprintf(fd, "---\n");
		close(fd);
	}
	/* also dump to stderr */
	fprintf(stderr, "\nCrash (signal %d), backtrace in %s\n", sig, path);
	backtrace_symbols_fd(frames, n, STDERR_FILENO);

	signal(sig, SIG_DFL);
	raise(sig);
}
static sqlite3    *mem_db;
static sqlite3    *disk_db;
static char        cur_art_path[PATH_MAX];
static char        cache_dir[PATH_MAX];
static const char *g_home;

/* Run script in ~/.config/qua-player/tui-actions/<name>. No-op if missing. */
static void run_action(const char *name, const char *path)
{
	char script[PATH_MAX];
	snprintf(script, sizeof(script), "%s/.config/qua-player/tui-actions/%s",
		 g_home ? g_home : "", name);
	if (access(script, X_OK) != 0) return;
	pid_t pid = fork();
	if (pid == 0) {
		pid_t p2 = fork();
		if (p2 == 0) {
			int nul = open("/dev/null", O_RDWR);
			if (nul >= 0) {
				dup2(nul, STDIN_FILENO);
				dup2(nul, STDOUT_FILENO);
				dup2(nul, STDERR_FILENO);
				close(nul);
			}
			setsid();
			if (path)
				execl(script, name, path, (char *)NULL);
			else
				execl(script, name, (char *)NULL);
			_exit(1);
		}
		_exit(0);
	}
	if (pid > 0)
		waitpid(pid, NULL, 0);
}

static const char *get_no_art_path(void)
{
	static char buf[PATH_MAX];
	static int inited;
	if (!inited) {
		const char *home = getenv("HOME");
		if (home)
			snprintf(buf, sizeof(buf), "%s/.config/qua-player/no-art.png", home);
		else
			buf[0] = '\0';
		inited = 1;
	}
	return buf;
}

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
		"       cover_w,cover_h,audiomd5"
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
		S(audiomd5,    16);
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
		 * optionally restricted by path filter.
		 * bm25 weights: title=10, artist=5, album=10,
		 *   album_artist=5, date=1, path=0.5 */
		if (filter_col[0] && strcmp(filter_col, "path") == 0 &&
		    strchr(filter_val, '%')) {
			snprintf(sql, sizeof(sql),
				 "SELECT t.rowid FROM tracks t"
				 " JOIN tracks_fts f ON t.rowid = f.rowid"
				 " WHERE tracks_fts MATCH ? AND t.path LIKE ?"
				 " ORDER BY MIN(bm25(tracks_fts, 10.0, 5.0, 10.0,"
				 "   5.0, 1.0, 0.5))"
				 "   OVER (PARTITION BY t.album, t.album_artist),"
				 "   t.album, t.path, t.disc_num, t.track_num, t.title;");
			if (sqlite3_prepare_v2(disk_db, sql, -1, &st, NULL) != SQLITE_OK) {
				fprintf(stderr, "FTS5: %s\n", sqlite3_errmsg(disk_db));
				return;
			}
			sqlite3_bind_text(st, 1, match, -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(st, 2, filter_val, -1, SQLITE_STATIC);
		} else {
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
		}
	} else if (filter_col[0]) {
		/* artist/album_artist: OR match when multiple (find tracks with any) */
		int use_or = (strcmp(filter_col, "artist") == 0 ||
			      strcmp(filter_col, "album_artist") == 0) &&
			     (strchr(filter_val, ',') || strstr(filter_val, " / ") ||
			      strstr(filter_val, " & ") || strchr(filter_val, ';'));
		if (use_or) {
			char fcopy[256];
			strncpy(fcopy, filter_val, sizeof(fcopy) - 1);
			fcopy[sizeof(fcopy) - 1] = '\0';
			/* normalize separators: " / ", " & ", ";" -> "," */
			for (char *c = fcopy; *c; c++) {
				if (c[0] == ' ' && c[1] == '/' && c[2] == ' ') { c[0] = ','; c[1] = ' '; c[2] = ' '; }
				else if (c[0] == ' ' && c[1] == '&' && c[2] == ' ') { c[0] = ','; c[1] = ' '; c[2] = ' '; }
				else if (c[0] == ';') *c = ',';
			}
			const char *parts[16];
			int nparts = 0;
			for (char *tok = strtok(fcopy, ","); tok && nparts < 16;
			     tok = strtok(NULL, ",")) {
				while (*tok == ' ') tok++;
				char *end = tok + strlen(tok);
				while (end > tok && end[-1] == ' ') end--;
				*end = '\0';
				if (!*tok) continue;
				parts[nparts++] = tok;
			}
			/* need persistent storage for parts — they point into fcopy modified by strtok */
			for (int i = 0; i < nparts && i < 16; i++) {
				strncpy(filter_part_bufs[i], parts[i], sizeof(filter_part_bufs[0]) - 1);
				filter_part_bufs[i][sizeof(filter_part_bufs[0]) - 1] = '\0';
				parts[i] = filter_part_bufs[i];
			}
			if (nparts > 0) {
				char wh[640], full_sql[896];
				size_t wh_rem;
				char *p = wh;
				p += snprintf(p, sizeof(wh), " WHERE (");
				for (int i = 0; i < nparts; i++) {
					wh_rem = (size_t)(sizeof(wh) - (p - wh));
					if (wh_rem < 48) break;
					if (i) p += snprintf(p, wh_rem, " OR ");
					wh_rem = (size_t)(sizeof(wh) - (p - wh));
					if (wh_rem < 48) break;
					/* exact match (case-insensitive) — not substring */
					p += snprintf(p, wh_rem, "LOWER(%s) = LOWER(?)", filter_col);
				}
				wh_rem = (size_t)(sizeof(wh) - (p - wh));
				snprintf(p, wh_rem, " OR %s = ?)%s;", filter_col, order);
				snprintf(full_sql, sizeof(full_sql), "SELECT rowid FROM tracks%s", wh);
				if (getenv("QUA_DEBUG_FILTER")) {
					fprintf(stderr, "qua-tui filter: %s\n", full_sql);
					for (int i = 0; i < nparts; i++)
						fprintf(stderr, "  ?%d = '%s'\n", i+1, parts[i]);
					fprintf(stderr, "  ?%d = '%s'\n", nparts+1, filter_val);
				}
				if (sqlite3_prepare_v2(mem_db, full_sql, -1, &st, NULL) != SQLITE_OK)
					return;
				for (int i = 0; i < nparts; i++)
					sqlite3_bind_text(st, i + 1, parts[i], -1, SQLITE_STATIC);
				sqlite3_bind_text(st, nparts + 1, filter_val, -1, SQLITE_STATIC);
			} else {
				use_or = 0;
			}
		}
		if (!use_or) {
			/* LIKE for path (contains %), = otherwise */
			const char *op = strchr(filter_val, '%') ? "LIKE" : "=";
			snprintf(sql, sizeof(sql),
				 "SELECT rowid FROM tracks"
				 " WHERE %s %s ?%s;", filter_col, op, order);
			if (sqlite3_prepare_v2(mem_db, sql, -1, &st, NULL) != SQLITE_OK)
				return;
			sqlite3_bind_text(st, 1, filter_val, -1, SQLITE_STATIC);
		}
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
	if (nview == 0) return -1;
	if (from >= nview) from = nview - 1;
	if (from < 0) from = 0;
	for (int i = from + 1; i < nview; i++)
		if (view[i].type == VIEW_TRACK) return i;
	return from;
}

static int prev_track(int from)
{
	if (nview == 0) return -1;
	if (from >= nview) from = nview - 1;
	if (from < 0) return -1;
	for (int i = from - 1; i >= 0; i--)
		if (view[i].type == VIEW_TRACK) return i;
	return from;
}

static int first_track(void)
{
	for (int i = 0; i < nview; i++)
		if (view[i].type == VIEW_TRACK) return i;
	return -1;
}

static int next_group(int from, int page_size)
{
	if (nview == 0) return -1;
	if (from >= nview) from = nview - 1;
	if (from < 0) from = 0;
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
	if (nview == 0) return -1;
	if (from >= nview) from = nview - 1;
	if (from < 0) return -1;
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
	return -1;
}

static void play_track(int view_idx)
{
	if (view_idx < 0 || view_idx >= nview) return;
	if (view[view_idx].type != VIEW_TRACK) return;
	int idx = view[view_idx].track_idx;
	if (idx < 0 || idx >= ntracks) return;
	run_action("play", tracks[idx].path);
}

/* ------------------------------------------------------------------ */
/* Session state                                                       */
/* ------------------------------------------------------------------ */

static void save_state(int selected, int top)
{
	if (!disk_db) return;
	const char *path = "";
	if (selected >= 0 && selected < nview &&
	    view[selected].type == VIEW_TRACK) {
		int idx = view[selected].track_idx;
		if (idx >= 0 && idx < ntracks)
			path = tracks[idx].path;
	}

	char sc[8], sa[8], st_top[16];
	snprintf(sc, sizeof(sc), "%d", sort_col);
	snprintf(sa, sizeof(sa), "%d", sort_asc);
	snprintf(st_top, sizeof(st_top), "%d", top);

	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(disk_db,
	    "INSERT OR REPLACE INTO state(key,val) VALUES(?,?)",
	    -1, &st, NULL) != SQLITE_OK) return;

	const char *kv[][2] = {
		{"sort_col",  sc},
		{"sort_asc",  sa},
		{"query",     query_buf},
		{"sel_path",  path},
		{"top",       st_top},
	};
	for (int i = 0; i < 5; i++) {
		sqlite3_bind_text(st, 1, kv[i][0], -1, SQLITE_STATIC);
		sqlite3_bind_text(st, 2, kv[i][1], -1, SQLITE_STATIC);
		sqlite3_step(st);
		sqlite3_reset(st);
	}
	sqlite3_finalize(st);
}

static char saved_sel_path[512];

static void load_state(int *out_top)
{
	*out_top = -1;
	saved_sel_path[0] = '\0';
	if (!disk_db) return;
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(disk_db,
	    "SELECT key,val FROM state", -1, &st, NULL) != SQLITE_OK)
		return;

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
			strncpy(saved_sel_path, v, sizeof(saved_sel_path) - 1);
		} else if (strcmp(k, "top") == 0) {
			*out_top = atoi(v);
		}
	}
	sqlite3_finalize(st);
}

static int find_saved_track(void)
{
	if (*saved_sel_path && view && tracks) {
		for (int i = 0; i < nview; i++) {
			if (view[i].type == VIEW_TRACK) {
				int idx = view[i].track_idx;
				if (idx >= 0 && idx < ntracks &&
				    strcmp(tracks[idx].path, saved_sel_path) == 0)
					return i;
			}
		}
	}
	return first_track();
}

/* ------------------------------------------------------------------ */
/* Audio MD5: fork ffmpeg, hash decoded PCM (threaded)                 */
/* ------------------------------------------------------------------ */

struct md5_job {
	int   track_idx;
	char  hash[33];
};

static struct md5_job *md5_jobs;
static int             md5_njobs;
static _Atomic int     md5_next;

static void *md5_worker(void *arg)
{
	(void)arg;
	for (;;) {
		int i = md5_next++;
		if (i >= md5_njobs) break;

		track_t *t = &tracks[md5_jobs[i].track_idx];
		int pfd[2];
		if (pipe(pfd) < 0) continue;

		pid_t pid = fork();
		if (pid == 0) {
			close(pfd[0]);
			dup2(pfd[1], STDOUT_FILENO);
			close(pfd[1]);
			int nul = open("/dev/null", O_WRONLY);
			if (nul >= 0) {
				dup2(nul, STDERR_FILENO);
				close(nul);
			}
			execlp("ffmpeg", "ffmpeg",
			       "-loglevel", "error",
			       "-i", t->path,
			       "-vn", "-map", "0:a",
			       "-c:a", "pcm_s32le",
			       "-f", "md5", "-",
			       NULL);
			_exit(1);
		}

		close(pfd[1]);
		char buf[128] = "";
		ssize_t nr = read(pfd[0], buf, sizeof(buf) - 1);
		close(pfd[0]);
		if (nr > 0) buf[nr] = '\0';

		int status;
		waitpid(pid, &status, 0);

		char *eq = strchr(buf, '=');
		if (eq) {
			eq++;
			char *nl = strchr(eq, '\n');
			if (nl) *nl = '\0';
			if (strlen(eq) == 32)
				memcpy(md5_jobs[i].hash, eq, 33);
		}
	}
	return NULL;
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

static void render_track_list(int top, int selected, int list_x, int list_w, int rows_h)
{
	int widths[NCOLS];
	calc_widths(list_w - 1, widths); /* -1 for leading space */

	/* column header row */
	attron(COLOR_PAIR(3) | A_BOLD);
	move(0, list_x);
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
	for (int x = getcurx(stdscr); x < list_x + list_w; x++) addch(' ');
	attroff(COLOR_PAIR(3) | A_BOLD);

	/* find group header for the selected track */
	int sel_hdr = -1;
	if (nview > 0 && selected >= 0 && selected < nview) {
		for (int i = selected; i >= 0; i--) {
			if (view[i].type == VIEW_HEADER) { sel_hdr = i; break; }
		}
	}

	/* sticky group header: if the top row's group header is off-screen,
	 * reserve row 1 for it and shift data down by one */
	int sticky = 0;
	int sticky_hdr = -1;
	if (nview > 0 && top > 0 && top < nview && view[top].type == VIEW_TRACK) {
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
		move(1, list_x);
		attron(A_BOLD | COLOR_PAIR(pair));
		print_field(view[sticky_hdr].header, list_w - 1);
		attroff(A_BOLD | COLOR_PAIR(pair));
	}

	/* track/header rows */
	int y0 = sticky ? 1 : 0;
	for (int y = y0; y < rows_h; y++) {
		int idx = top + y - y0;
		move(y + 1, list_x);
		if (idx < 0 || idx >= nview) {
			for (int x = 0; x < list_w; x++) addch(' ');
			continue;
		}

		view_row_t *vr = &view[idx];

		if (vr->type == VIEW_HEADER) {
			int pair = (idx == sel_hdr) ? 4 : 1;
			attron(A_BOLD | COLOR_PAIR(pair));
			print_field(vr->header, list_w - 1);
			attroff(A_BOLD | COLOR_PAIR(pair));
		} else {
			if (idx == selected)
				attron(COLOR_PAIR(2) | A_BOLD);
			else
				attron(COLOR_PAIR(5));
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
			if (getcurx(stdscr) < list_x)
				move(getcury(stdscr), list_x);
			for (int x = getcurx(stdscr); x < list_x + list_w; x++) addch(' ');
			if (idx == selected)
				attroff(COLOR_PAIR(2) | A_BOLD);
			else
				attroff(COLOR_PAIR(5));
		}
	}
}

/* print_field for a window (used by info_line_pad) */
static void print_field_win(WINDOW *win, const char *s, int width)
{
	if (width <= 0) return;
	int disp = 0;
	mbstate_t mbs = {0};
	for (const char *p = s; *p;) {
		wchar_t wc;
		size_t n = mbrtowc(&wc, p, MB_CUR_MAX, &mbs);
		if (n == 0 || n == (size_t)-1 || n == (size_t)-2) break;
		int cw = wcwidth(wc); if (cw < 0) cw = 1;
		if (*(p+n) && disp + cw >= width) { waddch(win, '~'); disp++; break; }
		if (disp + cw > width) break;
		for (size_t i = 0; i < n; i++) waddch(win, (unsigned char)p[i]);
		disp += cw; p += n;
	}
	while (disp < width) { waddch(win, ' '); disp++; }
}

/* info line written to a pad; y is logical pad row, max_rows limits wrapping */
static int info_line_pad(WINDOW *pad, int y, int x, int w, int max_rows,
			const char *label, const char *val)
{
	if (w <= 0 || y >= max_rows) return 0;
	if (!val) val = "";
	int lbl_w = 9;
	if (lbl_w > w) lbl_w = w;
	int rem = w - lbl_w;
	if (rem <= 0) return 0;

	wmove(pad, y, x);
	wattron(pad, A_BOLD);
	print_field_win(pad, label, lbl_w);
	wattroff(pad, A_BOLD);

	const char *p = val;
	int cols = 0;
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
	wattron(pad, COLOR_PAIR(5));
	wprintw(pad, "%.*s", first_bytes, p);
	wattroff(pad, COLOR_PAIR(5));
	p += first_bytes;

	int rows = 1;
	while (*p && y + rows < max_rows) {
		wmove(pad, y + rows, x + lbl_w);
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
		wattron(pad, COLOR_PAIR(5));
		wprintw(pad, "%.*s", nbytes, p);
		wattroff(pad, COLOR_PAIR(5));
		p += nbytes;
		rows++;
	}
	return rows;
}

static void get_cell_size(int *cw, int *ch)
{
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
	    ws.ws_xpixel > 0 && ws.ws_col > 0 && ws.ws_row > 0) {
		*cw = ws.ws_xpixel / ws.ws_col;
		*ch = ws.ws_ypixel / ws.ws_row;
	} else {
		*cw = 8; *ch = 16;
	}
}

#define MIN_INFO_ROWS 10  /* reserve rows for scrollable info panel */

static void art_area_dim(int rw, int rows_h, int *out_aw, int *out_split)
{
	int cw, ch;
	get_cell_size(&cw, &ch);
	int max_aw = rw;
	/* reserve Info bar + minimum info rows; art gets the rest */
	int max_art_h = rows_h - 1 - MIN_INFO_ROWS;  /* 1 for Info divider */
	if (max_art_h < 1) max_art_h = 1;
	/* largest square that fits within max_art_h */
	int aw_by_w = max_aw, split_by_w = (aw_by_w * cw) / ch;
	int aw_by_h = (max_art_h * ch) / cw, split_by_h = max_art_h;
	int aw, split;
	if (split_by_w <= max_art_h) {
		aw = aw_by_w;
		split = split_by_w;
	} else {
		aw = aw_by_h <= max_aw ? aw_by_h : max_aw;
		split = aw_by_h <= max_aw ? split_by_h : (max_aw * cw) / ch;
	}
	if (aw < 1) aw = 1;
	if (split < 1) split = 1;
	if (split > rows_h - 1 - MIN_INFO_ROWS)
		split = rows_h - 1 - MIN_INFO_ROWS;
	if (split < 1) split = 1;
	*out_aw = aw;
	*out_split = split;
}

static void render_sidebar(int selected, int side_x, int side_w, int rows_h)
{
	int aw, split;
	art_area_dim(side_w, rows_h, &aw, &split);

	if (selected != last_info_selected) {
		info_scroll = 0;
		last_info_selected = selected;
	}

	/* blank art area — image renders on top via kitty direct placement */
	for (int y = 0; y < split; y++) {
		move(y, side_x);
		for (int x = 0; x < aw; x++) addch(' ');
	}

	/* -- Info divider -- */
	int div_y = split;
	attron(COLOR_PAIR(3) | A_BOLD);
	mvprintw(div_y, side_x, "%-*s", aw, " Info");
	attroff(COLOR_PAIR(3) | A_BOLD);
	for (int x = aw; x < aw; x++) addch(' ');  /* no extension needed */

	/* -- Info panel (bottom) — scrollable pad -- */
	info_div_y = div_y;
	info_visible = rows_h - div_y - 1;
	info_height = 0;
	n_info_click = 0;
	track_t *t = NULL;
	if (selected >= 0 && selected < nview &&
	    view[selected].type == VIEW_TRACK) {
		int idx = view[selected].track_idx;
		if (idx >= 0 && idx < ntracks)
			t = &tracks[idx];
	}
	char dur[16] = "", hz[16] = "", bd[16] = "";
	const char *fmt = "";
	if (t) {
		int s = (int)t->duration;
		snprintf(dur, sizeof(dur), "%d:%02d", s/60, s%60);
		snprintf(hz,  sizeof(hz),  "%d Hz",  t->sample_rate);
		snprintf(bd,  sizeof(bd),  "%d bit", t->bit_depth);
		fmt = strcmp(t->format,"wavpack")==0 ? "WavPack" :
		      strcmp(t->format,"flac")   ==0 ? "FLAC"    : t->format;
	}

	info_pad = NULL;
	WINDOW *ipad = newpad(INFO_PAD_ROWS, aw);
	if (ipad) {
		wbkgd(ipad, 0);
		werase(ipad);
		int y = 0, h;
		y += info_line_pad(ipad, y, 1, aw - 1, INFO_PAD_ROWS, "Title",     t && t->title[0] ? t->title : "");
		h = info_line_pad(ipad, y, 1, aw - 1, INFO_PAD_ROWS, "Artist",    t ? t->artist : "");
		if (h > 0 && t) { info_click[n_info_click] = (typeof(info_click[0])){"artist", "", y, y+h}; strncpy(info_click[n_info_click].val, t->artist, 255); n_info_click++; }
		y += h;
		h = info_line_pad(ipad, y, 1, aw - 1, INFO_PAD_ROWS, "Album",     t ? t->album : "");
		if (h > 0 && t) { info_click[n_info_click] = (typeof(info_click[0])){"album", "", y, y+h}; strncpy(info_click[n_info_click].val, t->album, 255); n_info_click++; }
		y += h;
		h = info_line_pad(ipad, y, 1, aw - 1, INFO_PAD_ROWS, "AlbumArt",  t ? t->album_artist : "");
		if (h > 0 && t) { info_click[n_info_click] = (typeof(info_click[0])){"album_artist", "", y, y+h}; strncpy(info_click[n_info_click].val, t->album_artist, 255); n_info_click++; }
		y += h;
		if (t && t->track_num > 0) {
			char tn[8]; snprintf(tn, sizeof(tn), "%d", t->track_num);
			y += info_line_pad(ipad, y, 1, aw - 1, INFO_PAD_ROWS, "Track",   tn);
		}
		y += info_line_pad(ipad, y, 1, aw - 1, INFO_PAD_ROWS, "Duration", dur);
		y += info_line_pad(ipad, y, 1, aw - 1, INFO_PAD_ROWS, "Format",   fmt);
		y += info_line_pad(ipad, y, 1, aw - 1, INFO_PAD_ROWS, "Rate",     hz);
		y += info_line_pad(ipad, y, 1, aw - 1, INFO_PAD_ROWS, "Depth",    bd);
		y += info_line_pad(ipad, y, 1, aw - 1, INFO_PAD_ROWS, "Date",     t ? t->date : "");
		h = info_line_pad(ipad, y, 1, aw - 1, INFO_PAD_ROWS, "Genre",    t ? t->genre : "");
		if (h > 0 && t) { info_click[n_info_click] = (typeof(info_click[0])){"genre", "", y, y+h}; strncpy(info_click[n_info_click].val, t->genre, 255); n_info_click++; }
		y += h;
		h = info_line_pad(ipad, y, 1, aw - 1, INFO_PAD_ROWS, "Path",     t ? t->path : "");
		if (h > 0 && t) { info_click[n_info_click] = (typeof(info_click[0])){"path", "", y, y+h}; strncpy(info_click[n_info_click].val, t->path, sizeof(info_click[0].val)-1); n_info_click++; }
		y += h;
		h = info_line_pad(ipad, y, 1, aw - 1, INFO_PAD_ROWS, "MD5",      t ? t->audiomd5 : "");
		if (h > 0 && t && n_info_click < INFO_CLICK_MAX) { info_click[n_info_click] = (typeof(info_click[0])){"audiomd5", "", y, y+h}; strncpy(info_click[n_info_click].val, t->audiomd5, 255); n_info_click++; }
		y += h;
		info_height = y;

		if (info_scroll > info_height - info_visible && info_height > info_visible)
			info_scroll = info_height - info_visible;
		if (info_scroll < 0) info_scroll = 0;

		/* defer prefresh until after refresh() — otherwise refresh overwrites pad */
		info_pad = ipad;
		info_pad_sminrow = div_y + 1;
		info_pad_smincol = side_x;
		info_pad_smaxrow = rows_h;  /* extend to last content row to avoid ghost on resize */
		info_pad_smaxcol = side_x + aw - 1;
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
		"\033[%d;%dH\033_Gq=2,a=p,i=%u,p=1,c=%d,r=%d,C=1\033\\",
		y + 1, x + 1, kitty_img_id, cols, rows);
	write(STDOUT_FILENO, cmd, n);
}

static void render(int top, int selected)
{
	int rows_h = LINES - 2;
	if (top < 0) top = 0;
	if (top >= nview && nview > 0) top = nview - 1;
	int sidebar_w = (COLS * 38) / 100;  /* left side: art+info */
	int aw, split;
	art_area_dim(sidebar_w, rows_h, &aw, &split);
	int list_x  = aw;                 /* track list starts after sidebar */
	int list_w  = COLS - aw;

	render_track_list(top, selected, list_x, list_w, rows_h);
	render_sidebar(selected, 0, sidebar_w, rows_h);

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
		printw("  [%d results]", ntracks_in_view);
	}
	else if (*query_buf)
		printw(" [filter:%s] %d results", query_buf, ntracks_in_view);
	else
		printw(" %d tracks", ntracks_in_view);
	for (int x = getcurx(stdscr); x < COLS - (int)strlen(keys); x++) addch(' ');
	move(LINES-1, COLS - (int)strlen(keys));
	printw("%s", keys);
	attroff(COLOR_PAIR(3) | A_BOLD);

	refresh();

	/* prefresh info pad after refresh — stdscr would overwrite it otherwise */
	if (info_pad) {
		prefresh(info_pad, info_scroll, 0,
			 info_pad_sminrow, info_pad_smincol,
			 info_pad_smaxrow, info_pad_smaxcol);
		delwin(info_pad);
		info_pad = NULL;
	}

	/* kitty: album art after ncurses flush */
	static int  art_cols;     /* actual cols image occupies */
	static int  art_rows;     /* actual rows image occupies */
	int art_y = 0;
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
			int idx = view[selected].track_idx;
			if (idx >= 0 && idx < ntracks) {
				track_t *t = &tracks[idx];
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
							strncpy(resolved, get_no_art_path(),
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
		}

		if (strcmp(resolved, cur_art_path) != 0) {
			unsigned int old_id = kitty_img_id;
			int had_old = *cur_art_path;

			strncpy(cur_art_path, resolved,
				sizeof(cur_art_path) - 1);
			cur_art_path[sizeof(cur_art_path) - 1] = '\0';

			/* transmit new image (old still visible) */
			if (*cur_art_path) {
				/* Cover art sizing: aspect-ratio + centering is intentional, not a bug.
				 * Do not change without explicit user request. */
				int iw = db_cw, ih = db_ch;
				art_cols = aw;
				art_rows = art_h;
				if ((iw <= 0 || ih <= 0) &&
				    img_dimensions(cur_art_path, &iw, &ih) != 0) {
					iw = 0; ih = 0;
				}
				if (iw > 0 && ih > 0) {
					int cw, ch;
					get_cell_size(&cw, &ch);
					int area_pw = aw * cw;
					int area_ph = art_h * ch;
					double s = (double)area_pw / iw;
					if ((double)area_ph / ih < s)
						s = (double)area_ph / ih;
					int disp_pw = (int)(iw * s);
					int disp_ph = (int)(ih * s);
					art_cols = (disp_pw + cw - 1) / cw;
					art_rows = (disp_ph + ch - 1) / ch;
					if (art_cols < 1) art_cols = 1;
					if (art_cols > aw) art_cols = aw;
					if (art_rows < 1) art_rows = 1;
					if (art_rows > art_h) art_rows = art_h;
				}
				kitty_transmit_png(cur_art_path);
				/* place new on top, then delete old (center when narrower than aw) */
				int art_x = art_cols < aw ? (aw - art_cols) / 2 : 0;
				kitty_place(art_y, art_x, art_cols, art_rows);
				if (had_old)
					kitty_delete(old_id);
			} else {
				/* no new art — just delete old */
				if (had_old)
					kitty_delete(old_id);
			}
		}

		/* restore cursor for ncurses */
		write(STDOUT_FILENO, "\0338", 2);
	}
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
	sqlite3_initialize();
	static char dbpath[PATH_MAX];
	const char *home = getenv("HOME");
	if (!home) { fprintf(stderr, "$HOME not set\n"); return 1; }
	g_home = home;
	snprintf(dbpath, sizeof(dbpath), DB_PATH_FMT, home);

	/* optional arg: folder path — index it first, then open TUI filtered there */
	char start_dir[PATH_MAX] = "";
	if (argc > 1) {
		char resolved[PATH_MAX];
		if (!realpath(argv[1], resolved)) {
			fprintf(stderr, "bad path: %s\n", argv[1]);
			return 1;
		}
		strncpy(start_dir, resolved, sizeof(start_dir) - 1);
	}

	signal(SIGSEGV, crash_handler);
	signal(SIGABRT, crash_handler);
	signal(SIGBUS, crash_handler);
	{
		struct sigaction sa = {{0}};
		sa.sa_handler = sigchld_handler;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = SA_NOCLDSTOP;
		sigaction(SIGCHLD, &sa, NULL);
	}

	/* when folder passed: index it first (add/update in db) */
	if (start_dir[0]) {
		pid_t pid = fork();
		if (pid == 0) {
			int nul = open("/dev/null", O_WRONLY);
			if (nul >= 0) {
				dup2(nul, STDOUT_FILENO);
				dup2(nul, STDERR_FILENO);
				close(nul);
			}
			execlp("qua-music-lib", "qua-music-lib", start_dir, (char *)NULL);
			_exit(127);
		}
		int st;
		waitpid(pid, &st, 0);
		if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
			fprintf(stderr, "qua-music-lib failed\n");
			return 1;
		}
	}

	/* ensure db exists and has tracks; run qua-music-lib when needed */
	for (;;) {
		if (access(dbpath, F_OK) != 0) {
			pid_t pid = fork();
			if (pid == 0) {
				int nul = open("/dev/null", O_WRONLY);
				if (nul >= 0) {
					dup2(nul, STDOUT_FILENO);
					dup2(nul, STDERR_FILENO);
					close(nul);
				}
				execlp("qua-music-lib", "qua-music-lib",
				       start_dir[0] ? start_dir : ".", (char *)NULL);
				_exit(127);
			}
			int st;
			waitpid(pid, &st, 0);
			if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
				fprintf(stderr, "qua-music-lib failed (install or check PATH)\n");
				return 1;
			}
			continue;
		}

		mem_db = open_memdb(dbpath);
		if (!mem_db) { fprintf(stderr, "cannot open %s\n", dbpath); return 1; }
		if (load_tracks() == 0)
			break;
		sqlite3_close(mem_db);
		mem_db = NULL;
		/* db exists but lacks tracks — reinit */
		pid_t pid = fork();
		if (pid == 0) {
			int nul = open("/dev/null", O_WRONLY);
			if (nul >= 0) {
				dup2(nul, STDOUT_FILENO);
				dup2(nul, STDERR_FILENO);
				close(nul);
			}
			execlp("qua-music-lib", "qua-music-lib",
			       start_dir[0] ? start_dir : ".", (char *)NULL);
			_exit(127);
		}
		int st;
		waitpid(pid, &st, 0);
		if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
			fprintf(stderr, "qua-music-lib failed\n");
			return 1;
		}
	}

	/* open disk DB (state/FTS) — only after we have a valid db with tracks */
	if (sqlite3_open_v2(dbpath, &disk_db,
	    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) == SQLITE_OK) {
		sqlite3_exec(disk_db,
			"CREATE TABLE IF NOT EXISTS state ("
			"key TEXT PRIMARY KEY, val TEXT);",
			NULL, NULL, NULL);
	} else {
		disk_db = NULL;
	}

	snprintf(cache_dir, sizeof(cache_dir),
		 "%s/.cache/qua-music-tui", home);
	mkdir(cache_dir, 0755);

	view = malloc(MAX_VIEW * sizeof(view_row_t));
	if (!view) return 1;

	int top = 0;
	if (!start_dir[0]) {
		load_state(&top);  /* restore sort/query from last session */
	} else {
		strncpy(filter_col, "path", sizeof(filter_col) - 1);
		snprintf(filter_val, sizeof(filter_val), "%s%%", start_dir);
	}
	rebuild_view();

	setlocale(LC_ALL, "");
	initscr(); noecho(); cbreak();
	keypad(stdscr, TRUE); curs_set(0);
	scrollok(stdscr, FALSE);
	idlok(stdscr, FALSE);
	start_color(); use_default_colors();
	init_pair(1, COLOR_MAGENTA, -1);          /* group headers (pink on default) */
	init_pair(2, COLOR_YELLOW, 0);           /* selection cursor (bright yellow on black) */
	init_pair(3, COLOR_CYAN, 0);              /* top/bottom bars (cyan on black) */
	init_pair(4, COLOR_MAGENTA, -1);          /* active group header */
	init_pair(5, COLOR_YELLOW, -1);           /* regular track rows */
	mousemask(BUTTON1_CLICKED | BUTTON3_CLICKED | BUTTON4_PRESSED | BUTTON5_PRESSED, NULL);
	mouseinterval(0);
	int selected = find_saved_track();
	int rows_h   = LINES - 2;

	/* center viewport on restored selection */
	if (selected > rows_h / 2)
		top = selected - rows_h / 2;

	signal(SIGCONT, handle_sigcont);
	render(top, selected);

	for (;;) {
		int ch = getch();
		if (ch == ERR) {
			/* interrupted by signal (SIGCONT) */
			if (got_sigcont) {
				got_sigcont = 0;
				clearok(stdscr, TRUE);
				cur_art_path[0] = '\0';
				render(top, selected);
			}
			continue;
		}
		int prev = selected, old_top = top;
		int dirty = 0;

		/* mouse works in both normal and search mode */
		if (ch == KEY_MOUSE) {
			MEVENT ev;
			if (getmouse(&ev) == OK) {
				int rw = (COLS * 38) / 100;
				int aaw, asplit;
				art_area_dim(rw, LINES - 2, &aaw, &asplit);
				if (ev.x < aaw && ev.y > asplit &&
				    (ev.bstate & (BUTTON4_PRESSED | BUTTON5_PRESSED))) {
					/* scroll info panel */
					int max_scroll = info_height - info_visible;
					if (max_scroll > 0) {
						if (ev.bstate & BUTTON4_PRESSED)
							info_scroll = info_scroll > 3 ? info_scroll - 3 : 0;
						else
							info_scroll = info_scroll + 3 < max_scroll ? info_scroll + 3 : max_scroll;
						dirty = 1;
					}
				} else if (ev.x >= aaw && (ev.bstate & BUTTON4_PRESSED))
					selected = prev_track(selected);
				else if (ev.x >= aaw && (ev.bstate & BUTTON5_PRESSED))
					selected = next_track(selected);
				else if (ev.y == 0 &&
				 (ev.bstate & BUTTON1_CLICKED) && ev.x >= aaw) {
					int lw = COLS - aaw;
					int c = col_at_x(ev.x - aaw, lw);
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
					   ev.x >= aaw) {
					int has_sticky = top > 0 && top < nview &&
						view[top].type == VIEW_TRACK;
					int idx = top + (ev.y - 1) - has_sticky;
					if (idx >= 0 && idx < nview &&
					    view[idx].type == VIEW_TRACK) {
						if (ev.bstate & BUTTON3_CLICKED) {
							selected = idx;
							play_track(selected);
						} else if (ev.bstate & BUTTON1_CLICKED) {
							selected = idx;
						}
					}
				} else if ((ev.bstate & BUTTON1_CLICKED) &&
					   ev.x < aaw) {
					if (ev.y >= 0 && ev.y < asplit &&
					    selected >= 0 && selected < nview &&
					    view[selected].type == VIEW_TRACK &&
					    !tracks[view[selected].track_idx].cover[0]) {
						/* left-click on empty art: backfill */
						track_t *t = &tracks[view[selected].track_idx];
						pid_t pid = fork();
						if (pid == 0) {
							int nul = open("/dev/null", O_RDWR);
							if (nul >= 0) {
								dup2(nul, STDIN_FILENO);
								dup2(nul, STDOUT_FILENO);
								dup2(nul, STDERR_FILENO);
								close(nul);
							}
							execlp("qua-music-lib", "qua-music-lib",
							       "-i", t->path, (char *)NULL);
							_exit(127);
						} else if (pid > 0) {
							int st;
							waitpid(pid, &st, 0);
							clearok(stdscr, TRUE);
						}
						if (disk_db) {
							sqlite3_stmt *q;
							if (sqlite3_prepare_v2(disk_db,
							    "SELECT cover, cover_w, cover_h"
							    " FROM tracks WHERE path=?;",
							    -1, &q, NULL) == SQLITE_OK) {
								sqlite3_bind_text(q, 1, t->path, -1, SQLITE_STATIC);
								if (sqlite3_step(q) == SQLITE_ROW) {
									const char *cv = (const char *)sqlite3_column_text(q, 0);
									if (cv) strncpy(t->cover, cv, sizeof(t->cover) - 1);
									t->cover_w = sqlite3_column_int(q, 1);
									t->cover_h = sqlite3_column_int(q, 2);
								}
								sqlite3_finalize(q);
							}
						}
						cur_art_path[0] = '\0';
						dirty = 1;
					}
				} else if ((ev.bstate & BUTTON3_CLICKED) &&
					   ev.x < aaw) {
					/* right-click on info field: filter */
					int handled = 0;
					int irx = 0;
					int irw = aaw;
					int lbl_w = 9;
					int val_x = irx + 1 + lbl_w;
					int val_w = irw - 1 - lbl_w;
					int pad_row = ev.y > asplit ? info_scroll + (ev.y - asplit - 1) : -1;
					for (int i = 0; i < n_info_click && pad_row >= 0; i++) {
						if (pad_row >= info_click[i].y0 &&
						    pad_row < info_click[i].y1) {
							if (strcmp(info_click[i].db_col, "audiomd5") == 0) {
								if (!info_click[i].val[0]) {
									/* empty: compute MD5 */
									if (selected >= 0 && selected < nview &&
									    view[selected].type == VIEW_TRACK) {
										track_t *mt = &tracks[view[selected].track_idx];
										md5_jobs = malloc(sizeof(*md5_jobs));
										md5_jobs[0].track_idx = view[selected].track_idx;
										md5_jobs[0].hash[0] = '\0';
										md5_njobs = 1;
										md5_next = 0;
										md5_worker(NULL);
										if (md5_jobs[0].hash[0]) {
											memcpy(mt->audiomd5, md5_jobs[0].hash, 33);
											if (disk_db) {
												sqlite3_stmt *u;
												if (sqlite3_prepare_v2(disk_db,
												    "UPDATE tracks SET audiomd5=? WHERE path=?;",
												    -1, &u, NULL) == SQLITE_OK) {
													sqlite3_bind_text(u, 1, mt->audiomd5, -1, SQLITE_STATIC);
													sqlite3_bind_text(u, 2, mt->path, -1, SQLITE_STATIC);
													sqlite3_step(u);
													sqlite3_finalize(u);
												}
											}
											if (mem_db) {
												sqlite3_stmt *u;
												if (sqlite3_prepare_v2(mem_db,
												    "UPDATE tracks SET audiomd5=? WHERE path=?;",
												    -1, &u, NULL) == SQLITE_OK) {
													sqlite3_bind_text(u, 1, mt->audiomd5, -1, SQLITE_STATIC);
													sqlite3_bind_text(u, 2, mt->path, -1, SQLITE_STATIC);
													sqlite3_step(u);
													sqlite3_finalize(u);
												}
											}
										}
										free(md5_jobs);
										dirty = 1;
									}
								} else {
									/* non-empty: filter by MD5 */
									strncpy(filter_col, "audiomd5", sizeof(filter_col)-1);
									strncpy(filter_val, info_click[i].val, sizeof(filter_val)-1);
									copy_to_clipboard(filter_val);
									query_buf[0] = '\0';
									rebuild_view();
									selected = first_track();
									top = 0;
									dirty = 1;
								}
								handled = 1;
								break;
							}
							if (!info_click[i].val[0]) continue;
							if (strcmp(info_click[i].db_col, "path") == 0 && val_w > 0) {
								/* map click to char index in path */
								int ci = (pad_row - info_click[i].y0) * val_w
									+ (ev.x - val_x);
								const char *p = info_click[i].val;
								int plen = strlen(p);
								if (ci < 0) ci = 0;
								if (ci >= plen) ci = plen - 1;
								/* find next '/' at or after click, then walk back to segment start */
								int cut = ci;
								while (cut < plen && p[cut] != '/') cut++;
								if (cut >= plen) cut = plen - 1;
								while (cut > 0 && p[cut] != '/') cut--;
								if (cut <= 0) break;
								/* include slash in prefix so "path LIKE prefix/%" matches subpath */
								int n = (cut < plen && p[cut] == '/') ? cut + 1 : cut;
								strncpy(filter_col, "path", sizeof(filter_col)-1);
								snprintf(filter_val, sizeof(filter_val),
									 "%.*s%%", n, p);
							} else {
								strncpy(filter_col, info_click[i].db_col, sizeof(filter_col)-1);
								strncpy(filter_val, info_click[i].val, sizeof(filter_val)-1);
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
					if (!handled && ev.y >= 0 && ev.y < asplit &&
					    *cur_art_path &&
					    selected >= 0 && selected < nview &&
					    view[selected].type == VIEW_TRACK) {
						track_t *t = &tracks[view[selected].track_idx];
						char adir[PATH_MAX], src[PATH_MAX] = "";
						path_dir(t->path, adir, sizeof(adir));
						find_cover_art(adir, src, sizeof(src));
						if (*src)
							run_action("open-image", src);
					}
				}
			}
		} else if (search_mode) {
			/* search mode: keys go to query buffer,
			 * arrows scroll playlist / move cursor */
			switch (ch) {
			case 27: case 12: /* ESC / Ctrl+L — cancel search, clear */
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
				if (!query_buf[0]) {
					query_buf[0] = '\0';
					filter_col[0] = '\0';
					filter_val[0] = '\0';
					rebuild_view();
					selected = first_track();
					top = 0;
				}
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
					if (strlen(query_buf) >= 3 || !query_buf[0]) {
						rebuild_view();
						selected = first_track(); top = 0;
					}
					dirty = 1;
				}
				break;
			}
			case KEY_RESIZE:
				kitty_delete_all();
				resize_term(LINES, COLS);
				clearok(stdscr, TRUE);
				rows_h = LINES - 2;
				cur_art_path[0] = '\0';
				dirty = 1;
				render(top, selected);
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
			case 27: case 12: /* ESC / Ctrl+L — clear filters, q to quit */
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
				filter_col[0] = '\0';
				filter_val[0] = '\0';
				dirty = 1;
				break;
			case KEY_ENTER: case '\n': case KEY_RIGHT:
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
				    view[selected].type == VIEW_TRACK)
					run_action("open-dir",
					    tracks[view[selected].track_idx].path);
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
			case 's':
				run_action("stop", NULL);
				break;
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
			case 'm': {
				if (selected < 0 || selected >= nview ||
				    view[selected].type != VIEW_TRACK) break;
				track_t *t = &tracks[view[selected].track_idx];
				md5_jobs = malloc(sizeof(*md5_jobs));
				md5_jobs[0].track_idx = view[selected].track_idx;
				md5_jobs[0].hash[0] = '\0';
				md5_njobs = 1;
				md5_next = 0;
				md5_worker(NULL);
				if (md5_jobs[0].hash[0]) {
					memcpy(t->audiomd5, md5_jobs[0].hash, 33);
					if (disk_db) {
						sqlite3_stmt *u;
						if (sqlite3_prepare_v2(disk_db,
						    "UPDATE tracks SET audiomd5=?"
						    " WHERE path=?;",
						    -1, &u, NULL) == SQLITE_OK) {
							sqlite3_bind_text(u, 1, t->audiomd5, -1, SQLITE_STATIC);
							sqlite3_bind_text(u, 2, t->path, -1, SQLITE_STATIC);
							sqlite3_step(u);
							sqlite3_finalize(u);
						}
					}
					if (mem_db) {
						sqlite3_stmt *u;
						if (sqlite3_prepare_v2(mem_db,
						    "UPDATE tracks SET audiomd5=?"
						    " WHERE path=?;",
						    -1, &u, NULL) == SQLITE_OK) {
							sqlite3_bind_text(u, 1, t->audiomd5, -1, SQLITE_STATIC);
							sqlite3_bind_text(u, 2, t->path, -1, SQLITE_STATIC);
							sqlite3_step(u);
							sqlite3_finalize(u);
						}
					}
				}
				free(md5_jobs);
				dirty = 1;
				break;
			}
			case KEY_HOME: selected = first_track(); break;
			case KEY_END:  selected = last_track();  break;
			case KEY_RESIZE:
				kitty_delete_all();
				resize_term(LINES, COLS);
				clearok(stdscr, TRUE);
				rows_h = LINES - 2;
				cur_art_path[0] = '\0';
				dirty = 1;
				render(top, selected);
				break;
			default: break;
			}
		}

		if (selected >= 0) {
			if (selected < top)
				top = selected;
			else if (selected >= top + rows_h)
				top = selected - rows_h + 1;
		}
		if (top < 0) top = 0;

		/* sticky header steals a row — tighten clamp */
		if (nview > 0 && top > 0 && top < nview && view[top].type == VIEW_TRACK &&
		    selected >= top + rows_h - 1) {
			top = selected - rows_h + 2;
			if (top < 0) top = 0;
		}

		if (dirty || selected != prev || top != old_top)
			render(top, selected);
	}
quit:
	endwin();           /* restore terminal first so user gets shell back */
	fflush(stdout);
	kitty_delete_all();
	save_state(selected, top);
	/* OS reclaims all resources on exit — skip slow cleanup */
	/* if (disk_db) sqlite3_close(disk_db); */
	/* sqlite3_close(mem_db); */
	/* free(tracks); */
	/* free(view); */
	return 0;
}
