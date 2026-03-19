//! Database access for music library.

use rusqlite::Connection;
use std::path::Path;

/// Track loaded from the database (matches tui.c track_t subset).
#[derive(Debug, Clone)]
pub struct Track {
    pub path: String,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub album_artist: String,
    pub duration: f64,
    pub format: String,
    pub sample_rate: i32,
    pub bit_depth: i32,
    pub track_num: i32,
    pub date: String,
    pub genre: String,
    pub cover: String,
    pub cover_w: i32,
    pub cover_h: i32,
    pub audiomd5: String,
}

pub fn db_path() -> Option<std::path::PathBuf> {
    dirs::home_dir().map(|h| h.join(".config/qua-player/music.db"))
}

/// Session state for restore (tui.c parity + find/filter persistence).
#[derive(Default)]
pub struct SessionState {
    pub sel_path: Option<String>,
    pub top: usize,
    pub find_query: String,
    pub find_album: Option<String>,
    pub filter_query: String,
}

/// Load session state from state table.
pub fn load_state(db_path: &Path) -> SessionState {
    let conn = match Connection::open(db_path) {
        Ok(c) => c,
        Err(_) => return SessionState::default(),
    };
    let _ = conn.execute(
        "CREATE TABLE IF NOT EXISTS state (key TEXT PRIMARY KEY, val TEXT)",
        [],
    );
    let mut sel_path = None;
    let mut top = 0usize;
    let mut find_query = String::new();
    let mut find_album = None;
    let mut filter_query = String::new();
    if let Ok(mut stmt) = conn.prepare("SELECT key, val FROM state") {
        let rows = stmt.query_map([], |row| {
            Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?))
        });
        if let Ok(rows) = rows {
            for row in rows.flatten() {
                match row.0.as_str() {
                    "sel_path" if !row.1.is_empty() => sel_path = Some(row.1),
                    "top" => top = row.1.parse().unwrap_or(0),
                    "find_query" => find_query = row.1,
                    "find_album" if !row.1.is_empty() => find_album = Some(row.1),
                    "filter_query" => filter_query = row.1,
                    _ => {}
                }
            }
        }
    }
    SessionState {
        sel_path,
        top,
        find_query,
        find_album,
        filter_query,
    }
}

/// Save session state to state table.
pub fn save_state(
    db_path: &Path,
    sel_path: &str,
    top: usize,
    find_query: &str,
    find_album: Option<&str>,
    filter_query: &str,
) {
    let conn = match Connection::open(db_path) {
        Ok(c) => c,
        Err(_) => return,
    };
    let _ = conn.execute(
        "CREATE TABLE IF NOT EXISTS state (key TEXT PRIMARY KEY, val TEXT)",
        [],
    );
    let _ = conn.execute(
        "INSERT OR REPLACE INTO state(key, val) VALUES ('sel_path', ?1)",
        [sel_path],
    );
    let _ = conn.execute(
        "INSERT OR REPLACE INTO state(key, val) VALUES ('top', ?1)",
        [top.to_string()],
    );
    let _ = conn.execute(
        "INSERT OR REPLACE INTO state(key, val) VALUES ('find_query', ?1)",
        [find_query],
    );
    let _ = conn.execute(
        "INSERT OR REPLACE INTO state(key, val) VALUES ('find_album', ?1)",
        [find_album.unwrap_or("")],
    );
    let _ = conn.execute(
        "INSERT OR REPLACE INTO state(key, val) VALUES ('filter_query', ?1)",
        [filter_query],
    );
}

pub fn load_tracks(db_path: &Path) -> Result<Vec<Track>, String> {
    let conn = Connection::open(db_path).map_err(|e| e.to_string())?;
    let _ = conn.execute("ALTER TABLE tracks ADD COLUMN cover TEXT DEFAULT ''", []);
    let _ = conn.execute("ALTER TABLE tracks ADD COLUMN cover_w INTEGER DEFAULT 0", []);
    let _ = conn.execute("ALTER TABLE tracks ADD COLUMN cover_h INTEGER DEFAULT 0", []);
    let _ = conn.execute("ALTER TABLE tracks ADD COLUMN audiomd5 TEXT DEFAULT ''", []);
    let mut stmt = conn
        .prepare(
            "SELECT path, title, artist, album, album_artist, duration,
                    format, sample_rate, bit_depth, track_num, date, genre,
                    COALESCE(cover,''), COALESCE(cover_w,0), COALESCE(cover_h,0),
                    COALESCE(audiomd5,'')
             FROM tracks
             ORDER BY COALESCE(NULLIF(album_artist,''),NULLIF(artist,''),'~'),
                      COALESCE(NULLIF(date,''),'~'),
                      COALESCE(NULLIF(album,''),'~'), path, track_num, title",
        )
        .map_err(|e| e.to_string())?;
    let rows = stmt
        .query_map([], |row| {
            let path: String = row.get(0)?;
            let mut title: String = row
                .get::<_, Option<String>>(1)
                .ok()
                .flatten()
                .unwrap_or_default();
            let mut artist: String = row
                .get::<_, Option<String>>(2)
                .ok()
                .flatten()
                .unwrap_or_default();
            let album: String = row
                .get::<_, Option<String>>(3)
                .ok()
                .flatten()
                .unwrap_or_default();
            let album_artist: String = row
                .get::<_, Option<String>>(4)
                .ok()
                .flatten()
                .unwrap_or_default();
            let duration: f64 = row.get(5).unwrap_or(0.0);
            let format: String = row
                .get::<_, Option<String>>(6)
                .ok()
                .flatten()
                .unwrap_or_default();
            let sample_rate: i32 = row.get(7).unwrap_or(0);
            let bit_depth: i32 = row.get(8).unwrap_or(0);
            let track_num: i32 = row.get(9).unwrap_or(0);
            let date: String = row
                .get::<_, Option<String>>(10)
                .ok()
                .flatten()
                .unwrap_or_default();
            let genre: String = row
                .get::<_, Option<String>>(11)
                .ok()
                .flatten()
                .unwrap_or_default();
            let cover: String = row
                .get::<_, Option<String>>(12)
                .ok()
                .flatten()
                .unwrap_or_default();
            let cover_w: i32 = row.get(13).unwrap_or(0);
            let cover_h: i32 = row.get(14).unwrap_or(0);
            let audiomd5: String = row
                .get::<_, Option<String>>(15)
                .ok()
                .flatten()
                .unwrap_or_default();

            if title.is_empty() {
                title = path.rsplit('/').next().unwrap_or(&path).to_string();
                if let Some(dot) = title.rfind('.') {
                    title.truncate(dot);
                }
            }
            if artist.is_empty() {
                artist = "__".to_string();
            }

            Ok(Track {
                path,
                title,
                artist,
                album,
                album_artist,
                duration,
                format,
                sample_rate,
                bit_depth,
                track_num,
                date,
                genre,
                cover,
                cover_w,
                cover_h,
                audiomd5,
            })
        })
        .map_err(|e| e.to_string())?;
    let mut tracks = Vec::new();
    for row in rows {
        tracks.push(row.map_err(|e| e.to_string())?);
    }
    Ok(tracks)
}

/// Update track audiomd5 in DB (tui.c: click on empty MD5 to compute).
pub fn update_track_audiomd5(db_path: &Path, path: &str, md5: &str) {
    if let Ok(conn) = Connection::open(db_path) {
        let _ = conn.execute(
            "UPDATE tracks SET audiomd5=?1 WHERE path=?2",
            rusqlite::params![md5, path],
        );
    }
}
