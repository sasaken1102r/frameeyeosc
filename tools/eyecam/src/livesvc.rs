//! The live processing thread of `eyecam-rec --serve`: takes eye frames from the main (polling) thread, runs the
//! per-eye engines, publishes the shared memory, collects calibration samples and fits/stores calibrations.

use crate::driftlog;
use crate::live::{self, CalibFile, EyeEngine, Label, Params, Sample, UserParams};
use crate::liveshm::{self, LiveWriter};
use crate::preview::{self, Preview};
use crate::vision::{H, W};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, mpsc};
use std::time::{Duration, Instant};

/// A new wear (HMD taken off and put back on) when an eye's frames stop for this long.
const NEW_WEAR_GAP: f64 = 10.0;
/// An eye's published values are marked invalid when its frames stop for this long.
const STALE_AFTER: f64 = 0.25;
/// status.json's pupil_l / pupil_r: the share of frames with the pupil over this many seconds; cam_fps: the frames
/// a second over this many seconds.
const PUPIL_SHARE_S: f64 = 2.0;
/// status.json's cam_fps is null once an eye's last frame is older than this (s).
const CAM_FPS_FRESH: f64 = 0.5;
/// `relearn`'s refusals: live processing off (camera_lids off, or no buffers), no eye video coming in, a calibration
/// running.
pub const RELEARN_NOT_LIVE: &str = "目の値を出していないので、普段の目を覚え直せない";
pub const RELEARN_NO_VIDEO: &str = "目の映像が来ていないので、普段の目を覚え直せない";
pub const RELEARN_CALIBRATING: &str = "校正中なので、普段の目を覚え直せない";
/// Pupils react together: both eyes' diameters differ by less than this (recordings: median 0.3-0.6 mm, 99th
/// percentile 1.3-1.8 mm, part of it a fixed per-eye scale difference).
const PUPIL_LR_MAX_MM: f64 = 1.5;

/// The usual left - right difference (mm) over the last minute: the two eyes' scales are not identical (each is
/// normalised by its own iris radius), so the check compares the difference to this offset.
#[derive(Default)]
struct PupilOffset {
    recent: std::collections::VecDeque<(f64, f64)>,
}

impl PupilOffset {
    fn push(&mut self, t: f64, d: f64) {
        self.recent.push_back((t, d));
        while self.recent.front().is_some_and(|x| t - x.0 > 60.0) {
            self.recent.pop_front();
        }
    }

    /// The median once a second's worth of pairs is in (`fps`: the cameras' frame rate; 90 pairs at 90 fps), else 0.
    fn value(&self, fps: f64) -> f64 {
        if self.recent.len() < (fps.round() as usize).max(12) {
            return 0.0;
        }
        let mut v: Vec<f64> = self.recent.iter().map(|x| x.1).collect();
        crate::vision::median(&mut v)
    }
}

/// One eye's pupil history for the left/right consistency check.
#[derive(Default)]
struct PupilTrack {
    /// (time, mm) of accepted values over the last 10 s.
    recent: std::collections::VecDeque<(f64, f64)>,
    /// The latest raw value (time, mm), accepted or not.
    latest: Option<(f64, f64)>,
    /// The last accepted (ratio, mm, dilation).
    held: Option<(f64, f64, f64)>,
}

impl PupilTrack {
    fn median(&self) -> f64 {
        let mut v: Vec<f64> = self.recent.iter().map(|x| x.1).collect();
        crate::vision::median(&mut v)
    }

    fn accept(&mut self, t: f64, o: &live::Out) {
        self.recent.push_back((t, o.pupil_mm));
        while self.recent.front().is_some_and(|x| t - x.0 > 10.0) {
            self.recent.pop_front();
        }
        self.held = Some((o.pupil_ratio, o.pupil_mm, o.pupil_dilation));
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CollectKind {
    Wear,
    User,
    Pupil,
}

pub enum Msg {
    /// A raw frame of an anatomical eye (0 = L, 1 = R), 400x400 as stored; `upside_down`: its camera stores it upside
    /// down (the left eye's, see vision::UPSIDE_DOWN_EYE; decided by the camera, so --swap renaming the eyes does not
    /// move it, ring::Ring::upside_down). `pitch` and `open`: Valve's gaze pitch (degrees) and openness for that eye
    /// (NaN without them; the openness is a hint for the extended pupil search and goes into the drift log).
    Frame { eye: usize, upside_down: bool, t_cam_ns: u64, pitch: f32, open: f32, data: Vec<u8> },
    /// Start collecting labelled samples for a calibration.
    Collect(CollectKind),
    /// A protocol step began at `t0` (CLOCK_MONOTONIC_RAW seconds, the camera clock).
    Step { label: String, t0: f64, seconds: f64 },
    /// Fit what was collected and reply with a message to show (Ok) or the reason to redo (Err). With a directory,
    /// write calib_result.json (the values behind the result) and calib_samples.csv there.
    Finish(mpsc::Sender<Result<String, String>>, Option<PathBuf>),
    /// Drop what was collected (after writing the report into the directory, if given).
    Abort(Option<PathBuf>),
    /// Frames stopped (lost lock or no buffers): mark the outputs invalid now.
    Stale,
    SetLive(bool),
    /// Whether eyecam-rec holds the camera buffers (the shared memory's `live` needs both this and live on).
    Buffers(bool),
    /// `relearn`: start the auto baseline's warm-up again for both eyes (Worker::relearn); the reply is the reason
    /// when it can't.
    Relearn(mpsc::Sender<Result<(), String>>),
    Shutdown,
}

/// What the main thread shows in status.json.
pub struct Shared {
    /// The widen sensitivity (f64 bits), set from ctl.sock and read by the worker on every frame.
    pub widen_sensitivity: AtomicU64,
    pub calib_state: AtomicU32,
    pub recalib_suggested: AtomicBool,
    /// Both eyes' auto baselines are known (calib_state bit 4), else the warm-up still needed, in ms.
    pub baseline_ready: AtomicBool,
    pub warmup_ms: AtomicU32,
    /// calib.json holds at least one successful wear calibration (the widen step history).
    pub calib_saved: AtomicBool,
    /// The first-time setup (a successful `calib wear`, measured widen or not) is done; until then calib_state never
    /// says the auto baseline is ready.
    pub setup_done: AtomicBool,
    /// The last `calib wear` of this run: 0 none (or it failed), 1 widen measured, 2 widen not caught (history step).
    pub last_calib_widen: AtomicU32,
    /// The eyes whose part of the last `calib wear` failed: bit 1 left, bit 2 right (both: the calibration failed;
    /// one: it went through, that eye kept its earlier levels).
    pub calib_failed_eye: AtomicU32,
    /// Average processing time per frame over the last second, in microseconds.
    pub us_per_frame: AtomicU32,
    pub frames: AtomicU64,
    pub live_on: AtomicBool,
    /// Per eye, the share of the last PUPIL_SHARE_S of frames with the pupil (f32 bits; NaN: no frames, or live
    /// processing off). Counted as the calibration counts them.
    pub pupil_share: [AtomicU32; 2],
    /// Per eye, the frames a second its camera delivered over the last PUPIL_SHARE_S (f32 bits; camera timestamps,
    /// measured as the frames are taken from the buffers), and when the last one came (f64 bits, now_raw seconds).
    pub cam_fps: [AtomicU32; 2],
    pub cam_at: [AtomicU64; 2],
    /// The last `calib wear` (status.json `last_calib`): this run's, else the newest one saved under the data folder.
    pub last_calib: Mutex<Option<crate::status::LastCalib>>,
    /// The panel's eye preview is asked for until then (CLOCK_MONOTONIC ns; 0: off), set from ctl.sock (`preview on`
    /// renews it for preview::HOLD), and whether its file is there now (status.json `preview`).
    pub preview_until: AtomicU64,
    pub preview_on: AtomicBool,
    /// The drift log (`driftlog on` / `off` from ctl.sock), written by the worker as frames come.
    pub driftlog: Mutex<driftlog::Control>,
}

impl Default for Shared {
    fn default() -> Self {
        Self {
            widen_sensitivity: AtomicU64::new(live::DEFAULT_WIDEN_SENSITIVITY.to_bits()),
            calib_state: AtomicU32::new(0),
            recalib_suggested: AtomicBool::new(false),
            baseline_ready: AtomicBool::new(false),
            warmup_ms: AtomicU32::new(0),
            calib_saved: AtomicBool::new(false),
            setup_done: AtomicBool::new(false),
            last_calib_widen: AtomicU32::new(0),
            calib_failed_eye: AtomicU32::new(0),
            us_per_frame: AtomicU32::new(0),
            frames: AtomicU64::new(0),
            live_on: AtomicBool::new(false),
            pupil_share: [AtomicU32::new(f32::NAN.to_bits()), AtomicU32::new(f32::NAN.to_bits())],
            cam_fps: [AtomicU32::new(f32::NAN.to_bits()), AtomicU32::new(f32::NAN.to_bits())],
            cam_at: [AtomicU64::new(f64::NEG_INFINITY.to_bits()), AtomicU64::new(f64::NEG_INFINITY.to_bits())],
            last_calib: Mutex::new(None),
            preview_until: AtomicU64::new(0),
            preview_on: AtomicBool::new(false),
            driftlog: Mutex::new(driftlog::Control::default()),
        }
    }
}

impl Shared {
    pub fn widen_sensitivity(&self) -> f64 {
        f64::from_bits(self.widen_sensitivity.load(Ordering::Relaxed))
    }

    pub fn set_widen_sensitivity(&self, v: f64) {
        self.widen_sensitivity.store(v.to_bits(), Ordering::Relaxed);
    }

    /// `preview on` (for preview::HOLD from `now_ns`, CLOCK_MONOTONIC) or `preview off`.
    pub fn set_preview(&self, on: bool, now_ns: u64) {
        let until = if on { now_ns + preview::HOLD.as_nanos() as u64 } else { 0 };
        self.preview_until.store(until, Ordering::Relaxed);
    }

    /// Whether the panel's eye preview is asked for at `now_ns` (CLOCK_MONOTONIC).
    pub fn preview_wanted(&self, now_ns: u64) -> bool {
        now_ns < self.preview_until.load(Ordering::Relaxed)
    }

    /// The eye's recent pupil share (0..1), NaN when not known.
    pub fn pupil_share(&self, eye: usize) -> f64 {
        f32::from_bits(self.pupil_share[eye].load(Ordering::Relaxed)) as f64
    }

    /// Note a frame of camera `eye` taken at `t_cam` (s), handed over at `now` (now_raw s).
    pub fn note_camera_frame(&self, eye: usize, rate: &mut CamRate, t_cam: f64, now: f64) {
        let fps = rate.push(t_cam);
        self.cam_fps[eye].store((fps as f32).to_bits(), Ordering::Relaxed);
        self.cam_at[eye].store(now.to_bits(), Ordering::Relaxed);
    }

    /// The eye's camera frame rate at `now` (now_raw s): NaN without a frame in the last CAM_FPS_FRESH s.
    pub fn cam_fps(&self, eye: usize, now: f64) -> f64 {
        let at = f64::from_bits(self.cam_at[eye].load(Ordering::Relaxed));
        if now - at > CAM_FPS_FRESH { f64::NAN } else { f32::from_bits(self.cam_fps[eye].load(Ordering::Relaxed)) as f64 }
    }
}

/// One camera's frame rate: its frames (camera timestamps) over the last PUPIL_SHARE_S, in frames a second (NaN
/// with fewer than two). Starts over when the time goes back or the frames stop for a second.
#[derive(Default)]
pub struct CamRate {
    recent: std::collections::VecDeque<f64>,
}

impl CamRate {
    pub fn push(&mut self, t: f64) -> f64 {
        match self.recent.back() {
            // A frame of this eye arriving out of order: skip it rather than start over.
            Some(&b) if t <= b => return self.rate(b),
            // The video stopped for over a second: count from here.
            Some(&b) if t - b > 1.0 => self.recent.clear(),
            _ => {}
        }
        self.recent.push_back(t);
        while self.recent.front().is_some_and(|&f| t - f > PUPIL_SHARE_S) {
            self.recent.pop_front();
        }
        self.rate(t)
    }

    /// Frames per second over the kept frames, up to `t` (NaN with fewer than 2).
    fn rate(&self, t: f64) -> f64 {
        match (self.recent.len(), self.recent.front()) {
            (n, Some(&f)) if n >= 2 && t > f => (n - 1) as f64 / (t - f),
            _ => f64::NAN,
        }
    }
}

/// The recent frames of one eye: (time, pupil seen), for status.json's pupil share.
#[derive(Default)]
struct PupilSeen {
    recent: std::collections::VecDeque<(f64, bool)>,
    seen: usize,
}

impl PupilSeen {
    fn push(&mut self, t: f64, ok: bool) -> f64 {
        self.recent.push_back((t, ok));
        self.seen += ok as usize;
        while self.recent.front().is_some_and(|x| t - x.0 > PUPIL_SHARE_S) {
            let (_, old) = self.recent.pop_front().unwrap();
            self.seen -= old as usize;
        }
        self.seen as f64 / self.recent.len() as f64
    }
}

/// Cores worth pinning to: those with at least 3/4 of the largest cpu_capacity (the A720 / X4 cores here).
fn big_cores() -> Vec<usize> {
    let caps: Vec<(usize, u32)> = (0..64)
        .filter_map(|c| {
            let s = std::fs::read_to_string(format!("/sys/devices/system/cpu/cpu{c}/cpu_capacity")).ok()?;
            Some((c, s.trim().parse().ok()?))
        })
        .collect();
    let max = caps.iter().map(|c| c.1).max().unwrap_or(0);
    caps.into_iter().filter(|c| c.1 * 4 >= max * 3).map(|c| c.0).collect()
}

fn pin_to(cores: &[usize]) {
    if cores.is_empty() {
        return;
    }
    unsafe {
        let mut set: libc::cpu_set_t = std::mem::zeroed();
        for &c in cores {
            libc::CPU_SET(c, &mut set);
        }
        libc::sched_setaffinity(0, size_of::<libc::cpu_set_t>(), &set);
    }
}

/// The default calibration file, ~/.config/eyecam/calib.json.
pub fn calib_path() -> PathBuf {
    let home = std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("."));
    home.join(".config/eyecam/calib.json")
}

/// Read the calibration file (a missing or unreadable one is empty). A file from before the eyes were anatomical
/// (no `"eyes": "anatomical"`: its L was the right eye) is read for the other eye and saved again with the marker,
/// so it is converted once. The file as it was is kept first as `calib.json.pre-anatomical` (pre_anatomical_path;
/// one already there is never overwritten): an older eyecam saves calib.json again without the marker, and a newer
/// one would then exchange its eyes a second time; that copy is the way back.
fn load_calib(path: &Path) -> CalibFile {
    let Ok(text) = std::fs::read_to_string(path) else { return CalibFile::default() };
    match CalibFile::parse_converting(&text) {
        Ok((file, false)) => file,
        Ok((file, true)) => {
            let backup = pre_anatomical_path(path);
            let kept = std::fs::OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&backup)
                .and_then(|mut f| std::io::Write::write_all(&mut f, text.as_bytes()));
            match kept {
                Ok(()) => eprintln!("eyecam-rec: kept {} as {}", path.display(), backup.display()),
                Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => {
                    eprintln!("eyecam-rec: {} is there already: left as it is", backup.display())
                }
                Err(e) => eprintln!("eyecam-rec: {}: {e}", backup.display()),
            }
            match save_calib(path, &file) {
                Ok(()) => eprintln!("eyecam-rec: {}: converted to anatomical eyes (its L and R were the other eye)", path.display()),
                Err(e) => eprintln!("eyecam-rec: {e}"),
            }
            file
        }
        Err(e) => {
            eprintln!("eyecam-rec: ignoring {}: {e}", path.display());
            CalibFile::default()
        }
    }
}

/// Where load_calib keeps a calibration file from before the eyes were anatomical: next to it, `.pre-anatomical`
/// added to its name (calib.json.pre-anatomical).
fn pre_anatomical_path(path: &Path) -> PathBuf {
    let mut name = path.file_name().unwrap_or_default().to_os_string();
    name.push(".pre-anatomical");
    path.with_file_name(name)
}

fn save_calib(path: &Path, file: &CalibFile) -> Result<(), String> {
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    }
    let tmp = path.with_extension("json.tmp");
    std::fs::write(&tmp, file.to_json()).map_err(|e| format!("{}: {e}", tmp.display()))?;
    std::fs::rename(&tmp, path).map_err(|e| format!("{}: {e}", path.display()))
}

/// Wall-clock seconds (the drift log's `wall_unix`).
pub fn unix_now() -> f64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0.0, |d| d.as_secs_f64())
}

fn stamp() -> String {
    let t = unsafe { libc::time(std::ptr::null_mut()) };
    let mut tm: libc::tm = unsafe { std::mem::zeroed() };
    unsafe { libc::localtime_r(&t, &mut tm) };
    format!("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec)
}

struct Worker {
    writer: LiveWriter,
    shared: Arc<Shared>,
    calib_path: PathBuf,
    calib: CalibFile,
    engines: [EyeEngine; 2],
    params: [Params; 2],
    /// The widen sensitivity the params' curve was made from.
    sensitivity: f64,
    live: bool,
    buffers: bool,
    last_t: [f64; 2],
    last_seen: [Option<Instant>; 2],
    frames: [u64; 2],
    collect: Option<CollectKind>,
    steps: Vec<(Label, f64, f64)>,
    samples: [Vec<Sample>; 2],
    flipped: Vec<u8>,
    pupils: [PupilTrack; 2],
    pupil_seen: [PupilSeen; 2],
    pupil_offset: PupilOffset,
    pub pupil_conflicts: u64,
    pub pupil_pairs: u64,
    mono_minus_raw: i64,
    offset_at: Instant,
    busy: Duration,
    busy_n: u32,
    busy_since: Instant,
    log_n: u64,
    log_us: u64,
    log_since: Instant,
    /// The panel's eye preview: each eye's picture while it is asked for.
    preview: Preview,
}

impl Worker {
    /// A worker that hasn't seen a frame yet (live on, no buffers).
    fn new(writer: LiveWriter, shared: Arc<Shared>, calib_path: PathBuf, calib: CalibFile, preview_path: PathBuf) -> Self {
        Self {
            writer,
            shared,
            calib_path,
            calib,
            engines: [EyeEngine::default(), EyeEngine::default()],
            params: [Params::default(); 2],
            sensitivity: f64::NAN,
            live: true,
            buffers: false,
            last_t: [f64::NEG_INFINITY; 2],
            last_seen: [None; 2],
            frames: [0; 2],
            collect: None,
            steps: Vec::new(),
            samples: [Vec::new(), Vec::new()],
            flipped: vec![0; W * H],
            pupils: [PupilTrack::default(), PupilTrack::default()],
            pupil_seen: [PupilSeen::default(), PupilSeen::default()],
            pupil_offset: PupilOffset::default(),
            pupil_conflicts: 0,
            pupil_pairs: 0,
            mono_minus_raw: 0,
            offset_at: Instant::now() - Duration::from_secs(5),
            busy: Duration::ZERO,
            busy_n: 0,
            busy_since: Instant::now(),
            log_n: 0,
            log_us: 0,
            log_since: Instant::now(),
            preview: Preview::new(preview_path),
        }
    }

    fn calib_state(&self) -> u32 {
        let ready = self.calib.setup_done && self.engines.iter().all(EyeEngine::baseline_ready);
        self.params[0].wear.is_some() as u32 | (self.params[0].user.is_some() as u32) << 1 | (ready as u32) << 2
    }

    fn apply_calib(&mut self) {
        let hist = self.calib.history_params();
        for (e, p) in self.params.iter_mut().enumerate() {
            p.user = self.calib.user.map(|u| u[e]);
            p.hist = hist.map(|h| h[e]);
            p.pupil_measured = self.calib.pupil_measured;
        }
    }

    /// The auto baseline's progress and whether a step history exists, for status.json. While a relearn's warm-up
    /// runs, status.json says `warming` and counts it down, though the old levels stay in use (calib_state keeps 4).
    fn publish_baseline(&self) {
        let ready = self.engines.iter().all(|e| e.baseline_ready() && !e.relearning());
        let left = self.engines.iter().map(EyeEngine::warmup_remaining_s).fold(0.0, f64::max);
        self.shared.baseline_ready.store(ready, Ordering::Relaxed);
        self.shared.warmup_ms.store((left * 1000.0) as u32, Ordering::Relaxed);
        self.shared.calib_saved.store(!self.calib.history.is_empty(), Ordering::Relaxed);
        self.shared.setup_done.store(self.calib.setup_done, Ordering::Relaxed);
    }

    fn publish_state(&mut self) {
        self.publish_baseline();
        let state = self.calib_state();
        let recalib = self.engines.iter().any(|e| e.recalib_suggested);
        self.shared.calib_state.store(state, Ordering::Relaxed);
        self.shared.recalib_suggested.store(recalib, Ordering::Relaxed);
        let live = self.live && self.buffers;
        self.shared.live_on.store(live, Ordering::Relaxed);
        self.writer.update(|s| {
            s.calib_state = state;
            s.recalib_suggested = recalib as u32;
            s.live = live as u32;
        });
    }

    /// `relearn`: learn the relaxed eyes again, a new warm-up of the auto baseline for both eyes, while the eye video
    /// comes in. Unlike a new wear, this wear stays as it is: its calibration (params.wear, so calib_state bit 0 and
    /// the step and gap it measured), calib.json and its history (nothing is written), the geometry each engine learnt
    /// (lens edge, search window, iris radius, the closed-eye checks), the wearer's typical pitch, and the pupil
    /// checks. An eye with open-eye levels keeps using them (calib_state keeps bit 2; the eyelid, EyeWide and squint
    /// work as before) until its new ones are learnt, then moves over to those (EyeEngine::relearn); status.json says
    /// `baseline: warming` and counts the warm-up down meanwhile. Pressed again, the new warm-up starts over. An eye
    /// without levels yet warms up from the start as on a new wear (EyeWide and squint held at 0, blinks still show).
    fn relearn(&mut self) -> Result<(), String> {
        if !(self.live && self.buffers) {
            return Err(RELEARN_NOT_LIVE.into());
        }
        if self.collect.is_some() {
            return Err(RELEARN_CALIBRATING.into());
        }
        if self.last_seen.iter().all(Option::is_none) {
            return Err(RELEARN_NO_VIDEO.into());
        }
        for e in &mut self.engines {
            e.relearn();
        }
        self.publish_state();
        eprintln!(
            "eyecam-rec: relearning the relaxed eyes ({:.0} s warm-up; this wear's calibration kept: {})",
            live::WARMUP_S,
            if self.params[0].wear.is_some() { "yes" } else { "none" }
        );
        Ok(())
    }

    fn invalidate(&mut self, eye: Option<usize>) {
        for e in 0..2 {
            if eye.is_none_or(|x| x == e) {
                self.pupil_seen[e] = PupilSeen::default();
                self.shared.pupil_share[e].store(f32::NAN.to_bits(), Ordering::Relaxed);
            }
        }
        self.writer.update(|s| {
            for (e, v) in s.eyes.iter_mut().enumerate() {
                if eye.is_none_or(|x| x == e) {
                    v.valid = 0;
                }
            }
        });
    }

    fn frame(&mut self, eye: usize, upside_down: bool, t_cam_ns: u64, pitch: f32, open: f32, data: &[u8]) {
        if !self.live || data.len() != W * H {
            return;
        }
        let t = t_cam_ns as f64 * 1e-9;
        if t - self.last_t[eye] > NEW_WEAR_GAP && self.last_t[eye].is_finite() {
            // The HMD was off: a new wear. Its geometry and levels have to be learnt again.
            for e in &mut self.engines {
                e.reset();
            }
            for p in &mut self.params {
                p.wear = None;
            }
            self.pupils = [PupilTrack::default(), PupilTrack::default()];
            self.pupil_seen = [PupilSeen::default(), PupilSeen::default()];
            self.pupil_offset = PupilOffset::default();
            self.last_t = [f64::NEG_INFINITY; 2];
        }
        self.last_t[eye] = t;
        self.last_seen[eye] = Some(Instant::now());
        let img: &[u8] = if upside_down {
            for y in 0..H {
                self.flipped[y * W..(y + 1) * W].copy_from_slice(&data[(H - 1 - y) * W..(H - y) * W]);
            }
            &self.flipped
        } else {
            data
        };
        let sens = self.shared.widen_sensitivity();
        if sens != self.sensitivity {
            self.sensitivity = sens;
            let (start, width) = live::wide_curve(sens);
            for p in &mut self.params {
                (p.tune.wide_start, p.tune.wide_width) = (start, width);
            }
        }
        let started = Instant::now();
        let mut o = self.engines[eye].process_open(img, pitch as f64, open as f64, t, &self.params[eye]);
        let processed = started.elapsed();
        let started = Instant::now();
        self.check_pupils(eye, t, &mut o);
        let share = self.pupil_seen[eye].push(t, o.f.ok_pupil);
        self.shared.pupil_share[eye].store((share as f32).to_bits(), Ordering::Relaxed);
        {
            // The drift log, while on: what the engine just worked out, read only. Only handed to its thread here (no
            // disk under this lock; see driftlog::LiveLog)
            let mut drift = self.shared.driftlog.lock().unwrap();
            if drift.is_on() {
                let p = driftlog::Probe::of(&self.engines[eye], &o, &self.params[eye], t, open as f64);
                drift.push(eye, &p, unix_now());
            }
        }
        self.busy += processed + started.elapsed();
        // The panel's eye preview: this frame halved, with where the pupil was found and looked for, when one is due
        // (only while it is asked for; after the left / right pupil check, as the frame's values go out; its own time,
        // not live_ms)
        let img: &[u8] = if upside_down { &self.flipped } else { data };
        let wanted = self.shared.preview_wanted(liveshm::clock_ns(libc::CLOCK_MONOTONIC));
        let t_mono_now = (t_cam_ns as i64 + self.mono_minus_raw).max(0) as u64;
        self.preview.frame(wanted, eye, img, &o, t, t_mono_now);
        self.shared.preview_on.store(self.preview.is_on(), Ordering::Relaxed);
        self.busy_n += 1;
        self.frames[eye] += 1;
        if self.collect.is_some() {
            let label = self.steps.iter().rev().find(|(_, t0, sec)| t >= t0 + live::STEP_DELAY && t < t0 + sec).map(|s| s.0);
            if let Some(l) = label {
                let smp = self.engines[eye].calib_sample(l, &o, t);
                self.samples[eye].push(smp);
            }
        }
        if self.offset_at.elapsed() > Duration::from_secs(1) {
            let r0 = liveshm::clock_ns(libc::CLOCK_MONOTONIC_RAW);
            let m = liveshm::clock_ns(libc::CLOCK_MONOTONIC);
            let r1 = liveshm::clock_ns(libc::CLOCK_MONOTONIC_RAW);
            self.mono_minus_raw = m as i64 - ((r0 + r1) / 2) as i64;
            self.offset_at = Instant::now();
        }
        let t_mono = (t_cam_ns as i64 + self.mono_minus_raw).max(0) as u64;
        let frames = self.frames[eye];
        let state = self.calib_state();
        let recalib = self.engines.iter().any(|e| e.recalib_suggested);
        self.writer.update(|s| {
            s.t_mono_ns = s.t_mono_ns.max(t_mono);
            s.t_cam_raw_ns = s.t_cam_raw_ns.max(t_cam_ns);
            s.calib_state = state;
            s.recalib_suggested = recalib as u32;
            let v = &mut s.eyes[eye];
            v.t_mono_ns = t_mono;
            v.t_cam_raw_ns = t_cam_ns;
            v.frame_count = frames;
            v.valid = o.valid as u32;
            v.closed = o.closed as u32;
            v.eye_lid = o.eye_lid as f32;
            v.eye_wide = o.eye_wide as f32;
            v.eye_squint = o.eye_squint as f32;
            v.pupil_ratio = o.pupil_ratio as f32;
            v.pupil_mm = o.pupil_mm as f32;
            v.pupil_dilation = o.pupil_dilation as f32;
            v.confidence = o.confidence as f32;
            v.skin_up = o.skin_up as f32;
            v.aperture = o.aperture as f32;
        });
        self.shared.frames.fetch_add(1, Ordering::Relaxed);
        self.shared.calib_state.store(state, Ordering::Relaxed);
        self.publish_baseline();
        self.shared.recalib_suggested.store(recalib, Ordering::Relaxed);
    }

    /// Write calib_result.json and calib_samples.csv for a calibration attempt, and log the per-eye values. A wear
    /// calibration's becomes status.json's `last_calib` too. `failed_eye`: "L" / "R" / "LR" / "" (calib_failed_eye).
    fn report(
        &self,
        dir: Option<&Path>,
        kind: CollectKind,
        samples: &[Vec<Sample>; 2],
        result: &Result<String, String>,
        params: Option<String>,
        failed_eye: &str,
    ) {
        let (values, lines) = match kind {
            CollectKind::Wear => live::wear_report(samples),
            CollectKind::User => match (self.params[0].wear, self.params[1].wear) {
                (Some(l), Some(r)) => live::user_report(samples, &[l, r]),
                _ => ("null".into(), vec!["calib user: no wear calibration this wear".into()]),
            },
            CollectKind::Pupil => ("null".into(), Vec::new()),
        };
        for l in &lines {
            eprintln!("{l}");
        }
        let (ok, message) = match result {
            Ok(m) => (true, m.as_str()),
            Err(m) => (false, m.as_str()),
        };
        let kind_name = match kind {
            CollectKind::Wear => "wear",
            CollectKind::User => "user",
            CollectKind::Pupil => "pupil",
        };
        let json = format!(
            "{{\n  \"kind\": \"{kind_name}\",\n  {},\n  \"time\": {},\n  \"ok\": {ok},\n  \"failed_eye\": {},\n  \"message\": {},\n  \"values\": {values},\n  \"params\": {}\n}}\n",
            live::JSON_EYES_ANATOMICAL,
            crate::json::string(&stamp()),
            crate::json::string(failed_eye),
            crate::json::string(message),
            params.unwrap_or_else(|| "null".into())
        );
        if kind == CollectKind::Wear {
            *self.shared.last_calib.lock().unwrap() = crate::status::LastCalib::from_result(&json);
        }
        let Some(dir) = dir else { return };
        if let Err(e) = std::fs::write(dir.join("calib_result.json"), json) {
            eprintln!("eyecam-rec: calib_result.json: {e}");
        }
        if let Err(e) = std::fs::write(dir.join("calib_samples.csv"), live::samples_csv(samples)) {
            eprintln!("eyecam-rec: calib_samples.csv: {e}");
        }
    }

    /// Both eyes' pupils should agree within PUPIL_LR_MAX_MM (after the usual per-eye offset). When they do not, the
    /// eye whose value is further from its own last 10 s holds its last accepted value (with lower confidence) until
    /// they agree again. Only fresh values count (the engine holds the pupil while it is not seen whole).
    fn check_pupils(&mut self, eye: usize, t: f64, o: &mut live::Out) {
        if !(o.f.ok_pupil && o.pupil_mm.is_finite() && !o.closed) {
            return;
        }
        if self.pupils[eye].latest.is_some_and(|(_, m)| m == o.pupil_mm) {
            // Held by the engine (pupil not seen whole): nothing new to compare.
            return;
        }
        self.pupils[eye].latest = Some((t, o.pupil_mm));
        let other = 1 - eye;
        let offset = self.pupil_offset.value(self.engines[eye].fps());
        // Left minus right, whichever eye this is.
        let lr = |mine: f64, theirs: f64| if eye == 0 { mine - theirs } else { theirs - mine };
        let conflict = match self.pupils[other].latest {
            Some((to, mo)) if (t - to).abs() < 0.05 => {
                self.pupil_pairs += 1;
                let d = lr(o.pupil_mm, mo);
                if (d - offset).abs() <= PUPIL_LR_MAX_MM {
                    self.pupil_offset.push(t, d);
                    false
                } else {
                    let (me, mo_med) = (self.pupils[eye].median(), self.pupils[other].median());
                    // No history yet on one side: trust neither more than the other, keep this eye's value.
                    me.is_finite() && mo_med.is_finite() && (o.pupil_mm - me).abs() > (mo - mo_med).abs()
                }
            }
            _ => false,
        };
        if conflict {
            self.pupil_conflicts += 1;
            if let Some((ratio, mm, dil)) = self.pupils[eye].held {
                (o.pupil_ratio, o.pupil_mm, o.pupil_dilation) = (ratio, mm, dil);
            }
            o.confidence *= 0.5;
        } else {
            self.pupils[eye].accept(t, o);
        }
    }

    fn finish(&mut self, dir: Option<PathBuf>) -> Result<String, String> {
        let kind = self.collect.take().ok_or("校正中じゃない")?;
        let samples = std::mem::take(&mut self.samples);
        self.steps.clear();
        let mut params = None;
        let result = self.fit(kind, &samples, &mut params);
        if kind == CollectKind::Wear && result.is_err() {
            self.shared.last_calib_widen.store(0, Ordering::Relaxed);
        }
        let failed_eye = match kind {
            CollectKind::Wear => ["", "L", "R", "LR"][(self.shared.calib_failed_eye.load(Ordering::Relaxed) & 3) as usize],
            _ => "",
        };
        self.report(dir.as_deref(), kind, &samples, &result, params, failed_eye);
        self.publish_state();
        result
    }

    fn fit(&mut self, kind: CollectKind, samples: &[Vec<Sample>; 2], params_out: &mut Option<String>) -> Result<String, String> {
        let samples = samples.clone();
        match kind {
            CollectKind::Wear => {
                // A widen that was not caught does not fail the calibration: that eye uses the history's step.
                let hist = self.calib.history_params();
                let fallback = [0, 1].map(|e| hist.map_or(live::DEFAULT_WIDEN_STEP, |h| h[e].step));
                // One eye failing does not fail the calibration either: the other eye's levels are new, the failed
                // one keeps its earlier ones if they were measured on it (else gets provisional ones from the other
                // eye and the history).
                let out = live::fit_wear_settled(&samples, fallback, self.calib.measured_wear(), hist);
                let failed = out.as_ref().map_or([true, true], |o| o.failed);
                self.shared.calib_failed_eye.store(failed[0] as u32 | (failed[1] as u32) << 1, Ordering::Relaxed);
                let out = out?;
                let w = out.wear;
                *params_out = Some(live::wear_params_json(&w));
                // The calibration only sets where the baselines start (they keep following the wearer) and adds its
                // sizes to the history.
                for ((p, wp), eng) in self.params.iter_mut().zip(w).zip(&mut self.engines) {
                    p.wear = Some(wp);
                    eng.seed(&wp);
                }
                self.calib.wear = Some(w);
                self.calib.wear_time = stamp();
                // Only a widen step measured on both eyes goes into the history (a fallback would just repeat the
                // median, an eye that failed would repeat its earlier values).
                if out.for_history() {
                    self.calib.push_history(&self.calib.wear_time.clone(), &w);
                }
                self.calib.wear_widen_measured = Some(out.widen_measured);
                self.calib.wear_failed_eye = out.failed_eye().to_string();
                self.calib.setup_done = true;
                self.apply_calib();
                self.shared.last_calib_widen.store(if out.widen_measured { 1 } else { 2 }, Ordering::Relaxed);
                save_calib(&self.calib_path, &self.calib)?;
                Ok(out.message)
            }
            CollectKind::User => {
                let wear = match (self.params[0].wear, self.params[1].wear) {
                    (Some(l), Some(r)) => [l, r],
                    _ => return Err("先に calib wear をしてね".into()),
                };
                let (u, warnings) = live::fit_user(&samples, &wear, self.calib.user, true)?;
                *params_out = Some(live::user_params_json(&u));
                self.calib.user = Some(u);
                self.calib.user_time = stamp();
                self.calib.user_warnings = warnings.clone();
                save_calib(&self.calib_path, &self.calib)?;
                self.apply_calib();
                Ok(if warnings.is_empty() {
                    "校正できた（ユーザー）".to_string()
                } else {
                    format!("校正できた（ユーザー）。注意: {}", warnings.join("、"))
                })
            }
            CollectKind::Pupil => {
                let r = match (self.params[0].wear, self.params[1].wear) {
                    (Some(l), Some(r)) => Some([l.r_px, r.r_px]),
                    _ => None,
                };
                let pr = live::fit_pupil(&samples, r).ok_or("瞳孔の範囲は測れなかった")?;
                let mut u = self.calib.user.unwrap_or([UserParams::default(); 2]);
                for e in 0..2 {
                    (u[e].pd_min, u[e].pd_max) = pr[e];
                }
                self.calib.user = Some(u);
                self.calib.pupil_measured = true;
                if self.calib.user_time.is_empty() {
                    self.calib.user_time = stamp();
                }
                save_calib(&self.calib_path, &self.calib)?;
                self.apply_calib();
                Ok("瞳孔の範囲を保存した".to_string())
            }
        }
    }

    fn housekeeping(&mut self) {
        // The preview off once it is no longer asked for, frames or not (a panel that stopped renewing it)
        if !self.shared.preview_wanted(liveshm::clock_ns(libc::CLOCK_MONOTONIC)) {
            self.preview.keep(false);
            self.shared.preview_on.store(false, Ordering::Relaxed);
        }
        for e in 0..2 {
            if self.last_seen[e].is_some_and(|t| t.elapsed() > Duration::from_secs_f64(STALE_AFTER)) {
                self.last_seen[e] = None;
                self.invalidate(Some(e));
            }
        }
        if self.busy_since.elapsed() >= Duration::from_secs(1) {
            let us = (self.busy.as_micros() as u32).checked_div(self.busy_n).unwrap_or(0);
            self.shared.us_per_frame.store(us, Ordering::Relaxed);
            self.log_n += self.busy_n as u64;
            self.log_us += self.busy.as_micros() as u64;
            if self.log_since.elapsed() >= Duration::from_secs(30) {
                if self.log_n > 0 {
                    eprintln!(
                        "live: {} frames in the last {:.0} s, {:.2} ms per frame; pupils: {} of {} left/right pairs in conflict so far, usual L-R {:+.2} mm",
                        self.log_n,
                        self.log_since.elapsed().as_secs_f64(),
                        self.log_us as f64 / self.log_n as f64 / 1000.0,
                        self.pupil_conflicts,
                        self.pupil_pairs,
                        self.pupil_offset.value(self.engines[0].fps())
                    );
                    if self.preview.is_on() {
                        eprintln!(
                            "live: preview on, {} pictures so far, {:.0} us each",
                            self.preview.pictures,
                            self.preview.us_per_picture()
                        );
                    }
                }
                self.log_n = 0;
                self.log_us = 0;
                self.log_since = Instant::now();
            }
            self.busy = Duration::ZERO;
            self.busy_n = 0;
            self.busy_since = Instant::now();
        }
    }
}

/// Start the worker. It owns `<run_dir>/live`, reads/writes `calib`, and reports through `shared`. A calib.json
/// without a history of wear calibrations gets one from the calibration attempts saved under `data_dir`.
pub fn spawn(
    run_dir: &Path,
    calib: PathBuf,
    data_dir: Option<&Path>,
    shared: Arc<Shared>,
) -> Result<(mpsc::SyncSender<Msg>, std::thread::JoinHandle<()>), String> {
    let writer = LiveWriter::create(&run_dir.join("live"))?;
    // A preview file left by a run that was killed: its pictures go now
    let preview_path = run_dir.join(preview::FILE_NAME);
    if std::fs::remove_file(&preview_path).is_ok() {
        eprintln!("eyecam-rec: removed the eye preview left by an earlier run");
    }
    let (tx, rx) = mpsc::sync_channel::<Msg>(16);
    let mut calib_file = load_calib(&calib);
    if calib_file.history.is_empty()
        && let Some(dir) = data_dir
    {
        let found = CalibFile::history_from_results(dir);
        if !found.is_empty() {
            eprintln!("eyecam-rec: widen step history from {} saved wear calibrations in {}", found.len(), dir.display());
            calib_file.history = found;
            let extra = calib_file.history.len().saturating_sub(live::HISTORY_MAX);
            calib_file.history.drain(..extra);
            if let Err(e) = save_calib(&calib, &calib_file) {
                eprintln!("eyecam-rec: {e}");
            }
        }
    }
    // The last wear calibration, as saved under the data folder (status.json's last_calib after a restart)
    if let Some(dir) = data_dir {
        *shared.last_calib.lock().unwrap() = crate::status::LastCalib::newest_in(dir);
    }
    // The last wear calibration's widen, as it was saved (so a restart keeps last_calib_widen)
    shared.last_calib_widen.store(
        match calib_file.wear_widen_measured {
            Some(true) => 1,
            Some(false) => 2,
            None => 0,
        },
        Ordering::Relaxed,
    );
    shared.calib_failed_eye.store(
        match calib_file.wear_failed_eye.as_str() {
            "L" => 1,
            "R" => 2,
            _ => 0,
        },
        Ordering::Relaxed,
    );
    match calib_file.history_params() {
        Some(h) => eprintln!(
            "eyecam-rec: widen step L {:.3} / R {:.3}, open gap L {:.3} / R {:.3}, R L {:.1} / R {:.1} px (medians of {} wear calibrations)",
            h[0].step,
            h[1].step,
            h[0].gap,
            h[1].gap,
            h[0].r_px,
            h[1].r_px,
            calib_file.history.len()
        ),
        None => eprintln!("eyecam-rec: no wear calibration saved yet: default widen step until calib wear is done once"),
    }
    let sh = shared.clone();
    let handle = std::thread::Builder::new()
        .name("eyecam-live".into())
        .spawn(move || {
            let cores = big_cores();
            pin_to(&cores);
            let mut w = Worker::new(writer, sh, calib, calib_file, preview_path);
            w.apply_calib();
            w.publish_state();
            loop {
                match rx.recv_timeout(Duration::from_millis(50)) {
                    Ok(Msg::Frame { eye, upside_down, t_cam_ns, pitch, open, data }) => {
                        w.frame(eye, upside_down, t_cam_ns, pitch, open, &data)
                    }
                    Ok(Msg::Collect(kind)) => {
                        w.collect = Some(kind);
                        w.steps.clear();
                        w.samples = [Vec::new(), Vec::new()];
                        // A calibration starts from a clean pupil search: a prior left on something else (caught
                        // before the calibration) could keep the pupil out of reach for seconds.
                        for e in &mut w.engines {
                            e.reset_tracking();
                        }
                    }
                    Ok(Msg::Step { label, t0, seconds }) => w.steps.push((Label::parse(&label), t0, seconds)),
                    Ok(Msg::Finish(reply, dir)) => {
                        let _ = reply.send(w.finish(dir));
                    }
                    Ok(Msg::Abort(dir)) => {
                        if let Some(kind) = w.collect.take() {
                            let samples = std::mem::take(&mut w.samples);
                            w.report(dir.as_deref(), kind, &samples, &Err("途中で止めた".into()), None, "");
                        }
                        w.steps.clear();
                        w.samples = [Vec::new(), Vec::new()];
                    }
                    Ok(Msg::Stale) => {
                        w.last_seen = [None; 2];
                        w.invalidate(None);
                    }
                    Ok(Msg::SetLive(on)) => {
                        w.live = on;
                        if !on {
                            w.invalidate(None);
                        }
                        w.publish_state();
                    }
                    Ok(Msg::Buffers(held)) => {
                        w.buffers = held;
                        if !held {
                            w.last_seen = [None; 2];
                            w.invalidate(None);
                        }
                        w.publish_state();
                    }
                    Ok(Msg::Relearn(reply)) => {
                        let _ = reply.send(w.relearn());
                    }
                    Ok(Msg::Shutdown) | Err(mpsc::RecvTimeoutError::Disconnected) => break,
                    Err(mpsc::RecvTimeoutError::Timeout) => {}
                }
                w.housekeeping();
            }
            // The drift log's last part written and its file closed (waited for once the lock is let go)
            let closing = w.shared.driftlog.lock().unwrap().off(unix_now());
            closing.wait();
        })
        .map_err(|e| e.to_string())?;
    Ok((tx, handle))
}

/// The worker end to end on a recorded session (no root needed):
/// `EYECAM_SESSION=dir cargo test --release -- --ignored worker_on_a_session --nocapture`.
#[cfg(test)]
mod tests {
    use super::*;
    use crate::replay::Session;

    #[test]
    fn pupil_share_covers_the_last_two_seconds() {
        let mut p = PupilSeen::default();
        assert_eq!(p.push(0.0, true), 1.0);
        for k in 1..=90 {
            p.push(k as f64 / 90.0, k % 2 == 0);
        }
        // 91 frames over 1 s: 46 with the pupil.
        assert!((p.push(1.0 + 1.0 / 90.0, false) - 46.0 / 92.0).abs() < 1e-9);
        // 3 s later only the last 2 s count.
        let mut last = 0.0;
        for k in 0..=180 {
            last = p.push(2.0 + k as f64 / 90.0, k >= 90);
        }
        assert!((last - 91.0 / 181.0).abs() < 0.01, "{last}");
    }

    #[test]
    fn camera_rate_is_frames_a_second_over_the_last_two_seconds() {
        let mut r = CamRate::default();
        assert!(r.push(100.0).is_nan());
        let mut last = f64::NAN;
        for k in 1..300 {
            last = r.push(100.0 + k as f64 / 90.0);
        }
        assert!((last - 90.0).abs() < 1e-6, "{last}");
        for k in 1..60 {
            last = r.push(110.0 + k as f64 / 15.0);
        }
        assert!((last - 15.0).abs() < 1e-6, "after a gap it starts over: {last}");
        // A frame arriving out of order is skipped, not a reason to start over.
        let before = last;
        let back = r.push(110.0 + 30.0 / 15.0);
        assert!((back - before).abs() < 1e-9 && !back.is_nan(), "{back} vs {before}");
        let shared = Shared::default();
        let mut rate = CamRate::default();
        for k in 0..30 {
            shared.note_camera_frame(1, &mut rate, 5.0 + k as f64 / 15.0, 50.0 + k as f64 / 15.0);
        }
        assert!((shared.cam_fps(1, 52.0) - 15.0).abs() < 1e-3, "{}", shared.cam_fps(1, 52.0));
        assert!(shared.cam_fps(1, 53.0).is_nan(), "frames stopped");
        assert!(shared.cam_fps(0, 52.0).is_nan(), "no frames from that camera");
    }

    #[test]
    fn the_preview_is_on_only_while_renewed() {
        let shared = Shared::default();
        let s = 1_000_000_000u64;
        assert!(!shared.preview_wanted(5 * s), "off at start");
        shared.set_preview(true, 5 * s);
        assert!(shared.preview_wanted(5 * s) && shared.preview_wanted(8 * s - 1));
        assert!(!shared.preview_wanted(8 * s), "off by itself preview::HOLD after the last `on`");
        shared.set_preview(true, 7 * s);
        assert!(shared.preview_wanted(9 * s), "renewed");
        shared.set_preview(false, 7 * s);
        assert!(!shared.preview_wanted(7 * s), "`preview off` at once");
    }

    /// The worker end to end on made-up frames (no eye in them): the preview's file appears while it is asked for,
    /// holds both eyes' pictures, and goes once it is not renewed, frames or not.
    #[test]
    fn the_worker_writes_the_preview_while_asked() {
        let dir = std::env::temp_dir().join(format!("eyecam-worker-preview-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        // One left by a run that was killed goes at start
        let path = dir.join(preview::FILE_NAME);
        std::fs::write(&path, b"old").unwrap();
        let shared = Arc::new(Shared::default());
        let (tx, handle) = spawn(&dir, dir.join("calib.json"), None, shared.clone()).unwrap();
        tx.send(Msg::Buffers(true)).unwrap();
        let wait = |gone: bool| {
            for _ in 0..100 {
                if path.exists() != gone {
                    return true;
                }
                std::thread::sleep(Duration::from_millis(20));
            }
            false
        };
        assert!(wait(true), "the old file removed");
        let frame: Vec<u8> = (0..W * H).map(|i| (40 + (i % W) / 4 + (i / W) / 8) as u8).collect();
        let send = |k: u64| {
            for eye in 0..2 {
                let data = frame.clone();
                tx.send(Msg::Frame { eye, upside_down: eye == 0, t_cam_ns: 50_000_000_000 + k * 11_111_111, pitch: f32::NAN, open: f32::NAN, data }).unwrap();
            }
        };
        // Not asked: frames, but no file
        for k in 0..30 {
            send(k);
        }
        std::thread::sleep(Duration::from_millis(200));
        assert!(!path.exists());
        // Asked (as `preview on` does): both eyes' pictures, the left one turned upright like the engine's frame
        shared.set_preview(true, liveshm::clock_ns(libc::CLOCK_MONOTONIC));
        for k in 30..120 {
            send(k);
        }
        assert!(wait(false), "the file made");
        std::thread::sleep(Duration::from_millis(200));
        assert!(shared.preview_on.load(Ordering::Relaxed));
        let seen = preview::parse(&std::fs::read(&path).unwrap()).unwrap();
        assert!(seen.on && seen.count.iter().all(|&n| (10..=20).contains(&n)), "{:?}", seen.count);
        let mut upright = vec![0u8; W * H];
        for y in 0..H {
            upright[y * W..(y + 1) * W].copy_from_slice(&frame[(H - 1 - y) * W..(H - y) * W]);
        }
        let mut half = vec![0u8; preview::SIDE * preview::SIDE];
        preview::halve(&upright, &mut half);
        assert_eq!(seen.pictures[0], half, "the left camera's frame upright");
        preview::halve(&frame, &mut half);
        assert_eq!(seen.pictures[1], half);
        // Not renewed: off by itself (here 0.1 s from now), without any more frames
        shared.set_preview(true, liveshm::clock_ns(libc::CLOCK_MONOTONIC) + 100_000_000 - preview::HOLD.as_nanos() as u64);
        assert!(wait(true), "removed once it ran out");
        assert!(!shared.preview_on.load(Ordering::Relaxed));
        tx.send(Msg::Shutdown).unwrap();
        handle.join().unwrap();
        std::fs::remove_dir_all(&dir).unwrap();
    }

    /// The drift log through the worker: rows for both eyes while it is on, the last part written and the file closed
    /// when the worker stops (eyecam-rec exiting).
    #[test]
    fn the_worker_writes_the_drift_log_and_closes_it_at_exit() {
        let dir = std::env::temp_dir().join(format!("eyecam-worker-drift-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let shared = Arc::new(Shared::default());
        let (tx, handle) = spawn(&dir, dir.join("calib.json"), None, shared.clone()).unwrap();
        tx.send(Msg::Buffers(true)).unwrap();
        let path = {
            let mut c = shared.driftlog.lock().unwrap();
            c.dir = dir.join(driftlog::DIR_NAME);
            let log = driftlog::DriftLog::create(&c.dir, "test").unwrap().start().unwrap();
            let (path, closing) = c.on(log, 1.0);
            closing.wait();
            path
        };
        // 2.5 s of made-up frames (no eye in them), Valve's openness 0.8 on the left and 0.6 on the right
        let frame: Vec<u8> = (0..W * H).map(|i| (40 + (i % W) / 4) as u8).collect();
        for k in 0..225u64 {
            for eye in 0..2 {
                let data = frame.clone();
                let open = [0.8, 0.6][eye];
                tx.send(Msg::Frame { eye, upside_down: false, t_cam_ns: 50_000_000_000 + k * 1_000_000_000 / 90, pitch: -10.0, open, data }).unwrap();
            }
        }
        tx.send(Msg::Shutdown).unwrap();
        handle.join().unwrap();
        let st = shared.driftlog.lock().unwrap().status();
        assert!(!st.on && st.path == path.display().to_string(), "{st:?}");
        let text = std::fs::read_to_string(&path).unwrap();
        let lines: Vec<&str> = text.lines().collect();
        assert_eq!(lines[0], driftlog::HEADER);
        // Two whole seconds and the half second left, per eye
        assert_eq!(lines.len(), 1 + 6, "{text}");
        let head: Vec<&str> = driftlog::HEADER.split(',').collect();
        let col = |row: &str, name: &str| row.split(',').nth(head.iter().position(|h| *h == name).unwrap()).unwrap().to_string();
        let frames: Vec<String> = lines[1..].iter().map(|r| col(r, "frames")).collect();
        assert_eq!(frames.iter().filter(|f| *f == "90").count(), 4, "{frames:?}");
        for r in &lines[1..] {
            let open = if col(r, "eye") == "L" { "0.800" } else { "0.600" };
            assert_eq!((col(r, "valve_open").as_str(), col(r, "valve_pitch").as_str()), (open, "-10.0"), "{r}");
            assert_eq!(col(r, "pupil_share"), "0.000", "{r}");
            assert!(col(r, "wall_unix").parse::<f64>().unwrap() > 1.7e9, "{r}");
        }
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn relearn_keeps_this_wears_calibration() {
        let dir = std::env::temp_dir().join(format!("eyecam-relearn-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("calib.json");
        let wl = live::WearParams { r_px: 50.0, b_n: 0.8, b_w: 1.0, ap_n: 1.6, ap_cl: 0.5, pitch_n: -12.0, lower_px: 240.0 };
        let wr = live::WearParams { r_px: 52.0, b_n: 0.7, b_w: 0.9, ap_n: 1.5, ap_cl: 0.4, pitch_n: -13.0, lower_px: 236.0 };
        let mut file = CalibFile { wear: Some([wl, wr]), setup_done: true, ..CalibFile::default() };
        file.push_history("2026-10-17 10:00:00", &[wl, wr]);
        save_calib(&path, &file).unwrap();
        let saved = std::fs::read_to_string(&path).unwrap();
        let shared = Arc::new(Shared::default());
        let writer = LiveWriter::create(&dir.join("live")).unwrap();
        let mut w = Worker::new(writer, shared.clone(), path.clone(), load_calib(&path), dir.join("preview"));
        w.apply_calib();
        // Calibrated this wear: as Worker::fit leaves it
        for ((p, wp), eng) in w.params.iter_mut().zip([wl, wr]).zip(&mut w.engines) {
            p.wear = Some(wp);
            eng.seed(&wp);
        }
        w.publish_state();
        assert_eq!(shared.calib_state.load(Ordering::Relaxed), 1 | 4);
        assert!(shared.baseline_ready.load(Ordering::Relaxed));
        let wear = w.params.map(|p| p.wear);
        // Refused: no buffers, no video, a calibration running
        assert_eq!(w.relearn(), Err(RELEARN_NOT_LIVE.to_string()));
        w.buffers = true;
        assert_eq!(w.relearn(), Err(RELEARN_NO_VIDEO.to_string()));
        w.last_seen[1] = Some(Instant::now());
        w.collect = Some(CollectKind::Wear);
        assert_eq!(w.relearn(), Err(RELEARN_CALIBRATING.to_string()));
        w.collect = None;
        w.live = false;
        assert_eq!(w.relearn(), Err(RELEARN_NOT_LIVE.to_string()));
        w.live = true;
        assert!(shared.baseline_ready.load(Ordering::Relaxed), "nothing changed by a refusal");
        // Relearnt: both eyes warm up again, status.json counting it down, the old levels still in use...
        assert_eq!(w.relearn(), Ok(()));
        assert!(w.engines.iter().all(|e| e.relearning() && e.baseline_ready() && e.warmup_remaining_s() == live::WARMUP_S));
        assert_eq!(w.engines[0].baseline_px(), Some((40.0, 80.0)));
        assert!(!shared.baseline_ready.load(Ordering::Relaxed));
        assert_eq!(shared.warmup_ms.load(Ordering::Relaxed), (live::WARMUP_S * 1000.0) as u32);
        // ...this wear's calibration, the history and calib.json kept (calib_state: calibrated, levels in use)
        assert_eq!(w.params.map(|p| p.wear), wear);
        assert_eq!(shared.calib_state.load(Ordering::Relaxed), 1 | 4);
        assert!(shared.calib_saved.load(Ordering::Relaxed) && shared.setup_done.load(Ordering::Relaxed));
        assert_eq!(w.calib.history.len(), 1);
        assert_eq!(std::fs::read_to_string(&path).unwrap(), saved);
        // A wear calibration afterwards seeds the levels at once, as before
        w.engines[0].seed(&wl);
        assert!(w.engines[0].baseline_ready() && !w.engines[0].relearning());
        // An eye without levels yet (just put on) warms up from the start, as on a new wear
        w.engines[1].reset();
        assert_eq!(w.relearn(), Ok(()));
        assert!(!w.engines[1].baseline_ready() && !w.engines[1].relearning());
        assert_eq!(shared.calib_state.load(Ordering::Relaxed), 1);
        drop(w);
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn an_old_calib_file_is_converted_once() {
        let dir = std::env::temp_dir().join(format!("eyecam-calib-convert-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("calib.json");
        // As builds before 2026-10-07 saved it: no "eyes", L = the right eye.
        let old = r#"{
  "version": 1,
  "setup_done": true,
  "wear": {"time": "w", "widen": "measured", "failed_eye": "R", "L": {"r_px": 61, "b_n": 0.7, "b_w": 0.9, "ap_n": 1.4, "ap_cl": 0.5, "pitch_n": -12, "lower_px": 230}, "R": {"r_px": 51, "b_n": 0.8, "b_w": 1, "ap_n": 1.5, "ap_cl": 0.6, "pitch_n": -14, "lower_px": 240}},
  "history": [
    {"time": "a", "L": {"step": 0.18, "gap": 0.8, "r_px": 60}, "R": {"step": 0.2, "gap": 0.9, "r_px": 50}}
  ]
}
"#;
        std::fs::write(&path, old).unwrap();
        let file = load_calib(&path);
        let w = file.wear.unwrap();
        assert_eq!((w[0].r_px, w[1].r_px, file.wear_failed_eye.as_str()), (51.0, 61.0, "L"));
        assert_eq!((file.history[0].1[0].r_px, file.history[0].1[1].r_px), (50.0, 60.0));
        // Saved with the marker: the same values from then on, never swapped again.
        let saved = std::fs::read_to_string(&path).unwrap();
        assert!(saved.contains(live::JSON_EYES_ANATOMICAL), "{saved}");
        // The file as it was, kept next to it first
        let backup = dir.join("calib.json.pre-anatomical");
        assert_eq!(pre_anatomical_path(&path), backup);
        assert_eq!(std::fs::read_to_string(&backup).unwrap(), old);
        let again = load_calib(&path);
        assert_eq!((again.wear, again.history.clone(), again.wear_failed_eye.as_str()), (file.wear, file.history.clone(), "L"));
        assert_eq!(std::fs::read_to_string(&path).unwrap(), saved);
        // A file written now is left as it is.
        let now = file.to_json();
        std::fs::write(&path, &now).unwrap();
        let before = std::fs::metadata(&path).unwrap().modified().unwrap();
        let read = load_calib(&path);
        assert_eq!((read.wear, read.wear_failed_eye.as_str()), (file.wear, "L"));
        assert_eq!(std::fs::read_to_string(&path).unwrap(), now);
        assert_eq!(std::fs::metadata(&path).unwrap().modified().unwrap(), before);
        // An older eyecam saved it again without the marker (its own L = the right eye): converted again, and the
        // first copy kept, not overwritten by this one.
        let older = old.replace("\"r_px\": 61", "\"r_px\": 62");
        std::fs::write(&path, &older).unwrap();
        let reread = load_calib(&path);
        assert_eq!(reread.wear.unwrap()[1].r_px, 62.0);
        assert_eq!(std::fs::read_to_string(&backup).unwrap(), old);
        // None at all: empty, nothing written.
        std::fs::remove_file(&path).unwrap();
        assert!(load_calib(&path).wear.is_none() && !path.exists());
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    #[ignore]
    fn worker_on_a_session() {
        let s = Session::open(Path::new(&std::env::var("EYECAM_SESSION").unwrap()), false).unwrap();
        let dir = std::env::temp_dir().join(format!("eyecam-worker-test-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let calib = dir.join("calib.json");
        let shared = Arc::new(Shared::default());
        let (tx, handle) = spawn(&dir, calib.clone(), None, shared.clone()).unwrap();
        tx.send(Msg::Buffers(true)).unwrap();
        // The protocol's first four steps (lead_in, close, normal, widen) as a wear calibration.
        tx.send(Msg::Collect(CollectKind::Wear)).unwrap();
        for (label, t0, sec) in s.cues.iter().take(4) {
            tx.send(Msg::Step { label: label.clone(), t0: *t0, seconds: *sec }).unwrap();
        }
        let end = s.cues[3].1 + s.cues[3].2;
        let mut img = vec![0u8; W * H];
        for k in 0..s.frames(0).min(s.frames(1)) {
            for e in 0..2 {
                let t = s.t_cam[e][k];
                if t > end {
                    continue;
                }
                s.read(e, k, &mut img).unwrap();
                if s.upside_down(e) {
                    // Back to the stored (upside-down) orientation, as the polling loop sends it.
                    let mut raw = vec![0u8; W * H];
                    for y in 0..H {
                        raw[y * W..(y + 1) * W].copy_from_slice(&img[(H - 1 - y) * W..(H - y) * W]);
                    }
                    img = raw;
                }
                let pitch = s.valve.nearest(t).map_or(f32::NAN, |j| s.valve.pitch[e][j] as f32);
                let open = s.valve.nearest(t).map_or(f32::NAN, |j| s.valve.open[e][j] as f32);
                let upside_down = s.upside_down(e);
                tx.send(Msg::Frame { eye: e, upside_down, t_cam_ns: (t * 1e9) as u64, pitch, open, data: img.clone() }).unwrap();
            }
        }
        let (rtx, rrx) = mpsc::channel();
        tx.send(Msg::Finish(rtx, Some(dir.clone()))).unwrap();
        let result = rrx.recv().unwrap();
        eprintln!("wear calibration: {result:?}; {} us/frame", shared.us_per_frame.load(Ordering::Relaxed));
        assert!(result.is_ok());
        assert_eq!(shared.calib_state.load(Ordering::Relaxed) & 5, 5, "calibrated and the baseline seeded");
        assert!(shared.calib_saved.load(Ordering::Relaxed));
        let result = std::fs::read_to_string(dir.join("calib_result.json")).unwrap();
        assert!(crate::json::parse(&result).is_ok() && result.contains("\"ok\": true"), "{result}");
        assert!(std::fs::read_to_string(dir.join("calib_samples.csv")).unwrap().lines().count() > 1000);
        // ...and status.json's last_calib, read back from it
        assert!(result.contains("\"failed_eye\": "), "{result}");
        let last = shared.last_calib.lock().unwrap().clone().expect("last_calib");
        assert!(last.ok && last.pupil_frames[0] > 0.0 && last.normal_frames[0] > 0.0, "{last:?}");
        let text = std::fs::read_to_string(&calib).unwrap();
        let saved = crate::live::CalibFile::parse(&text).unwrap();
        assert!(saved.wear.is_some() && saved.history.len() == 1);
        let bytes = std::fs::read(dir.join("live")).unwrap();
        assert_eq!(&bytes[0..4], b"EYCM");
        let frames_l = u64::from_le_bytes(bytes[56 + 16..56 + 24].try_into().unwrap());
        assert!(frames_l > 1000, "{frames_l}");
        tx.send(Msg::Shutdown).unwrap();
        handle.join().unwrap();
        let bytes = std::fs::read(dir.join("live")).unwrap();
        assert_eq!(u32::from_le_bytes(bytes[52..56].try_into().unwrap()), 0, "live cleared at exit");
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
