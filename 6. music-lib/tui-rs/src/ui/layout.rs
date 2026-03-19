//! Layout constants and helpers (tui.c parity: 38% sidebar, 62% list).

use ratatui::layout::{Constraint, Direction, Layout, Rect};

use crate::art;

/// Max sidebar width (38%), list gets remainder after actual aw (tui.c parity).
pub const SIDEBAR_PCT: u16 = 38;

/// Min rows reserved for info panel below art (like MIN_INFO_ROWS in tui.c).
pub const MIN_INFO_ROWS: u16 = 10;

/// Full layout: sidebar, list, status bar. Reserves bottom row for status.
pub fn split_full(area: Rect) -> (Rect, Rect, Rect) {
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Min(1),
            Constraint::Length(1),
        ])
        .split(area);
    let content = chunks[0];
    let status = chunks[1];
    let (sidebar, list) = split_main(content);
    (sidebar, list, status)
}

/// Splits the main content area into sidebar (left) and list (right).
/// List starts at aw (actual sidebar content width), so list gets more space
/// when the art square is narrower than 38% (tui.c: list_x = aw, list_w = COLS - aw).
pub fn split_main(area: Rect) -> (Rect, Rect) {
    let sidebar_max = (area.width as u32 * SIDEBAR_PCT as u32 / 100) as u16;
    let (aw, _) = art_area_dim(sidebar_max, area.height);
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
    (sidebar, list)
}

/// Art area dimensions (tui.c art_area_dim): largest visual square fitting above info.
fn art_area_dim(rw: u16, rows_h: u16) -> (u16, u16) {
    let (cw, ch) = art::get_cell_size();
    let cw = cw.max(1) as u16;
    let ch = ch.max(1) as u16;
    let max_aw = rw;
    let max_art_h = rows_h.saturating_sub(MIN_INFO_ROWS).max(1);
    /* largest square that fits: aw*cw == split*ch (visually square) */
    let split_by_w = ((max_aw as u32) * (cw as u32) / (ch as u32)) as u16;
    let aw_by_h = ((max_art_h as u32) * (ch as u32) / (cw as u32)) as u16;
    let (aw, split) = if split_by_w <= max_art_h {
        (max_aw, split_by_w)
    } else {
        if aw_by_h <= max_aw {
            (aw_by_h, max_art_h)
        } else {
            (max_aw, ((max_aw as u32) * (cw as u32) / (ch as u32)) as u16)
        }
    };
    let aw = aw.max(1);
    let split = split.max(1).min(rows_h.saturating_sub(MIN_INFO_ROWS));
    (aw, split.max(1))
}

/// Splits sidebar into art area (top) and info area (bottom).
/// Art area is visually square (tui.c art_area_dim: uses cell aspect ratio).
/// Info gets MIN_INFO_ROWS; art gets the rest, capped to square.
pub fn split_sidebar(area: Rect) -> (Rect, Rect, Rect) {
    let total = area.height;
    if total <= 1 {
        return (area, area, Rect::default());
    }
    let (aw, split) = art_area_dim(area.width, total);

    /* Art area: may be narrower than full sidebar when height-constrained */
    let art_area = Rect {
        x: area.x,
        y: area.y,
        width: aw,
        height: split,
    };

    /* Info area directly below art (no divider line) */
    let rest_top = area.y + split;
    let rest_height = total.saturating_sub(split);
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

    (art_area, div_area, info_area)
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
