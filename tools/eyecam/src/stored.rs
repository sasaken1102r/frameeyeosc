//! What eyecam-rec keeps in its data folder (~/eyecam) that the panel shows and can delete ("保存してるデータ" on its
//! Advanced tab): the eye-camera recordings (`rec_<time>`, about 2.5 GB each) and the eye images that calibrations
//! keep while `dev` is on in settings.json (CALIB_IMAGES in each `calib_<time>`, about 560 MB each). A thread of its own
//! (Keeper) counts them every SCAN_EVERY_S and right after a delete, for status.json `stored`, and does the deletes
//! asked for with `delete rec` / `delete calib_images` on ctl.sock (request_delete), never while a recording or a
//! calibration runs, and none starts while one is under way (`deleting`, checked under the status lock).
//!
//! Everything else in a calibration's folder stays: its calib_result.json is what the widen step history is rebuilt
//! from when calib.json has none (live::CalibFile::history_from_results), what status.json's last_calib comes from
//! after a restart (status::LastCalib::newest_in), and what the panel copies into its records and squint-training
//! recordings; meta.txt, frames.csv, valve.csv, cues.csv and calib_samples.csv are small text files kept beside it.
//! The squint-training recordings (contrib/) and the drift logs (drift/) are not touched: the first has a delete of
//! its own on the panel (that warns to send them first), the second is a few MB of numbers.

use crate::status::Status;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, mpsc};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

/// How often the folder is counted again (s).
pub const SCAN_EVERY_S: f64 = 10.0;
/// The eye images a calibration keeps with `dev` on (eyecam-rec's Session and dump_ring), the only files a
/// `delete calib_images` removes from it.
pub const CALIB_IMAGES: [&str; 4] = ["eye_L.raw", "eye_R.raw", "headers.bin", "lock_dump.bin"];

/// The reply to `delete` while a recording or a calibration runs.
pub const DELETE_BUSY: &str = "録画や校正のあいだは消せないよ";
/// ...and while another delete is under way.
pub const DELETE_UNDER_WAY: &str = "消しているところ（終わるまで待ってね）";
/// The reply to `start` / `calib` while a delete is under way.
pub const START_DELETING: &str = "保存してるデータを消しているところ（終わるまで待ってね）";
/// The reply to `delete` with anything but rec or calib_images.
pub const DELETE_BAD_KIND: &str = "delete は rec か calib_images で";

/// What `delete` takes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    /// The eye-camera recordings: each `rec_<time>` folder, whole.
    Rec,
    /// The calibrations' eye images: CALIB_IMAGES in each `calib_<time>` folder (the folder and the rest stay).
    CalibImages,
}

impl Kind {
    pub fn name(self) -> &'static str {
        match self {
            Kind::Rec => "rec",
            Kind::CalibImages => "calib_images",
        }
    }

    pub fn parse(word: &str) -> Option<Self> {
        [Kind::Rec, Kind::CalibImages].into_iter().find(|k| k.name() == word)
    }
}

/// How many (recordings, or calibrations with eye images) and their bytes.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Usage {
    pub count: u64,
    pub bytes: u64,
}

impl Usage {
    fn to_json(self) -> String {
        format!("{{\"count\":{},\"bytes\":{}}}", self.count, self.bytes)
    }
}

/// A delete that ended: what it took, and why not all of it (the first file that couldn't go and the system's reason,
/// "" when it went through).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Deleted {
    pub kind: &'static str,
    pub usage: Usage,
    pub error: String,
    /// When it ended (Unix seconds).
    pub unix: f64,
}

/// status.json `stored`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Stored {
    /// When it was last counted (Unix seconds; 0 before the first count).
    pub scanned_unix: f64,
    pub rec: Usage,
    pub calib_images: Usage,
    /// The delete under way ("rec" / "calib_images"), "" for none.
    pub deleting: &'static str,
    /// The last delete since eyecam-rec started.
    pub deleted: Option<Deleted>,
}

impl Stored {
    pub fn to_json(&self) -> String {
        let deleted = self.deleted.as_ref().map_or("null".into(), |d| {
            format!(
                "{{\"kind\":{},\"count\":{},\"bytes\":{},\"error\":{},\"unix\":{:.3}}}",
                crate::json::string(d.kind),
                d.usage.count,
                d.usage.bytes,
                crate::json::string(&d.error),
                d.unix
            )
        });
        format!(
            "{{\"scanned_unix\":{:.3},\"rec\":{},\"calib_images\":{},\"deleting\":{},\"deleted\":{deleted}}}",
            self.scanned_unix,
            self.rec.to_json(),
            self.calib_images.to_json(),
            crate::json::string(self.deleting),
        )
    }
}

/// Whether a name is `<prefix>YYYY-MM-DD_HH-MM-SS` (eyecam-rec's local_stamp), digits where the pattern has them.
fn stamped(name: &str, prefix: &str) -> bool {
    const PATTERN: &[u8] = b"9999-99-99_99-99-99";
    name.strip_prefix(prefix).is_some_and(|rest| {
        rest.len() == PATTERN.len()
            && rest.bytes().zip(PATTERN).all(|(c, &p)| if p == b'9' { c.is_ascii_digit() } else { c == p })
    })
}

/// The folders under `dir` named `<prefix><time>`, real folders only (a link is never one), by name.
fn folders(dir: &Path, prefix: &str) -> Vec<PathBuf> {
    let Ok(entries) = fs::read_dir(dir) else { return Vec::new() };
    let mut found: Vec<PathBuf> = entries
        .filter_map(|e| e.ok())
        .filter(|e| e.file_name().to_str().is_some_and(|n| stamped(n, prefix)))
        .filter(|e| e.file_type().is_ok_and(|t| t.is_dir()))
        .map(|e| e.path())
        .collect();
    found.sort();
    found
}

/// The bytes of the files in a folder and its folders (links are not followed, and count as nothing).
fn tree_bytes(dir: &Path) -> u64 {
    let Ok(entries) = fs::read_dir(dir) else { return 0 };
    entries
        .filter_map(|e| e.ok())
        .map(|e| match e.file_type() {
            Ok(t) if t.is_dir() => tree_bytes(&e.path()),
            Ok(t) if t.is_file() => e.metadata().map_or(0, |m| m.len()),
            _ => 0,
        })
        .sum()
}

/// A calibration's eye images that are there (regular files only) and their bytes.
fn calib_images(dir: &Path) -> Vec<(PathBuf, u64)> {
    CALIB_IMAGES
        .iter()
        .map(|name| dir.join(name))
        .filter_map(|p| fs::symlink_metadata(&p).ok().filter(|m| m.is_file()).map(|m| (p, m.len())))
        .collect()
}

/// Count one kind in the data folder `dir`.
pub fn usage(dir: &Path, kind: Kind) -> Usage {
    let mut u = Usage::default();
    match kind {
        Kind::Rec => {
            for d in folders(dir, "rec_") {
                u.count += 1;
                u.bytes += tree_bytes(&d);
            }
        }
        Kind::CalibImages => {
            for d in folders(dir, "calib_") {
                let images = calib_images(&d);
                if !images.is_empty() {
                    u.count += 1;
                    u.bytes += images.iter().map(|(_, b)| b).sum::<u64>();
                }
            }
        }
    }
    u
}

/// Delete one kind in the data folder `dir`: what went (counted as `usage` counts it), and why not all of it if some
/// couldn't go (the first reason; the rest is still tried).
pub fn delete(dir: &Path, kind: Kind) -> (Usage, Result<(), String>) {
    let mut gone = Usage::default();
    let mut first_error = None;
    let mut failed = |path: &Path, e: std::io::Error| {
        eprintln!("eyecam-rec: can't delete {}: {e}", path.display());
        first_error.get_or_insert_with(|| format!("{}: {e}", path.display()));
    };
    match kind {
        Kind::Rec => {
            for d in folders(dir, "rec_") {
                let bytes = tree_bytes(&d);
                // (links in it are removed, never followed)
                match fs::remove_dir_all(&d) {
                    Ok(()) => {
                        gone.count += 1;
                        gone.bytes += bytes;
                    }
                    Err(e) => failed(&d, e),
                }
            }
        }
        Kind::CalibImages => {
            for d in folders(dir, "calib_") {
                let mut any = false;
                for (path, bytes) in calib_images(&d) {
                    match fs::remove_file(&path) {
                        Ok(()) => {
                            any = true;
                            gone.bytes += bytes;
                        }
                        Err(e) => failed(&path, e),
                    }
                }
                gone.count += any as u64;
            }
        }
    }
    (gone, first_error.map_or(Ok(()), Err))
}

/// The states in which nothing is deleted: a recording or a calibration runs (`searching` comes before a recording).
pub fn busy_state(state: &str) -> bool {
    matches!(state, "searching" | "recording" | "calibrating")
}

/// What the keeper is asked.
enum Job {
    Delete(Kind),
}

/// The thread that counts and deletes (see the module's doc).
pub struct Keeper {
    tx: mpsc::Sender<Job>,
}

impl Keeper {
    /// Start counting the data folder `dir` into `status` until `shutdown`. With `fake`, a delete takes nothing (the
    /// fake daemon: it says what it would have deleted, and counts the folder as it is).
    pub fn spawn(dir: PathBuf, status: Arc<Mutex<Status>>, fake: bool, shutdown: Arc<AtomicBool>) -> (Self, JoinHandle<()>) {
        let (tx, rx) = mpsc::channel::<Job>();
        let thread = thread::spawn(move || {
            // Below the live work (eyecam-rec runs at nice 10 already)
            unsafe { libc::setpriority(libc::PRIO_PROCESS, libc::syscall(libc::SYS_gettid) as libc::id_t, 15) };
            let scan = || {
                let stored = Stored {
                    scanned_unix: unix_now(),
                    rec: usage(&dir, Kind::Rec),
                    calib_images: usage(&dir, Kind::CalibImages),
                    ..Stored::default()
                };
                let mut s = status.lock().unwrap();
                s.stored = Stored { deleting: s.stored.deleting, deleted: s.stored.deleted.take(), ..stored };
            };
            let mut next = Instant::now();
            while !shutdown.load(Ordering::SeqCst) {
                if Instant::now() >= next {
                    scan();
                    next = Instant::now() + Duration::from_secs_f64(SCAN_EVERY_S);
                }
                let Ok(Job::Delete(kind)) = rx.recv_timeout(Duration::from_millis(200)) else { continue };
                let (usage, result) = if fake { (usage(&dir, kind), Ok(())) } else { delete(&dir, kind) };
                eprintln!(
                    "eyecam-rec: {}deleted {} ({} of them, {} MB){}",
                    if fake { "(fake, nothing) " } else { "" },
                    kind.name(),
                    usage.count,
                    usage.bytes >> 20,
                    result.as_ref().err().map_or(String::new(), |e| format!("; not all: {e}"))
                );
                let error = result.err().unwrap_or_default();
                {
                    let mut s = status.lock().unwrap();
                    s.stored.deleting = "";
                    s.stored.deleted = Some(Deleted { kind: kind.name(), usage, error, unix: unix_now() });
                }
                scan();
                next = Instant::now() + Duration::from_secs_f64(SCAN_EVERY_S);
            }
        });
        (Self { tx }, thread)
    }

    /// `delete <kind>` (from the control thread): refused while a recording or a calibration runs or another delete is
    /// under way, else marked `deleting` in the status (under its lock, as idle checks it before a start) and handed to
    /// the thread. The reply line.
    pub fn request_delete(&self, kind: Kind, status: &Mutex<Status>) -> String {
        {
            let mut s = status.lock().unwrap();
            if busy_state(s.state) {
                return format!("err {DELETE_BUSY}");
            }
            if !s.stored.deleting.is_empty() {
                return format!("err {DELETE_UNDER_WAY}");
            }
            s.stored.deleting = kind.name();
        }
        if self.tx.send(Job::Delete(kind)).is_err() {
            status.lock().unwrap().stored.deleting = "";
            return "err 終了中".into();
        }
        "ok".into()
    }
}

fn unix_now() -> f64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0.0, |d| d.as_secs_f64())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A data folder in the temp dir, as eyecam-rec leaves it: two recordings, a calibration with `dev` (its eye
    /// images and the text beside them), one without, the squint-training recordings and a drift log, and things
    /// that only look like ours.
    fn data_folder(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("eyecam-stored-{tag}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        let write = |path: &str, bytes: usize| {
            let p = dir.join(path);
            fs::create_dir_all(p.parent().unwrap()).unwrap();
            fs::write(p, vec![b'x'; bytes]).unwrap();
        };
        for rec in ["rec_2026-10-12_09-59-05", "rec_2026-10-16_21-14-05"] {
            for (name, bytes) in [("eye_L.raw", 1000), ("eye_R.raw", 1000), ("frames.csv", 50), ("meta.txt", 20)] {
                write(&format!("{rec}/{name}"), bytes);
            }
        }
        let text = ["calib_result.json", "calib_samples.csv", "meta.txt", "frames.csv", "valve.csv", "cues.csv"];
        for name in text {
            write(&format!("calib_2026-10-31_17-03-35/{name}"), 30);
            write(&format!("calib_2026-10-30_08-00-00/{name}"), 30);
        }
        for (name, bytes) in [("eye_L.raw", 700), ("eye_R.raw", 700), ("headers.bin", 10), ("lock_dump.bin", 40)] {
            write(&format!("calib_2026-10-31_17-03-35/{name}"), bytes);
        }
        write("contrib/rec_2026-10-25_17-09-40/eye_L.raw", 900);
        write("contrib/rec_2026-10-25_17-09-40.calib_result.json", 30);
        write("contrib/aborted/rec_2026-10-25_14-20-55/eye_L.raw", 900);
        write("drift/eyes_2026-10-31_10-41-01.eyecam.csv", 60);
        // Not ours: another name, a file named like a recording, a link to a recording elsewhere
        write("rec_2026-10-12_09-59-05_copy/eye_L.raw", 500);
        write("rec_mine/eye_L.raw", 500);
        write("rec_2026-10-20_00-00-00", 5);
        let elsewhere = dir.join("elsewhere");
        write("elsewhere/rec_2026-10-21_00-00-00/eye_L.raw", 500);
        std::os::unix::fs::symlink(elsewhere.join("rec_2026-10-21_00-00-00"), dir.join("rec_2026-10-21_00-00-00")).unwrap();
        dir
    }

    #[test]
    fn counts_the_recordings_and_the_calibrations_eye_images() {
        let dir = data_folder("count");
        assert_eq!(usage(&dir, Kind::Rec), Usage { count: 2, bytes: 2 * 2070 });
        assert_eq!(usage(&dir, Kind::CalibImages), Usage { count: 1, bytes: 1450 });
        assert_eq!(usage(&dir.join("missing"), Kind::Rec), Usage::default());
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn deleting_recordings_takes_only_the_rec_folders() {
        let dir = data_folder("rec");
        let (gone, result) = delete(&dir, Kind::Rec);
        assert_eq!((gone, result), (Usage { count: 2, bytes: 2 * 2070 }, Ok(())));
        assert!(!dir.join("rec_2026-10-12_09-59-05").exists() && !dir.join("rec_2026-10-16_21-14-05").exists());
        // The calibrations (with their images), the squint-training recordings, the drift log and what isn't ours stay
        for kept in [
            "calib_2026-10-31_17-03-35/calib_result.json",
            "calib_2026-10-31_17-03-35/eye_L.raw",
            "calib_2026-10-30_08-00-00/calib_result.json",
            "contrib/rec_2026-10-25_17-09-40/eye_L.raw",
            "contrib/aborted/rec_2026-10-25_14-20-55/eye_L.raw",
            "drift/eyes_2026-10-31_10-41-01.eyecam.csv",
            "rec_2026-10-12_09-59-05_copy/eye_L.raw",
            "rec_mine/eye_L.raw",
            "rec_2026-10-20_00-00-00",
            "elsewhere/rec_2026-10-21_00-00-00/eye_L.raw",
        ] {
            assert!(dir.join(kept).exists(), "{kept}");
        }
        // (the link itself is left too: it isn't a folder of ours)
        assert!(fs::symlink_metadata(dir.join("rec_2026-10-21_00-00-00")).is_ok());
        assert_eq!(usage(&dir, Kind::Rec), Usage::default());
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn deleting_calibration_images_keeps_what_the_widen_history_and_last_calib_read() {
        let dir = data_folder("calib");
        // A successful wear calibration, as the history and last_calib read it back
        let result = r#"{"kind": "wear", "eyes": "anatomical", "time": "2026-10-31 17:03:35", "ok": true,
            "values": {"L": {"widen_step": 0.3, "open_gap": 1.1, "r_px": 40}, "R": {"widen_step": 0.25, "open_gap": 1.0, "r_px": 41}}}"#;
        fs::write(dir.join("calib_2026-10-31_17-03-35/calib_result.json"), result).unwrap();
        let history = crate::live::CalibFile::history_from_results(&dir);
        let (gone, outcome) = delete(&dir, Kind::CalibImages);
        assert_eq!((gone, outcome), (Usage { count: 1, bytes: 1450 }, Ok(())));
        for name in CALIB_IMAGES {
            assert!(!dir.join("calib_2026-10-31_17-03-35").join(name).exists(), "{name}");
        }
        // Every text file stays, in both calibrations: the widen history and last_calib read the same as before
        for calib in ["calib_2026-10-31_17-03-35", "calib_2026-10-30_08-00-00"] {
            for name in ["calib_result.json", "calib_samples.csv", "meta.txt", "frames.csv", "valve.csv", "cues.csv"] {
                assert!(dir.join(calib).join(name).exists(), "{calib}/{name}");
            }
        }
        assert_eq!(history.len(), 1);
        assert_eq!(crate::live::CalibFile::history_from_results(&dir), history);
        assert_eq!(crate::status::LastCalib::newest_in(&dir).map(|c| c.time), Some("2026-10-31 17:03:35".into()));
        // The recordings and the rest are not touched
        assert_eq!(usage(&dir, Kind::Rec).count, 2);
        assert!(dir.join("contrib/rec_2026-10-25_17-09-40/eye_L.raw").exists());
        assert_eq!(usage(&dir, Kind::CalibImages), Usage::default());
        // Again: nothing more to take
        assert_eq!(delete(&dir, Kind::CalibImages), (Usage::default(), Ok(())));
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn a_link_in_place_of_an_eye_image_is_left_alone() {
        let dir = data_folder("link");
        let calib = dir.join("calib_2026-10-30_08-00-00");
        std::os::unix::fs::symlink(dir.join("rec_mine/eye_L.raw"), calib.join("eye_L.raw")).unwrap();
        assert_eq!(usage(&dir, Kind::CalibImages).count, 1);
        let _ = delete(&dir, Kind::CalibImages);
        assert!(fs::symlink_metadata(calib.join("eye_L.raw")).is_ok() && dir.join("rec_mine/eye_L.raw").exists());
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn a_delete_is_refused_while_recording_or_calibrating_and_blocks_a_start() {
        let dir = data_folder("refuse");
        let status = Arc::new(Mutex::new(Status::default()));
        let shutdown = Arc::new(AtomicBool::new(false));
        let (keeper, thread) = Keeper::spawn(dir.clone(), status.clone(), false, shutdown.clone());
        for state in ["searching", "recording", "calibrating"] {
            status.lock().unwrap().state = state;
            assert_eq!(keeper.request_delete(Kind::Rec, &status), format!("err {DELETE_BUSY}"), "{state}");
            assert_eq!(status.lock().unwrap().stored.deleting, "");
        }
        // Idle: under way at once (a start is refused by idle until it is over), then done and counted again
        status.lock().unwrap().state = "idle";
        assert_eq!(keeper.request_delete(Kind::Rec, &status), "ok");
        let reply = keeper.request_delete(Kind::CalibImages, &status);
        let deleting = status.lock().unwrap().stored.deleting;
        assert!(reply == format!("err {DELETE_UNDER_WAY}") || (reply == "ok" && deleting == "calib_images"), "{reply}");
        let start = Instant::now();
        while !status.lock().unwrap().stored.deleting.is_empty() && start.elapsed() < Duration::from_secs(10) {
            thread::sleep(Duration::from_millis(20));
        }
        let s = status.lock().unwrap().clone();
        assert_eq!(s.stored.deleting, "");
        assert_eq!(s.stored.rec, Usage::default());
        assert!(s.stored.scanned_unix > 0.0);
        assert!(dir.join("calib_2026-10-31_17-03-35/calib_result.json").exists());
        assert!(!dir.join("rec_2026-10-12_09-59-05").exists());
        let json = s.stored.to_json();
        assert!(crate::json::parse(&json).is_ok(), "{json}");
        shutdown.store(true, Ordering::SeqCst);
        thread.join().unwrap();
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn the_fake_daemon_deletes_nothing() {
        let dir = data_folder("fake");
        let status = Arc::new(Mutex::new(Status { state: "idle", ..Status::default() }));
        let shutdown = Arc::new(AtomicBool::new(false));
        let (keeper, thread) = Keeper::spawn(dir.clone(), status.clone(), true, shutdown.clone());
        assert_eq!(keeper.request_delete(Kind::Rec, &status), "ok");
        let start = Instant::now();
        while status.lock().unwrap().stored.deleted.is_none() && start.elapsed() < Duration::from_secs(10) {
            thread::sleep(Duration::from_millis(20));
        }
        let deleted = status.lock().unwrap().stored.deleted.clone().unwrap();
        assert_eq!((deleted.kind, deleted.usage.count, deleted.error.as_str()), ("rec", 2, ""));
        assert!(dir.join("rec_2026-10-12_09-59-05/eye_L.raw").exists());
        shutdown.store(true, Ordering::SeqCst);
        thread.join().unwrap();
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn stored_is_one_line_of_json() {
        let s = Stored {
            scanned_unix: 1.5,
            rec: Usage { count: 13, bytes: 25_000_000_000 },
            calib_images: Usage { count: 52, bytes: 7 },
            deleting: "rec",
            deleted: Some(Deleted { kind: "calib_images", usage: Usage { count: 2, bytes: 9 }, error: String::new(), unix: 2.0 }),
        };
        assert_eq!(
            s.to_json(),
            "{\"scanned_unix\":1.500,\"rec\":{\"count\":13,\"bytes\":25000000000},\"calib_images\":{\"count\":52,\"bytes\":7},\"deleting\":\"rec\",\"deleted\":{\"kind\":\"calib_images\",\"count\":2,\"bytes\":9,\"error\":\"\",\"unix\":2.000}}"
        );
        assert!(Stored::default().to_json().ends_with(",\"deleting\":\"\",\"deleted\":null}"));
        assert_eq!(Kind::parse("rec"), Some(Kind::Rec));
        assert_eq!(Kind::parse("calib_images"), Some(Kind::CalibImages));
        assert_eq!(Kind::parse("calib"), None);
        assert!(stamped("rec_2026-10-12_09-59-05", "rec_") && !stamped("rec_2026-10-12_09-59-05_2", "rec_"));
        assert!(!stamped("calib_2026-10-12_09-59-05", "rec_") && !stamped("rec_2026-1x-12_09-59-05", "rec_"));
    }
}
