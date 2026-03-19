/* View layer: group headers + track rows (tui.c parity). */

use crate::db::Track;

#[derive(Clone, Debug)]
pub enum ViewRow {
    Header(String),
    Track(usize), /* index into tracks slice */
}

/// Build view from tracks: insert headers when group key changes.
/// Group key: album_artist|artist + album + parent dir (tui.c default).
pub fn build_view(tracks: &[Track]) -> Vec<ViewRow> {
    let mut view = Vec::with_capacity(tracks.len() * 2);
    let mut cur_group = String::new();

    for (idx, t) in tracks.iter().enumerate() {
        let aa = if t.album_artist.is_empty() {
            t.artist.as_str()
        } else {
            t.album_artist.as_str()
        };
        let dirlen = t.path.rfind('/').map(|p| p + 1).unwrap_or(0);
        let gbuf = format!(
            "{}\x1f{}\x1f{}",
            aa,
            t.album,
            &t.path[..dirlen.min(t.path.len())]
        );

        if gbuf != cur_group {
            cur_group = gbuf.clone();
            let aa = if t.album_artist.is_empty() {
                &t.artist
            } else {
                &t.album_artist
            };
            let header = if aa.is_empty() && t.date.is_empty() && t.album.is_empty() {
                "(untagged)".to_string()
            } else {
                let aa = if aa.is_empty() { "?" } else { aa };
                let dt = if t.date.is_empty() { "" } else { &t.date };
                let al = if t.album.is_empty() { "?" } else { &t.album };
                let sep = if !dt.is_empty() && !t.album.is_empty() {
                    " - "
                } else {
                    ""
                };
                format!("{} - {}{}{}", aa, dt, sep, al)
            };
            view.push(ViewRow::Header(header));
        }

        view.push(ViewRow::Track(idx));
    }

    view
}

pub fn next_track(view: &[ViewRow], from: usize) -> usize {
    if view.is_empty() {
        return 0;
    }
    let from = from.min(view.len().saturating_sub(1));
    for i in (from + 1)..view.len() {
        if matches!(view[i], ViewRow::Track(_)) {
            return i;
        }
    }
    from
}

pub fn prev_track(view: &[ViewRow], from: usize) -> usize {
    if view.is_empty() {
        return 0;
    }
    let from = from.min(view.len().saturating_sub(1));
    for i in (0..from).rev() {
        if matches!(view[i], ViewRow::Track(_)) {
            return i;
        }
    }
    0
}

pub fn next_group(view: &[ViewRow], from: usize, page_size: usize) -> usize {
    if view.is_empty() {
        return 0;
    }
    let from = from.min(view.len().saturating_sub(1));
    for i in (from + 1)..view.len() {
        if matches!(view[i], ViewRow::Header(_)) {
            return next_track(view, i);
        }
    }
    let mut r = from;
    for _ in 0..page_size {
        let next = next_track(view, r);
        if next == r {
            break;
        }
        r = next;
    }
    r
}

pub fn prev_group(view: &[ViewRow], from: usize, page_size: usize) -> usize {
    if view.is_empty() {
        return 0;
    }
    let from = from.min(view.len().saturating_sub(1));
    let mut cur_hdr = None;
    for i in (0..=from).rev() {
        if matches!(view[i], ViewRow::Header(_)) {
            cur_hdr = Some(i);
            break;
        }
    }
    if let Some(cur_hdr) = cur_hdr {
        for i in (0..cur_hdr).rev() {
            if matches!(view[i], ViewRow::Header(_)) {
                return next_track(view, i);
            }
        }
    }
    let mut r = from;
    for _ in 0..page_size {
        let prev = prev_track(view, r);
        if prev == r {
            break;
        }
        r = prev;
    }
    r
}

pub fn first_track(view: &[ViewRow]) -> usize {
    for i in 0..view.len() {
        if matches!(view[i], ViewRow::Track(_)) {
            return i;
        }
    }
    0
}

/// Get track index at view position; returns None for headers.
pub fn track_at<'a>(
    view: &'a [ViewRow],
    view_idx: usize,
    tracks: &'a [Track],
) -> Option<&'a Track> {
    match view.get(view_idx)? {
        ViewRow::Track(i) => tracks.get(*i),
        ViewRow::Header(_) => None,
    }
}

/// Find the group header index for the row at view_idx (tui.c sticky header).
/// Scans backward from view_idx; returns (header_idx, header_label) or None.
pub fn group_header_at(view: &[ViewRow], view_idx: usize) -> Option<(usize, &str)> {
    if view_idx >= view.len() {
        return None;
    }
    for i in (0..=view_idx.min(view.len().saturating_sub(1))).rev() {
        if let ViewRow::Header(h) = &view[i] {
            return Some((i, h.as_str()));
        }
    }
    None
}
