//! Layout constants and helpers (tui.c parity: 38% sidebar, 62% list).

use ratatui::layout::{Constraint, Direction, Layout, Rect};

use crate::art;

/// Max sidebar width (38%), list gets remainder after actual aw (tui.c parity).
pub const SIDEBAR_PCT: u16 = 38;

/// Min rows reserved for info panel below art (like MIN_INFO_ROWS in tui.c).
pub const MIN_INFO_ROWS: u16 = 9;

/// Full layout: art+info (left), list (right), status. Single art_area_dim call.
pub fn split_full(area: Rect) -> (Rect, Rect, Rect, Rect, Rect, Rect) {
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Min(1),
            Constraint::Length(1),
        ])
        .split(area);
    let content = chunks[0];
    let status = chunks[1];
    let (sidebar, list, art_area, div_area, info_area) = split_main(content);
    (sidebar, list, status, art_area, div_area, info_area)
}

/// Splits content: art+info (left) | list (right). Computes (aw, split) once, builds all rects.
/// No second art_area_dim call — single source of truth, no rounding gap.
pub fn split_main(area: Rect) -> (Rect, Rect, Rect, Rect, Rect) {
    let max_cols = (area.width as u32 * SIDEBAR_PCT as u32 / 100) as u16;
    let (mut aw, mut split) = art_area_dim(max_cols, area.height);
    loop {
        let (aw_next, split_next) = art_area_dim(aw, area.height);
        if aw_next == aw && split_next == split {
            break;
        }
        aw = aw_next;
        split = split_next;
    }

    let sidebar = Rect {
        x: area.x,
        y: area.y,
        width: aw,
        height: area.height,
    };
    let list = Rect {
        x: area.x + aw,
        y: area.y,
        width: area.width.saturating_sub(aw),
        height: area.height,
    };

    let art_area = Rect {
        x: area.x,
        y: area.y,
        width: aw,
        height: split,
    };
    let rest_top = area.y + split;
    let rest_height = area.height.saturating_sub(split);
    let div_area = Rect {
        x: area.x,
        y: rest_top,
        width: aw,
        height: 0,
    };
    let info_area = Rect {
        x: area.x,
        y: rest_top,
        width: aw,
        height: rest_height,
    };

    (sidebar, list, art_area, div_area, info_area)
}

/// Art area dimensions: largest square fitting above info.
/// Target the INNER area (inside 1-cell border) so image region is square.
/// Inner: (aw-2)*cw == (split-2)*ch  =>  aw = 2 + (split-2)*ch/cw
fn art_area_dim(rw: u16, rows_h: u16) -> (u16, u16) {
    let (cw, ch) = art::get_cell_size();
    let cw = cw.max(1) as u32;
    let ch = ch.max(1) as u32;
    let max_aw = rw as u32;
    let max_art_h = rows_h.saturating_sub(MIN_INFO_ROWS).max(2) as u32;
    /* max split for square inner: (split-2)*ch/cw <= max_aw-2  =>  split <= 2 + (max_aw-2)*cw/ch */
    let split_by_w = 2 + (max_aw.saturating_sub(2) * cw / ch);
    let split = split_by_w.min(max_art_h).max(2);
    let aw = 2 + ((split - 2) * ch / cw);
    let aw = aw.min(max_aw).max(2);
    ((aw as u16).max(1), (split as u16).max(1).min(rows_h.saturating_sub(MIN_INFO_ROWS)))
}

/// Inner rect for bordered Art block (1-cell border). Kitty image draws here.
pub fn art_inner(art_area: Rect) -> Rect {
    if art_area.width < 2 || art_area.height < 2 {
        return Rect::default();
    }
    Rect {
        x: art_area.x + 1,
        y: art_area.y + 1,
        width: art_area.width - 2,
        height: art_area.height - 2,
    }
}
