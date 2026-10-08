//! Settings: built-in defaults, the JSON file the panel writes, and command-line options on top.

use crate::Args;
use clap::ArgMatches;
use clap::parser::ValueSource;
use serde::{Deserialize, Serialize};
use std::collections::HashSet;
use std::fs;
use std::io;
use std::net::IpAddr;
use std::os::unix::fs::MetadataExt;
use std::path::PathBuf;
use std::time::{Duration, Instant};

// How often the config file's modification time is checked (one stat; the file is only read when it changed).
// Often, so a gaze capture the panel asks for starts within a tenth of a second.
const CHECK_INTERVAL: Duration = Duration::from_millis(100);
// Allowed gaze zero points and gains (the panel's steppers stay inside these too).
const GAZE_OFFSET_RANGE: std::ops::RangeInclusive<f32> = -0.5..=0.5;
const GAZE_GAIN_RANGE: std::ops::RangeInclusive<f32> = 0.5..=2.0;
// Allowed headset tilt (degrees).
const GAZE_ROLL_RANGE: std::ops::RangeInclusive<f32> = -20.0..=20.0;
// camera_lid_floor, in VRCFT units: up to relaxed open.
const CAMERA_LID_FLOOR_RANGE: std::ops::RangeInclusive<f32> = 0.0..=0.75;
// lid_open_snap, in VRCFT units: relaxed open (0.75) is off.
const LID_OPEN_SNAP_RANGE: std::ops::RangeInclusive<f32> = 0.0..=0.75;
// lid_open_snap used to be a share of the eye's open reading (0.70 to 1.00, 0.80 by default). A value above 0.75 in
// config.json is one of those, and goes over as where it started for a fit that reads this shut and 1.000 open (the
// author's eyes read 0.003 and 0.046): 0.80 -> 0.53, the new default; 1.00 -> 0.75, off as before. 0.70 and 0.75 read
// as the new kind; anything above 1.00 was never allowed and still isn't.
const OLD_SNAP_CLOSED: f32 = 0.025;
// A fitted eye's open readings must be at least this far above its closed one.
const LID_FIT_MIN_RANGE: f32 = 0.1;
// A gaze capture's target name is only echoed back, so it is kept short.
const MAX_TARGET_CHARS: usize = 16;
// The most bits pupil_bits sends the pupil dilation as (PupilDilation1 .. PupilDilation8).
pub const MAX_PUPIL_BITS: u8 = 4;
// A gaze capture lasts this long (sample time) unless the request says otherwise...
pub const CAPTURE_SECONDS: f64 = 2.0;
// ...within these limits...
const CAPTURE_SECONDS_RANGE: std::ops::RangeInclusive<f64> = 0.5..=5.0;
// ...and skips its first half second, while the eyes settle, unless the request says otherwise (at most all but
// the last 0.2 s).
pub const CAPTURE_SKIP: f64 = 0.5;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize, clap::ValueEnum)]
#[serde(rename_all = "lowercase")]
pub enum OutputKind {
    /// VRChat avatar parameters (eyelid 0.75 = relaxed, 1.0 = widened)
    Vrchat,
    /// VRCFaceTracking's ETVR Tracking Module (six values, eyelid 1.0 = relaxed)
    Etvr,
    /// VRCFaceTracking's LiveLink module (Live Link Face packets: eyelids, widening and gaze per eye)
    #[value(name = "livelink")]
    LiveLink,
}

impl OutputKind {
    pub fn default_port(self) -> u16 {
        match self {
            Self::Vrchat => 9000,
            Self::Etvr => 8889,
            Self::LiveLink => crate::livelink::DEFAULT_PORT,
        }
    }
}

/// How `EyeTrackingActive` goes out in VRChat mode (VRCFaceTracking's templates use a bool; some
/// avatars declare it as a float and stop on a bool).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize, clap::ValueEnum)]
#[serde(rename_all = "lowercase")]
pub enum ActiveType {
    /// true / false
    Bool,
    /// 1.0 / 0.0
    Float,
    /// never sent
    Off,
}

/// How easily a fitted eye widens (see main.rs, WIDEN_*): never, or from a small, medium or large rise above its
/// expected open reading. Eyes without an eye fit use the lid marks instead.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize, clap::ValueEnum)]
#[serde(rename_all = "lowercase")]
pub enum Widen {
    /// never widens
    Off,
    /// only a large rise widens
    Low,
    /// the default
    Normal,
    /// a small rise widens
    High,
}

/// Everything that can change while running. Field names are the config.json keys, and the
/// defaults match the command-line defaults in `Args`.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct Settings {
    pub sending: bool,
    pub output: OutputKind,
    /// "auto" for the Steam Link PC, else an IP address or host name without a port.
    pub host: String,
    /// None uses the output's default port.
    pub port: Option<u16>,
    /// Without a trailing slash; empty for no prefix.
    pub prefix: String,
    pub eye_tracking_active: ActiveType,
    /// With the VRChat output, also send the avatar parameters Steam Link's own OSC sends (LeftEyeX, RightEyeLid,
    /// ...), without the prefix. Ignored by the other outputs.
    pub steamlink_params: bool,
    /// In VRChat mode, also send VRChat's own eye tracking input (/tracking/eye/*), which moves the eyes of
    /// avatars without VRCFT parameters.
    pub native_eyes: bool,
    /// Use eyecam-rec's eye-camera values (see eyecam_live) while they are fresh: each eye's eyelid from relaxed open
    /// up (widening), where it sees the eye closed or open through a squint (see main.rs, Smoother::check_lids), and
    /// its squint, once the camera is calibrated for this wear; and its pupil. Blinks stay the eye server's.
    pub camera_lids: bool,
    /// In LiveLink mode, also send the eye camera's pupils (only those) straight to VRChat over OSC, on port 9000 of
    /// the LiveLink target's host: VRCFT's LiveLink module has no pupils. Ignored by the other outputs.
    pub pupils_to_vrchat: bool,
    /// How the pupil dilation goes to VRChat besides the float: 0 = only the float, 1..=4 = also as that many bool
    /// parameters (PupilDilation1, 2, 4, 8), for avatars that take it bit-packed the way VRCFT does. One value for
    /// every avatar: the headset can't see which parameters the avatar has.
    pub pupil_bits: u8,
    /// How easily a fitted eye widens.
    pub lid_widen: Widen,
    /// How the eyes move: 2 (the default) as now; 1 as up to 0.7.5, the gaze and eyelid processing of that release
    /// (see main.rs, process). Fixes since (the eye cameras' left and right, the eye fit) apply either way.
    pub eye_behavior: u8,
    /// The settings file has no `lid_widen` (written by 0.5.x or earlier, whose lid_scale_* did nothing for fitted
    /// eyes): a fitted eye's scale is then ignored (see main.rs, lid_scales). Not a setting of its own.
    #[serde(skip)]
    pub scales_predate_fit: bool,
    pub raw: bool,
    pub gaze_min_cutoff: f32,
    pub gaze_beta: f32,
    pub gaze_d_cutoff: f32,
    pub gaze_deadzone: f32,
    pub gaze_hold_below: f32,
    pub independent_eyes: bool,
    pub lid_min_cutoff: f32,
    pub lid_beta: f32,
    pub lid_closed: f32,
    pub lid_open: f32,
    pub lid_widen_start: f32,
    pub lid_wide: f32,
    pub lid_scale_left: Option<f32>,
    pub lid_scale_right: Option<f32>,
    pub lid_calibration: bool,
    pub lid_sync: f32,
    pub gaze_quality_limit: f32,
    pub blink_hold_ms: f32,
    pub despike: bool,
    pub blink_sync_below: f32,
    /// While the eye camera's values for an eye are used and it sees the eye open, the sent eyelid (VRCFT) goes no
    /// lower than this, so a hard squint shows narrowed and the squint parameter carries the rest; closing (a blink
    /// within the grace period, or the camera seeing the eye closed) still reaches 0. 0 = off.
    pub camera_lid_floor: f32,
    /// For eyes without the eye camera's eyelid: an eyelid (VRCFT, as the fit or the lid marks map it) at least this
    /// open eases smoothly up to relaxed open, so an open eye the eye tracker reads a little low goes out open (see
    /// main.rs, snapped_open). 0.75 = off.
    pub lid_open_snap: f32,
    /// Gaze zero point and how far the gaze goes, on the -1..1 scale (see `correct_gaze`).
    pub gaze_offset_x: f32,
    pub gaze_offset_y: f32,
    pub gaze_gain_x: f32,
    pub gaze_gain_up: f32,
    pub gaze_gain_down: f32,
    /// How far the headset sits tilted, in degrees (positive: looking right reads higher). Undone around the zero
    /// point before the gains; see `correct_gaze`.
    pub gaze_roll_deg: f32,
    /// Degrees below straight ahead (the tracker's own, before the zero point and gains) from where the
    /// sideways gaze is held; 0 disables. See `Smoother::hold_down_x`.
    pub gaze_down_hold_x_deg: f32,
    /// Each eye's own sideways zero point and gain, used only for an eye's own gaze when it stands in for the other
    /// (that one's gaze unreliable). None uses gaze_offset_x / gaze_gain_x. The Frame shares the up/down gaze
    /// between the eyes, so there is no per-eye y. The panel's fits write the same zero point for both eyes.
    pub gaze_offset_x_left: Option<f32>,
    pub gaze_offset_x_right: Option<f32>,
    pub gaze_gain_x_left: Option<f32>,
    pub gaze_gain_x_right: Option<f32>,
    /// Stream each sample's sent gaze to the panel, which shows it as dots (debug). Not a command-line option.
    pub gaze_debug_dots: bool,
    /// Each eye's Frame openness measured by the panel's eye fit: eyes shut, and open while looking
    /// up, straight ahead and down. None until fitted; see `lid_fit`. Not command-line options.
    pub lid_fit_closed_left: Option<f32>,
    pub lid_fit_closed_right: Option<f32>,
    pub lid_fit_up_left: Option<f32>,
    pub lid_fit_up_right: Option<f32>,
    pub lid_fit_open_left: Option<f32>,
    pub lid_fit_open_right: Option<f32>,
    pub lid_fit_down_left: Option<f32>,
    pub lid_fit_down_right: Option<f32>,
}

/// One eye's fitted openness readings (Frame openness, before any scale).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct LidFit {
    pub closed: f32,
    pub up: f32,
    pub open: f32,
    pub down: f32,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            sending: true,
            output: OutputKind::Vrchat,
            host: "auto".into(),
            port: None,
            prefix: "/FT".into(),
            eye_tracking_active: ActiveType::Bool,
            steamlink_params: false,
            native_eyes: false,
            camera_lids: true,
            pupils_to_vrchat: true,
            pupil_bits: 0,
            eye_behavior: 2,
            lid_widen: Widen::Normal,
            scales_predate_fit: false,
            raw: false,
            gaze_min_cutoff: 0.3,
            gaze_beta: 1.5,
            gaze_d_cutoff: 0.5,
            gaze_deadzone: 0.005,
            gaze_hold_below: 0.5,
            independent_eyes: false,
            lid_min_cutoff: 6.0,
            lid_beta: 5.0,
            lid_closed: 0.30,
            lid_open: 0.80,
            lid_widen_start: 0.92,
            lid_wide: 1.00,
            lid_scale_left: None,
            lid_scale_right: None,
            lid_calibration: true,
            lid_sync: 0.4,
            gaze_quality_limit: 0.0,
            blink_hold_ms: 80.0,
            despike: true,
            blink_sync_below: 0.35,
            camera_lid_floor: 0.0,
            lid_open_snap: 0.53,
            gaze_offset_x: 0.0,
            gaze_offset_y: 0.0,
            gaze_gain_x: 1.0,
            gaze_gain_up: 1.0,
            gaze_gain_down: 1.0,
            gaze_roll_deg: 0.0,
            gaze_down_hold_x_deg: 24.0,
            gaze_offset_x_left: None,
            gaze_offset_x_right: None,
            gaze_gain_x_left: None,
            gaze_gain_x_right: None,
            gaze_debug_dots: false,
            lid_fit_closed_left: None,
            lid_fit_closed_right: None,
            lid_fit_up_left: None,
            lid_fit_up_right: None,
            lid_fit_open_left: None,
            lid_fit_open_right: None,
            lid_fit_down_left: None,
            lid_fit_down_right: None,
        }
    }
}

impl Settings {
    pub fn port(&self) -> u16 {
        self.port.unwrap_or(self.output.default_port())
    }

    /// Whether the eyes move as now (eye_behavior 2), not as up to 0.7.5 (1).
    pub fn v2(&self) -> bool {
        self.eye_behavior >= 2
    }

    /// Each eye's fit, if all four of its readings are set.
    pub fn lid_fit(&self) -> [Option<LidFit>; 2] {
        let fit = |closed: Option<f32>, up: Option<f32>, open: Option<f32>, down: Option<f32>| {
            Some(LidFit {
                closed: closed?,
                up: up?,
                open: open?,
                down: down?,
            })
        };
        [
            fit(
                self.lid_fit_closed_left,
                self.lid_fit_up_left,
                self.lid_fit_open_left,
                self.lid_fit_down_left,
            ),
            fit(
                self.lid_fit_closed_right,
                self.lid_fit_up_right,
                self.lid_fit_open_right,
                self.lid_fit_down_right,
            ),
        ]
    }

    /// The eight lid fit readings: closed, up, open and down for the left eye, then the right.
    fn lid_fit_readings(&self) -> [[Option<f32>; 4]; 2] {
        [
            [self.lid_fit_closed_left, self.lid_fit_up_left, self.lid_fit_open_left, self.lid_fit_down_left],
            [
                self.lid_fit_closed_right,
                self.lid_fit_up_right,
                self.lid_fit_open_right,
                self.lid_fit_down_right,
            ],
        ]
    }

    pub fn validate(&self) -> Result<(), String> {
        let numbers = [
            self.gaze_min_cutoff,
            self.gaze_beta,
            self.gaze_d_cutoff,
            self.gaze_deadzone,
            self.gaze_hold_below,
            self.lid_min_cutoff,
            self.lid_beta,
            self.lid_closed,
            self.lid_open,
            self.lid_widen_start,
            self.lid_wide,
            self.lid_sync,
            self.gaze_quality_limit,
            self.blink_hold_ms,
            self.blink_sync_below,
            self.camera_lid_floor,
            self.lid_open_snap,
            self.gaze_offset_x,
            self.gaze_offset_y,
            self.gaze_gain_x,
            self.gaze_gain_up,
            self.gaze_gain_down,
            self.gaze_roll_deg,
            self.gaze_down_hold_x_deg,
        ];
        let scales = [self.lid_scale_left, self.lid_scale_right];
        let eye_offsets = [self.gaze_offset_x_left, self.gaze_offset_x_right];
        let eye_gains = [self.gaze_gain_x_left, self.gaze_gain_x_right];
        let lid_fit = self.lid_fit_readings();
        let fitted = lid_fit.iter().flatten().flatten();
        if !numbers
            .iter()
            .chain(scales.iter().flatten())
            .chain(eye_offsets.iter().chain(&eye_gains).flatten())
            .chain(fitted.clone())
            .all(|value| value.is_finite())
        {
            return Err("settings must be finite numbers".into());
        }
        let host_ok = self.host.parse::<IpAddr>().is_ok()
            || !(self.host.is_empty() || self.host.contains(|c: char| c == ':' || c.is_whitespace()));
        if !host_ok {
            return Err("host must be \"auto\", an IP address or a host name without a port".into());
        }
        if self.port == Some(0) {
            return Err("port must be between 1 and 65535".into());
        }
        if !(self.prefix.is_empty() || self.prefix.starts_with('/')) {
            return Err("prefix must be empty or an OSC path starting with /".into());
        }
        // Every number is finite by now, so plain comparisons are safe.
        if self.lid_closed >= self.lid_open {
            return Err("lid_closed must be below lid_open".into());
        }
        if self.lid_open > self.lid_widen_start {
            return Err("lid_widen_start must not be below lid_open".into());
        }
        let cutoffs_ok = [self.gaze_min_cutoff, self.gaze_d_cutoff, self.lid_min_cutoff]
            .iter()
            .all(|cutoff| *cutoff > 0.0);
        let non_negative_ok = [self.gaze_beta, self.lid_beta, self.gaze_deadzone]
            .iter()
            .all(|value| *value >= 0.0);
        if !cutoffs_ok || !non_negative_ok {
            return Err("filter cutoffs must be positive; betas and the deadzone non-negative".into());
        }
        if !scales.iter().flatten().all(|scale| *scale > 0.0) {
            return Err("lid_scale_left/right must be positive".into());
        }
        if !(1..=2).contains(&self.eye_behavior) {
            return Err("eye_behavior must be 1 (as up to 0.7.5) or 2".into());
        }
        if self.pupil_bits > MAX_PUPIL_BITS {
            return Err(format!("pupil_bits must be 0 (a float only) to {MAX_PUPIL_BITS}"));
        }
        if self.lid_sync < 0.0 {
            return Err("lid_sync must be non-negative".into());
        }
        if self.gaze_quality_limit < 0.0 || self.blink_hold_ms < 0.0 || self.blink_sync_below < 0.0 {
            return Err("gaze_quality_limit, blink_hold_ms and blink_sync_below must be non-negative".into());
        }
        if !CAMERA_LID_FLOOR_RANGE.contains(&self.camera_lid_floor) {
            return Err("camera_lid_floor must be between 0 and 0.75".into());
        }
        if !LID_OPEN_SNAP_RANGE.contains(&self.lid_open_snap) {
            return Err("lid_open_snap must be between 0 and 0.75 (0.75 = off)".into());
        }
        if !(GAZE_OFFSET_RANGE.contains(&self.gaze_offset_x) && GAZE_OFFSET_RANGE.contains(&self.gaze_offset_y)) {
            return Err("gaze_offset_x/y must be between -0.5 and 0.5".into());
        }
        if !eye_offsets.iter().flatten().all(|offset| GAZE_OFFSET_RANGE.contains(offset)) {
            return Err("gaze_offset_x_left/right must be between -0.5 and 0.5".into());
        }
        if !eye_gains.iter().flatten().all(|gain| GAZE_GAIN_RANGE.contains(gain)) {
            return Err("gaze_gain_x_left/right must be between 0.5 and 2".into());
        }
        let gains = [self.gaze_gain_x, self.gaze_gain_up, self.gaze_gain_down];
        if !gains.iter().all(|gain| GAZE_GAIN_RANGE.contains(gain)) {
            return Err("gaze_gain_x/up/down must be between 0.5 and 2".into());
        }
        if !GAZE_ROLL_RANGE.contains(&self.gaze_roll_deg) {
            return Err("gaze_roll_deg must be between -20 and 20".into());
        }
        if !(0.0..=45.0).contains(&self.gaze_down_hold_x_deg) {
            return Err("gaze_down_hold_x_deg must be between 0 and 45".into());
        }
        if !lid_fit.iter().all(|eye| eye.iter().all(Option::is_some) || eye.iter().all(Option::is_none)) {
            return Err("an eye's lid_fit_* values must all be set, or all null".into());
        }
        if !fitted.clone().all(|value| (0.0..=1.5).contains(value)) {
            return Err("lid_fit_* values must be between 0 and 1.5".into());
        }
        let apart = |fit: &LidFit| [fit.up, fit.open, fit.down].iter().all(|open| *open >= fit.closed + LID_FIT_MIN_RANGE);
        if !self.lid_fit().iter().flatten().all(apart) {
            return Err("lid_fit_closed must be at least 0.1 below the open lid_fit_* values".into());
        }
        Ok(())
    }
}

/// A request from the panel to average the gaze for a moment while the user looks at a target.
#[derive(Clone, Debug, PartialEq)]
pub struct GazeCapture {
    /// A new id is a new request.
    pub id: i64,
    /// Which target the user looks at ("center", "up", ...); only passed back with the result.
    pub target: String,
    /// How long it lasts (sample time) and how much of its start is skipped, in seconds.
    pub seconds: f64,
    pub skip: f64,
}

/// Keys the panel uses to send one-off requests rather than settings.
#[derive(Default, Deserialize)]
#[serde(default)]
struct Requests {
    /// Bumped to make the learned eyelid calibration start over.
    calibration_reset: i64,
    /// `{"id": 3, "target": "center", "seconds": 2.0, "skip": 0.3}` (the last two optional). Read loosely, so a bad
    /// value never rejects the settings.
    gaze_capture: serde_json::Value,
}

impl Requests {
    /// The gaze capture request's id (0 without one) and the request itself.
    fn gaze_capture(&self) -> (i64, Option<GazeCapture>) {
        let id = self.gaze_capture.get("id").and_then(serde_json::Value::as_i64);
        let target = self.gaze_capture.get("target").and_then(serde_json::Value::as_str);
        let number = |key: &str| self.gaze_capture.get(key).and_then(serde_json::Value::as_f64);
        let seconds = number("seconds")
            .unwrap_or(CAPTURE_SECONDS)
            .clamp(*CAPTURE_SECONDS_RANGE.start(), *CAPTURE_SECONDS_RANGE.end());
        let skip = number("skip").unwrap_or(CAPTURE_SKIP).clamp(0.0, seconds - 0.2);
        match (id, target) {
            (Some(id), Some(target)) => (
                id,
                Some(GazeCapture {
                    id,
                    target: target.chars().take(MAX_TARGET_CHARS).collect(),
                    seconds,
                    skip,
                }),
            ),
            _ => (0, None),
        }
    }
}

/// What config.json asks for besides settings.
#[derive(Clone, Debug, PartialEq)]
struct Asked {
    calibration_reset: i64,
    capture_id: i64,
    capture: Option<GazeCapture>,
}

/// Parse config.json: missing keys keep their defaults and unknown keys are ignored.
fn parse(text: &str) -> Result<(Settings, Asked), String> {
    let mut settings: Settings = serde_json::from_str(text).map_err(|error| error.to_string())?;
    let written: serde_json::Value = serde_json::from_str(text).map_err(|error| error.to_string())?;
    let scaled = ["lid_scale_left", "lid_scale_right"]
        .iter()
        .any(|name| written.get(name).is_some_and(|value| !value.is_null()));
    settings.scales_predate_fit = scaled && written.get("lid_widen").is_none();
    if let Some(share) = written.get("lid_open_snap").and_then(serde_json::Value::as_f64) {
        if share > f64::from(*LID_OPEN_SNAP_RANGE.end()) && share <= 1.0 {
            settings.lid_open_snap = snap_from_share(share as f32);
        }
    }
    let requests: Requests = serde_json::from_str(text).map_err(|error| error.to_string())?;
    let (capture_id, capture) = requests.gaze_capture();
    let asked = Asked {
        calibration_reset: requests.calibration_reset,
        capture_id,
        capture,
    };
    Ok((settings, asked))
}

/// Where a lid_open_snap from when it was a share of the eye's open reading started, as an eyelid (VRCFT, to 0.01):
/// for a fit that reads OLD_SNAP_CLOSED shut and 1.000 open, through the fit's closed margin.
pub fn snap_from_share(share: f32) -> f32 {
    let fraction = (share - OLD_SNAP_CLOSED) / (1.0 - OLD_SNAP_CLOSED);
    let lid = crate::LID_RELAXED * (fraction - crate::LID_FIT_CLOSED_MARGIN) / (1.0 - crate::LID_FIT_CLOSED_MARGIN);
    ((lid * 100.0).round() / 100.0).clamp(*LID_OPEN_SNAP_RANGE.start(), *LID_OPEN_SNAP_RANGE.end())
}

/// Ids of the options that were typed on the command line rather than left at their defaults.
pub fn given_options(matches: &ArgMatches) -> HashSet<String> {
    matches
        .ids()
        .filter(|id| matches.value_source(id.as_str()) == Some(ValueSource::CommandLine))
        .map(|id| id.to_string())
        .collect()
}

/// Split `--target HOST:PORT` (HOST may be a bracketed IPv6 address).
pub fn split_target(target: &str) -> Option<(String, u16)> {
    let (host, port) = target.rsplit_once(':')?;
    let host = host
        .strip_prefix('[')
        .and_then(|host| host.strip_suffix(']'))
        .unwrap_or(host);
    let port = port.parse().ok()?;
    (!host.is_empty()).then(|| (host.to_owned(), port))
}

/// Overwrite `settings` with the options given on the command line; returns the keys they pin.
pub fn apply_args(settings: &mut Settings, args: &Args, given: &HashSet<String>) -> Vec<&'static str> {
    let mut locked = Vec::new();
    macro_rules! pin {
        ($($field:ident),*) => {$(
            if given.contains(stringify!($field)) {
                settings.$field = args.$field;
                locked.push(stringify!($field));
            }
        )*};
    }
    pin!(output, eye_tracking_active, steamlink_params, native_eyes, lid_widen, pupil_bits, eye_behavior);
    // Given on the command line: this is 0.6.0 or later, whatever the file says
    if given.contains("lid_widen") {
        settings.scales_predate_fit = false;
    }
    if given.contains("target") {
        locked.push("host");
        // main() has already rejected anything that is neither "auto" nor HOST:PORT.
        match split_target(&args.target) {
            Some((host, port)) => {
                settings.host = host;
                settings.port = Some(port);
                locked.push("port");
            }
            _ => settings.host = "auto".into(),
        }
    }
    if given.contains("port") && !locked.contains(&"port") {
        settings.port = args.port;
        locked.push("port");
    }
    if given.contains("prefix") {
        settings.prefix.clone_from(&args.prefix);
        locked.push("prefix");
    }
    pin!(
        raw,
        gaze_min_cutoff,
        gaze_beta,
        gaze_d_cutoff,
        gaze_deadzone,
        gaze_hold_below,
        independent_eyes,
        lid_min_cutoff,
        lid_beta,
        lid_closed,
        lid_open,
        lid_widen_start,
        lid_wide,
        lid_scale_left,
        lid_scale_right
    );
    if given.contains("no_lid_calibration") {
        settings.lid_calibration = !args.no_lid_calibration;
        locked.push("lid_calibration");
    }
    pin!(lid_sync, gaze_quality_limit, blink_hold_ms);
    if given.contains("no_despike") {
        settings.despike = !args.no_despike;
        locked.push("despike");
    }
    if given.contains("no_camera_lids") {
        settings.camera_lids = !args.no_camera_lids;
        locked.push("camera_lids");
    }
    if given.contains("no_pupils_to_vrchat") {
        settings.pupils_to_vrchat = !args.no_pupils_to_vrchat;
        locked.push("pupils_to_vrchat");
    }
    pin!(
        blink_sync_below,
        camera_lid_floor,
        lid_open_snap,
        gaze_offset_x,
        gaze_offset_y,
        gaze_gain_x,
        gaze_gain_up,
        gaze_gain_down,
        gaze_roll_deg,
        gaze_down_hold_x_deg,
        gaze_offset_x_left,
        gaze_offset_x_right,
        gaze_gain_x_left,
        gaze_gain_x_right
    );
    // "/" alone means no prefix, like "".
    let prefix = settings.prefix.trim_end_matches('/');
    settings.prefix = prefix.to_owned();
    locked
}

pub struct Reload {
    pub settings: Settings,
    pub reset_calibration: bool,
    pub gaze_capture: Option<GazeCapture>,
}

/// Watches the config file and merges it with the command line whenever it changes.
pub struct Config {
    pub path: Option<PathBuf>,
    args: Args,
    given: HashSet<String>,
    pub locked: Vec<&'static str>,
    pub error: Option<String>,
    // Modification time, size and inode; None while the file does not exist.
    stamp: Option<(i64, i64, u64, u64)>,
    last_check: Instant,
    // calibration_reset and the gaze capture id from the last good read (0 without a file);
    // None until there has been one.
    reset: Option<i64>,
    capture_id: Option<i64>,
}

impl Config {
    pub fn new(path: Option<PathBuf>, args: Args, given: HashSet<String>) -> Self {
        let locked = apply_args(&mut Settings::default(), &args, &given);
        Self {
            path,
            args,
            given,
            locked,
            error: None,
            stamp: None,
            last_check: Instant::now(),
            reset: None,
            capture_id: None,
        }
    }

    /// The settings to start with. A broken file falls back to the defaults (plus options) and is
    /// reported in `error`; only invalid command-line options are fatal.
    pub fn load(&mut self) -> Result<Settings, String> {
        let mut fallback = Settings::default();
        apply_args(&mut fallback, &self.args, &self.given);
        fallback.validate()?;
        self.stamp = self.stamp();
        match self.read() {
            Ok((settings, asked)) => {
                if asked.is_some() {
                    eprintln!("Loaded {}", self.display_path());
                }
                // Whatever the file asked for before this start is not repeated.
                self.reset = Some(asked.as_ref().map_or(0, |asked| asked.calibration_reset));
                self.capture_id = Some(asked.as_ref().map_or(0, |asked| asked.capture_id));
                Ok(settings)
            }
            Err(error) => {
                eprintln!("{}: {error}; using the defaults", self.display_path());
                self.error = Some(error);
                Ok(fallback)
            }
        }
    }

    /// Check the file once a second; returns new settings when it changed and is valid.
    pub fn poll(&mut self) -> Option<Reload> {
        if self.last_check.elapsed() < CHECK_INTERVAL {
            return None;
        }
        self.last_check = Instant::now();
        self.check()
    }

    fn check(&mut self) -> Option<Reload> {
        let stamp = self.stamp();
        if stamp == self.stamp {
            return None;
        }
        self.stamp = stamp;
        match self.read() {
            Ok((settings, asked)) => {
                match asked {
                    Some(_) => eprintln!("Loaded {}", self.display_path()),
                    None => eprintln!("{} is gone; using the defaults", self.display_path()),
                }
                self.error = None;
                // Removing the file resets the counters to 0 but is not itself a request.
                let reset = asked.as_ref().map(|asked| asked.calibration_reset);
                let reset_calibration = reset.is_some_and(|new| self.reset.is_some_and(|old| old != new));
                self.reset = Some(reset.unwrap_or(0));
                let capture_id = asked.as_ref().map(|asked| asked.capture_id);
                let new_capture = capture_id.is_some_and(|new| self.capture_id.is_some_and(|old| old != new));
                self.capture_id = Some(capture_id.unwrap_or(0));
                let gaze_capture = asked.and_then(|asked| asked.capture).filter(|_| new_capture);
                Some(Reload {
                    settings,
                    reset_calibration,
                    gaze_capture,
                })
            }
            Err(error) => {
                eprintln!("{}: {error}; keeping the previous settings", self.display_path());
                self.error = Some(error);
                None
            }
        }
    }

    /// The merged settings, and the requests if the file exists.
    fn read(&self) -> Result<(Settings, Option<Asked>), String> {
        let text = match self.path.as_deref().map(fs::read_to_string) {
            Some(Ok(text)) => Some(text),
            Some(Err(error)) if error.kind() != io::ErrorKind::NotFound => {
                return Err(error.to_string());
            }
            _ => None,
        };
        let (mut settings, asked) = match text {
            Some(text) => parse(&text).map(|(settings, asked)| (settings, Some(asked)))?,
            None => (Settings::default(), None),
        };
        apply_args(&mut settings, &self.args, &self.given);
        settings.validate()?;
        Ok((settings, asked))
    }

    fn stamp(&self) -> Option<(i64, i64, u64, u64)> {
        let metadata = fs::metadata(self.path.as_deref()?).ok()?;
        Some((metadata.mtime(), metadata.mtime_nsec(), metadata.size(), metadata.ino()))
    }

    fn display_path(&self) -> String {
        self.path
            .as_deref()
            .map_or_else(|| "config".into(), |path| path.display().to_string())
    }
}

/// `$XDG_CONFIG_HOME/frameeyeosc` or `~/.config/frameeyeosc`.
pub fn config_dir() -> Option<PathBuf> {
    let config = std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".config")))?;
    Some(config.join("frameeyeosc"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use clap::{CommandFactory, FromArgMatches};
    use std::path::Path;

    fn cli(argv: &[&str]) -> (Args, HashSet<String>) {
        let matches = Args::command()
            .try_get_matches_from(std::iter::once("frameeyeosc").chain(argv.iter().copied()))
            .unwrap();
        (Args::from_arg_matches(&matches).unwrap(), given_options(&matches))
    }

    fn merged(file: &str, argv: &[&str]) -> Result<(Settings, Vec<&'static str>), String> {
        let (args, given) = cli(argv);
        let (mut settings, _) = parse(file)?;
        let locked = apply_args(&mut settings, &args, &given);
        settings.validate()?;
        Ok((settings, locked))
    }

    #[test]
    fn command_line_defaults_match_the_config_defaults() {
        let (settings, locked) = merged("{}", &[]).unwrap();
        assert_eq!(settings, Settings::default());
        assert!(locked.is_empty());
        // The option's own default is the setting's
        let (args, _) = cli(&[]);
        assert_eq!(args.eye_tracking_active, Settings::default().eye_tracking_active);
    }

    #[test]
    fn scales_from_before_lid_widen_are_marked() {
        let old = r#"{"lid_scale_left": 1.15, "lid_fit_closed_left": 0.2, "lid_fit_up_left": 0.9,
            "lid_fit_open_left": 0.85, "lid_fit_down_left": 0.7}"#;
        assert!(merged(old, &[]).unwrap().0.scales_predate_fit);
        // Written by 0.6.0 (lid_widen in it), or no scale at all, or --lid-widen given: not
        let new = old.replacen('{', r#"{"lid_widen": "normal", "#, 1);
        assert!(!merged(&new, &[]).unwrap().0.scales_predate_fit);
        assert!(!merged(r#"{"lid_scale_left": null}"#, &[]).unwrap().0.scales_predate_fit);
        assert!(!merged(old, &["--lid-widen", "low"]).unwrap().0.scales_predate_fit);
    }

    #[test]
    fn lid_widen_is_one_of_four() {
        for (text, kind) in [("off", Widen::Off), ("low", Widen::Low), ("normal", Widen::Normal), ("high", Widen::High)] {
            let (settings, _) = merged(&format!(r#"{{"lid_widen": "{text}"}}"#), &[]).unwrap();
            assert_eq!(settings.lid_widen, kind);
        }
        assert_eq!(merged("{}", &[]).unwrap().0.lid_widen, Widen::Normal);
        assert!(merged(r#"{"lid_widen": "max"}"#, &[]).is_err());
        let (settings, locked) = merged(r#"{"lid_widen": "off"}"#, &["--lid-widen", "high"]).unwrap();
        assert_eq!((settings.lid_widen, locked), (Widen::High, vec!["lid_widen"]));
    }

    #[test]
    fn eye_tracking_active_is_bool_float_or_off() {
        for (text, kind) in [("bool", ActiveType::Bool), ("float", ActiveType::Float), ("off", ActiveType::Off)] {
            let (settings, _) = merged(&format!(r#"{{"eye_tracking_active": "{text}"}}"#), &[]).unwrap();
            assert_eq!(settings.eye_tracking_active, kind);
        }
        assert!(merged(r#"{"eye_tracking_active": "int"}"#, &[]).is_err());
        let (settings, locked) = merged(r#"{"eye_tracking_active": "off"}"#, &["--eye-tracking-active", "float"]).unwrap();
        assert_eq!((settings.eye_tracking_active, locked), (ActiveType::Float, vec!["eye_tracking_active"]));
    }

    #[test]
    fn steamlink_params_come_from_the_file_or_the_command_line() {
        assert!(!merged("{}", &[]).unwrap().0.steamlink_params);
        assert!(merged(r#"{"steamlink_params": true}"#, &[]).unwrap().0.steamlink_params);
        assert!(merged(r#"{"steamlink_params": "yes"}"#, &[]).is_err());
        let (settings, locked) = merged(r#"{"steamlink_params": false}"#, &["--steamlink-params"]).unwrap();
        assert!(settings.steamlink_params);
        assert_eq!(locked, ["steamlink_params"]);
    }

    #[test]
    fn missing_keys_keep_defaults_and_unknown_keys_are_ignored() {
        let (settings, asked) =
            parse(r#"{"version": 1, "language": "en", "lid_open": 0.85, "port": 9001, "extra": [1]}"#)
                .unwrap();
        assert_eq!(settings.lid_open, 0.85);
        assert_eq!(settings.port, Some(9001));
        assert_eq!(settings.lid_closed, 0.30);
        assert_eq!(settings.host, "auto");
        assert_eq!((asked.calibration_reset, asked.capture_id, asked.capture), (0, 0, None));
        assert_eq!(parse(r#"{"calibration_reset": 3}"#).unwrap().1.calibration_reset, 3);
        // Integers are fine where a decimal is expected.
        assert_eq!(parse(r#"{"lid_min_cutoff": 10}"#).unwrap().0.lid_min_cutoff, 10.0);
    }

    #[test]
    fn bad_files_and_values_are_rejected() {
        assert!(merged("{\"lid_open\": 0.8,", &[]).is_err());
        assert!(merged(r#"{"output": "osc"}"#, &[]).is_err());
        assert!(merged(r#"{"port": 70000}"#, &[]).is_err());
        assert!(merged(r#"{"port": 0}"#, &[]).is_err());
        assert!(merged(r#"{"gaze_beta": "fast"}"#, &[]).is_err());
        assert!(merged(r#"{"lid_closed": 0.9}"#, &[]).is_err());
        assert!(merged(r#"{"lid_widen_start": 0.5}"#, &[]).is_err());
        assert!(merged(r#"{"gaze_min_cutoff": 0}"#, &[]).is_err());
        assert!(merged(r#"{"lid_scale_left": -1}"#, &[]).is_err());
        assert!(merged(r#"{"lid_sync": -0.1}"#, &[]).is_err());
        assert!(merged(r#"{"gaze_quality_limit": -0.01}"#, &[]).is_err());
        assert!(merged(r#"{"blink_hold_ms": -1}"#, &[]).is_err());
        assert!(merged(r#"{"blink_sync_below": -0.1}"#, &[]).is_err());
        assert!(merged(r#"{"camera_lid_floor": -0.05}"#, &[]).is_err());
        assert!(merged(r#"{"camera_lid_floor": 0.8}"#, &[]).is_err());
        assert!(merged(r#"{"camera_lid_floor": "low"}"#, &[]).is_err());
        assert!(merged(r#"{"despike": 1}"#, &[]).is_err());
        assert!(merged(r#"{"lid_wide": 1e300}"#, &[]).is_err());
        assert!(merged(r#"{"host": "192.168.0.60:9000"}"#, &[]).is_err());
        assert!(merged(r#"{"prefix": "FT"}"#, &[]).is_err());
        assert!(merged(r#"{"host": "fe80::1"}"#, &[]).is_ok());
        assert!(merged(r#"{"gaze_offset_y": -0.6}"#, &[]).is_err());
        assert!(merged(r#"{"gaze_gain_up": 2.5}"#, &[]).is_err());
        assert!(merged(r#"{"gaze_gain_x": 0.4}"#, &[]).is_err());
        assert!(merged(r#"{"gaze_offset_x": 0.5, "gaze_gain_down": 0.5}"#, &[]).is_ok());
        assert!(merged(r#"{"gaze_down_hold_x_deg": -1}"#, &[]).is_err());
        assert!(merged(r#"{"gaze_roll_deg": 20.5}"#, &[]).is_err());
        assert!(merged(r#"{"gaze_roll_deg": -20}"#, &[]).is_ok());
        assert!(merged(r#"{"gaze_down_hold_x_deg": 0}"#, &[]).is_ok());
        assert!(merged(r#"{"gaze_offset_x_left": 0.6}"#, &[]).is_err());
        assert!(merged(r#"{"gaze_gain_x_right": 3}"#, &[]).is_err());
        assert!(merged(r#"{"gaze_offset_x_left": -0.02, "gaze_gain_x_right": 1.1}"#, &[]).is_ok());
        let fitted = r#""lid_fit_closed_left": 0.15, "lid_fit_up_left": 0.93, "lid_fit_open_left": 0.92,
            "lid_fit_down_left": 0.78"#;
        assert!(merged(&format!("{{{fitted}}}"), &[]).is_ok());
        assert!(merged(&format!(r#"{{{fitted}, "lid_fit_open_right": 0.8}}"#), &[]).is_err());
        assert!(merged(r#"{"lid_fit_closed_left": 0.5, "lid_fit_up_left": 0.9, "lid_fit_open_left": 0.9,
            "lid_fit_down_left": 0.55}"#, &[]).is_err());
        assert!(merged(r#"{"lid_fit_closed_left": -0.1, "lid_fit_up_left": 0.9, "lid_fit_open_left": 0.9,
            "lid_fit_down_left": 0.8}"#, &[]).is_err());
    }

    #[test]
    fn livelink_is_an_output() {
        let (settings, _) = merged(r#"{"output": "livelink"}"#, &[]).unwrap();
        assert_eq!(settings.output, OutputKind::LiveLink);
        assert_eq!(settings.port(), 11111);
        let (settings, locked) = merged(r#"{"output": "etvr"}"#, &["--output", "livelink"]).unwrap();
        assert_eq!(settings.output, OutputKind::LiveLink);
        assert_eq!(locked, ["output"]);
        // The status file and the panel spell it the same way
        assert_eq!(serde_json::to_string(&OutputKind::LiveLink).unwrap(), r#""livelink""#);
        assert!(merged(r#"{"output": "live-link"}"#, &[]).is_err());
    }

    #[test]
    fn command_line_options_win_and_are_locked() {
        let file = r#"{"output": "etvr", "host": "10.0.0.2", "port": 9100, "raw": false,
            "lid_open": 0.85, "lid_calibration": true, "gaze_beta": 2.0}"#;
        let (settings, locked) = merged(
            file,
            &["--target", "192.168.0.60:9000", "--raw", "--no-lid-calibration", "--gaze-beta", "1.5"],
        )
        .unwrap();
        assert_eq!((settings.host.as_str(), settings.port), ("192.168.0.60", Some(9000)));
        assert!(settings.raw && !settings.lid_calibration);
        assert_eq!(settings.gaze_beta, 1.5);
        // Not given on the command line, so the file's values stay.
        assert_eq!(settings.output, OutputKind::Etvr);
        assert_eq!(settings.lid_open, 0.85);
        assert_eq!(locked, ["host", "port", "raw", "gaze_beta", "lid_calibration"]);

        let (settings, locked) =
            merged(file, &["--target", "auto", "--port", "9001", "--output", "vrchat"]).unwrap();
        assert_eq!((settings.host.as_str(), settings.port), ("auto", Some(9001)));
        assert_eq!(settings.output, OutputKind::Vrchat);
        assert_eq!(locked, ["output", "host", "port"]);

        let (settings, locked) = merged(file, &["--port", "9002"]).unwrap();
        assert_eq!((settings.host.as_str(), settings.port), ("10.0.0.2", Some(9002)));
        assert_eq!(locked, ["port"]);

        let file = r#"{"despike": true, "blink_hold_ms": 120, "gaze_quality_limit": 0.05}"#;
        let (settings, locked) = merged(file, &["--no-despike", "--blink-sync-below", "0"]).unwrap();
        assert!(!settings.despike);
        assert_eq!((settings.blink_hold_ms, settings.gaze_quality_limit), (120.0, 0.05));
        assert_eq!(settings.blink_sync_below, 0.0);
        assert_eq!(locked, ["despike", "blink_sync_below"]);

        // lid_open_snap: 0.53 by default, 0 to 0.75 (0.75 = off) allowed, and the command line wins over the file
        assert_eq!(merged("{}", &[]).unwrap().0.lid_open_snap, 0.53);
        for snap in ["0", "0.5", "0.7", "0.75"] {
            let file = format!(r#"{{"lid_open_snap": {snap}}}"#);
            assert_eq!(merged(&file, &[]).unwrap().0.lid_open_snap, snap.parse::<f32>().unwrap());
        }
        for snap in ["-0.01", "1.05", "\"on\""] {
            assert!(merged(&format!(r#"{{"lid_open_snap": {snap}}}"#), &[]).is_err(), "{snap}");
        }
        // A share of the open reading from before (above 0.75): where it started, as an eyelid
        for (share, lid) in [("0.8", 0.53), ("0.85", 0.59), ("0.9", 0.64), ("1.0", 0.75), ("1", 0.75)] {
            assert_eq!(merged(&format!(r#"{{"lid_open_snap": {share}}}"#), &[]).unwrap().0.lid_open_snap, lid, "{share}");
        }
        let (settings, locked) = merged(r#"{"lid_open_snap": 0.9}"#, &["--lid-open-snap", "0.6"]).unwrap();
        assert_eq!((settings.lid_open_snap, locked), (0.6, vec!["lid_open_snap"]));
        assert!(merged("{}", &["--lid-open-snap", "0.8"]).is_err());

        // camera_lid_floor: off (0) by default, 0 and 0.75 allowed, and the command line wins over the file
        assert_eq!(merged("{}", &[]).unwrap().0.camera_lid_floor, 0.0);
        for floor in ["0", "0.75"] {
            let file = format!(r#"{{"camera_lid_floor": {floor}}}"#);
            assert_eq!(merged(&file, &[]).unwrap().0.camera_lid_floor, floor.parse::<f32>().unwrap());
        }
        let (settings, locked) = merged(r#"{"camera_lid_floor": 0.4}"#, &["--camera-lid-floor", "0.1"]).unwrap();
        assert_eq!((settings.camera_lid_floor, locked), (0.1, vec!["camera_lid_floor"]));
        assert!(merged("{}", &["--camera-lid-floor", "0.9"]).is_err());

        // Negative numbers work as option values.
        let (settings, locked) = merged("{}", &["--gaze-offset-y", "-0.1", "--gaze-gain-down", "1.2"]).unwrap();
        assert_eq!((settings.gaze_offset_y, settings.gaze_gain_down), (-0.1, 1.2));
        assert_eq!(locked, ["gaze_offset_y", "gaze_gain_down"]);
        let (settings, locked) = merged(r#"{"gaze_roll_deg": 3.5}"#, &["--gaze-roll-deg", "-6.5"]).unwrap();
        assert_eq!(settings.gaze_roll_deg, -6.5);
        assert_eq!(locked, ["gaze_roll_deg"]);
        assert_eq!(merged(r#"{"gaze_roll_deg": 3.5}"#, &[]).unwrap().0.gaze_roll_deg, 3.5);
    }

    #[test]
    fn gaze_capture_requests_are_read_loosely() {
        let (_, asked) = parse(r#"{"gaze_capture": {"id": 4, "target": "up"}}"#).unwrap();
        let expected = GazeCapture {
            id: 4,
            target: "up".into(),
            seconds: CAPTURE_SECONDS,
            skip: CAPTURE_SKIP,
        };
        assert_eq!((asked.capture_id, asked.capture), (4, Some(expected)));
        // The length and the skipped start, kept within limits.
        let length = |text: &str| {
            let capture = parse(text).unwrap().1.capture.unwrap();
            (capture.seconds, capture.skip)
        };
        assert_eq!(length(r#"{"gaze_capture": {"id": 1, "target": "up", "seconds": 3, "skip": 0.3}}"#), (3.0, 0.3));
        assert_eq!(length(r#"{"gaze_capture": {"id": 1, "target": "up", "seconds": 60, "skip": -1}}"#), (5.0, 0.0));
        assert_eq!(length(r#"{"gaze_capture": {"id": 1, "target": "up", "seconds": 0.1, "skip": 9}}"#), (0.5, 0.3));
        assert_eq!(length(r#"{"gaze_capture": {"id": 1, "target": "up", "seconds": "x"}}"#), (2.0, 0.5));
        // Anything odd is no request, and never breaks the settings.
        for odd in [r#"{"gaze_capture": 3}"#, r#"{"gaze_capture": {"id": "x", "target": "up"}}"#] {
            let (settings, asked) = parse(odd).unwrap();
            assert_eq!((asked.capture_id, asked.capture), (0, None));
            assert_eq!(settings, Settings::default());
        }
        let (_, asked) = parse(r#"{"gaze_capture": {"id": 1, "target": "a-very-long-target-name"}}"#).unwrap();
        assert_eq!(asked.capture.unwrap().target.chars().count(), MAX_TARGET_CHARS);
    }

    #[test]
    fn native_eyes_are_off_until_asked_for() {
        assert!(!merged("{}", &[]).unwrap().0.native_eyes);
        assert!(merged(r#"{"native_eyes": true}"#, &[]).unwrap().0.native_eyes);
        let (settings, locked) = merged(r#"{"native_eyes": false}"#, &["--native-eyes"]).unwrap();
        assert!(settings.native_eyes);
        assert_eq!(locked, ["native_eyes"]);
    }

    #[test]
    fn camera_lids_are_on_until_turned_off() {
        assert!(merged("{}", &[]).unwrap().0.camera_lids);
        assert!(!merged(r#"{"camera_lids": false}"#, &[]).unwrap().0.camera_lids);
        assert!(merged(r#"{"camera_lids": 1}"#, &[]).is_err());
        let (settings, locked) = merged(r#"{"camera_lids": true}"#, &["--no-camera-lids"]).unwrap();
        assert!(!settings.camera_lids);
        assert_eq!(locked, ["camera_lids"]);
    }

    #[test]
    fn eye_behavior_is_2_unless_set_to_1() {
        assert_eq!(merged("{}", &[]).unwrap().0.eye_behavior, 2);
        assert!(merged("{}", &[]).unwrap().0.v2());
        let (v1, _) = merged(r#"{"eye_behavior": 1}"#, &[]).unwrap();
        assert!(v1.eye_behavior == 1 && !v1.v2());
        for bad in ["0", "3", "\"v1\"", "1.5"] {
            assert!(merged(&format!(r#"{{"eye_behavior": {bad}}}"#), &[]).is_err(), "{bad}");
        }
        let (settings, locked) = merged(r#"{"eye_behavior": 1}"#, &["--eye-behavior", "2"]).unwrap();
        assert_eq!((settings.eye_behavior, locked), (2, vec!["eye_behavior"]));
    }

    #[test]
    fn pupil_bits_are_0_to_4() {
        assert_eq!(merged("{}", &[]).unwrap().0.pupil_bits, 0);
        for bits in 0..=4 {
            assert_eq!(merged(&format!(r#"{{"pupil_bits": {bits}}}"#), &[]).unwrap().0.pupil_bits, bits);
        }
        for bad in ["5", "-1", "2.5", "\"3\"", "true"] {
            assert!(merged(&format!(r#"{{"pupil_bits": {bad}}}"#), &[]).is_err(), "{bad}");
        }
        let (settings, locked) = merged(r#"{"pupil_bits": 1}"#, &["--pupil-bits", "3"]).unwrap();
        assert_eq!((settings.pupil_bits, locked), (3, vec!["pupil_bits"]));
        assert!(Args::command().try_get_matches_from(["frameeyeosc", "--pupil-bits", "5"]).is_err());
    }

    #[test]
    fn pupils_go_to_vrchat_until_turned_off() {
        assert!(merged("{}", &[]).unwrap().0.pupils_to_vrchat);
        assert!(!merged(r#"{"pupils_to_vrchat": false}"#, &[]).unwrap().0.pupils_to_vrchat);
        assert!(merged(r#"{"pupils_to_vrchat": "no"}"#, &[]).is_err());
        let (settings, locked) = merged(r#"{"pupils_to_vrchat": true}"#, &["--no-pupils-to-vrchat"]).unwrap();
        assert!(!settings.pupils_to_vrchat);
        assert_eq!(locked, ["pupils_to_vrchat"]);
    }

    #[test]
    fn prefix_can_be_empty() {
        for prefix in ["", "/"] {
            let (settings, locked) = merged("{}", &["--prefix", prefix]).unwrap();
            assert_eq!(settings.prefix, "");
            assert_eq!(locked, ["prefix"]);
        }
        assert_eq!(merged(r#"{"prefix": "/"}"#, &[]).unwrap().0.prefix, "");
        assert_eq!(merged(r#"{"prefix": "/FT/"}"#, &[]).unwrap().0.prefix, "/FT");
    }

    #[test]
    fn targets_split_into_host_and_port() {
        assert_eq!(split_target("192.168.0.60:9000"), Some(("192.168.0.60".into(), 9000)));
        assert_eq!(split_target("[::1]:8889"), Some(("::1".into(), 8889)));
        assert_eq!(split_target("pc.local:9000"), Some(("pc.local".into(), 9000)));
        assert_eq!(split_target("192.168.0.60"), None);
        assert_eq!(split_target(":9000"), None);
    }

    fn write_atomically(path: &Path, text: &str) {
        let temporary = path.with_extension("tmp");
        fs::write(&temporary, text).unwrap();
        fs::rename(temporary, path).unwrap();
    }

    #[test]
    fn broken_file_keeps_the_previous_settings_until_fixed() {
        let dir = std::env::temp_dir().join(format!("frameeyeosc-config-test-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let path = dir.join("config.json");
        let (args, given) = cli(&["--lid-sync", "0.2"]);
        let mut config = Config::new(Some(path.clone()), args, given);

        // No file yet: defaults plus options, and no error.
        let settings = config.load().unwrap();
        assert_eq!(settings.lid_sync, 0.2);
        assert!(config.error.is_none() && config.check().is_none());

        write_atomically(&path, r#"{"lid_open": 0.85, "lid_sync": 0.9, "calibration_reset": 0}"#);
        let reload = config.check().unwrap();
        assert_eq!((reload.settings.lid_open, reload.settings.lid_sync), (0.85, 0.2));
        assert!(!reload.reset_calibration);

        write_atomically(&path, r#"{"lid_open": 0.9"#);
        assert!(config.check().is_none());
        assert!(config.error.is_some());
        write_atomically(&path, r#"{"lid_open": 0.1}"#);
        assert!(config.check().is_none());
        assert_eq!(config.error.as_deref(), Some("lid_closed must be below lid_open"));

        write_atomically(&path, r#"{"lid_open": 0.9, "calibration_reset": 1}"#);
        let reload = config.check().unwrap();
        assert_eq!(reload.settings.lid_open, 0.9);
        assert!(reload.reset_calibration && config.error.is_none());
        // Unchanged file: nothing to do.
        assert!(config.check().is_none());

        // Removing the file brings back the defaults without resetting the calibration, and the
        // panel's next write starts counting from 0 again.
        fs::remove_file(&path).unwrap();
        let reload = config.check().unwrap();
        assert_eq!(reload.settings.lid_open, 0.80);
        assert!(!reload.reset_calibration);
        write_atomically(&path, r#"{"calibration_reset": 0}"#);
        assert!(!config.check().unwrap().reset_calibration);
        write_atomically(&path, r#"{"calibration_reset": 1}"#);
        assert!(config.check().unwrap().reset_calibration);

        // A gaze capture is asked for once per new id.
        write_atomically(&path, r#"{"calibration_reset": 1, "gaze_capture": {"id": 1, "target": "up"}}"#);
        let reload = config.check().unwrap();
        assert_eq!(reload.gaze_capture.map(|capture| capture.target), Some("up".into()));
        assert!(!reload.reset_calibration);
        write_atomically(&path, r#"{"lid_open": 0.85, "calibration_reset": 1, "gaze_capture": {"id": 1, "target": "up"}}"#);
        assert!(config.check().unwrap().gaze_capture.is_none());
        write_atomically(&path, r#"{"calibration_reset": 1, "gaze_capture": {"id": 2, "target": "down"}}"#);
        assert_eq!(config.check().unwrap().gaze_capture.map(|capture| capture.id), Some(2));
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn broken_file_at_startup_falls_back_to_defaults() {
        let dir = std::env::temp_dir().join(format!("frameeyeosc-config-start-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let path = dir.join("config.json");
        fs::write(&path, "not json").unwrap();
        let (args, given) = cli(&[]);
        let mut config = Config::new(Some(path), args, given);
        assert_eq!(config.load().unwrap(), Settings::default());
        assert!(config.error.is_some());
        // Invalid options on the command line are still fatal.
        let (args, given) = cli(&["--lid-closed", "0.9"]);
        assert!(Config::new(None, args, given).load().is_err());
        fs::remove_dir_all(dir).unwrap();
    }
}
