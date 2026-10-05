//! The live processing thread of `eyecam-rec --serve`: takes eye frames from the main (polling) thread, runs the
//! per-eye engines, publishes the shared memory, collects calibration samples and fits/stores calibrations.

use crate::live::{self, CalibFile, EyeEngine, Label, Params, Sample, UserParams};
use crate::liveshm::{self, LiveWriter};
use crate::vision::{H, W};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, mpsc};
use std::time::{Duration, Instant};

/// A new wear (HMD taken off and put back on) when an eye's frames stop for this long.
const NEW_WEAR_GAP: f64 = 10.0;
/// An eye's published values are marked invalid when its frames stop for this long.
const STALE_AFTER: f64 = 0.25;
/// status.json's pupil_l / pupil_r: the share of frames with the pupil over this many seconds.
const PUPIL_SHARE_S: f64 = 2.0;
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

    fn value(&self) -> f64 {
        if self.recent.len() < 90 {
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
    /// A raw frame of an anatomical eye (0 = L, 1 = R), 400x400 as stored (the right eye upside down).
    Frame { eye: usize, t_cam_ns: u64, pitch: f32, data: Vec<u8> },
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

    /// The eye's recent pupil share (0..1), NaN when not known.
    pub fn pupil_share(&self, eye: usize) -> f64 {
        f32::from_bits(self.pupil_share[eye].load(Ordering::Relaxed)) as f64
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

fn save_calib(path: &Path, file: &CalibFile) -> Result<(), String> {
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    }
    let tmp = path.with_extension("json.tmp");
    std::fs::write(&tmp, file.to_json()).map_err(|e| format!("{}: {e}", tmp.display()))?;
    std::fs::rename(&tmp, path).map_err(|e| format!("{}: {e}", path.display()))
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
}

impl Worker {
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

    /// The auto baseline's progress and whether a step history exists, for status.json.
    fn publish_baseline(&self) {
        let ready = self.engines.iter().all(EyeEngine::baseline_ready);
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

    fn frame(&mut self, eye: usize, t_cam_ns: u64, pitch: f32, data: &[u8]) {
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
        let img: &[u8] = if eye == 1 {
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
        let mut o = self.engines[eye].process(img, pitch as f64, t, &self.params[eye]);
        self.check_pupils(eye, t, &mut o);
        let share = self.pupil_seen[eye].push(t, o.f.ok_pupil);
        self.shared.pupil_share[eye].store((share as f32).to_bits(), Ordering::Relaxed);
        self.busy += started.elapsed();
        self.busy_n += 1;
        self.frames[eye] += 1;
        if self.collect.is_some() {
            let label = self.steps.iter().rev().find(|(_, t0, sec)| t >= t0 + live::STEP_DELAY && t < t0 + sec).map(|s| s.0);
            if let Some(l) = label {
                self.samples[eye].push(Sample::from_out(l, &o, t));
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

    /// Write calib_result.json and calib_samples.csv for a calibration attempt, and log the per-eye values.
    fn report(&self, dir: Option<&Path>, kind: CollectKind, samples: &[Vec<Sample>; 2], result: &Result<String, String>, params: Option<String>) {
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
        let Some(dir) = dir else { return };
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
            "{{\n  \"kind\": \"{kind_name}\",\n  \"time\": {},\n  \"ok\": {ok},\n  \"message\": {},\n  \"values\": {values},\n  \"params\": {}\n}}\n",
            crate::json::string(&stamp()),
            crate::json::string(message),
            params.unwrap_or_else(|| "null".into())
        );
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
        let offset = self.pupil_offset.value();
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
        self.report(dir.as_deref(), kind, &samples, &result, params);
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
                // one keeps its earlier ones (or gets provisional ones from the other eye and the history).
                let out = live::fit_wear_settled(&samples, fallback, self.calib.wear, hist);
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
                        self.pupil_offset.value()
                    );
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
    let (tx, rx) = mpsc::sync_channel::<Msg>(16);
    let mut calib_file = match std::fs::read_to_string(&calib) {
        Ok(text) => CalibFile::parse(&text).unwrap_or_else(|e| {
            eprintln!("eyecam-rec: ignoring {}: {e}", calib.display());
            CalibFile::default()
        }),
        Err(_) => CalibFile::default(),
    };
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
            let mut w = Worker {
                writer,
                shared: sh,
                calib_path: calib,
                calib: calib_file,
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
            };
            w.apply_calib();
            w.publish_state();
            loop {
                match rx.recv_timeout(Duration::from_millis(50)) {
                    Ok(Msg::Frame { eye, t_cam_ns, pitch, data }) => w.frame(eye, t_cam_ns, pitch, &data),
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
                            w.report(dir.as_deref(), kind, &samples, &Err("途中で止めた".into()), None);
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
                    Ok(Msg::Shutdown) | Err(mpsc::RecvTimeoutError::Disconnected) => break,
                    Err(mpsc::RecvTimeoutError::Timeout) => {}
                }
                w.housekeeping();
            }
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
                if e == 1 {
                    // Back to the stored (upside-down) orientation, as the polling loop sends it.
                    let mut raw = vec![0u8; W * H];
                    for y in 0..H {
                        raw[y * W..(y + 1) * W].copy_from_slice(&img[(H - 1 - y) * W..(H - y) * W]);
                    }
                    img = raw;
                }
                let pitch = s.valve.nearest(t).map_or(f32::NAN, |j| s.valve.pitch[e][j] as f32);
                tx.send(Msg::Frame { eye: e, t_cam_ns: (t * 1e9) as u64, pitch, data: img.clone() }).unwrap();
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
