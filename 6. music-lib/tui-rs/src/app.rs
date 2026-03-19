/* App state and main run loop. */

use crate::art;
use crate::db::Track;
use crate::ui::{self, SidebarClick, SidebarState, TrackListState};
use crate::view::{self, ViewRow};
use crossterm::{
    event::{self, DisableBracketedPaste, DisableMouseCapture, EnableBracketedPaste, EnableMouseCapture, Event, KeyCode, KeyEventKind, KeyModifiers, MouseButton, MouseEventKind},
    execute,
    terminal::{disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen},
};
use ratatui::prelude::*;
use std::collections::HashMap;
use std::io::{self, stdout};
use std::path::Path;
use std::process::Command;

/// Substring search mode (Ctrl+F, tab to cycle). Exact mode = info-panel click.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FindMode {
    Everything,
    Artist,   /* artist + album_artist */
    Album,
    Track,   /* title */
    Genre,
}

impl FindMode {
    fn as_str(self) -> &'static str {
        match self {
            FindMode::Everything => "everything",
            FindMode::Artist => "artist",
            FindMode::Album => "album",
            FindMode::Track => "track",
            FindMode::Genre => "genre",
        }
    }
    fn next(self) -> Self {
        match self {
            FindMode::Everything => FindMode::Artist,
            FindMode::Artist => FindMode::Album,
            FindMode::Album => FindMode::Track,
            FindMode::Track => FindMode::Genre,
            FindMode::Genre => FindMode::Everything,
        }
    }
    fn prev(self) -> Self {
        match self {
            FindMode::Everything => FindMode::Genre,
            FindMode::Artist => FindMode::Everything,
            FindMode::Album => FindMode::Artist,
            FindMode::Track => FindMode::Album,
            FindMode::Genre => FindMode::Track,
        }
    }
}

fn field_contains(t: &Track, word: &str) -> bool {
    let w = word.to_lowercase();
    let f = |s: &str| s.to_lowercase().contains(&w);
    f(&t.path)
        || f(&t.title)
        || f(&t.artist)
        || f(&t.album)
        || f(&t.album_artist)
        || f(&t.format)
        || f(&t.date)
        || f(&t.genre)
        || f(&t.cover)
        || f(&t.duration.to_string())
        || f(&t.sample_rate.to_string())
        || f(&t.bit_depth.to_string())
        || f(&t.track_num.to_string())
}

fn field_contains_mode(t: &Track, word: &str, mode: FindMode) -> bool {
    let w = word.to_lowercase();
    let f = |s: &str| s.to_lowercase().contains(&w);
    match mode {
        FindMode::Everything => field_contains(t, word),
        FindMode::Artist => f(&t.artist) || f(&t.album_artist),
        FindMode::Album => f(&t.album),
        FindMode::Track => f(&t.title),
        FindMode::Genre => f(&t.genre),
    }
}

fn track_matches_mode(t: &Track, q: &str, mode: FindMode) -> bool {
    if q.is_empty() {
        return true;
    }
    q.split_whitespace()
        .all(|word| !word.is_empty() && field_contains_mode(t, word, mode))
}

fn track_matches_album_exact(t: &Track, album: &str) -> bool {
    t.album.eq_ignore_ascii_case(album)
}

/// Match for filter n/p: query as substring in title or artist only (mirrors line_with_highlights).
fn track_matches_filter_display(t: &Track, query: &str) -> bool {
    if query.trim().is_empty() {
        return false;
    }
    let q = query.to_lowercase();
    t.title.to_lowercase().contains(&q) || t.artist.to_lowercase().contains(&q)
}

/// Match track by exact column (tui.c right-click filter).
fn track_matches_column(t: &Track, col: &str, val: &str) -> bool {
    if val.is_empty() {
        return false;
    }
    let f = |s: &str| s.eq_ignore_ascii_case(val);
    match col {
        "artist" => f(&t.artist),
        "album" => f(&t.album),
        "album_artist" => f(&t.album_artist),
        "genre" => f(&t.genre),
        "path" => t.path.starts_with(val),
        "audiomd5" => f(&t.audiomd5),
        _ => false,
    }
}

fn find_next_view_filter(
    view: &[ViewRow],
    tracks: &[Track],
    query: &str,
    cursor: usize,
    include: bool,
) -> Option<usize> {
    if query.is_empty() {
        return Some(cursor);
    }
    let n = view.len();
    if n == 0 {
        return None;
    }
    let start = if include { 0 } else { 1 };
    for i in start..n {
        let idx = (cursor + i) % n;
        if let Some(ViewRow::Track(ti)) = view.get(idx) {
            if let Some(t) = tracks.get(*ti) {
                if track_matches_filter_display(t, query) {
                    return Some(idx);
                }
            }
        }
    }
    None
}

fn find_prev_view_filter(
    view: &[ViewRow],
    tracks: &[Track],
    query: &str,
    cursor: usize,
    include: bool,
) -> Option<usize> {
    if query.is_empty() {
        return Some(cursor);
    }
    let n = view.len();
    if n == 0 {
        return None;
    }
    let start = if include { 0 } else { 1 };
    for i in start..n {
        let idx = (cursor + n - i) % n;
        if let Some(ViewRow::Track(ti)) = view.get(idx) {
            if let Some(t) = tracks.get(*ti) {
                if track_matches_filter_display(t, query) {
                    return Some(idx);
                }
            }
        }
    }
    None
}

pub fn play_track(path: &str) {
    let _ = Command::new("qua-play-this").arg(path).spawn();
}

fn find_view_index_for_path(view: &[ViewRow], tracks: &[Track], path: &str) -> Option<usize> {
    for (i, vr) in view.iter().enumerate() {
        if let ViewRow::Track(idx) = vr {
            if tracks.get(*idx).map(|t| t.path.as_str()) == Some(path) {
                return Some(i);
            }
        }
    }
    None
}

fn copy_to_clipboard(val: &str) {
    #[cfg(unix)]
    {
        let _ = Command::new("sh")
            .args(["-c", &format!("echo -n {} | xclip -selection clipboard 2>/dev/null || echo -n {} | xsel -ib 2>/dev/null", sh_escape(val), sh_escape(val))])
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .spawn();
    }
}

#[cfg(unix)]
fn sh_escape(s: &str) -> String {
    let mut o = String::from("'");
    for c in s.chars() {
        if c == '\'' {
            o.push_str("'\\''");
        } else {
            o.push(c);
        }
    }
    o.push('\'');
    o
}

/// Compute audio MD5 via ffmpeg (tui.c md5_worker parity).
fn compute_audio_md5(path: &str) -> Option<String> {
    let output = Command::new("ffmpeg")
        .args([
            "-loglevel", "error", "-i", path, "-vn", "-map", "0:a",
            "-c:a", "pcm_s32le", "-f", "md5", "-",
        ])
        .output()
        .ok()?;
    let s = String::from_utf8_lossy(&output.stdout);
    let line = s.lines().next()?;
    let eq = line.find('=')?;
    let hash = line[eq + 1..].trim();
    if hash.len() == 32 && hash.chars().all(|c| c.is_ascii_hexdigit()) {
        Some(hash.to_string())
    } else {
        None
    }
}

pub fn run(db_path: &std::path::Path, mut tracks: Vec<Track>) -> io::Result<()> {
    enable_raw_mode()?;
    let mut stdout = stdout();
    execute!(stdout, EnterAlternateScreen, EnableBracketedPaste, EnableMouseCapture)?;
    let mut terminal = Terminal::new(CrosstermBackend::new(stdout))?;

    let full_view = view::build_view(&tracks);
    let state = crate::db::load_state(db_path);
    let mut find_query = state.find_query;
    let mut find_input = String::new(); /* typing buffer; find_query only applied on Enter */
    let mut find_album_exact = state.find_album; /* 'a' key: find by exact album (tui.c) */
    let mut find_mode = false;
    let mut find_mode_kind = FindMode::Everything; /* tab to cycle when find popup open */
    let mut filter_query = state.filter_query;
    let mut find_column: Option<(String, String)> = None; /* right-click filter (not persisted) */
    /* Build initial view (filtered if find active) to resolve selection */
    let (init_view, init_tracks): (Vec<ViewRow>, Vec<Track>) = if let Some((ref col, ref val)) = find_column {
        let ft: Vec<_> = tracks.iter().filter(|t| track_matches_column(t, col, val)).cloned().collect();
        (view::build_view(&ft), ft)
    } else if find_album_exact.is_some() {
        let album = find_album_exact.as_ref().unwrap();
        let ft: Vec<_> = tracks.iter().filter(|t| track_matches_album_exact(t, album)).cloned().collect();
        (view::build_view(&ft), ft)
    } else if find_query.is_empty() {
        (full_view.clone(), tracks.clone())
    } else {
        let ft: Vec<_> = tracks.iter().filter(|t| track_matches_mode(t, &find_query, FindMode::Everything)).cloned().collect();
        (view::build_view(&ft), ft)
    };
    let mut selection = state
        .sel_path
        .as_ref()
        .and_then(|p| find_view_index_for_path(&init_view, &init_tracks, p))
        .unwrap_or_else(|| view::first_track(&init_view));
    let mut scroll_top = state.top;
    if scroll_top >= init_view.len() {
        scroll_top = 0;
    }
    /* center viewport on restored selection (tui.c) */
    if let Ok(sz) = terminal.size() {
        let (_, list_chunk, _, _, _, _) = ui::split_full(Rect::new(0, 0, sz.width, sz.height));
        let rows_h = list_chunk.height.saturating_sub(3).max(1) as usize;
        if selection > rows_h / 2 {
            scroll_top = selection.saturating_sub(rows_h / 2);
        }
    }
    let mut filter_mode = false;
    let mut sidebar_state = SidebarState::default();
    let mut sidebar_clicks = ratatui_interact::traits::ClickRegionRegistry::<SidebarClick>::new();
    let mut list_state = TrackListState::default();
    let mut last_art_track_path: Option<String> = None; /* track under cursor — redraw when view changes (find) or selection moves */
    let mut last_art_path: Option<std::path::PathBuf> = None;
    let mut last_term_size: Option<(u16, u16)> = None; /* redraw when terminal resizes; art_area can stay same due to square rounding */
    let mut art_cover_cache: HashMap<std::path::PathBuf, Option<std::path::PathBuf>> = HashMap::new();
    let mut art_redraw_count: u32 = 0;
    let mut first_art_draw = true;
    let mut last_click_row: Option<usize> = None;
    /* Layout from last draw — used for click detection to match what's on screen */
    let mut last_layout: Option<(Rect, Rect, Rect, usize)> = None; /* (list_chunk, info_area, art_area, visible) */

    /* Cached filtered state — rebuild when find_query, find_mode_kind, or find_album_exact changes */
    let mut filtered_tracks: Vec<Track> = Vec::new();
    let mut filtered_view: Vec<ViewRow> = Vec::new();
    let mut last_find_query = String::new();
    let mut last_find_mode = FindMode::Everything;
    let mut last_find_album: Option<String> = None;
    let mut last_find_column: Option<(String, String)> = None;
    let mut dirty = true;
    let mut pending_md5: Option<(String, String)> = None; /* (path, md5) — apply before view resolve to avoid borrow */

    loop {
        /* Apply deferred MD5 updates (avoids borrow conflict with tracks_ref) */
        if let Some((path, md5)) = pending_md5.take() {
            crate::db::update_track_audiomd5(db_path, &path, &md5);
            if let Some(tr) = tracks.iter_mut().find(|x| x.path == path) {
                tr.audiomd5 = md5.clone();
            }
            /* Update filtered_tracks when find active — display uses it, not tracks */
            for tr in filtered_tracks.iter_mut() {
                if tr.path == path {
                    tr.audiomd5 = md5;
                    break;
                }
            }
            dirty = true;
        }
        /* Resolve view/tracks: no clone when no find active */
        let (view, tracks_ref): (&[ViewRow], &[Track]) = if let Some((ref col, ref val)) = find_column {
            if last_find_column.as_ref() != Some(&(col.clone(), val.clone())) {
                last_find_column = find_column.clone();
                last_find_album = None;
                last_find_query = String::new();
                filtered_tracks = tracks.iter().filter(|t| track_matches_column(t, col, val)).cloned().collect();
                filtered_view = view::build_view(&filtered_tracks);
            }
            (&filtered_view, &filtered_tracks)
        } else if find_album_exact.is_some() {
            let album = find_album_exact.as_ref().unwrap();
            if last_find_album.as_ref() != Some(album) {
                last_find_album = find_album_exact.clone();
                last_find_query = String::new();
                filtered_tracks = tracks.iter().filter(|t| track_matches_album_exact(t, album)).cloned().collect();
                filtered_view = view::build_view(&filtered_tracks);
            }
            (&filtered_view, &filtered_tracks)
        } else if find_query.is_empty() {
            last_find_album = None;
            last_find_column = None;
            (&full_view, &tracks)
        } else {
            last_find_album = None;
            last_find_column = None;
            /* Rebuild only on find_query change (Enter) or mode change when popup closed. Tab in find popup does not trigger. */
            let query_changed = last_find_query != find_query;
            let mode_changed = last_find_mode != find_mode_kind;
            if query_changed || (mode_changed && !find_mode) {
                last_find_query = find_query.clone();
                last_find_mode = find_mode_kind;
                filtered_tracks = tracks.iter().filter(|t| track_matches_mode(t, &find_query, find_mode_kind)).cloned().collect();
                filtered_view = view::build_view(&filtered_tracks);
            }
            (&filtered_view, &filtered_tracks)
        };
        let n = view.len();
        if n == 0 {
            selection = 0;
            scroll_top = 0;
        } else if selection >= n {
            selection = view::first_track(view);
        } else if matches!(view.get(selection), Some(ViewRow::Header(_))) {
            /* Never select a header row; move to nearest track */
            let next = view::next_track(view, selection);
            selection = if next != selection {
                next
            } else {
                view::prev_track(view, selection)
            };
        }

        if dirty {
            let area = match terminal.size() {
                Ok(sz) => Rect::new(0, 0, sz.width, sz.height),
                Err(_) => Rect::new(0, 0, 80, 24),
            };
            let (sidebar_chunk, list_chunk, status_chunk, art_area, _div_area, info_area) =
                ui::split_full(area);
            let visible = list_chunk.height.saturating_sub(3).max(1) as usize;
            if selection >= scroll_top + visible {
                scroll_top = selection.saturating_sub(visible).saturating_add(1);
            }
            if selection < scroll_top {
                scroll_top = selection;
            }
            let find_label = if let Some((ref col, ref val)) = find_column {
                Some(format!("{}: {}", col, val))
            } else if let Some(ref a) = find_album_exact {
                Some(format!("album: {}", a))
            } else if !find_query.is_empty() {
                Some(if matches!(find_mode_kind, FindMode::Everything) {
                    format!("find: {}", find_query)
                } else {
                    format!("find: {} [{}]", find_query, find_mode_kind.as_str())
                })
            } else {
                None
            };
            terminal.draw(|f| {
                ui::render(
                    f,
                    view,
                    tracks_ref,
                    sidebar_chunk,
                    list_chunk,
                    status_chunk,
                    art_area,
                    _div_area,
                    info_area,
                    selection,
                    scroll_top,
                    &find_query,
                    &find_input,
                    find_mode,
                    find_mode_kind.as_str(),
                    find_album_exact.as_deref(),
                    find_column.as_ref(),
                    find_label.as_deref(),
                    &filter_query,
                    filter_mode,
                    &mut sidebar_state,
                    &mut list_state,
                    &mut sidebar_clicks,
                    art_redraw_count,
                );
            })?;

            /* Kitty cover art — redraw when selection/cover/terminal changed. Skip expensive work when neither changed. */
            if std::mem::take(&mut first_art_draw) {
                /* First startup: always draw */
                art::kitty_delete_all();
                if let Some(t) = view::track_at(view, selection, tracks_ref) {
                    let dir = Path::new(&t.path)
                        .parent()
                        .map(|p| p.to_path_buf())
                        .unwrap_or_default();
                    let cover = art_cover_cache
                        .get(&dir)
                        .cloned()
                        .unwrap_or_else(|| {
                            let c = art::resolve_cover(t).or_else(art::no_art_path);
                            art_cover_cache.insert(dir, c.clone());
                            c
                        });
                    last_art_path = cover.clone();
                    last_art_track_path = Some(t.path.clone());
                    last_term_size = Some((area.width, area.height));
                    if let Some(ref c) = cover {
                        let art_inner = ui::art_inner(art_area);
                        if art_inner.height > 0 && art_inner.width > 0 {
                            art::kitty_draw(c, art_inner, t);
                        }
                    }
                    art_redraw_count += 1;
                }
            } else {
                let term_size = (area.width, area.height);
                let term_changed = last_term_size != Some(term_size);
                let current_track_path =
                    view::track_at(view, selection, tracks_ref).map(|t| t.path.clone());
                let track_changed = current_track_path.as_deref() != last_art_track_path.as_deref();
                let need_redraw = if term_changed || track_changed {
                    let prev_track_path = last_art_track_path.clone();
                    last_art_track_path = current_track_path.clone();
                    last_term_size = Some(term_size);
                    let cover_changed = if let Some(t) = view::track_at(view, selection, tracks_ref) {
                        let dir = Path::new(&t.path)
                            .parent()
                            .map(|p| p.to_path_buf())
                            .unwrap_or_default();
                        /* Same dir = same album = same cover — skip lookup and draw */
                        let same_dir = prev_track_path
                            .as_ref()
                            .and_then(|p| Path::new(p).parent())
                            .map(|p| p.to_path_buf())
                            .as_ref()
                            == Some(&dir);
                        if same_dir {
                            false
                        } else {
                            let cover = art_cover_cache
                                .get(&dir)
                                .cloned()
                                .unwrap_or_else(|| {
                                    let c = art::resolve_cover(t).or_else(art::no_art_path);
                                    art_cover_cache.insert(dir, c.clone());
                                    c
                                });
                            let changed = cover.as_ref() != last_art_path.as_ref();
                            last_art_path = cover;
                            changed
                        }
                    } else {
                        last_art_path = None;
                        true
                    };
                    term_changed || cover_changed
                } else {
                    false
                };

                if need_redraw {
                    art::kitty_delete_all();
                    if let Some(t) = view::track_at(view, selection, tracks_ref) {
                        if let Some(ref c) = last_art_path {
                            let art_inner = ui::art_inner(art_area);
                            if art_inner.height > 0 && art_inner.width > 0 {
                                art::kitty_draw(c, art_inner, t);
                            }
                        }
                    }
                    art_redraw_count += 1;
                }
            }
            /* Store layout from draw so click detection matches what's on screen */
            last_layout = Some((list_chunk, info_area, art_area, visible));
            dirty = false;
        }

        let (list_chunk, info_area, art_area, visible) =
            last_layout.unwrap_or_else(|| {
                if let Ok(sz) = terminal.size() {
                    let area = Rect::new(0, 0, sz.width, sz.height);
                    let (_sidebar, list, _status, art_area, _div, info_area) = ui::split_full(area);
                    let visible = list.height.saturating_sub(3).max(1) as usize;
                    (list, info_area, art_area, visible)
                } else {
                    (
                        Rect::default(),
                        Rect::default(),
                        Rect::default(),
                        20,
                    )
                }
            });

        match event::read()? {
            Event::Resize(_, _) => dirty = true,
            Event::Paste(s) => {
                if find_mode {
                    const MAX: usize = 512;
                    for c in s.chars() {
                        if find_input.len() < MAX && !c.is_control() {
                            find_input.push(c);
                        }
                    }
                    dirty = true;
                } else if filter_mode {
                    const MAX: usize = 512;
                    for c in s.chars() {
                        if filter_query.len() < MAX && !c.is_control() {
                            filter_query.push(c);
                        }
                    }
                    if let Some(idx) = find_next_view_filter(view, tracks_ref, &filter_query, selection, true) {
                        selection = idx;
                        if selection >= scroll_top + visible {
                            scroll_top = selection.saturating_sub(visible - 1);
                        } else if selection < scroll_top {
                            scroll_top = selection;
                        }
                        dirty = true;
                    }
                }
            }
            Event::Key(key) => {
                if key.kind != KeyEventKind::Press {
                    continue;
                }
                /* Copy path: Ctrl+C (primary), Ctrl+P, Ctrl+Alt+C */
                let do_copy = (key.modifiers.contains(KeyModifiers::CONTROL) && key.code == KeyCode::Char('c'))
                    || (key.modifiers.contains(KeyModifiers::CONTROL) && key.code == KeyCode::Char('p'))
                    || (key.modifiers.contains(KeyModifiers::CONTROL)
                        && key.modifiers.contains(KeyModifiers::ALT)
                        && key.code == KeyCode::Char('c'));
                if do_copy {
                    if let Some(t) = view::track_at(&view, selection, tracks_ref) {
                        copy_to_clipboard(&t.path);
                    }
                    continue;
                }
                if find_mode {
                    match key.code {
                        KeyCode::Esc => {
                            find_mode = false;
                            find_input.clear();
                            dirty = true;
                        }
                        KeyCode::Tab => {
                            find_mode_kind = find_mode_kind.next();
                            dirty = true;
                        }
                        KeyCode::BackTab => {
                            find_mode_kind = find_mode_kind.prev();
                            dirty = true;
                        }
                        KeyCode::Enter => {
                            find_query = find_input.clone();
                            find_mode = false;
                            find_column = None;
                            find_album_exact = None;
                            last_find_column = None;
                            last_find_album = None;
                            selection = 0;
                            scroll_top = 0;
                            dirty = true;
                        }
                        KeyCode::Backspace => {
                            find_input.pop();
                            dirty = true;
                        }
                        KeyCode::Char(c) if !c.is_control() => {
                            if find_input.len() < 512 {
                                find_input.push(c);
                                dirty = true;
                            }
                        }
                        _ => {}
                    }
                    continue;
                }
                if key.modifiers.contains(KeyModifiers::CONTROL) && key.code == KeyCode::Char('f') {
                    find_mode = true;
                    filter_mode = false; /* mutual exclusion: only one popup active */
                    find_input.clear(); /* start with empty line */
                    dirty = true;
                    continue;
                }
                if key.code == KeyCode::Char('/') {
                    filter_mode = true;
                    find_mode = false; /* mutual exclusion: only one popup active */
                    filter_query.clear(); /* start with empty line */
                    dirty = true;
                    continue;
                }
                if filter_mode {
                    match key.code {
                        KeyCode::Esc => {
                            filter_mode = false;
                            filter_query.clear();
                            dirty = true;
                        }
                        KeyCode::Enter => {
                            filter_mode = false;
                            dirty = true;
                        }
                        KeyCode::Backspace => {
                            filter_query.pop();
                            if let Some(idx) = find_next_view_filter(view, tracks_ref, &filter_query, selection, true) {
                                selection = idx;
                                if selection >= scroll_top + visible {
                                    scroll_top = selection.saturating_sub(visible - 1);
                                } else if selection < scroll_top {
                                    scroll_top = selection;
                                }
                            }
                            dirty = true;
                        }
                        KeyCode::Char(c) if !c.is_control() => {
                            if filter_query.len() < 512 {
                                filter_query.push(c);
                                if let Some(idx) = find_next_view_filter(view, tracks_ref, &filter_query, selection, true) {
                                    selection = idx;
                                    if selection >= scroll_top + visible {
                                        scroll_top = selection.saturating_sub(visible - 1);
                                    } else if selection < scroll_top {
                                        scroll_top = selection;
                                    }
                                }
                                dirty = true;
                            }
                        }
                        _ => {}
                    }
                    continue;
                }
                if key.code == KeyCode::Char('n') && !filter_query.is_empty()
                {
                    if let Some(idx) = find_next_view_filter(view, tracks_ref, &filter_query, selection, false) {
                        selection = idx;
                        if selection >= scroll_top + visible {
                            scroll_top = selection.saturating_sub(visible - 1);
                        } else if selection < scroll_top {
                            scroll_top = selection;
                        }
                        dirty = true;
                    }
                    continue;
                }
                if (key.code == KeyCode::Char('p') || key.code == KeyCode::Char('?'))
                    && !filter_query.is_empty()
                {
                    if let Some(idx) = find_prev_view_filter(view, tracks_ref, &filter_query, selection, false) {
                        selection = idx;
                        if selection >= scroll_top + visible {
                            scroll_top = selection.saturating_sub(visible - 1);
                        } else if selection < scroll_top {
                            scroll_top = selection;
                        }
                        dirty = true;
                    }
                    continue;
                }
                match key.code {
                    KeyCode::Char('c') => {
                        /* Clear any active find (column, album, query) — back to full view */
                        if find_column.is_some() || find_album_exact.is_some() || !find_query.is_empty() {
                            find_column = None;
                            find_album_exact = None;
                            find_query.clear();
                            find_input.clear();
                            last_find_column = None;
                            last_find_album = None;
                            last_find_query.clear();
                            dirty = true;
                        }
                    }
                    KeyCode::Char('y') => {
                        /* y = yank/copy path (no Ctrl — works in all terminals) */
                        if let Some(t) = view::track_at(&view, selection, tracks_ref) {
                            copy_to_clipboard(&t.path);
                        }
                        dirty = true;
                    }
                    KeyCode::Char('q') | KeyCode::Esc => {
                        let path = view::track_at(&view, selection, tracks_ref)
                            .map(|t| t.path.as_str())
                            .unwrap_or("");
                        crate::db::save_state(db_path, path, scroll_top, &find_query, find_album_exact.as_deref(), &filter_query);
                        break;
                    }
                    KeyCode::Up | KeyCode::Char('k') => {
                        selection = view::prev_track(&view, selection);
                        dirty = true;
                    }
                    KeyCode::Down | KeyCode::Char('j') => {
                        selection = view::next_track(&view, selection);
                        dirty = true;
                    }
                    KeyCode::PageUp | KeyCode::Char('u') => {
                        selection = view::prev_group(&view, selection, visible);
                        dirty = true;
                    }
                    KeyCode::PageDown | KeyCode::Char('d') => {
                        selection = view::next_group(&view, selection, visible);
                        dirty = true;
                    }
                    KeyCode::Char('a') => {
                        /* tui.c: 'a' = find by exact album of selected track */
                        if let Some(t) = view::track_at(&view, selection, tracks_ref) {
                            find_album_exact = Some(t.album.clone());
                            find_column = None;
                            find_query.clear();
                            find_input.clear();
                            last_find_column = None;
                            last_find_query.clear();
                            last_find_album = None;
                            selection = 0;
                            scroll_top = 0;
                            dirty = true;
                        }
                    }
                    KeyCode::Enter => {
                        if let Some(t) = view::track_at(&view, selection, tracks_ref) {
                            play_track(&t.path);
                        }
                    }
                    _ => {}
                }
            }
            Event::Mouse(ev) => {
                let in_art = art_area.width > 0 && art_area.height > 0
                    && ev.column >= art_area.x && ev.column < art_area.x + art_area.width
                    && ev.row >= art_area.y && ev.row < art_area.y + art_area.height;
                let in_info = info_area.width > 0 && info_area.height > 0
                    && ev.column >= info_area.x && ev.column < info_area.x + info_area.width
                    && ev.row >= info_area.y && ev.row < info_area.y + info_area.height;
                let in_list = ev.column >= list_chunk.x
                    && ev.column < list_chunk.x + list_chunk.width
                    && ev.row >= list_chunk.y
                    && ev.row < list_chunk.y + list_chunk.height;
                match ev.kind {
                    MouseEventKind::Down(MouseButton::Left) => {
                        if let Some(click) = sidebar_clicks.handle_click(ev.column, ev.row) {
                            if let SidebarClick::InfoField(col, val) = click {
                                if col == "audiomd5" && val.is_empty() {
                                    /* tui.c: empty MD5 click = compute and fill */
                                    let path = view::track_at(view, selection, tracks_ref).map(|t| t.path.clone());
                                    if let Some(path) = path {
                                        if let Some(md5) = compute_audio_md5(&path) {
                                            crate::db::update_track_audiomd5(db_path, &path, &md5);
                                            pending_md5 = Some((path, md5));
                                            dirty = true;
                                        }
                                    }
                                } else {
                                    /* Left-click info field: filter/search by that value */
                                    find_column = Some((col.clone(), val.clone()));
                                    find_query.clear();
                                    find_album_exact = None;
                                    find_input.clear();
                                    last_find_query.clear();
                                    last_find_album = None;
                                    last_find_column = None; /* force rebuild on next loop */
                                    selection = view::first_track(view);
                                    scroll_top = 0;
                                    dirty = true;
                                }
                            }
                        } else if in_info {
                            /* fallback when registry misses: manual line_idx + info_field_at_line */
                            let line_idx = (ev.row.saturating_sub(info_area.y) as usize)
                                .saturating_add(sidebar_state.info_scroll as usize);
                            let field = view::track_at(view, selection, tracks_ref)
                                .and_then(|t| ui::info_field_at_line(line_idx, Some(t))
                                    .map(|(c, v)| (c, v, t.path.clone())));
                            if let Some((col, val, path)) = field {
                                if col == "audiomd5" && val.is_empty() {
                                        if let Some(md5) = compute_audio_md5(&path) {
                                            crate::db::update_track_audiomd5(db_path, &path, &md5);
                                            pending_md5 = Some((path, md5));
                                            dirty = true;
                                        }
                                } else {
                                    find_column = Some((col, val));
                                    find_query.clear();
                                    find_album_exact = None;
                                    find_input.clear();
                                    last_find_query.clear();
                                    last_find_album = None;
                                    last_find_column = None; /* force rebuild */
                                    selection = view::first_track(view);
                                    scroll_top = 0;
                                    dirty = true;
                                }
                            }
                        } else if in_list {
                            const HEADER_ROWS: u16 = 1; /* block top border (List, not Table) */
                            if ev.row >= list_chunk.y + HEADER_ROWS {
                                let row_idx = (ev.row - list_chunk.y - HEADER_ROWS) as usize;
                                let has_sticky = scroll_top > 0
                                    && matches!(view.get(scroll_top), Some(ViewRow::Track(_)));
                                let idx = if has_sticky && row_idx > 0 {
                                    scroll_top + row_idx - 1
                                } else if !has_sticky {
                                    scroll_top + row_idx
                                } else {
                                    n
                                };
                                if idx < n && matches!(view.get(idx), Some(ViewRow::Track(_))) {
                                    if last_click_row == Some(idx) {
                                        if let Some(t) = view::track_at(&view, idx, tracks_ref) {
                                            play_track(&t.path);
                                        }
                                        last_click_row = None;
                                    } else {
                                        selection = idx;
                                        last_click_row = Some(idx);
                                    }
                                    dirty = true;
                                }
                            }
                        }
                    }
                    MouseEventKind::Down(MouseButton::Right) => {
                        /* Right-click art: open cover. Right-click info field: filter + copy to clipboard */
                        let mut handled = false;
                        if let Some(click) = sidebar_clicks.handle_click(ev.column, ev.row) {
                            if matches!(click, SidebarClick::Art) {
                                if let Some(ref path) = last_art_path {
                                    art::open_image(path);
                                    handled = true;
                                }
                            } else if let SidebarClick::InfoField(col, val) = click {
                                if col == "audiomd5" && val.is_empty() {
                                    let path = view::track_at(view, selection, tracks_ref).map(|t| t.path.clone());
                                    if let Some(path) = path {
                                        if let Some(md5) = compute_audio_md5(&path) {
                                            crate::db::update_track_audiomd5(db_path, &path, &md5);
                                            pending_md5 = Some((path, md5));
                                            dirty = true;
                                        }
                                    }
                                    handled = true;
                                } else {
                                    find_column = Some((col.clone(), val.clone()));
                                    find_query.clear();
                                    find_album_exact = None;
                                    find_input.clear();
                                    last_find_query.clear();
                                    last_find_album = None;
                                    last_find_column = None;
                                    copy_to_clipboard(&val);
                                    selection = view::first_track(view);
                                    scroll_top = 0;
                                    dirty = true;
                                    handled = true;
                                }
                            }
                        }
                        if !handled && in_info {
                            /* fallback when registry misses: manual line_idx + info_field_at_line */
                            let line_idx = (ev.row.saturating_sub(info_area.y) as usize)
                                .saturating_add(sidebar_state.info_scroll as usize);
                            let field_opt = view::track_at(view, selection, tracks_ref)
                                .and_then(|t| ui::info_field_at_line(line_idx, Some(t)).map(|(c, v)| (c, v, t.path.clone())));
                            if let Some((col, val, path)) = field_opt {
                                if col == "audiomd5" && val.is_empty() {
                                    if let Some(md5) = compute_audio_md5(&path) {
                                        crate::db::update_track_audiomd5(db_path, &path, &md5);
                                        pending_md5 = Some((path, md5));
                                        dirty = true;
                                    }
                                } else {
                                    find_column = Some((col.clone(), val.clone()));
                                    find_query.clear();
                                    find_album_exact = None;
                                    find_input.clear();
                                    last_find_query.clear();
                                    last_find_album = None;
                                    last_find_column = None;
                                    copy_to_clipboard(&val);
                                    selection = view::first_track(view);
                                    scroll_top = 0;
                                    dirty = true;
                                }
                            }
                        } else if !handled && in_art {
                            if let Some(ref path) = last_art_path {
                                art::open_image(path);
                            }
                        } else if !handled && in_list {
                            /* tui.c BUTTON3_CLICKED: right-click on track = select + play */
                            const HEADER_ROWS: u16 = 1; /* block top border */
                            if ev.row >= list_chunk.y + HEADER_ROWS {
                                let row_idx = (ev.row - list_chunk.y - HEADER_ROWS) as usize;
                                let has_sticky = scroll_top > 0
                                    && matches!(view.get(scroll_top), Some(ViewRow::Track(_)));
                                let idx = if has_sticky && row_idx > 0 {
                                    scroll_top + row_idx - 1
                                } else if !has_sticky {
                                    scroll_top + row_idx
                                } else {
                                    n
                                };
                                if idx < n && matches!(view.get(idx), Some(ViewRow::Track(_))) {
                                    if let Some(t) = view::track_at(&view, idx, tracks_ref) {
                                        selection = idx;
                                        play_track(&t.path);
                                    }
                                    dirty = true;
                                }
                            }
                        }
                    }
                    MouseEventKind::ScrollDown => {
                        if in_info {
                            sidebar_state.info_scroll = sidebar_state.info_scroll.saturating_add(1);
                            dirty = true;
                        } else if in_list {
                            selection = view::next_track(&view, selection);
                            dirty = true;
                        }
                    }
                    MouseEventKind::ScrollUp => {
                        if in_info {
                            sidebar_state.info_scroll =
                                sidebar_state.info_scroll.saturating_sub(1);
                            dirty = true;
                        } else if in_list {
                            selection = view::prev_track(&view, selection);
                            dirty = true;
                        }
                    }
                    _ => {}
                }
            }
            _ => {}
        }
    }

    art::kitty_delete_all();
    disable_raw_mode()?;
    execute!(terminal.backend_mut(), DisableMouseCapture, DisableBracketedPaste, LeaveAlternateScreen)?;
    terminal.show_cursor()?;
    Ok(())
}
