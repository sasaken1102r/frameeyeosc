//! User settings changed from the panel (`set ...` on ctl.sock), kept in ~/.config/eyecam/settings.json.

use crate::json::{self, Json};
use std::path::{Path, PathBuf};

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Settings {
    /// 0 (strictest) .. 1 (most sensitive); see `live::wide_curve`.
    pub widen_sensitivity: f64,
}

impl Default for Settings {
    fn default() -> Self {
        Self { widen_sensitivity: crate::live::DEFAULT_WIDEN_SENSITIVITY }
    }
}

/// The default settings file, ~/.config/eyecam/settings.json.
pub fn path() -> PathBuf {
    let home = std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("."));
    home.join(".config/eyecam/settings.json")
}

impl Settings {
    /// The settings in `path`; defaults for a missing file, and for values that are missing or out of range.
    pub fn load(path: &Path) -> Self {
        let mut s = Settings::default();
        let Ok(text) = std::fs::read_to_string(path) else { return s };
        match json::parse(&text) {
            Ok(j) => {
                if let Some(v) = j.get("widen_sensitivity").and_then(Json::num).filter(|v| (0.0..=1.0).contains(v)) {
                    s.widen_sensitivity = v;
                }
            }
            Err(e) => eprintln!("eyecam-rec: ignoring {}: {e}", path.display()),
        }
        s
    }

    /// Write a new file next to `path` and rename it over `path`.
    pub fn save(&self, path: &Path) -> Result<(), String> {
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
        }
        let tmp = path.with_extension("json.tmp");
        let text = format!("{{\n  \"version\": 1,\n  \"widen_sensitivity\": {}\n}}\n", json::num(self.widen_sensitivity));
        std::fs::write(&tmp, text).map_err(|e| format!("{}: {e}", tmp.display()))?;
        std::fs::rename(&tmp, path).map_err(|e| format!("{}: {e}", path.display()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trips_and_falls_back_to_defaults() {
        let dir = std::env::temp_dir().join(format!("eyecam-settings-test-{}", std::process::id()));
        let p = dir.join("settings.json");
        assert_eq!(Settings::load(&p), Settings::default());
        Settings { widen_sensitivity: 0.25 }.save(&p).unwrap();
        assert_eq!(Settings::load(&p).widen_sensitivity, 0.25);
        std::fs::write(&p, "{\"widen_sensitivity\": 3}").unwrap();
        assert_eq!(Settings::load(&p), Settings::default());
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
