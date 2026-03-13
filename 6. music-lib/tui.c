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
#include <sqlite3.h>
#include <ncurses.h>

#define DB_PATH       "/home/free2/code/musl2gcc/6. music-lib/music.db"
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
	/* label       db_col          min_w  weight  visible  group  getter       */
	{ "#",        "track_num",       3,    0,      1,       0,    g_tracknum   },
	{ "Title",    "title",           0,    3,      1,       0,    g_title      },
	{ "Artist",   "artist",          0,    2,      1,       1,    g_artist     },
	{ "Album",    "album",           0,    2,      0,       1,    g_album      },
	{ "AlbumArt", "album_artist",    0,    1,      0,       1,    g_albumart   },
	{ "Dur",      "duration",        5,    0,      1,       0,    g_duration   },
	{ "Fmt",      "format",          4,    0,      1,       1,    g_format     },
	{ "Hz",       "sample_rate",     5,    0,      1,       0,    g_samplerate },
	{ "Bt",       "bit_depth",       2,    0,      1,       0,    g_bitdepth   },
	{ "Date",     "date",            10,   0,      0,       1,    g_date       },
	{ "Genre",    "genre",           0,    1,      0,       1,    g_genre      },
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
static int         sort_col = 3;   /* default: sort by album */
static int         sort_asc = 1;
static sqlite3    *mem_db;

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

	sqlite3_stmt *st;
	const char *sql =
		"SELECT path,format,sample_rate,bit_depth,duration,"
		"       title,artist,album,album_artist,track_num,disc_num,date,genre"
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
#undef S
	}
	sqlite3_finalize(st);
	return 0;
}

/* ------------------------------------------------------------------ */
/* View rebuild                                                        */
/* rebuild_view() queries mem_db with current query_buf+sort,            */
/* maps rowids back to tracks[] indices, inserts synthetic headers.   */
/* ------------------------------------------------------------------ */

/* build a LIKE pattern from query_buf word into buf: '%word%' */
static void like_pat(const char *word, char *buf, int n)
{
	snprintf(buf, n, "%%%s%%", word);
}

static void rebuild_view(void)
{
	nview = 0;

	/* build WHERE clause from query_buf words */
	char where[1024] = "";
	char pat[QUERY_MAX + 4];
	/* split query_buf by spaces, each word must match at least one field */
	char fcopy[QUERY_MAX];
	strncpy(fcopy, query_buf, sizeof(fcopy)-1);
	char *words[32];
	int   nwords = 0;
	for (char *tok = strtok(fcopy, " "); tok && nwords < 32; tok = strtok(NULL, " "))
		words[nwords++] = tok;

	if (nwords > 0) {
		strcpy(where, " WHERE");
		for (int w = 0; w < nwords; w++) {
			if (w > 0) strcat(where, " AND");
			strcat(where,
			       " (title LIKE ? OR artist LIKE ? OR album LIKE ?"
			       "  OR album_artist LIKE ? OR genre LIKE ?)");
		}
	}

	/* ORDER BY */
	char order[128];
	const char *col = col_defs[sort_col].db_col;
	snprintf(order, sizeof(order),
	         " ORDER BY COALESCE(NULLIF(%s,''),'~') %s,"
	         " COALESCE(NULLIF(album,''),'~'), disc_num, track_num, title",
	         col ? col : "album",
	         sort_asc ? "ASC" : "DESC");

	char sql[2048];
	snprintf(sql, sizeof(sql),
	         "SELECT rowid FROM tracks%s%s;", where, order);

	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(mem_db, sql, -1, &st, NULL) != SQLITE_OK) return;

	/* bind LIKE patterns */
	int param = 1;
	for (int w = 0; w < nwords; w++) {
		like_pat(words[w], pat, sizeof(pat));
		for (int f = 0; f < 5; f++)   /* 5 fields per word */
			sqlite3_bind_text(st, param++, pat, -1, SQLITE_TRANSIENT);
	}

	/* walk results, insert headers when group value changes */
	char cur_group[256] = "";
	char gbuf[256];

	while (sqlite3_step(st) == SQLITE_ROW && nview + 2 < MAX_VIEW) {
		int rowid   = sqlite3_column_int(st, 0);
		int idx     = rowid - 1;  /* tracks[] is 0-based, rowid is 1-based */
		if (idx < 0 || idx >= ntracks) continue;
		track_t *t  = &tracks[idx];

		/* group header */
		if (col_defs[sort_col].group) {
			col_defs[sort_col].get(t, gbuf, sizeof(gbuf));
			if (strcmp(gbuf, cur_group) != 0) {
				view_row_t *vr = &view[nview++];
				vr->type = VIEW_HEADER;
				strncpy(vr->header, *gbuf ? gbuf : "(untagged)",
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

	for (int c = 0; c < NCOLS; c++) {
		if (!col_defs[c].visible) continue;
		if (col_defs[c].weight == 0)
			widths[c] = col_defs[c].min_w;
		else
			widths[c] = total_weight > 0 ? (rem * col_defs[c].weight) / total_weight : 0;
	}
}

static void render_left(int top, int selected, int lw, int rows_h)
{
	int widths[NCOLS];
	calc_widths(lw - 1, widths); /* -1 for leading space */

	/* column header row */
	attron(A_REVERSE | A_BOLD);
	move(0, 0);
	addch(' ');
	int first = 1;
	for (int c = 0; c < NCOLS; c++) {
		if (!col_defs[c].visible) continue;
		if (!first) addch(' ');
		first = 0;
		/* mark sort column */
		if (c == sort_col) {
			attron(A_UNDERLINE);
			print_field(col_defs[c].label, widths[c]);
			attroff(A_UNDERLINE);
		} else {
			print_field(col_defs[c].label, widths[c]);
		}
	}
	for (int x = getcurx(stdscr); x < lw; x++) addch(' ');
	attroff(A_REVERSE | A_BOLD);

	/* track/header rows */
	for (int y = 0; y < rows_h; y++) {
		int idx = top + y;
		move(y + 1, 0);
		if (idx >= nview) { clrtoeol(); continue; }

		view_row_t *vr = &view[idx];

		if (vr->type == VIEW_HEADER) {
			attron(A_BOLD | COLOR_PAIR(1));
			addch(' ');
			print_field(vr->header, lw - 1);
			attroff(A_BOLD | COLOR_PAIR(1));
		} else {
			if (idx == selected) attron(A_REVERSE);
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
				print_field(buf, widths[c]);
			}
			if (idx == selected) attroff(A_REVERSE);
		}
	}
}

static void info_line(int y, int x, int w, const char *label, const char *val)
{
	if (y >= LINES - 1 || !val || !*val) return;
	attron(A_BOLD);
	mvprintw(y, x, "%-10s", label);
	attroff(A_BOLD);
	move(y, x + 10);
	print_field(val, w - 10);
}

static void render_right(int selected, int rx, int rw, int rows_h)
{
	attron(A_REVERSE | A_BOLD);
	mvprintw(0, rx, "%-*s", rw, " Info");
	attroff(A_REVERSE | A_BOLD);

	for (int y = 1; y <= rows_h; y++) mvprintw(y, rx, "%-*s", rw, "");

	if (selected < 0 || selected >= nview) return;
	if (view[selected].type != VIEW_TRACK) return;

	track_t *t = &tracks[view[selected].track_idx];
	char dur[16], hz[16], bd[16];
	int s = (int)t->duration;
	snprintf(dur, sizeof(dur), "%d:%02d", s/60, s%60);
	snprintf(hz,  sizeof(hz),  "%d Hz",  t->sample_rate);
	snprintf(bd,  sizeof(bd),  "%d bit", t->bit_depth);
	const char *fmt = strcmp(t->format,"wavpack")==0 ? "WavPack" :
	                  strcmp(t->format,"flac")   ==0 ? "FLAC"    : t->format;

	int y = 1;
	attron(COLOR_PAIR(1) | A_BOLD);
	mvprintw(y++, rx, " %-*s", rw-1, *t->title ? t->title : "(no title)");
	attroff(COLOR_PAIR(1) | A_BOLD);
	y++;
	info_line(y++, rx+1, rw-1, "Artist",    t->artist);
	info_line(y++, rx+1, rw-1, "Album",     t->album);
	info_line(y++, rx+1, rw-1, "AlbumArt",  t->album_artist);
	y++;
	if (t->track_num > 0) {
		char tn[8]; snprintf(tn, sizeof(tn), "%d", t->track_num);
		info_line(y++, rx+1, rw-1, "Track",   tn);
	}
	info_line(y++, rx+1, rw-1, "Duration",  dur);
	info_line(y++, rx+1, rw-1, "Format",    fmt);
	info_line(y++, rx+1, rw-1, "Rate",      hz);
	info_line(y++, rx+1, rw-1, "Depth",     bd);
	info_line(y++, rx+1, rw-1, "Date",      t->date);
	info_line(y++, rx+1, rw-1, "Genre",     t->genre);
	y++;
	info_line(y,   rx+1, rw-1, "Path",      t->path);
}

static void render(int top, int selected)
{
	int rows_h = LINES - 2;
	int lw     = (COLS * 62) / 100;
	int rw     = COLS - lw - 1;
	int rx     = lw + 1;

	render_left(top, selected, lw, rows_h);
	render_right(selected, rx, rw, rows_h);

	/* divider */
	mvaddch(0, lw, ACS_TTEE);
	for (int y = 1; y <= rows_h; y++) mvaddch(y, lw, ACS_VLINE);

	/* status / query_buf bar */
	attron(A_REVERSE);
	int ntracks_in_view = 0;
	for (int i = 0; i < nview; i++)
		if (view[i].type == VIEW_TRACK) ntracks_in_view++;
	if (*query_buf)
		mvprintw(LINES-1, 0, " > %s  [%d results]  ESC: clear", query_buf, ntracks_in_view);
	else
		mvprintw(LINES-1, 0, " > _   %d tracks  |  arrows/PgUp/PgDn  Enter: play  ESC: quit",
		         ntracks_in_view);
	clrtoeol();
	attroff(A_REVERSE);

	refresh();
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
	const char *dbpath = argc > 1 ? argv[1] : DB_PATH;

	mem_db = open_memdb(dbpath);
	if (!mem_db) return 1;

	if (load_tracks() != 0) { fprintf(stderr, "load_tracks failed\n"); return 1; }

	view = malloc(MAX_VIEW * sizeof(view_row_t));
	if (!view) return 1;

	rebuild_view();

	setlocale(LC_ALL, "");
	initscr(); noecho(); cbreak();
	keypad(stdscr, TRUE); curs_set(0);
	start_color(); use_default_colors();
	init_pair(1, COLOR_CYAN, -1);
	mousemask(BUTTON1_CLICKED | BUTTON4_PRESSED | BUTTON5_PRESSED, NULL);
	mouseinterval(0);

	int top      = 0;
	int selected = first_track();
	int rows_h   = LINES - 2;

	render(top, selected);

	for (;;) {
		int ch = getch();
		int prev = selected, old_top = top;
		int dirty = 0;

		switch (ch) {
		case 27: /* ESC */
			if (*query_buf) {
				query_buf[0] = '\0';
				rebuild_view();
				selected = first_track();
				top = 0;
				dirty = 1;
			} else {
				goto quit;
			}
			break;
		case KEY_ENTER: case '\n':
			play_track(selected);
			break;
		case KEY_UP:    case 'k': selected = prev_track(selected); break;
		case KEY_DOWN:  case 'j': selected = next_track(selected); break;
		case KEY_PPAGE: case 'u': {
			int i = selected;
			for (int n = rows_h; n > 0; n--) i = prev_track(i);
			selected = i; break;
		}
		case KEY_NPAGE: case 'd': {
			int i = selected;
			for (int n = rows_h; n > 0; n--) i = next_track(i);
			selected = i; break;
		}
		case KEY_HOME: selected = first_track(); break;
		case KEY_END:  selected = last_track();  break;
		case KEY_RESIZE: rows_h = LINES - 2; dirty = 1; break;
		case KEY_BACKSPACE: case 127: {
			int len = strlen(query_buf);
			if (len > 0) {
				query_buf[len-1] = '\0';
				rebuild_view();
				selected = first_track(); top = 0;
				dirty = 1;
			}
			break;
		}
		case KEY_MOUSE: {
			MEVENT ev;
			if (getmouse(&ev) == OK) {
				if (ev.bstate & BUTTON4_PRESSED)
					selected = prev_track(selected);
				else if (ev.bstate & BUTTON5_PRESSED)
					selected = next_track(selected);
				else if ((ev.bstate & BUTTON1_CLICKED) &&
				         ev.y >= 1 && ev.y < LINES - 1) {
					int idx = top + (ev.y - 1);
					if (idx >= 0 && idx < nview && view[idx].type == VIEW_TRACK) {
						if (idx == selected) play_track(selected);
						else selected = idx;
					}
				}
			}
			break;
		}
		default:
			/* fzf: any printable key appends to query_buf */
			if (ch >= 32 && ch < 127) {
				int len = strlen(query_buf);
				if (len < QUERY_MAX - 1) {
					query_buf[len]   = (char)ch;
					query_buf[len+1] = '\0';
					rebuild_view();
					selected = first_track(); top = 0;
					dirty = 1;
				}
			}
			break;
		}

		if (selected < top)
			top = selected;
		else if (selected >= top + rows_h)
			top = selected - rows_h + 1;

		if (dirty || selected != prev || top != old_top)
			render(top, selected);
	}
quit:
	endwin();
	sqlite3_close(mem_db);
	free(tracks);
	free(view);
	return 0;
}
