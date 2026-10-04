//! User settings, kept in ~/.config/eyecam/settings.json: `widen_sensitivity` is changed from the panel
//! (`set ...` on ctl.sock), `dev` only by hand.

use crate::json::{self, Json};
use std::path::{Path, PathBuf};
use std::time::SystemTime;

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Settings {
    /// 0 (strictest) .. 1 (most sensitive); see `live::wide_curve`.
    pub widen_sensitivity: f64,
    /// Developer mode: calibrations also keep their eye images (eye_*.raw, headers.bin, lock_dump.bin), as
    /// recordings do. Off for ordinary users, whose calibrations keep only the small text files.
    pub dev: bool,
}

impl Default for Settings {
    fn default() -> Self {
        Self { widen_sensitivity: crate::live::DEFAULT_WIDEN_SENSITIVITY, dev: false }
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
                if let Some(Json::Bool(v)) = j.get("dev") {
                    s.dev = *v;
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
        let text = format!(
            "{{\n  \"version\": 1,\n  \"widen_sensitivity\": {},\n  \"dev\": {}\n}}\n",
            json::num(self.widen_sensitivity),
            self.dev
        );
        std::fs::write(&tmp, text).map_err(|e| format!("{}: {e}", tmp.display()))?;
        std::fs::rename(&tmp, path).map_err(|e| format!("{}: {e}", path.display()))
    }
}

/// The settings file, read again whenever its modification time changes (so a hand edit of `dev` shows without a
/// restart). A missing file counts as one state of its own.
pub struct Watch {
    path: PathBuf,
    stamp: Option<Option<SystemTime>>,
    settings: Settings,
}

impl Watch {
    pub fn new(path: PathBuf) -> Self {
        Self { path, stamp: None, settings: Settings::default() }
    }

    /// The current settings (one stat per call; the file is read only when it changed).
    pub fn get(&mut self) -> Settings {
        let stamp = std::fs::metadata(&self.path).and_then(|m| m.modified()).ok();
        if self.stamp != Some(stamp) {
            self.stamp = Some(stamp);
            self.settings = Settings::load(&self.path);
        }
        self.settings
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
        Settings { widen_sensitivity: 0.25, dev: false }.save(&p).unwrap();
        assert_eq!(Settings::load(&p).widen_sensitivity, 0.25);
        Settings { widen_sensitivity: 0.75, dev: true }.save(&p).unwrap();
        assert_eq!(Settings::load(&p), Settings { widen_sensitivity: 0.75, dev: true });
        std::fs::write(&p, "{\"widen_sensitivity\": 3}").unwrap();
        assert_eq!(Settings::load(&p), Settings::default());
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn reads_dev_by_hand_and_ignores_unknown_keys() {
        let dir = std::env::temp_dir().join(format!("eyecam-settings-dev-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let p = dir.join("settings.json");
        let cases = [
            ("{\"version\": 1, \"widen_sensitivity\": 0.4, \"dev\": true, \"future\": [1, 2]}", 0.4, true),
            ("{\"widen_sensitivity\": 0.4, \"dev\": false}", 0.4, false),
            // Not a bool: the default (off).
            ("{\"dev\": 1}", crate::live::DEFAULT_WIDEN_SENSITIVITY, false),
            ("{\"dev\": \"true\"}", crate::live::DEFAULT_WIDEN_SENSITIVITY, false),
        ];
        for (text, widen, dev) in cases {
            std::fs::write(&p, text).unwrap();
            assert_eq!(Settings::load(&p), Settings { widen_sensitivity: widen, dev }, "{text}");
        }

        // Watch: missing file -> defaults; created -> read; rewritten -> read again.
        std::fs::remove_file(&p).unwrap();
        let mut w = Watch::new(p.clone());
        assert!(!w.get().dev);
        std::fs::write(&p, "{\"dev\": true}").unwrap();
        assert!(w.get().dev);
        // A different length guarantees a different file even on a coarse clock; set the time explicitly too.
        std::fs::write(&p, "{\"dev\": false }").unwrap();
        let later = SystemTime::now() + std::time::Duration::from_secs(5);
        std::fs::File::options().write(true).open(&p).unwrap().set_modified(later).unwrap();
        assert!(!w.get().dev);
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
