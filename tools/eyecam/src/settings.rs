//! User settings, kept in ~/.config/eyecam/settings.json: `widen_sensitivity`, `lid_veto` and `source` are changed
//! from the panel (`set ...` on ctl.sock; `lid_veto` and `source` by hand too), `dev` only by hand.

use crate::dspsrc::Source;
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
    /// The eyelid veto (`veto`): off unless switched on here (`set lid_veto`, or by hand; followed while running).
    pub lid_veto: bool,
    /// Where the camera buffers come from (dspsrc::Source): auto (an installed eyecam-grab, else dsp_service) unless
    /// set (`set source`, or by hand). Read when eyecam-rec starts (--source overrides it); written to the file only
    /// when it isn't the default.
    pub source: Source,
}

impl Default for Settings {
    fn default() -> Self {
        Self { widen_sensitivity: crate::live::DEFAULT_WIDEN_SENSITIVITY, dev: false, lid_veto: false, source: Source::Auto }
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
                if let Some(Json::Bool(v)) = j.get("lid_veto") {
                    s.lid_veto = *v;
                }
                if let Some(v) = j.get("source") {
                    match v.str().and_then(Source::parse) {
                        Some(source) => s.source = source,
                        None => eprintln!("eyecam-rec: ignoring settings.json source (grab, dsp or auto)"),
                    }
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
        // `source` only when set, so the file stays as it was for everyone who never set it.
        let source = match self.source {
            Source::Auto => String::new(),
            s => format!(",\n  \"source\": \"{}\"", s.label()),
        };
        let text = format!(
            "{{\n  \"version\": 1,\n  \"widen_sensitivity\": {},\n  \"dev\": {},\n  \"lid_veto\": {}{source}\n}}\n",
            json::num(self.widen_sensitivity),
            self.dev,
            self.lid_veto
        );
        std::fs::write(&tmp, text).map_err(|e| format!("{}: {e}", tmp.display()))?;
        std::fs::rename(&tmp, path).map_err(|e| format!("{}: {e}", path.display()))
    }

    /// Change one setting in `path` (a `set ...` from ctl.sock), keeping the others as the file has them.
    pub fn update(path: &Path, change: impl FnOnce(&mut Settings)) -> Result<(), String> {
        let mut s = Settings::load(path);
        change(&mut s);
        s.save(path)
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
        Settings { widen_sensitivity: 0.25, dev: false, lid_veto: true, ..Settings::default() }.save(&p).unwrap();
        assert_eq!(Settings::load(&p).widen_sensitivity, 0.25);
        Settings { widen_sensitivity: 0.75, dev: true, lid_veto: false, ..Settings::default() }.save(&p).unwrap();
        assert_eq!(Settings::load(&p), Settings { widen_sensitivity: 0.75, dev: true, lid_veto: false, ..Settings::default() });
        std::fs::write(&p, "{\"widen_sensitivity\": 3}").unwrap();
        assert_eq!(Settings::load(&p), Settings::default());
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn a_set_keeps_the_other_settings() {
        let dir = std::env::temp_dir().join(format!("eyecam-settings-set-{}", std::process::id()));
        let p = dir.join("settings.json");
        // No file yet: the defaults with the one change
        Settings::update(&p, |s| s.lid_veto = true).unwrap();
        assert_eq!(Settings::load(&p), Settings { lid_veto: true, ..Settings::default() });
        Settings { widen_sensitivity: 0.3, dev: true, lid_veto: true, ..Settings::default() }.save(&p).unwrap();
        // `set widen_sensitivity` keeps `dev` and `lid_veto`...
        Settings::update(&p, |s| s.widen_sensitivity = 0.8).unwrap();
        assert_eq!(Settings::load(&p), Settings { widen_sensitivity: 0.8, dev: true, lid_veto: true, ..Settings::default() });
        // ...and `set lid_veto` keeps `widen_sensitivity` and `dev`, both ways
        Settings::update(&p, |s| s.lid_veto = false).unwrap();
        assert_eq!(Settings::load(&p), Settings { widen_sensitivity: 0.8, dev: true, lid_veto: false, ..Settings::default() });
        Settings::update(&p, |s| s.lid_veto = true).unwrap();
        assert_eq!(Settings::load(&p), Settings { widen_sensitivity: 0.8, dev: true, lid_veto: true, ..Settings::default() });
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
            assert_eq!(Settings::load(&p), Settings { widen_sensitivity: widen, dev, lid_veto: false, ..Settings::default() }, "{text}");
        }
        // lid_veto: off unless true (not a bool: off).
        for (text, on) in [("{\"lid_veto\": false}", false), ("{\"lid_veto\": true}", true), ("{\"lid_veto\": 1}", false), ("{}", false)] {
            std::fs::write(&p, text).unwrap();
            assert_eq!(Settings::load(&p).lid_veto, on, "{text}");
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

    #[test]
    fn reads_the_source_and_writes_it_only_when_set() {
        let dir = std::env::temp_dir().join(format!("eyecam-settings-source-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let p = dir.join("settings.json");
        for (text, source) in [
            ("{}", Source::Auto),
            ("{\"source\": \"grab\"}", Source::Grab),
            ("{\"source\": \"dsp\"}", Source::Dsp),
            ("{\"source\": \"auto\", \"dev\": true}", Source::Auto),
            // Unknown or not a string: the default.
            ("{\"source\": \"udmabuf\"}", Source::Auto),
            ("{\"source\": 1}", Source::Auto),
        ] {
            std::fs::write(&p, text).unwrap();
            assert_eq!(Settings::load(&p).source, source, "{text}");
        }
        // The default is not written: the file is the same as before `source` existed.
        Settings::default().save(&p).unwrap();
        let text = std::fs::read_to_string(&p).unwrap();
        assert!(!text.contains("source"), "{text}");
        assert!(text.ends_with("\"lid_veto\": false\n}\n"), "{text}");
        // A set one is kept by a `set ...` of another setting, eyecam-grab set by hand too.
        std::fs::write(&p, "{\"source\": \"grab\"}").unwrap();
        Settings::update(&p, |s| s.lid_veto = true).unwrap();
        assert_eq!(Settings::load(&p).source, Source::Grab);
        std::fs::write(&p, "{\"source\": \"dsp\", \"lid_veto\": true}").unwrap();
        Settings::update(&p, |s| s.widen_sensitivity = 0.6).unwrap();
        let s = Settings::load(&p);
        assert_eq!((s.source, s.lid_veto, s.widen_sensitivity), (Source::Dsp, true, 0.6));
        assert!(crate::json::parse(&std::fs::read_to_string(&p).unwrap()).is_ok());
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn set_source_keeps_the_rest_and_auto_leaves_the_file_as_before() {
        let dir = std::env::temp_dir().join(format!("eyecam-settings-set-source-{}", std::process::id()));
        let p = dir.join("settings.json");
        Settings { widen_sensitivity: 0.3, dev: true, lid_veto: true, ..Settings::default() }.save(&p).unwrap();
        let before = std::fs::read_to_string(&p).unwrap();
        // `set source dsp` / `grab`: written, the other settings kept
        for source in [Source::Dsp, Source::Grab] {
            Settings::update(&p, |s| s.source = source).unwrap();
            assert_eq!(Settings::load(&p), Settings { widen_sensitivity: 0.3, dev: true, lid_veto: true, source });
            assert!(std::fs::read_to_string(&p).unwrap().contains(&format!("\"source\": \"{}\"", source.label())));
        }
        // `set source auto`: the key goes, and the file is the same as before it was set
        Settings::update(&p, |s| s.source = Source::Auto).unwrap();
        assert_eq!(std::fs::read_to_string(&p).unwrap(), before);
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
