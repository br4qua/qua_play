//! Cover art via Kitty graphics protocol (tui.c parity).

use base64::{engine::general_purpose::STANDARD as B64, Engine};
use ratatui::layout::Rect;
use std::fs;
use std::path::{Path, PathBuf};

use crate::db::Track;

static mut KITTY_ID: u32 = 0;

fn is_image_ext(path: &Path) -> bool {
    path.extension()
        .and_then(|e| e.to_str())
        .map(|e| {
            let el = e.to_lowercase();
            el == "jpg" || el == "jpeg" || el == "png" || el == "webp" || el == "bmp"
        })
        .unwrap_or(false)
}

fn is_cover_name(path: &Path) -> bool {
    path.file_name()
        .and_then(|n| n.to_str())
        .map(|s| {
            let lo = s.to_lowercase();
            lo.starts_with('c') || lo.starts_with('f')
        })
        .unwrap_or(false)
}

/// Find best cover in dir: prefer cover/front named, else biggest image, then one subdir level.
pub fn find_cover_art(dir: &Path) -> Option<PathBuf> {
    let entries = fs::read_dir(dir).ok()?;
    let mut best_named: Option<(PathBuf, u64)> = None;
    let mut best_any: Option<(PathBuf, u64)> = None;
    let mut subdirs: Vec<PathBuf> = Vec::new();

    for e in entries.flatten() {
        let path = e.path();
        if path.file_name()
            .and_then(|n| n.to_str())
            .map(|s| s.starts_with('.'))
            .unwrap_or(false)
        {
            continue;
        }
        let meta = e.metadata().ok()?;
        if meta.is_dir() {
            subdirs.push(path);
            continue;
        }
        if !meta.is_file() || !is_image_ext(&path) {
            continue;
        }
        let sz = meta.len();
        if is_cover_name(&path) && best_named.as_ref().map_or(true, |(_, s)| sz > *s) {
            best_named = Some((path.clone(), sz));
        }
        if best_any.as_ref().map_or(true, |(_, s)| sz > *s) {
            best_any = Some((path.clone(), sz));
        }
    }

    if let Some((p, _)) = best_named {
        return Some(p);
    }
    if let Some((p, _)) = best_any {
        return Some(p);
    }

    for sub in subdirs {
        let entries = fs::read_dir(&sub).ok()?;
        for e in entries.flatten() {
            let path = e.path();
            if path.file_name().and_then(|n| n.to_str()).map(|s| s.starts_with('.')).unwrap_or(false) {
                continue;
            }
            let meta = e.metadata().ok()?;
            if !meta.is_file() || !is_image_ext(&path) {
                continue;
            }
            let sz = meta.len();
            if is_cover_name(&path) && best_named.as_ref().map_or(true, |(_, s)| sz > *s) {
                best_named = Some((path.clone(), sz));
            }
            if best_any.as_ref().map_or(true, |(_, s)| sz > *s) {
                best_any = Some((path.clone(), sz));
            }
        }
    }

    best_named.map(|(p, _)| p).or_else(|| best_any.map(|(p, _)| p))
}

fn path_dir(filepath: &str) -> PathBuf {
    Path::new(filepath).parent().unwrap_or(Path::new("")).to_path_buf()
}

/// Fallback when no cover art found (tui.c parity: ~/.config/qua-player/no-art.png).
pub fn no_art_path() -> Option<PathBuf> {
    let p = dirs::home_dir()?.join(".config/qua-player/no-art.png");
    if p.is_file() {
        Some(p)
    } else {
        None
    }
}

/// Resolve cover path: prefer DB cache, else find in dir.
pub fn resolve_cover(track: &Track) -> Option<PathBuf> {
    if !track.cover.is_empty() {
        let p = PathBuf::from(&track.cover);
        if p.exists() {
            return Some(p);
        }
    }
    let dir = path_dir(&track.path);
    if dir.as_os_str().is_empty() {
        return None;
    }
    find_cover_art(&dir)
}

/// Get terminal cell size in pixels (for image scaling). Public for layout square math.
pub fn get_cell_size() -> (i32, i32) {
    #[cfg(unix)]
    unsafe {
        use libc::{ioctl, winsize, STDOUT_FILENO, TIOCGWINSZ};
        let mut ws: winsize = std::mem::zeroed();
        if ioctl(STDOUT_FILENO, TIOCGWINSZ, &mut ws) == 0
            && ws.ws_col > 0
            && ws.ws_row > 0
            && ws.ws_xpixel > 0
            && ws.ws_ypixel > 0
        {
            let cw = (ws.ws_xpixel as i32) / (ws.ws_col as i32);
            let ch = (ws.ws_ypixel as i32) / (ws.ws_row as i32);
            return (cw.max(1), ch.max(1));
        }
    }
    (8, 16) /* fallback */
}

/// Image dimensions (supports png, jpeg, gif, webp, bmp).
fn img_dimensions(path: &Path) -> Option<(u32, u32)> {
    image::image_dimensions(path).ok()
}

/// Output Kitty graphics to stdout (call after terminal.draw).
/// Scales image to fit art area with aspect ratio, centers when narrower.
pub fn kitty_draw(cover_path: &Path, art_area: Rect, track: &crate::db::Track) {
    let aw = art_area.width as i32;
    let art_h = art_area.height as i32;
    let art_y = art_area.y as i32;
    let art_x_base = art_area.x as i32;

    let (cw, ch) = get_cell_size();
    // eprintln!(
    //     "cell size: {}x{} px, art_inner: {}x{} cells, visual: {}x{} px",
    //     cw, ch, aw, art_h, aw * cw, art_h * ch
    // );
    let (iw, ih) = if track.cover_w > 0 && track.cover_h > 0 {
        (track.cover_w as i32, track.cover_h as i32)
    } else {
        img_dimensions(cover_path)
            .map(|(w, h)| (w as i32, h as i32))
            .unwrap_or((0, 0))
    };

    let (mut art_cols, mut art_rows, mut art_x) = (aw, art_h, 0i32);
    if iw > 0 && ih > 0 {
        let area_pw = aw * cw;
        let area_ph = art_h * ch;
        let sw = area_pw as f64 / iw as f64;
        let sh = area_ph as f64 / ih as f64;
        let s = sw.min(sh);
        let disp_pw = (iw as f64 * s) as i32;
        let disp_ph = (ih as f64 * s) as i32;
        art_cols = ((disp_pw + cw - 1) / cw).clamp(1, aw);
        art_rows = ((disp_ph + ch - 1) / ch).clamp(1, art_h);
        art_x = if art_cols < aw { (aw - art_cols) / 2 } else { 0 };
    }

    unsafe {
        KITTY_ID += 1;
        let id = KITTY_ID;
        let path_str = cover_path.to_string_lossy().into_owned();
        let b64 = B64.encode(path_str.as_bytes());
        let cmd = format!("\x1b7\x1b_Gq=2,a=t,f=100,t=f,i={};{}\x1b\\", id, b64);
        let _ = std::io::Write::write_all(&mut std::io::stdout(), cmd.as_bytes());
        let _ = std::io::Write::flush(&mut std::io::stdout());

        let y = art_y + 1;
        let x = art_x_base + art_x + 1;
        let place = format!(
            "\x1b[{};{}H\x1b_Gq=2,a=p,i={},p=1,c={},r={},C=1\x1b\\",
            y, x, id, art_cols, art_rows
        );
        let _ = std::io::Write::write_all(&mut std::io::stdout(), place.as_bytes());
        let _ = std::io::Write::write_all(&mut std::io::stdout(), b"\x1b8");
        let _ = std::io::Write::flush(&mut std::io::stdout());
    }
}

pub fn kitty_delete_all() {
    let cmd = b"\x1b_Gq=2,a=d,d=A\x1b\\";
    let _ = std::io::Write::write_all(&mut std::io::stdout(), cmd);
    let _ = std::io::Write::flush(&mut std::io::stdout());
}

/// Open image in external viewer. Tries tui-actions/open-image (tui.c parity)
/// first, else xdg-open.
pub fn open_image(path: &Path) {
    let script = dirs::home_dir()
        .map(|h| h.join(".config/qua-player/tui-actions/open-image"));
    if let Some(ref p) = script {
        if fs::metadata(p).map_or(false, |m| m.is_file()) {
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                if fs::metadata(p).map_or(false, |m| m.permissions().mode() & 0o111 != 0) {
                    let _ = std::process::Command::new(p)
                        .arg(path)
                        .stdin(std::process::Stdio::null())
                        .stdout(std::process::Stdio::null())
                        .stderr(std::process::Stdio::null())
                        .spawn();
                    return;
                }
            }
        }
    }
    let _ = std::process::Command::new("xdg-open").arg(path).spawn();
}
