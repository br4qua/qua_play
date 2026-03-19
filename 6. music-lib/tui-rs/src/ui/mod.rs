//! UI components (layout, sidebar, track list).

mod layout;
mod sidebar;
mod track_list;

pub use layout::{art_inner, split_full, split_sidebar};
pub use sidebar::{info_field_at_line, render as render_sidebar, SidebarClick, SidebarState};
pub use track_list::{render as render_track_list, TrackListState};

use ratatui::prelude::*;
use ratatui::widgets::{Block, Borders, Paragraph};

use crate::db::Track;
use crate::view::ViewRow;

/// Top-level render: sidebar, track list, status bar, find/filter popups.
pub fn render(
    f: &mut Frame,
    view: &[ViewRow],
    tracks: &[Track],
    sidebar_chunk: Rect,
    list_chunk: Rect,
    status_chunk: Rect,
    selection: usize,
    scroll_top: usize,
    _find_query: &str,
    find_input: &str, /* typed buffer shown in Find popup */
    find_mode: bool,
    find_mode_label: &str, /* current search mode: everything, artist, album, track, genre */
    _find_album_exact: Option<&str>, /* 'a' key: exact album filter (tui.c) */
    _find_column: Option<&(String, String)>,
    find_label: Option<&str>, /* block title: "find: X [artist]", "artist: Y", "album: Z" */
    filter_query: &str,
    filter_mode: bool,
    sidebar_state: &mut SidebarState,
    list_state: &mut TrackListState,
    sidebar_clicks: &mut ratatui_interact::traits::ClickRegionRegistry<SidebarClick>,
) {
    let selected = crate::view::track_at(view, selection, tracks);
    render_sidebar(f, sidebar_chunk, selected, sidebar_state, sidebar_clicks);
    let find_term = find_label;
    render_track_list(
        f,
        list_chunk,
        view,
        tracks,
        scroll_top,
        selection,
        filter_query,
        find_term,
        list_state,
    );

    if find_mode {
        let w = 50u16;
        let h = 3u16;
        /* compute list rect from frame area so find box is centered in list, not full terminal */
        let area = f.area();
        let (_, list_rect, _) = split_full(area);
        let inner = Rect {
            x: list_rect.x + (list_rect.width.saturating_sub(w)) / 2,
            y: list_rect.y + (list_rect.height.saturating_sub(h)) / 2,
            width: w,
            height: h,
        };
        let fill: String = (0..inner.height)
            .map(|_| " ".repeat(inner.width as usize))
            .collect::<Vec<_>>()
            .join("\n");
        f.render_widget(
            Paragraph::new(fill).style(Style::default().bg(Color::Black).fg(Color::Black)),
            inner,
        );
        let block = Block::default()
            .borders(Borders::ALL)
            .border_style(Style::default().fg(Color::Cyan))
            .style(Style::default().bg(Color::Black))
            .title(if find_mode_label == "everything" {
                " Find ".into()
            } else {
                format!(" Find ({}) ", find_mode_label)
            });
        let prompt = format!("{}_", find_input);
        f.render_widget(
            Paragraph::new(prompt)
                .block(block)
                .style(Style::default().fg(Color::Cyan).bg(Color::Black)),
            inner,
        );
    }
    if filter_mode {
        let w = 50u16;
        let h = 3u16;
        let area = f.area();
        let (_, list_rect, _) = split_full(area);
        let inner = Rect {
            x: list_rect.x + (list_rect.width.saturating_sub(w)) / 2,
            y: list_rect.y + (list_rect.height.saturating_sub(h)) / 2,
            width: w,
            height: h,
        };
        let fill: String = (0..inner.height)
            .map(|_| " ".repeat(inner.width as usize))
            .collect::<Vec<_>>()
            .join("\n");
        f.render_widget(
            Paragraph::new(fill).style(Style::default().bg(Color::Black).fg(Color::Black)),
            inner,
        );
        let block = Block::default()
            .borders(Borders::ALL)
            .border_style(Style::default().fg(Color::Cyan))
            .style(Style::default().bg(Color::Black))
            .title(" Filter ");
        let prompt = format!("{}_", filter_query);
        f.render_widget(
            Paragraph::new(prompt)
                .block(block)
                .style(Style::default().fg(Color::Cyan).bg(Color::Black)),
            inner,
        );
    }

    let keys = if find_mode {
        "esc: close  tab: mode  n/p: next/prev  enter: confirm"
    } else if filter_mode {
        "esc: close  n/p: next/prev  enter: confirm"
    } else {
        "Ctrl+F: find  /: filter  Ctrl+C: copy  a: album  j/k: track  u/d: group  enter: play  click info: filter  s: stop  o: open  q: quit"
    };
    let status = format!(" {} tracks ", tracks.len());
    let pad = status_chunk
        .width
        .saturating_sub(status.len() as u16)
        .saturating_sub(keys.len() as u16) as usize;
    let line = Line::from(vec![
        Span::raw(&status),
        Span::raw(" ".repeat(pad)),
        Span::raw(keys),
    ]);
    let para = Paragraph::new(line)
        .style(Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD));
    f.render_widget(para, status_chunk);
}
