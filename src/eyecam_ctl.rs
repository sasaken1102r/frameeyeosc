//! camera_lids for eyecam-rec too: while it is off, eyecam-rec is told to stop working out eye values from the camera
//! video ("live off" on its control socket), and "live on" once it is on again. frameeyeosc always runs while eyecam
//! is of any use, so it is the one that says so (not the panel, which may be closed).
//!
//! eyecam-rec takes "live on" / "live off" only while it is idle or in error (it holds the camera buffers then), and
//! starts with live on (after a restart, or once it has the buffers). So its status.json is read every CHECK_INTERVAL,
//! and the wanted state is sent whenever it is idle or in error and its "live" differs: at once, then again every
//! RESEND while status.json doesn't show it yet (a refused or lost command). All of it on a thread of its own, so
//! the eye data loop never waits for eyecam-rec. Nothing is sent while eyecam-rec isn't running (no fresh
//! status.json).

use serde::Deserialize;
use std::fs::OpenOptions;
use std::io::{BufRead, BufReader, Read, Write};
use std::os::unix::fs::OpenOptionsExt;
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

// How often status.json is read.
const CHECK_INTERVAL: Duration = Duration::from_secs(1);
// A command not shown in status.json yet is sent again after this long.
const RESEND: Duration = Duration::from_secs(3);
// The longest a command may take to send and answer.
const REPLY_TIMEOUT: Duration = Duration::from_secs(2);
// status.json older than this: eyecam-rec isn't running (it rewrites it about ten times a second).
const STALE_SEC: f64 = 5.0;
// eyecam-rec writes one short line; anything larger isn't its status file.
const MAX_STATUS_BYTES: u64 = 64 * 1024;

/// The part of eyecam-rec's status.json this needs.
#[derive(Debug, Default, Deserialize)]
struct EyecamStatus {
    #[serde(default)]
    state: String,
    #[serde(default)]
    live: bool,
    #[serde(default)]
    updated_unix: f64,
}

/// What to send now, if anything: the wanted state, while eyecam-rec runs (status.json fresh), is idle or in error, and
/// its live differs; not again within RESEND of sending the same.
fn decide(
    wanted: bool,
    status: Option<&EyecamStatus>,
    now_unix: f64,
    sent: Option<(bool, Instant)>,
    now: Instant,
) -> Option<bool> {
    let status = status?;
    if (now_unix - status.updated_unix).abs() > STALE_SEC {
        return None;
    }
    if status.state != "idle" && status.state != "error" {
        return None;
    }
    if status.live == wanted {
        return None;
    }
    if let Some((on, at)) = sent
        && on == wanted
        && now.saturating_duration_since(at) < RESEND
    {
        return None;
    }
    Some(wanted)
}

/// eyecam-rec's status.json, if it is a regular file of a sensible size that parses. Opened without blocking or
/// following a symlink, like the live file.
fn read_status(path: &Path) -> Option<EyecamStatus> {
    let mut file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NONBLOCK | libc::O_NOFOLLOW)
        .open(path)
        .ok()?;
    let metadata = file.metadata().ok()?;
    if !metadata.is_file() || metadata.len() > MAX_STATUS_BYTES {
        return None;
    }
    let mut text = String::new();
    Read::take(&mut file, MAX_STATUS_BYTES).read_to_string(&mut text).ok()?;
    serde_json::from_str(&text).ok()
}

/// One command on eyecam-rec's control socket: connect, write the line, read the one-line reply.
fn send(path: &Path, command: &str) -> Result<String, String> {
    let mut stream = UnixStream::connect(path).map_err(|error| error.to_string())?;
    stream.set_read_timeout(Some(REPLY_TIMEOUT)).map_err(|error| error.to_string())?;
    stream.set_write_timeout(Some(REPLY_TIMEOUT)).map_err(|error| error.to_string())?;
    stream.write_all(format!("{command}\n").as_bytes()).map_err(|error| error.to_string())?;
    let mut reply = String::new();
    BufReader::new(Read::take(stream, 256)).read_line(&mut reply).map_err(|error| error.to_string())?;
    Ok(reply.trim_end().to_owned())
}

fn unix_now() -> f64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map_or(0.0, |time| time.as_secs_f64())
}

/// Keeps eyecam-rec's live processing in step with camera_lids (see the module).
pub struct LiveControl {
    dir: PathBuf,
    sent: Option<(bool, Instant)>,
    // The last line logged, so a command refused again and again is said once.
    logged: Option<String>,
}

impl LiveControl {
    /// For eyecam-rec's folder (status.json and ctl.sock), the one its live file is in.
    pub fn new(dir: PathBuf) -> Self {
        Self {
            dir,
            sent: None,
            logged: None,
        }
    }

    /// One check: read status.json and send what is due. Returns what to log, only when it is new.
    pub fn step(&mut self, wanted: bool, now: Instant) -> Option<String> {
        let status = read_status(&self.dir.join("status.json"));
        let on = decide(wanted, status.as_ref(), unix_now(), self.sent, now)?;
        self.sent = Some((on, now));
        let command = if on { "live on" } else { "live off" };
        let line = match send(&self.dir.join("ctl.sock"), command) {
            Ok(reply) if reply == "ok" => {
                format!("Told eyecam-rec \"{command}\" (camera_lids {})", if on { "on" } else { "off" })
            }
            Ok(reply) => format!("eyecam-rec answered \"{reply}\" to \"{command}\"; trying again"),
            Err(error) => format!("Couldn't tell eyecam-rec \"{command}\" ({error}); trying again"),
        };
        if self.logged.as_deref() == Some(line.as_str()) {
            return None;
        }
        self.logged = Some(line.clone());
        Some(line)
    }

    /// Run it on a thread of its own, every CHECK_INTERVAL, with camera_lids as `wanted` holds it.
    pub fn spawn(mut self, wanted: Arc<AtomicBool>) -> std::io::Result<std::thread::JoinHandle<()>> {
        std::thread::Builder::new().name("eyecam-live".into()).spawn(move || {
            loop {
                if let Some(line) = self.step(wanted.load(Ordering::Relaxed), Instant::now()) {
                    eprintln!("{line}");
                }
                std::thread::sleep(CHECK_INTERVAL);
            }
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::eyecam_live::tests::TempDir;
    use std::os::unix::net::UnixListener;

    fn status(state: &str, live: bool, age: f64) -> EyecamStatus {
        EyecamStatus {
            state: state.into(),
            live,
            updated_unix: 1000.0 - age,
        }
    }

    #[test]
    fn it_sends_only_where_eyecam_rec_takes_it_and_differs() {
        let now = Instant::now();
        let at = |state: &str, live: bool| status(state, live, 0.1);
        // In step: nothing; off while idle or in error: off
        assert_eq!(decide(true, Some(&at("idle", true)), 1000.0, None, now), None);
        assert_eq!(decide(false, Some(&at("idle", true)), 1000.0, None, now), Some(false));
        assert_eq!(decide(false, Some(&at("error", true)), 1000.0, None, now), Some(false));
        assert_eq!(decide(true, Some(&at("idle", false)), 1000.0, None, now), Some(true));
        // Busy, or waiting for the buffers: later
        for busy in ["calibrating", "recording", "searching", "waiting_fds", "stopped", ""] {
            assert_eq!(decide(false, Some(&at(busy, true)), 1000.0, None, now), None, "{busy}");
        }
        // Not running (status.json stale, or none)
        assert_eq!(decide(false, Some(&status("idle", true, 6.0)), 1000.0, None, now), None);
        assert_eq!(decide(false, None, 1000.0, None, now), None);
        // Just sent: not again until RESEND; the other way at once
        let sent = Some((false, now));
        assert_eq!(decide(false, Some(&at("idle", true)), 1000.0, sent, now + Duration::from_secs(1)), None);
        assert_eq!(decide(false, Some(&at("idle", true)), 1000.0, sent, now + RESEND), Some(false));
        assert_eq!(decide(true, Some(&at("idle", false)), 1000.0, sent, now), Some(true));
    }

    /// A stand-in eyecam-rec: answers each command on `dir`/ctl.sock with `reply`, and returns what it got.
    fn fake_eyecam(dir: &Path, reply: &'static str) -> std::sync::mpsc::Receiver<String> {
        let listener = UnixListener::bind(dir.join("ctl.sock")).unwrap();
        let (tx, rx) = std::sync::mpsc::channel();
        std::thread::spawn(move || {
            for stream in listener.incoming() {
                let mut stream = stream.unwrap();
                let mut line = String::new();
                BufReader::new(&stream).read_line(&mut line).unwrap();
                stream.write_all(format!("{reply}\n").as_bytes()).unwrap();
                let _ = tx.send(line.trim_end().to_owned());
            }
        });
        rx
    }

    fn write_status(dir: &Path, state: &str, live: bool) {
        let json = format!(r#"{{"version":1,"state":"{state}","live":{live},"updated_unix":{}}}"#, unix_now());
        std::fs::write(dir.join("status.json"), json).unwrap();
    }

    #[test]
    fn it_tells_a_stand_in_eyecam_rec_and_follows_its_restarts() {
        let dir = TempDir::new("live-ctl");
        let received = fake_eyecam(&dir.0, "ok");
        let mut control = LiveControl::new(dir.0.clone());
        let start = Instant::now();
        // Idle with live on, camera_lids off: "live off"
        write_status(&dir.0, "idle", true);
        let line = control.step(false, start).unwrap();
        assert!(line.starts_with("Told eyecam-rec \"live off\""), "{line}");
        assert_eq!(received.recv_timeout(Duration::from_secs(2)).unwrap(), "live off");
        // Not shown yet: not again right away
        assert_eq!(control.step(false, start + Duration::from_secs(1)), None);
        assert!(received.try_recv().is_err());
        // Shown: nothing
        write_status(&dir.0, "idle", false);
        assert_eq!(control.step(false, start + Duration::from_secs(2)), None);
        // eyecam-rec restarted (live on again): sent again
        write_status(&dir.0, "idle", true);
        control.step(false, start + Duration::from_secs(10));
        assert_eq!(received.recv_timeout(Duration::from_secs(2)).unwrap(), "live off");
        // Turned on again: "live on" at once
        write_status(&dir.0, "idle", false);
        assert!(control.step(true, start + Duration::from_secs(10)).unwrap().contains("\"live on\""));
        assert_eq!(received.recv_timeout(Duration::from_secs(2)).unwrap(), "live on");
        // Calibrating: waits
        write_status(&dir.0, "calibrating", true);
        assert_eq!(control.step(false, start + Duration::from_secs(20)), None);
        assert!(received.try_recv().is_err());
    }

    #[test]
    fn a_refusal_or_no_socket_is_said_once_and_tried_again() {
        let dir = TempDir::new("live-ctl-err");
        let start = Instant::now();
        let mut control = LiveControl::new(dir.0.clone());
        write_status(&dir.0, "idle", true);
        // No socket
        let line = control.step(false, start).unwrap();
        assert!(line.starts_with("Couldn't tell eyecam-rec \"live off\""), "{line}");
        assert_eq!(control.step(false, start + RESEND), None);
        // Refused: said, then tried again quietly
        let received = fake_eyecam(&dir.0, "err 録画中");
        let line = control.step(false, start + RESEND * 2).unwrap();
        assert!(line.contains("answered \"err 録画中\""), "{line}");
        assert_eq!(received.recv_timeout(Duration::from_secs(2)).unwrap(), "live off");
        assert_eq!(control.step(false, start + RESEND * 3), None);
        assert_eq!(received.recv_timeout(Duration::from_secs(2)).unwrap(), "live off");
        // A status file that isn't one: nothing
        std::fs::write(dir.0.join("status.json"), "not json").unwrap();
        assert_eq!(control.step(false, start + RESEND * 4), None);
    }
}
