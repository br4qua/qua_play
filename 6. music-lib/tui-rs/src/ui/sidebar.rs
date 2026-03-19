//! Sidebar: art area + Info divider + scrollable info panel (tui.c parity).

use ratatui::prelude::*;
use ratatui::widgets::{Block, Borders, Paragraph, Scrollbar, ScrollbarOrientation, ScrollbarState};
use ratatui_interact::traits::ClickRegionRegistry;

use crate::db::Track;
use crate::ui::layout::split_sidebar;

/// Click action for sidebar (art right-click open cover, info left/right filter).
#[derive(Clone)]
pub enum SidebarClick {
    Art,
    InfoField(String, String), /* col, filter_val */
}

#[derive(Default)]
pub struct SidebarState {
    pub info_scroll: u16,
}

pub fn render(
    f: &mut Frame,
    area: Rect,
    selected: Option<&Track>,
    state: &mut SidebarState,
    clicks: &mut ClickRegionRegistry<SidebarClick>,
) {
    clicks.clear();
    let (art_area, div_area, info_area) = split_sidebar(area);

    /* Register art area for right-click (open cover) */
    if art_area.width > 0 && art_area.height > 0 {
        clicks.register(art_area, SidebarClick::Art);
    }

    /* Art area with border (like Tracks, Info) — image rendered on top via kitty */
    let art_block = Block::default()
        .borders(Borders::ALL)
        .title(" Art ")
        .style(Style::default().fg(Color::White));
    f.render_widget(art_block, art_area);

    /* -- Info section: bordered like Tracks, div + info combined -- */
    let info_section = Rect {
        x: div_area.x,
        y: div_area.y,
        width: div_area.width,
        height: div_area.height + info_area.height,
    };
    let info_block = Block::default()
        .borders(Borders::ALL)
        .title(" Info ")
        .style(Style::default().fg(Color::White));
    let inner = info_block.inner(info_section);

    /* -- Info panel (scrollable, 1 row per line for fast scroll) -- */
    let lines = info_lines(selected);
    let content_height = lines.len() as u16;
    let visible = inner.height;
    let max_scroll = content_height.saturating_sub(visible).max(0);
    if state.info_scroll > max_scroll {
        state.info_scroll = max_scroll;
    }
    let start = state.info_scroll as usize;
    let end = (start + visible as usize).min(lines.len());
    /* Truncate each line to one row so wrapping doesn't break scroll math */
    let w = inner.width.saturating_sub(1) as usize; /* 1 for scrollbar */
    let visible_lines: Vec<Line> = lines[start..end]
        .iter()
        .map(|s| Line::from(truncate_info_line(s, w)))
        .collect();
    let para = Paragraph::new(visible_lines)
        .scroll((0, 0))
        .style(Style::default().fg(Color::White))
        .block(info_block);
    f.render_widget(para, info_section);

    /* scrollbar: use (max_scroll + 1) positions so thumb reaches bottom when at end */
    if content_height > visible {
        let num_positions = (max_scroll as usize) + 1;
        let mut sb_state = ScrollbarState::default()
            .content_length(num_positions)
            .viewport_content_length(1)
            .position(state.info_scroll as usize);
        let sb = Scrollbar::new(ScrollbarOrientation::VerticalRight)
            .thumb_symbol("█")
            .track_symbol(Some("│"));
        f.render_stateful_widget(sb, inner, &mut sb_state);
    }

    /* Register clickable info lines (right-click filter) */
    if inner.width > 0 && inner.height > 0 {
        for line_offset in 0..(end.saturating_sub(start)) {
            let line_idx = start + line_offset;
            if let Some((col, val)) = info_field_at_line(line_idx, selected) {
                let row_rect = Rect::new(
                    inner.x,
                    inner.y + line_offset as u16,
                    inner.width,
                    1,
                );
                clicks.register(row_rect, SidebarClick::InfoField(col, val));
            }
        }
    }
}

fn truncate_info_line(s: &str, width: usize) -> String {
    if width == 0 {
        return String::new();
    }
    let s = s.trim_end();
    let max_len = width.saturating_sub(1);
    if s.chars().count() <= max_len {
        return s.to_string();
    }
    let mut len = 0;
    for (i, _) in s.chars().enumerate() {
        if len >= max_len {
            return format!("{}~", &s[..s.char_indices().nth(i).map(|(o, _)| o).unwrap_or(s.len())]);
        }
        len += 1;
    }
    s.to_string()
}

/// Line index to clickable field (tui.c info_click parity).
/// Returns (column_name, filter_value) for right-click filter.
/// Path: parent directory prefix for "filter to folder".
pub fn info_field_at_line(line_idx: usize, track: Option<&Track>) -> Option<(String, String)> {
    let t = track?;
    let track_off = if t.track_num > 0 { 1 } else { 0 };
    let (col, val) = match line_idx {
        1 => ("artist", t.artist.clone()),
        2 => ("album", t.album.clone()),
        3 => ("album_artist", t.album_artist.clone()),
        i if i == 9 + track_off => ("genre", t.genre.clone()),
        i if i == 10 + track_off => ("path", path_dir_prefix(&t.path)),
        i if i == 11 + track_off => ("audiomd5", t.audiomd5.clone()),
        _ => return None,
    };
    /* Register MD5 row even when empty (tui.c: click to compute or filter) */
    Some((col.to_string(), val))
}

fn path_dir_prefix(path: &str) -> String {
    std::path::Path::new(path)
        .parent()
        .map(|p| format!("{}/", p.display()))
        .unwrap_or_default()
}

fn info_lines(t: Option<&Track>) -> Vec<String> {
    let mut out = Vec::with_capacity(16);
    if let Some(t) = t {
        out.push(format!("Title:    {}", t.title));
        out.push(format!("Artist:   {}", t.artist));
        out.push(format!("Album:    {}", t.album));
        out.push(format!("AlbumArt: {}", t.album_artist));
        if t.track_num > 0 {
            out.push(format!("Track:    {}", t.track_num));
        }
        let s = t.duration as i32;
        out.push(format!("Duration: {}:{:02}", s / 60, s % 60));
        let fmt = if t.format == "wavpack" {
            "WavPack"
        } else if t.format == "flac" {
            "FLAC"
        } else {
            &t.format
        };
        out.push(format!("Format:   {}", fmt));
        out.push(format!("Rate:     {} Hz", t.sample_rate));
        out.push(format!("Depth:    {} bit", t.bit_depth));
        out.push(format!("Date:     {}", t.date));
        out.push(format!("Genre:    {}", t.genre));
        out.push(format!("Path:     {}", t.path));
        out.push(format!("MD5:      {}", t.audiomd5));
    } else {
        out.push("(no track selected)".to_string());
    }
    out
}
