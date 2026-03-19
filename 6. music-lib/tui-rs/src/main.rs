/* qua-music-tui — ratatui music library browser */

mod app;
mod art;
mod db;
mod ui;
mod view;

fn main() -> Result<(), String> {
    let db_path = db::db_path().ok_or("$HOME not set")?;
    if !db_path.exists() {
        return Err(format!("Database not found: {}", db_path.display()));
    }
    let tracks = db::load_tracks(&db_path)?;
    if tracks.is_empty() {
        return Err("No tracks in database. Run qua-music-lib to index.".to_string());
    }
    app::run(&db_path, tracks).map_err(|e| e.to_string())
}
