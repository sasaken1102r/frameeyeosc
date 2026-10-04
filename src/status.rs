//! status.json, which the panel reads to show what is being sent and where.

use crate::capture::CaptureResult;
use crate::config::{OutputKind, Settings};
use serde::Serialize;
use std::fs::{self, DirBuilder};
use std::io;
use std::os::unix::fs::DirBuilderExt;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

// Ten times a second, whether or not eye tracking is running.
pub const STATUS_INTERVAL: Duration = Duration::from_millis(100);

#[derive(Serialize)]
pub struct Status<'a> {
    pub version: u32,
    pub pid: u32,
    pub time: f64,
    pub started: f64,
    pub sending: bool,
    pub output: OutputKind,
    pub target_mode: &'static str,
    pub target: Option<String>,
    /// Where the eye camera's pupils go straight to VRChat in LiveLink mode (see pupils_to_vrchat); None otherwise.
    pub pupil_target: Option<String>,
    pub rate: f32,
    /// Samples from the eye tracker in the last second (sent or not): about 90-136 while streaming,
    /// and it has been seen at 15. None until tracking has run for a second (the count is still filling).
    pub tracker_rate: Option<f32>,
    /// Samples the eye server published in the last second that frameeyeosc did not read, because it was still busy
    /// with the one before. None like tracker_rate.
    pub missed_rate: Option<f32>,
    /// The longest frameeyeosc took over one sample in the last second, in ms: from reading it until ready to read the
    /// next (processing, sending, this file). Longer than a frame (11.1 ms at 90 a second), samples can be missed.
    /// None like tracker_rate.
    pub max_processing_ms: Option<f32>,
    /// Datagrams dropped in the last second because the network could not take them at once (sending never waits).
    pub dropped_rate: f32,
    pub tracking: bool,
    pub raw: Option<RawValues>,
    pub sent: Option<SentValues>,
    pub calibration: CalibrationStatus,
    pub config_path: Option<&'a Path>,
    pub calibration_path: Option<&'a Path>,
    pub config_error: Option<&'a str>,
    /// Why the eye tracker's shared memory can't be read (frameeyeosc keeps trying again); None while it can.
    pub source_error: Option<&'a str>,
    /// The eye ("left" / "right") the Frame tracks alone while "Track Dominant Eye Only" is on; None while it
    /// is off, or can't be told (no eye data, or shared-memory version 4).
    pub dominant_eye: Option<&'static str>,
    /// Whether a relaxed open eye reads 1.000 (SteamOS 0.4.3), so widening can't come through.
    pub openness_saturated: bool,
    /// eyecam-rec's eye-camera values; None while its file can't be opened at all (eyecam-rec not running).
    pub camera: Option<CameraStatus<'a>>,
    pub locked: &'a [&'static str],
    pub effective: &'a Settings,
    /// The latest gaze capture the panel asked for.
    pub gaze_capture: Option<&'a CaptureResult>,
}

/// Readings from the Frame: gaze in -1..1 before smoothing, openness before and after the per-eye scale.
#[derive(Serialize)]
pub struct RawValues {
    pub openness: [f32; 2],
    pub openness_scaled: [f32; 2],
    pub gaze: [f32; 2],
    pub gaze_left: [f32; 2],
    pub gaze_right: [f32; 2],
}

/// What goes out (or would, while paused). `lids` is on the output's own scale.
#[derive(Serialize)]
pub struct SentValues {
    pub lids: [f32; 2],
    pub lids_vrcft: [f32; 2],
    pub gaze: [f32; 2],
    pub gaze_left: [f32; 2],
    pub gaze_right: [f32; 2],
    /// Each eye's squint (0 for an eye whose squint is not sent); None while no squint goes out (VRChat only).
    pub squint: Option<[f32; 2]>,
    /// The pupil dilation; None while it does not go out (to VRChat, directly or next to LiveLink).
    pub pupil_dilation: Option<f32>,
}

/// What eyecam-rec's live file says, and what of it is used.
#[derive(Serialize)]
pub struct CameraStatus<'a> {
    /// The file reads right, eyecam-rec is processing, and at least one eye's values are fresh.
    pub present: bool,
    /// Bit 0: calibrated for this wear (`calib wear`), bit 1: for this user (`calib user`).
    pub calib_state: u32,
    pub recalib_suggested: bool,
    /// Whether the camera drives each eye's eyelid (from relaxed open up) and squint now.
    pub used: [bool; 2],
    /// Whether each eye's pupil comes from the camera now.
    pub pupil_used: [bool; 2],
    /// Why the file can't be used (a header this does not know, ...); None while it can.
    pub error: Option<&'a str>,
}

#[derive(Serialize)]
pub struct CalibrationStatus {
    pub enabled: bool,
    pub relaxed: [f32; 2],
    pub scales: [f32; 2],
    /// Whether each eye uses the panel's eye fit instead (no learning, no scale).
    pub fitted: [bool; 2],
    pub learning: bool,
}

/// Four decimals are plenty for display and keep the file short.
pub fn round(values: [f32; 2]) -> [f32; 2] {
    values.map(round_one)
}

pub fn round_one(value: f32) -> f32 {
    (value * 10_000.0).round() / 10_000.0
}

pub fn unix_time(time: SystemTime) -> f64 {
    let seconds = time.duration_since(UNIX_EPOCH).map_or(0.0, |since| since.as_secs_f64());
    (seconds * 1000.0).round() / 1000.0
}

/// `$XDG_RUNTIME_DIR/frameeyeosc/status.json`, or under `/run/user/<uid>` without it.
pub fn status_path() -> PathBuf {
    std::env::var_os("XDG_RUNTIME_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(format!("/run/user/{}", unsafe { libc::getuid() })))
        .join("frameeyeosc")
        .join("status.json")
}

/// Replaces the status file atomically so the panel never reads a half-written one.
pub struct StatusFile {
    path: PathBuf,
    last_write: Option<Instant>,
    failing: bool,
}

impl StatusFile {
    pub fn new(path: PathBuf) -> Self {
        Self {
            path,
            last_write: None,
            failing: false,
        }
    }

    pub fn due(&self) -> bool {
        self.last_write
            .is_none_or(|written| written.elapsed() >= STATUS_INTERVAL)
    }

    pub fn write(&mut self, status: &Status) {
        self.last_write = Some(Instant::now());
        match self.replace(status) {
            Ok(()) => self.failing = false,
            // Report once, not ten times a second.
            Err(error) if !self.failing => {
                eprintln!("Could not write {}: {error}", self.path.display());
                self.failing = true;
            }
            Err(_) => {}
        }
    }

    fn replace(&self, status: &Status) -> io::Result<()> {
        let json = serde_json::to_vec(status)?;
        let temporary = self.path.with_extension("json.tmp");
        if let Err(error) = fs::write(&temporary, &json) {
            let Some(dir) = self.path.parent().filter(|_| error.kind() == io::ErrorKind::NotFound)
            else {
                return Err(error);
            };
            DirBuilder::new().recursive(true).mode(0o700).create(dir)?;
            fs::write(&temporary, &json)?;
        }
        fs::rename(temporary, &self.path)
    }
}
