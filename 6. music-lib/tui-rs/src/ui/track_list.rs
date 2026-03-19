//! Track list: group headers + track rows (tui.c parity).

use ratatui::prelude::*;
use ratatui::widgets::{Block, Borders, HighlightSpacing, List, ListItem, ListState};
use unicode_width::{UnicodeWidthChar, UnicodeWidthStr};

use crate::db::Track;
use crate::view::ViewRow;

const COL_DUR_W: usize = 7;

#[derive(Default)]
pub struct TrackListState {
    pub list_state: ListState,
}

fn format_duration(secs: f64) -> String {
    let s = secs as i32;
    format!("{}:{:02}", s / 60, s % 60)
}

/// Truncate by Unicode display width (CJK = 2 cols). Preserves column alignment.
fn truncate_cell(s: &str, max_width: usize) -> String {
    let s = s.trim();
    if max_width == 0 {
        return String::new();
    }
    let max_with_tilde = max_width.saturating_sub(1);
    let mut width = 0;
    for (i, c) in s.char_indices() {
        let cw = c.width_cjk().unwrap_or(1);
        if width + cw > max_with_tilde {
            return format!("{}~", &s[..i]);
        }
        width += cw;
    }
    s.to_string()
}

/// Pad string with spaces to reach target display width (fixes CJK alignment).
fn pad_to_width(s: &str, target_width: usize) -> String {
    let w = s.width_cjk();
    if w >= target_width {
        return s.to_string();
    }
    format!("{}{}", s, " ".repeat(target_width - w))
}

/// Build a Line with search query matches highlighted.
fn line_with_highlights(
    text: &str,
    query: &str,
    normal_style: Style,
    match_style: Style,
) -> Line<'static> {
    if query.is_empty() {
        return Line::from(Span::styled(text.to_string(), normal_style));
    }
    let q = query.to_lowercase();
    let text_lower = text.to_lowercase();
    /* Positions from text_lower must map to text; skip highlights if lengths differ (e.g. ß→ss) */
    if text.len() != text_lower.len() {
        return Line::from(Span::styled(text.to_string(), normal_style));
    }
    let mut spans = Vec::new();
    let mut last = 0;
    let mut start = 0;
    while let Some(pos) = text_lower[start..].find(&q) {
        let lo = start + pos;
        let hi = lo + q.len();
        if hi > text.len() || !text.is_char_boundary(lo) || !text.is_char_boundary(hi) {
            break;
        }
        if lo > last {
            spans.push(Span::styled(text[last..lo].to_string(), normal_style));
        }
        spans.push(Span::styled(text[lo..hi].to_string(), match_style));
        last = hi;
        start = hi;
    }
    if last < text.len() {
        spans.push(Span::styled(text[last..].to_string(), normal_style));
    }
    if spans.is_empty() {
        Line::from(Span::styled(text.to_string(), normal_style))
    } else {
        Line::from(spans)
    }
}

pub fn render(
    f: &mut Frame,
    area: Rect,
    view: &[ViewRow],
    tracks: &[Track],
    scroll_top: usize,
    selected: usize,
    highlight_query: &str,
    find_term: Option<&str>,
    state: &mut TrackListState,
) {
    let inner_h = area.height.saturating_sub(2);
    let visible_rows = inner_h.max(1) as usize;

    let block_title = {
        let mut t = " Tracks".to_string();
        if let Some(ft) = find_term {
            if !ft.is_empty() {
                t.push_str(&format!(" ({})", ft));
            }
        }
        if !highlight_query.is_empty() {
            t.push_str(&format!(" (filter: {})", highlight_query));
        }
        t.push(' ');
        t
    };

    if view.is_empty() {
        let find_filtered = find_term.map_or(false, |s| !s.is_empty());
        let empty_msg = if find_filtered {
            Line::from("(no matches)")
        } else {
            Line::from("(no tracks)")
        };
        let list = List::new(vec![ListItem::new(empty_msg)])
            .block(
                Block::default()
                    .borders(Borders::ALL)
                    .title(block_title),
            );
        *state.list_state.offset_mut() = 0;
        state.list_state.select(None);
        f.render_stateful_widget(list, area, &mut state.list_state);
        return;
    }

    /* Sticky header (tui.c): when scroll_top > 0 and top row is a Track,
     * hoist the group header so it stays visible at top */
    let (sticky_header, data_start, data_rows) = if scroll_top > 0
        && scroll_top < view.len()
        && matches!(view.get(scroll_top), Some(ViewRow::Track(_)))
    {
        if let Some((_, label)) = crate::view::group_header_at(view, scroll_top) {
            let sticky = Some(label.to_string());
            (sticky, scroll_top, visible_rows.saturating_sub(1))
        } else {
            (None, scroll_top, visible_rows)
        }
    } else {
        let start = scroll_top.min(view.len().saturating_sub(1));
        (None, start, visible_rows)
    };

    let end = (data_start + data_rows).min(view.len());
    let window = &view[data_start..end];

    /* highlight_idx: offset by 1 when sticky (sticky row is display row 0) */
    let highlight_idx = if selected >= data_start && selected < end {
        if sticky_header.is_some() {
            Some(selected.saturating_sub(data_start) + 1)
        } else {
            Some(selected.saturating_sub(data_start))
        }
    } else {
        None
    };

    /* Fixed: "  " + hash(3) + "  " + "  " + "  " + dur(7) = 18 cols. Remaining for title+artist. */
    let list_w = area.width.saturating_sub(2) as usize;
    let rem = list_w
        .saturating_sub(18); /* 2+3+2+2+2+7 = fixed width */
    let title_w = ((rem * 3) / 5).max(1);
    let artist_w = rem.saturating_sub(title_w).saturating_sub(1).max(1);

    let header_style = Style::default().fg(Color::Magenta);
    let normal = Style::default();
    let matched = Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD);

    let mut items: Vec<ListItem> = window
        .iter()
        .map(|vr| {
            match vr {
                ViewRow::Header(h) => ListItem::new(Line::from(vec![
                    Span::styled("══ ", header_style),
                    Span::styled(h.clone(), header_style),
                ])),
                ViewRow::Track(idx) => {
                    let t = match tracks.get(*idx) {
                        Some(t) => t,
                        None => return ListItem::new(Line::from("")),
                    };
                    let hash = if t.track_num > 0 {
                        format!("{:>3}", t.track_num)
                    } else {
                        "   ".to_string()
                    };
                    let title = truncate_cell(&t.title, title_w);
                    let artist = truncate_cell(&t.artist, artist_w);
                    let dur = format_duration(t.duration);
                    let dur_w = COL_DUR_W;
                    let title_pad = pad_to_width(&title, title_w);
                    let artist_pad = pad_to_width(&artist, artist_w);
                    let line = if highlight_query.is_empty() {
                        Line::from(format!(
                            "  {}  {}  {}  {:>7}",
                            hash, title_pad, artist_pad, dur
                        ))
                    } else {
                        let mut spans: Vec<Span> = vec![Span::raw(format!("  {}  ", hash))];
                        spans.extend(
                            line_with_highlights(&title_pad, highlight_query, normal, matched)
                                .spans
                                .into_iter(),
                        );
                        spans.push(Span::raw("  "));
                        spans.extend(
                            line_with_highlights(&artist_pad, highlight_query, normal, matched)
                                .spans
                                .into_iter(),
                        );
                        spans.push(Span::raw(format!("  {0:>1$}", dur, dur_w)));
                        Line::from(spans)
                    };
                    ListItem::new(line)
                }
            }
        })
        .collect();

    /* Prepend sticky header row when hoisting (tui.c) */
    if let Some(ref label) = sticky_header {
        items.insert(
            0,
            ListItem::new(Line::from(vec![
                Span::styled("══ ", header_style),
                Span::styled(label.clone(), header_style),
            ])),
        );
    }

    let list = List::new(items)
        .block(
            Block::default()
                .borders(Borders::ALL)
                .title(block_title),
        )
        /* bg only: preserves span fg (yellow matches) when row selected */
        .highlight_style(Style::default().bg(Color::Indexed(8)))
        .highlight_symbol(" ")
        .highlight_spacing(HighlightSpacing::Always);

    *state.list_state.offset_mut() = 0;
    state.list_state.select(highlight_idx);
    f.render_stateful_widget(list, area, &mut state.list_state);
}
