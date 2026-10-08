//! The live per-eye engine: features (`feat`) -> causal estimates -> VRCFT-style outputs, plus calibration.
//!
//! Follows the choices of the cross-wear evaluation (analysis/xwear.py and analysis/mapping.py): features normalised by the iris radius R (a long-term median of the
//! limbus, no calibration), "gated" closed detection (box brightness > 1.3 x its long-term open median AND no
//! pupil for 3 frames), EyeWide from the skin line (by default 0 below 32.5% of the widen step above the baseline,
//! 0.5 at 60%, 1 at 87.5%, adjustable with the widen sensitivity; above 0.3 only after 100 ms above 0.5), EyeSquint from the pitch-corrected aperture, EyeLid on the VRCFT
//! scale (0 closed, 0.75 normal, 1 full widen), pupil
//! diameter in iris units and mm. Everything is causal; values are 5-frame causal medians of the per-frame
//! features with no further smoothing (frameeyeosc smooths). Wide and squint are 0 while closed and for 80 ms after.
//!
//! The cameras do not always run at 90 frames a second (Valve's eye tracker sets the rate: 72, 80, 90, 120, and as
//! low as 15 on some headsets), so every duration here is in seconds, turned into frames at the rate measured from
//! the frame times (`FrameRate`). The frame counts in the comments are those at 90 fps, where the results are the
//! same as when they were fixed counts.
//!
//! No per-wear calibration is needed: after the HMD goes on, the open-eye levels (skin line and aperture, in pixels)
//! are the mode of the first 30 s of usable frames (pupil seen, eye open, gaze within 15 degrees of the wearer's
//! typical pitch), and from then on they follow slowly (60 s time constant, frames with EyeWide < 0.2 only). The widen
//! step and the closed aperture come from the median of the user's past wear calibrations (in iris radii) times the
//! current iris radius, or population defaults when there is none. A wear calibration only sets the starting levels
//! (and adds its step to the history). EyeWide is 0 while warming up and for 0.3 s after the gaze drops by 8 degrees
//! (the upper lid lags behind a downward glance).

use crate::feat::{self, Extractor, Features, Pupil};
use crate::json::{self, Json};
use crate::vision::{self, Quad};
use std::collections::VecDeque;

/// The frame rate assumed until the frame times say otherwise (the rate the durations below were tuned at).
pub const FPS: f64 = 90.0;
/// Assumed iris radius in mm (HVID 11.8 mm); only scales pupil_mm.
pub const R_MM: f64 = 5.9;
/// Wide/squint stay 0 for this long after the last closed frame (6 frames at 90 fps; with the closed frame,
/// int(0.08 s * 90) = 7).
const HOLD_S: f64 = 6.0 / FPS;
/// A pupil counts as seen whole when at least this share of the 64 rays reached its edge ...
const PUPIL_MIN_VIS: f64 = 0.75;
/// ... and its roundness (b/a) is within this of the usual one.
const PUPIL_MAX_BA_DEV: f64 = 0.08;
/// Time after a blink before the pupil size is trusted again (27 frames at 90 fps).
const PUPIL_SETTLE_S: f64 = 0.3;
/// After this long without a pupil (10 frames at 90 fps; at least 2 frames), the search forgets where the pupil
/// was (its prior): a wrong blob caught once (during a blink, say) would otherwise keep the real pupil out of reach
/// for seconds.
pub const PRIOR_RESET_S: f64 = 10.0 / FPS;
/// Closed needs no pupil for this long (3 frames at 90 fps; at least 2 frames).
const CLOSED_NO_PUPIL_S: f64 = 3.0 / FPS;
/// The lid features are causal medians over this long (5 frames at 90 fps; an odd number, at least 3).
const MEDIAN_S: f64 = 5.0 / FPS;
/// The long-run medians (iris radius, the open eye's box level, pupil size and darkness, typical pitch) take a
/// value about this often a second: every 2nd frame at 90 fps, every frame at 45 fps or less.
const LONG_HZ: f64 = 45.0;
/// The default widen sensitivity (see `wide_curve`): 0.5 at 60% of the widen step. Calibration-free on the 5
/// protocol sessions and the 3-minute free-use session this was the balance: 0.5 at 70% (sensitivity 0) missed too
/// much of true widen (0.63 of widen frames), 0.5 at 50% (sensitivity 1) widened falsely twice a minute in free use.
pub const DEFAULT_WIDEN_SENSITIVITY: f64 = 0.5;

/// The EyeWide curve for a widen sensitivity in 0..1, as (start, width): EyeWide = (rise above the baseline - start x
/// step) / (width x step). 0 = dead zone 40% of the step and 0.5 at 70% (1 at 100%); 1 = dead zone 25% and 0.5 at
/// 50% (1 at 75%); the dead zone and the 0.5 point move linearly in between.
pub fn wide_curve(sensitivity: f64) -> (f64, f64) {
    let s = if sensitivity.is_finite() { sensitivity.clamp(0.0, 1.0) } else { DEFAULT_WIDEN_SENSITIVITY };
    let start = 0.4 - 0.15 * s;
    let half = 0.7 - 0.2 * s;
    (start, 2.0 * (half - start))
}
/// EyeWide shows above `WIDE_OFF` only once it has stayed above 0.5 for this long (9 frames at 90 fps, 100 ms; at
/// least 2 frames), and then until it falls below `WIDE_OFF` (hysteresis): short spikes of the skin line stay at
/// most `WIDE_OFF`.
const WIDE_ON_S: f64 = 9.0 / FPS;
const WIDE_OFF: f64 = 0.3;
/// Auto baseline: seconds of usable frames before EyeWide is enabled (about 35 s of wearing).
pub const WARMUP_S: f64 = 30.0;
/// Usable frames for the baseline: Valve pitch within this many degrees of the wearer's typical pitch.
const PITCH_GATE: f64 = 15.0;
/// After warm-up the baselines follow the open-eye levels with this time constant (s), using only frames with
/// EyeWide below `TRACK_MAX_WIDE` (and EyeSquint below it, for the aperture). Faster (20 s) followed the lid down
/// during downward glances and widened falsely more.
const TRACK_TAU_S: f64 = 60.0;
const TRACK_MAX_WIDE: f64 = 0.2;
/// For this long after a wear starts the iris radius is taken from the history (the running estimate is settling).
const R_PRIOR_S: f64 = 10.0;
/// When Valve's pitch falls by `GAZE_DROP_DEG` within `GAZE_DROP_WINDOW` s, EyeWide is 0 for `GAZE_DROP_HOLD` s: the
/// upper lid follows a downward glance late, and the skin line briefly sits high above the pupil.
const GAZE_DROP_DEG: f64 = 8.0;
const GAZE_DROP_WINDOW: f64 = 0.3;
const GAZE_DROP_HOLD: f64 = 0.3;
/// Wear calibrations kept in calib.json's history.
pub const HISTORY_MAX: usize = 100;
/// Reaction time trimmed from the start of every calibration step.
pub const STEP_DELAY: f64 = 0.8;

/// Population defaults (medians over the 5 recorded sessions, both eyes).
pub const DEFAULT_WIDEN_STEP: f64 = 0.19;
pub const DEFAULT_OPEN_GAP: f64 = 0.88; // normal aperture - closed aperture, in R
pub const DEFAULT_PITCH_N: f64 = -14.4; // Valve gaze pitch while looking "normal", degrees

/// Per-wear levels for one eye (feature units: iris radii; lower_px in image pixels).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct WearParams {
    pub r_px: f64,
    pub b_n: f64,
    pub b_w: f64,
    pub ap_n: f64,
    pub ap_cl: f64,
    pub pitch_n: f64,
    pub lower_px: f64,
}

/// Per-user shape parameters for one eye.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct UserParams {
    /// Aperture vs Valve pitch (degrees): a0 + a1 p + a2 p^2; only differences are used.
    pub pitch_c: [f64; 3],
    pub pitch_lo: f64,
    pub pitch_hi: f64,
    /// Squint aperture as a fraction of the way from closed to normal.
    pub f_sq: f64,
    pub pd_min: f64,
    pub pd_max: f64,
}

impl Default for UserParams {
    fn default() -> Self {
        Self { pitch_c: [0.0; 3], pitch_lo: -90.0, pitch_hi: 90.0, f_sq: 0.31, pd_min: 0.6, pd_max: 1.05 }
    }
}

impl UserParams {
    fn pitch_curve(&self, p: f64) -> f64 {
        let p = p.clamp(self.pitch_lo, self.pitch_hi);
        self.pitch_c[0] + self.pitch_c[1] * p + self.pitch_c[2] * p * p
    }
}

/// The user's typical per-wear sizes for one eye: medians over the stored wear calibrations.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct History {
    /// Widen step (skin line, widen - normal) and open gap (aperture, normal - closed), in iris radii.
    pub step: f64,
    pub gap: f64,
    /// Iris radius, px.
    pub r_px: f64,
}

/// Knobs for offline evaluation (`--replay`); the defaults are what live uses.
#[derive(Clone, Copy, Debug)]
pub struct Tune {
    /// EyeWide = (rise above baseline - start * step) / (width * step).
    pub wide_start: f64,
    pub wide_width: f64,
    /// Follow the baselines after warm-up.
    pub track: bool,
    /// Hold EyeWide at 0 after a downward glance.
    pub gaze_drop: bool,
}

impl Default for Tune {
    fn default() -> Self {
        let (wide_start, wide_width) = wide_curve(DEFAULT_WIDEN_SENSITIVITY);
        Self { wide_start, wide_width, track: true, gaze_drop: true }
    }
}

/// What the engine knows about the calibration, for one eye.
#[derive(Clone, Copy, Debug, Default)]
pub struct Params {
    /// Calibrated this wear (the engine was seeded with it; also needed for `calib user`).
    pub wear: Option<WearParams>,
    /// The user's history of wear calibrations (widen step, open gap, iris radius).
    pub hist: Option<History>,
    pub user: Option<UserParams>,
    /// The pupil range was measured (else UserParams' defaults).
    pub pupil_measured: bool,
    pub tune: Tune,
}

/// One frame's result for one eye.
#[derive(Clone, Copy, Debug)]
pub struct Out {
    pub f: Features,
    pub valid: bool,
    pub closed: bool,
    pub eye_lid: f64,
    pub eye_wide: f64,
    pub eye_squint: f64,
    pub pupil_ratio: f64,
    pub pupil_mm: f64,
    pub pupil_dilation: f64,
    pub confidence: f64,
    /// Normalised features (5-frame causal medians) and the R and pitch used.
    pub skin_up: f64,
    pub aperture: f64,
    pub lower: f64,
    pub box_f: f64,
    pub r: f64,
    pub pitch: f64,
    pub xmax: f64,
    /// The pupil search's left edge (`feat::search_x_min`).
    pub x_min: f64,
    /// The levels in use for this frame (calibrated, or the automatic baseline + stored step).
    pub b_n: f64,
    pub b_w: f64,
}

/// `seconds` in frames at `fps`: rounded, and at least `min`.
fn frames_in(seconds: f64, fps: f64, min: u32) -> u32 {
    ((seconds * fps).round() as u32).max(min)
}

/// The camera's frame rate from the frame times: the median of the last RATE_WINDOW frame intervals, rounded to
/// whole frames a second (the cameras run at whole rates). FPS until RATE_MIN intervals are known; gaps of half a
/// second or more (frames stopped) are not intervals.
#[derive(Clone, Debug)]
pub struct FrameRate {
    last_t: f64,
    dt: VecDeque<f64>,
    fps: f64,
}

const RATE_WINDOW: usize = 31;
const RATE_MIN: usize = 5;

impl Default for FrameRate {
    fn default() -> Self {
        Self { last_t: f64::NAN, dt: VecDeque::with_capacity(RATE_WINDOW), fps: FPS }
    }
}

impl FrameRate {
    /// Note a frame taken at `t` (s).
    pub fn push(&mut self, t: f64) {
        let d = t - self.last_t;
        if t.is_finite() {
            self.last_t = t;
        }
        if !(d > 0.0 && d < 0.5) {
            return;
        }
        if self.dt.len() == RATE_WINDOW {
            self.dt.pop_front();
        }
        self.dt.push_back(d);
        if self.dt.len() >= RATE_MIN {
            let mut v: Vec<f64> = self.dt.iter().copied().collect();
            self.fps = (1.0 / vision::median(&mut v)).round().max(1.0);
        }
    }

    /// Frames a second.
    pub fn fps(&self) -> f64 {
        self.fps
    }
}

/// A NaN-aware causal running median over the last `n` frames (`n` may change from frame to frame).
struct Win {
    v: VecDeque<f64>,
}

impl Win {
    fn new() -> Self {
        Self { v: VecDeque::with_capacity(8) }
    }

    fn push(&mut self, x: f64, n: usize) -> f64 {
        while self.v.len() >= n.max(1) {
            self.v.pop_front();
        }
        self.v.push_back(x);
        let mut f: Vec<f64> = self.v.iter().copied().filter(|x| x.is_finite()).collect();
        vision::median(&mut f)
    }
}

/// A long-run median over the last `span_s` seconds of samples, recomputed about once a second.
struct LongMedian {
    v: VecDeque<f64>,
    span_s: f64,
    since: usize,
    pub value: f64,
}

impl LongMedian {
    fn new(span_s: f64) -> Self {
        Self { v: VecDeque::new(), span_s, since: 0, value: f64::NAN }
    }

    /// Add a sample, samples coming `hz` times a second: it keeps the last `span_s * hz` (at 45 a second: 45 x
    /// span_s) and recomputes every `hz` samples.
    fn push(&mut self, x: f64, hz: f64) {
        if !x.is_finite() {
            return;
        }
        let cap = ((self.span_s * hz).round() as usize).max(1);
        let every = (hz.round() as usize).max(1);
        while self.v.len() >= cap {
            self.v.pop_front();
        }
        self.v.push_back(x);
        self.since += 1;
        // Recompute often while warming up, then once per `every` samples.
        if self.since >= every || self.v.len() < 64 {
            self.since = 0;
            let mut s: Vec<f64> = self.v.iter().copied().collect();
            self.value = vision::median(&mut s);
        }
    }

    fn len(&self) -> usize {
        self.v.len()
    }

}

/// The engine for one eye. Feed it upright frames in order; `reset` when a new wear starts.
pub struct EyeEngine {
    ex: Extractor,
    prev: Option<(f64, f64)>,
    prev_p: Option<Pupil>,
    frame_no: u64,
    rate: FrameRate,
    /// The lens edge's mean image: summed column means, how many frames, and the last one's number.
    cm_sum: Vec<f64>,
    cm_n: u32,
    cm_last: u64,
    xmax: f64,
    x_min: f64,
    r_est: LongMedian,
    skin: Win,
    ap: Win,
    lo: Win,
    bx: Win,
    pd: Win,
    nopupil: u32,
    box_open: LongMedian,
    /// The open eye's pupil semi-axis (px) and dark level, for telling a real pupil from a crease on a closed lid.
    a_open: LongMedian,
    inner_open: LongMedian,
    /// The open eye's pupil roundness (b/a), for telling a fully visible pupil from one a lid cuts into.
    ba_open: LongMedian,
    /// Frames since the eye was last closed.
    since_closed: u32,
    last_wide: f64,
    wide_gate: WideGate,
    last_sq: f64,
    last_open: f64,
    last_pd: f64,
    base: AutoBase,
    /// Time of this wear's first frame.
    t_first: f64,
    /// Recent Valve pitch (time, degrees) for the downward-glance hold, and the hold's end.
    pitch_recent: VecDeque<(f64, f64)>,
    gaze_hold_until: f64,
    /// Kept for the shared memory / status.json; the baselines follow the wearer now, so it stays false.
    pub recalib_suggested: bool,
}

/// The per-wear open-eye levels (pixels): learnt from the first `WARMUP_S` of usable frames, then followed slowly.
struct AutoBase {
    /// Warm-up values of the skin line height above the pupil and of the aperture, px.
    skin: Vec<f64>,
    ap: Vec<f64>,
    /// Seconds of usable frames so far, and since the levels were last worked out.
    seconds: f64,
    since_s: f64,
    b: f64,
    ap_n: f64,
    ready: bool,
    /// The wearer's typical Valve pitch: median over the last minute of frames with the pupil seen.
    pitch_typ: LongMedian,
}

impl Default for AutoBase {
    fn default() -> Self {
        Self {
            skin: Vec::new(),
            ap: Vec::new(),
            seconds: 0.0,
            since_s: 0.0,
            b: f64::NAN,
            ap_n: f64::NAN,
            ready: false,
            pitch_typ: LongMedian::new(60.0),
        }
    }
}

/// The densest value of `v`: a histogram with bins of `bw` between its 1st and 99th percentiles, smoothed over 5
/// bins. The open eye's level is the mode; glances, expressions and blinks only add tails.
fn mode_est(v: &[f64], bw: f64) -> f64 {
    let mut s: Vec<f64> = v.iter().copied().filter(|x| x.is_finite()).collect();
    if s.len() < 20 || bw.is_nan() || bw <= 0.0 {
        return f64::NAN;
    }
    let lo = vision::percentile(&mut s, 1.0);
    let hi = vision::percentile(&mut s, 99.0);
    if hi - lo < 3.0 * bw {
        return vision::median(&mut s);
    }
    let n = ((hi - lo) / bw).floor() as usize + 1;
    let mut h = vec![0.0; n];
    for &x in &s {
        if x >= lo && x <= hi {
            h[(((x - lo) / bw) as usize).min(n - 1)] += 1.0;
        }
    }
    let mut best = (0usize, f64::NEG_INFINITY);
    for k in 0..n {
        let v: f64 = h[k.saturating_sub(2)..=(k + 2).min(n - 1)].iter().sum::<f64>() / 5.0;
        if v > best.1 {
            best = (k, v);
        }
    }
    lo + (best.0 as f64 + 0.5) * bw
}

impl AutoBase {
    fn warmup_remaining_s(&self) -> f64 {
        if self.ready { 0.0 } else { (WARMUP_S - self.seconds).clamp(0.0, WARMUP_S) }
    }

    /// Add one usable frame's levels during warm-up (`bw`: histogram bin, px; `dt`: the frame interval, s). The
    /// levels are worked out every half second of usable frames (45 frames at 90 fps) and once WARMUP_S is reached
    /// (2700 frames at 90 fps; both within half a frame).
    fn warm(&mut self, skin: f64, ap: f64, bw: f64, dt: f64) {
        if self.ready {
            return;
        }
        self.skin.push(skin);
        self.ap.push(ap);
        self.seconds += dt;
        self.since_s += dt;
        let done = self.seconds >= WARMUP_S - 0.5 * dt;
        if (self.since_s >= 0.5 - 0.5 * dt && self.skin.len() >= 20) || done {
            self.since_s = 0.0;
            self.b = mode_est(&self.skin, bw);
            let a = mode_est(&self.ap, bw);
            if a.is_finite() {
                self.ap_n = a;
            }
        }
        if done && self.b.is_finite() {
            self.ready = true;
            self.skin = Vec::new();
            self.ap = Vec::new();
        }
    }

    fn seed(&mut self, b: f64, ap_n: f64) {
        if b.is_finite() && ap_n.is_finite() {
            (self.b, self.ap_n, self.ready) = (b, ap_n, true);
            self.skin = Vec::new();
            self.ap = Vec::new();
        }
    }
}

impl Default for EyeEngine {
    fn default() -> Self {
        Self {
            ex: Extractor::default(),
            prev: None,
            prev_p: None,
            frame_no: 0,
            rate: FrameRate::default(),
            cm_sum: vec![0.0; vision::W],
            cm_n: 0,
            cm_last: 0,
            xmax: 346.0,
            x_min: feat::X_MIN,
            // 30 s for R, 60 s for the open box level, 20 s for the open pupil.
            r_est: LongMedian::new(30.0),
            skin: Win::new(),
            ap: Win::new(),
            lo: Win::new(),
            bx: Win::new(),
            pd: Win::new(),
            nopupil: 0,
            box_open: LongMedian::new(60.0),
            a_open: LongMedian::new(20.0),
            inner_open: LongMedian::new(20.0),
            ba_open: LongMedian::new(20.0),
            since_closed: u32::MAX / 2,
            last_wide: 0.0,
            wide_gate: WideGate::default(),
            last_sq: 0.0,
            last_open: 1.0,
            last_pd: f64::NAN,
            base: AutoBase::default(),
            t_first: f64::NAN,
            pitch_recent: VecDeque::new(),
            gaze_hold_until: f64::NEG_INFINITY,
            recalib_suggested: false,
        }
    }
}

fn clamp01(v: f64) -> f64 {
    v.clamp(0.0, 1.0)
}

/// Hysteresis on EyeWide (see `WIDE_ON_S`).
#[derive(Clone, Copy, Debug, Default)]
struct WideGate {
    on: bool,
    run: u32,
}

impl WideGate {
    /// The value to report for this frame's ungated EyeWide `w`; `on_frames` is WIDE_ON_S in frames.
    fn step(&mut self, w: f64, on_frames: u32) -> f64 {
        self.run = if w > 0.5 { self.run + 1 } else { 0 };
        if w < WIDE_OFF {
            self.on = false;
        } else if self.run >= on_frames {
            self.on = true;
        }
        if self.on { w } else { w.min(WIDE_OFF) }
    }
}

impl EyeEngine {
    /// Forget everything learnt about the current wear (lens edge, R, open levels).
    pub fn reset(&mut self) {
        *self = Self { ex: std::mem::take(&mut self.ex), ..Self::default() };
    }

    /// Forget where the pupil was (the search prior, the last pupil the lids are measured around, the run of frames
    /// without one), so that a calibration starts from a clean search. What belongs to the wear stays: lens edge,
    /// search window, iris radius, open-eye levels, and the open eye's pupil size, darkness and iris-box brightness
    /// (the closed-lid crease check). Replays of the 14 recorded calibrations after 30 s of another session gave the
    /// same pupil counts with those kept or cleared, and fewer "pupils" on closed eyes with them kept.
    pub fn reset_tracking(&mut self) {
        self.prev = None;
        self.prev_p = None;
        self.nopupil = 0;
    }

    /// The lens edge and iris radius the engine currently uses.
    pub fn geometry(&self) -> (f64, f64) {
        (self.xmax, self.r_est.value)
    }

    /// The camera's frame rate as measured from the frame times (FPS until a few frames are in).
    pub fn fps(&self) -> f64 {
        self.rate.fps()
    }

    /// The open-eye levels are known (warm-up over, or seeded by a calibration).
    pub fn baseline_ready(&self) -> bool {
        self.base.ready
    }

    /// Seconds of usable frames still needed before EyeWide is enabled (0 when ready).
    pub fn warmup_remaining_s(&self) -> f64 {
        self.base.warmup_remaining_s()
    }

    /// The open-eye levels (skin line height, aperture; px) once ready.
    pub fn baseline_px(&self) -> Option<(f64, f64)> {
        self.base.ready.then_some((self.base.b, self.base.ap_n))
    }

    /// Start from these open-eye levels (px) instead of warming up; they are followed from there.
    pub fn seed_px(&mut self, b: f64, ap_n: f64) {
        self.base.seed(b, ap_n);
    }

    /// Start from a wear calibration's levels.
    pub fn seed(&mut self, w: &WearParams) {
        self.base.seed(w.b_n * w.r_px, w.ap_n * w.r_px);
    }

    /// Process one upright frame. `pitch`: Valve gaze pitch of this eye (degrees) or NaN; `t`: frame time (s).
    pub fn process(&mut self, img: &[u8], pitch: f64, t: f64, params: &Params) -> Out {
        self.frame_no += 1;
        self.rate.push(t);
        let fps = self.rate.fps();
        // The long-run medians take every `stride`-th frame, `long_hz` a second.
        let stride = ((fps / LONG_HZ).round() as u64).max(1);
        let long_hz = fps / stride as f64;
        let sampled = self.frame_no.is_multiple_of(stride);
        // Lens edge: the first frame, then the mean of one frame every 0.5 s for the first 20 s.
        let cm_due = self.cm_n < 40 && self.frame_no - self.cm_last >= frames_in(0.5, fps, 1) as u64;
        if cm_due {
            self.cm_last = self.frame_no;
        }
        if self.frame_no == 1 || cm_due {
            for (a, b) in self.cm_sum.iter_mut().zip(feat::column_means(img)) {
                *a += b;
            }
            self.cm_n += 1;
            let m: Vec<f64> = self.cm_sum.iter().map(|v| v / self.cm_n as f64).collect();
            self.xmax = feat::occluder_x(&m);
            self.x_min = feat::search_x_min(&m);
        }
        self.ex.search = feat::Search::live(self.x_min);
        let r_ready = self.r_est.len() >= 10;
        let r_ref = r_ready.then_some(self.r_est.value);
        let no_prior = self.prev.is_none();
        let (mut f, mut p) = self.ex.extract(img, self.xmax, self.prev, r_ref, self.prev_p.as_ref());
        // A "pupil" found while the iris box is as bright as lid skin must look like the open eye's pupil (as dark, mostly
        // visible, 0.45-1.3 x its size); otherwise it is a dark crease or lash line on the closed lid. Bright light also
        // lights the box this much, and the pupil then shrinks to 0.55-0.65 x its usual size but stays as dark and whole
        // (creases on the closed lid: interior 24-28 levels brighter, under half visible, 0.23-0.42 x the size).
        // A pupil found without the prior (after it was dropped, or in the retry away from it) is checked the same way
        // whatever the box: the box is then measured around the blob itself, and a lash or lid line caught while the
        // eye is closing has dark lashes in its box (replays: the eye counted as open through a whole close step).
        let box_bright = self.box_open.len() >= 30 && f.box_mean > 1.3 * self.box_open.value;
        let unanchored = no_prior || f.diag.retried;
        if let Some(pp) = p
            && (box_bright || unanchored)
            && self.a_open.len() >= 30
            && (!(0.45..=1.3).contains(&(pp.a / self.a_open.value)) || pp.inner > self.inner_open.value + 15.0 || pp.vis < 0.5)
        {
            p = None;
            f.ok_pupil = false;
            f.diag.gated = true;
            (f.pupil_cx, f.pupil_cy, f.pupil_a, f.pupil_b, f.pupil_vis) = (f64::NAN, f64::NAN, f64::NAN, f64::NAN, f64::NAN);
            (f.iris_r, f.iris_n) = (f64::NAN, 0);
        }
        if let Some(pp) = p {
            self.prev = Some((pp.cx, pp.cy));
            self.prev_p = Some(pp);
            if !box_bright && sampled {
                self.a_open.push(pp.a, long_hz);
                self.inner_open.push(pp.inner, long_hz);
                if pp.vis >= PUPIL_MIN_VIS {
                    self.ba_open.push(pp.b / pp.a, long_hz);
                }
            }
        }
        if f.ok_pupil && f.iris_r.is_finite() && f.iris_n >= 4 && sampled {
            self.r_est.push(f.iris_r, long_hz);
        }
        if !self.t_first.is_finite() {
            self.t_first = t;
        }
        // Iris radius: the running estimate, but the history's for the first seconds of a wear.
        let r = match params.hist {
            Some(h) if h.r_px.is_finite() && (t - self.t_first < R_PRIOR_S || !r_ready) => h.r_px,
            _ if r_ready => self.r_est.value,
            _ if f.iris_r.is_finite() => f.iris_r,
            _ => 2.0 * f.pupil_a,
        };
        // Lid geometry in pixels (causal medians over MEDIAN_S, 5 frames at 90 fps); the outputs below are in iris
        // radii.
        let med_n = (frames_in(MEDIAN_S, fps, 3) | 1) as usize;
        let skin_px = self.skin.push(f.pupil_cy - f.upper_skin_y, med_n);
        let ap_px = self.ap.push(f.lower_y - f.upper_y, med_n);
        let lo_px = self.lo.push(f.lower_y - f.pupil_cy, med_n);
        let (skin, ap, lo) = (skin_px / r, ap_px / r, lo_px / r);
        let box_f = self.bx.push(f.box_mean, med_n);
        // The pupil's size is only taken from a pupil seen whole: most rays reach its edge, its roundness is the
        // usual one for this camera angle (a lid cutting into it flattens the fit), and not while the lids are
        // still opening after a blink (300 ms). Otherwise the last good value is kept.
        let roundness_ok = self.ba_open.len() < 30 || (f.pupil_b / f.pupil_a - self.ba_open.value).abs() <= PUPIL_MAX_BA_DEV;
        let settled = self.since_closed > frames_in(PUPIL_SETTLE_S, fps, 1);
        let pupil_whole = f.ok_pupil && f.pupil_vis >= PUPIL_MIN_VIS && roundness_ok && settled;
        let pd = self.pd.push(if pupil_whole { 2.0 * f.pupil_a / r } else { f64::NAN }, med_n);
        self.nopupil = if f.ok_pupil { 0 } else { self.nopupil + 1 };
        if self.nopupil >= frames_in(PRIOR_RESET_S, fps, 2) && self.prev.is_some() {
            // prev_p stays: the lids and the box are still measured where the pupil last was.
            self.prev = None;
            f.diag.prior_reset = true;
        }
        if f.ok_pupil && sampled {
            self.box_open.push(box_f, long_hz);
        }

        // Closed: lid skin covers the box (brighter than the open-eye level) and no pupil for CLOSED_NO_PUPIL_S (3
        // frames at 90 fps).
        let no_pupil_long = self.nopupil >= frames_in(CLOSED_NO_PUPIL_S, fps, 2);
        let closed = self.box_open.len() >= 30 && box_f > 1.3 * self.box_open.value && no_pupil_long;
        self.since_closed = if closed { 0 } else { self.since_closed.saturating_add(1) };
        // Wide and squint are 0 on a closed frame and for HOLD_S after it.
        let hold = self.since_closed <= frames_in(HOLD_S, fps, 1);

        // Downward glance: the pitch fell by GAZE_DROP_DEG within the last GAZE_DROP_WINDOW.
        if pitch.is_finite() {
            self.pitch_recent.push_back((t, pitch));
            while self.pitch_recent.front().is_some_and(|x| t - x.0 > GAZE_DROP_WINDOW) {
                self.pitch_recent.pop_front();
            }
            let top = self.pitch_recent.iter().map(|x| x.1).fold(f64::NEG_INFINITY, f64::max);
            if params.tune.gaze_drop && top - pitch > GAZE_DROP_DEG {
                self.gaze_hold_until = t + GAZE_DROP_HOLD;
            }
        }
        let gaze_hold = t < self.gaze_hold_until;

        // Levels: the auto baseline (px) and the history's step and gap (iris radii) at the current radius.
        let user = params.user.unwrap_or_default();
        let (step_r, gap_r, cal_q) = params.hist.map_or((DEFAULT_WIDEN_STEP, DEFAULT_OPEN_GAP, 0.7), |h| (h.step, h.gap, 1.0));
        let (step_px, gap_px) = (step_r * r, gap_r * r);
        let open_seen = f.ok_pupil && !closed && skin_px.is_finite();
        if open_seen && pitch.is_finite() && sampled {
            self.base.pitch_typ.push(pitch, long_hz);
        }
        let p_typ = self.base.pitch_typ.value;
        let usable = open_seen && (!pitch.is_finite() || (p_typ.is_finite() && (pitch - p_typ).abs() < PITCH_GATE));
        if usable && !self.base.ready {
            self.base.warm(skin_px, ap_px, 0.01 * r, 1.0 / fps);
        } else if usable && params.tune.track && step_px > 0.0 {
            // One frame's share of the time constant.
            let k = 1.0 / (TRACK_TAU_S * fps);
            if self.last_wide < TRACK_MAX_WIDE {
                self.base.b += (skin_px - self.base.b).clamp(-0.5 * step_px, 0.5 * step_px) * k;
            }
            if self.last_wide < TRACK_MAX_WIDE && self.last_sq < TRACK_MAX_WIDE && ap_px.is_finite() && self.base.ap_n.is_finite() {
                self.base.ap_n += (ap_px - self.base.ap_n).clamp(-0.25 * gap_px, 0.25 * gap_px) * k;
            }
        }
        let ready = self.base.ready;
        let (b_px, ap_n_px) = (self.base.b, self.base.ap_n);

        let tune = params.tune;
        let wide = if hold || !ready || gaze_hold {
            0.0
        } else if skin_px.is_finite() && step_px > 0.0 {
            clamp01((skin_px - (b_px + tune.wide_start * step_px)) / (tune.wide_width * step_px))
        } else {
            self.last_wide
        };
        let wide = if wide.is_finite() { wide } else { 0.0 };
        self.last_wide = wide;
        let wide = self.wide_gate.step(wide, frames_in(WIDE_ON_S, fps, 2));

        // Squint and openness from the pitch-corrected aperture (the user's curve is in iris radii).
        let p0 = if p_typ.is_finite() { p_typ } else { DEFAULT_PITCH_N };
        let apc = if pitch.is_finite() { ap_px - (user.pitch_curve(pitch) - user.pitch_curve(p0)) * r } else { ap_px };
        let ap_cl_px = ap_n_px - gap_px;
        let ap_sq = ap_cl_px + user.f_sq * (ap_n_px - ap_cl_px);
        // Without a user calibration (squint depth, pitch curve) the aperture alone is not trusted: no squint, and the
        // lid only closes when the eye is closed. Nothing either before the open level is known.
        let user_cal = params.user.is_some() && ready;
        let squint = if hold || !user_cal {
            0.0
        } else if apc.is_finite() && ap_n_px > ap_sq {
            clamp01((ap_n_px - apc) / (ap_n_px - ap_sq))
        } else if !f.ok_pupil {
            // Lids not measured and no pupil, but not closed: lashes over the pupil, as in a squint.
            1.0
        } else {
            self.last_sq
        };
        let squint = if squint.is_finite() { squint } else { 0.0 };
        self.last_sq = squint;

        let open = if !user_cal {
            1.0
        } else if apc.is_finite() && ap_n_px > ap_cl_px {
            clamp01((apc - ap_cl_px) / (ap_n_px - ap_cl_px))
        } else {
            self.last_open
        };
        let open = if open.is_finite() { open } else { 1.0 };
        self.last_open = open;
        let eye_lid = if closed { 0.0 } else { 0.75 * open + 0.25 * wide };

        // The pupil keeps its last whole-pupil value while closed, just after reopening, and while partly hidden.
        if pupil_whole && pd.is_finite() && !hold && !closed {
            self.last_pd = pd;
        }
        let pupil_ratio = self.last_pd;
        let pupil_dilation = clamp01((pupil_ratio - user.pd_min) / (user.pd_max - user.pd_min));
        let cal_q = if ready { cal_q } else { 0.3 };
        let (b_n, b_w) = if ready { (b_px / r, (b_px + step_px) / r) } else { (f64::NAN, f64::NAN) };

        let lids = f.upper_skin_y.is_finite() as u8 as f64 * 0.25 + f.lower_y.is_finite() as u8 as f64 * 0.25;
        let seen = if f.ok_pupil { 0.5 * f.pupil_vis } else if closed { 0.5 } else { 0.0 };
        let confidence = clamp01((seen + lids) * cal_q * if r_ready { 1.0 } else { 0.5 });
        let valid = r.is_finite() && (f.ok_pupil || closed || f.upper_skin_y.is_finite());
        Out {
            f,
            valid,
            closed,
            eye_lid,
            eye_wide: wide,
            eye_squint: squint,
            pupil_ratio,
            pupil_mm: pupil_ratio * R_MM,
            pupil_dilation,
            confidence,
            skin_up: skin,
            aperture: ap,
            lower: lo,
            box_f,
            r,
            pitch,
            xmax: self.xmax,
            x_min: self.x_min,
            b_n,
            b_w,
        }
    }
}

// ------------------------------------------------------------------------------------------------- calibration

/// One labelled frame collected during a calibration (or a recording with bright/dark steps).
#[derive(Clone, Copy, Debug)]
pub struct Sample {
    pub label: Label,
    pub ok: bool,
    /// Normalised by `r` (the radius in use for that frame).
    pub skin: f64,
    pub ap: f64,
    pub pd: f64,
    pub pitch: f64,
    pub lower_px: f64,
    pub r: f64,
    /// This frame's own iris radius fit (NaN unless the pupil and 4+ limbus points were found).
    pub iris_r: f64,
    /// Frame time (camera clock, s).
    pub t: f64,
    /// What the pupil search saw.
    pub seen: Seen,
}

/// Why a frame has no pupil.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Miss {
    /// It has one.
    None,
    /// No dark blob passed the size and shape tests (`Seen::rejected` says what the dropped ones failed).
    NoBlob,
    /// Blobs were found, but none gave a pupil's edge all round.
    Refine,
    /// A pupil was found while the iris box was as bright as lid skin and it did not look like the open pupil.
    Gated,
}

impl Miss {
    pub fn name(self) -> &'static str {
        match self {
            Miss::None => "",
            Miss::NoBlob => "no_blob",
            Miss::Refine => "refine",
            Miss::Gated => "gated",
        }
    }
}

/// What the pupil search saw in a calibration frame (calib_result.json's diag and calib_samples.csv).
#[derive(Clone, Copy, Debug)]
pub struct Seen {
    /// Pupil centre, px (upright image; NaN without one).
    pub cx: f64,
    pub cy: f64,
    /// The search window's left edge and the lens edge.
    pub x_min: f64,
    pub xmax: f64,
    /// The search window's median minus its dark level.
    pub contrast: f64,
    pub miss: Miss,
    /// feat::REJ_* bits of the blobs that were dropped.
    pub rejected: u8,
    /// The pupil came from a blob cut by the window's left edge.
    pub left_edge: bool,
    /// The search ran a second time without its prior.
    pub retried: bool,
    /// The engine forgot its prior on this frame.
    pub prior_reset: bool,
}

impl Default for Seen {
    fn default() -> Self {
        let n = f64::NAN;
        Self { cx: n, cy: n, x_min: n, xmax: n, contrast: n, miss: Miss::None, rejected: 0, left_edge: false, retried: false, prior_reset: false }
    }
}

impl Seen {
    pub fn of(o: &Out) -> Self {
        let d = &o.f.diag;
        let miss = if o.f.ok_pupil {
            Miss::None
        } else if d.gated {
            Miss::Gated
        } else if d.candidates == 0 {
            Miss::NoBlob
        } else {
            Miss::Refine
        };
        Self {
            cx: o.f.pupil_cx,
            cy: o.f.pupil_cy,
            x_min: o.x_min,
            xmax: o.xmax,
            contrast: d.p50 - d.lo,
            miss,
            rejected: d.rejected,
            left_edge: d.left_edge,
            retried: d.retried,
            prior_reset: d.prior_reset,
        }
    }
}

impl Sample {
    /// The same sample in units of radius `r`.
    fn rescaled(&self, r: f64) -> Sample {
        let k = self.r / r;
        Sample { skin: self.skin * k, ap: self.ap * k, pd: self.pd * k, r, ..*self }
    }
}

/// The radius a calibration uses: the median per-frame iris fit over the given labels, else the radius in use.
fn calib_radius(s: &[Sample], labels: &[Label]) -> f64 {
    let (r, n) = med_of(s, |x| (labels.contains(&x.label) && x.ok).then_some(x.iris_r));
    if n >= 20 { r } else { med_of(s, |x| Some(x.r)).0 }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Label {
    LeadIn,
    Normal,
    Widen,
    Close,
    Squint,
    LookUp,
    LookDown,
    Bright,
    Dark,
    Other,
}

impl Label {
    pub fn parse(s: &str) -> Label {
        match s {
            "lead_in" => Label::LeadIn,
            "normal" => Label::Normal,
            "widen" => Label::Widen,
            "close" => Label::Close,
            "squint" => Label::Squint,
            "look_up" => Label::LookUp,
            "look_down" => Label::LookDown,
            "bright" => Label::Bright,
            "dark" => Label::Dark,
            _ => Label::Other,
        }
    }
}

impl Sample {
    pub fn from_out(label: Label, o: &Out, t: f64) -> Self {
        Sample {
            label,
            ok: o.f.ok_pupil,
            skin: o.skin_up,
            ap: o.aperture,
            pd: if o.f.ok_pupil { 2.0 * o.f.pupil_a / o.r } else { f64::NAN },
            pitch: o.pitch,
            lower_px: o.f.lower_y,
            r: o.r,
            iris_r: if o.f.ok_pupil && o.f.iris_n >= 4 { o.f.iris_r } else { f64::NAN },
            t,
            seen: Seen::of(o),
        }
    }
}

fn med_of(s: &[Sample], pick: impl Fn(&Sample) -> Option<f64>) -> (f64, usize) {
    let mut v: Vec<f64> = s.iter().filter_map(pick).filter(|x| x.is_finite()).collect();
    let n = v.len();
    (vision::median(&mut v), n)
}

/// Calibration problems of one eye: the text that follows "左目" / "右目" / "両目".
pub const MSG_PUPIL: &str = "の瞳がうまく見えなかった（HMD のかぶり方を直して、もう一度）";
pub const MSG_LID_LINE: &str = "の上まぶたの線が見つからなかった（HMD のかぶり方を直して、もう一度）";
pub const MSG_LIDS: &str = "のまぶたの線が見つからなかった（HMD のかぶり方を直して、もう一度）";
pub const MSG_CLOSE: &str = "を閉じたのが検出できなかった（もう一度、しっかり閉じてね）";
pub const MSG_SQUINT_MISSING: &str = "の細めが測れなかった（もう一度）";
pub const MSG_SQUINT_SHALLOW: &str = "の細めが浅かった（もう一度、しっかり細めてね）";
pub const MSG_LOOK_DOWN: &str = ": 下を見ても目の開きが変わっていない（もう一度、しっかり下を見てね）";
pub const MSG_LOWER_UP: &str = ": 下を見たら下まぶたが上に動いた（検出の失敗かも。もう一度）";
/// Valve's gaze was missing (not one eye's problem).
pub const MSG_NO_GAZE: &str = "視線の向き（Valve）が取れなかった。frameeyeosc が動いているか確かめて、もう一度";
/// A wear calibration that went through without the widen on an eye.
pub const NOTE_WIDEN: &str = "見開きは取れなかったので、いつもの幅を使うよ";

/// One eye's calibration problem: its text (MSG_*) and details, the eye's own value and what is needed (either may
/// be empty).
#[derive(Clone, Debug, PartialEq)]
pub struct EyeIssue {
    pub text: &'static str,
    pub value: String,
    pub need: String,
}

impl EyeIssue {
    fn new(text: &'static str, value: String, need: &str) -> Self {
        Self { text, value, need: need.to_string() }
    }

    /// "[value、need]", "[value]" or "".
    fn details(&self) -> String {
        bracket(&self.value, &self.need)
    }

    /// The message for one eye (0 = left).
    pub fn message(&self, eye: usize) -> String {
        format!("{}{}{}", ["左目", "右目"][eye], self.text, self.details())
    }
}

fn bracket(value: &str, need: &str) -> String {
    match (value.is_empty(), need.is_empty()) {
        (true, true) => String::new(),
        (false, true) => format!("[{value}]"),
        (true, false) => format!("[{need}]"),
        (false, false) => format!("[{value}、{need}]"),
    }
}

/// The message for the eyes' problems (at most one each): "両目…[左 a・右 b、need]" when both have the same one, else
/// one message per eye joined with "。" (left first).
pub fn eyes_message(issues: &[Option<EyeIssue>; 2]) -> String {
    match issues {
        [Some(l), Some(r)] if l.text == r.text && l.need == r.need => {
            let value = if l.value.is_empty() { String::new() } else { format!("左 {}・右 {}", l.value, r.value) };
            format!("両目{}{}", l.text, bracket(&value, &l.need))
        }
        _ => issues.iter().enumerate().filter_map(|(e, i)| Some(i.as_ref()?.message(e))).collect::<Vec<_>>().join("。"),
    }
}

/// Why one eye's wear calibration did not go through.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum WearFail {
    /// Too few normal frames with the pupil: (frames with it, normal frames, frames needed).
    Pupil { seen: usize, frames: usize, need: usize },
    /// The pupil was seen, but too few of those frames had the upper lid's skin line: (frames with both, normal
    /// frames, frames needed).
    LidLine { seen: usize, frames: usize, need: usize },
    /// The closed eye was not caught.
    Close,
}

impl WearFail {
    pub fn issue(&self) -> EyeIssue {
        match *self {
            WearFail::Pupil { seen, frames, need } => EyeIssue::new(MSG_PUPIL, format!("{seen}/{frames}"), &format!("{need} 必要")),
            WearFail::LidLine { seen, frames, need } => {
                EyeIssue::new(MSG_LID_LINE, format!("{seen}/{frames}"), &format!("{need} 必要"))
            }
            WearFail::Close => EyeIssue::new(MSG_CLOSE, String::new(), ""),
        }
    }

    /// What went wrong, for the note in a calibration that went through with the other eye.
    fn reason(&self) -> &'static str {
        match self {
            WearFail::Pupil { .. } => "瞳がうまく見えなかった",
            WearFail::LidLine { .. } => "上まぶたの線が見つからなかった",
            WearFail::Close => "閉じたのが検出できなかった",
        }
    }
}

/// One eye's wear calibration: its levels and whether its widen was measured (else it has `fallback_step`), or why
/// it failed.
pub type EyeWear = Result<(WearParams, bool), WearFail>;

/// Levels from normal / widen / close frames, each eye on its own. An eye needs the pupil (and the upper lid's skin
/// line) in NEED_NORMAL of its normal frames and its closed level; an eye whose widen was not caught gets
/// `fallback_step` (iris radii) as its widen step instead of failing.
pub fn fit_wear_eyes(samples: &[Vec<Sample>; 2], fallback_step: [f64; 2]) -> [EyeWear; 2] {
    [0, 1].map(|e| {
        let s = &samples[e];
        let need = WearNeeds::of(s);
        // One radius for the whole calibration (as the prototype's block calibration does): the running estimate
        // may still be settling while the steps are recorded.
        let r_px = calib_radius(s, &[Label::Normal, Label::Widen]);
        let s: Vec<Sample> = s.iter().map(|x| x.rescaled(r_px)).collect();
        let s = &s;
        let is = |l: Label| move |x: &Sample| x.label == l;
        let normal: Vec<Sample> = s.iter().copied().filter(is(Label::Normal)).collect();
        let widen: Vec<Sample> = s.iter().copied().filter(is(Label::Widen)).collect();
        let close: Vec<Sample> = s.iter().copied().filter(is(Label::Close)).collect();
        let n_pupil = normal.iter().filter(|x| x.ok).count();
        let (b_n, nn) = med_of(&normal, |x| x.ok.then_some(x.skin));
        let (b_w, nw) = med_of(&widen, |x| x.ok.then_some(x.skin));
        let (ap_n, _) = med_of(&normal, |x| x.ok.then_some(x.ap));
        let (ap_cl, nc) = med_of(&close, |x| Some(x.ap));
        if n_pupil < need.normal {
            return Err(WearFail::Pupil { seen: n_pupil, frames: normal.len(), need: need.normal });
        }
        if nn < need.normal {
            return Err(WearFail::LidLine { seen: nn, frames: normal.len(), need: need.normal });
        }
        let widen_ok = nw >= need.widen && b_w - b_n >= MIN_WIDEN_STEP;
        let b_w = if widen_ok { b_w } else { b_n + fallback_step[e] };
        if nc < need.close || ap_n - ap_cl < MIN_OPEN_GAP || (ap_n - ap_cl).is_nan() {
            return Err(WearFail::Close);
        }
        let (pitch_n, _) = med_of(&normal, |x| Some(x.pitch));
        let (lower_px, _) = med_of(&normal, |x| x.ok.then_some(x.lower_px));
        Ok((WearParams { r_px, b_n, b_w, ap_n, ap_cl, pitch_n, lower_px }, widen_ok))
    })
}

/// The message for a wear calibration whose eyes did not both go through (both eyes' problems).
pub fn wear_fail_message(eyes: &[EyeWear; 2]) -> String {
    eyes_message(&[0, 1].map(|e| eyes[e].as_ref().err().map(WearFail::issue)))
}

/// Both eyes' levels, or the reason (both eyes' problems) to redo it. Also returns whether the widen step was
/// measured on both eyes (only then does it belong in the history).
pub fn fit_wear(samples: &[Vec<Sample>; 2], fallback_step: [f64; 2]) -> Result<([WearParams; 2], bool), String> {
    match fit_wear_eyes(samples, fallback_step) {
        [Ok((l, ml)), Ok((r, mr))] => Ok(([l, r], ml && mr)),
        eyes => Err(with_rate_note(wear_fail_message(&eyes), samples)),
    }
}

/// Levels for an eye whose wear calibration failed and that has no earlier ones: the other eye's (in iris radii),
/// with the history's widen step and open gap (else the defaults) and its own iris radius (the history's, else the
/// one in use during the calibration, else the other eye's). EyeWide and the lids start from these and follow the
/// wearer from there.
pub fn provisional_wear(own: &[Sample], other: &WearParams, hist: Option<History>) -> WearParams {
    let own_r = calib_radius(own, &[Label::Normal, Label::Widen]);
    let r_px = [hist.map_or(f64::NAN, |h| h.r_px), own_r].into_iter().find(|r| r.is_finite() && *r > 0.0).unwrap_or(other.r_px);
    let step = hist.map(|h| h.step).filter(|v| v.is_finite()).unwrap_or(DEFAULT_WIDEN_STEP);
    let gap = hist.map(|h| h.gap).filter(|v| v.is_finite()).unwrap_or(DEFAULT_OPEN_GAP);
    WearParams { r_px, b_w: other.b_n + step, ap_cl: other.ap_n - gap, lower_px: f64::NAN, ..*other }
}

/// A wear calibration that went through, maybe with one eye failed.
#[derive(Clone, Debug, PartialEq)]
pub struct WearOutcome {
    pub wear: [WearParams; 2],
    /// The eyes that failed (at most one): they have their `previous` levels, or provisional ones.
    pub failed: [bool; 2],
    /// The widen step was measured on the eyes that went through.
    pub widen_measured: bool,
    pub message: String,
}

impl WearOutcome {
    /// Its sizes belong in the history: both eyes went through and measured the widen.
    pub fn for_history(&self) -> bool {
        self.widen_measured && self.failed == [false, false]
    }

    /// "L", "R" or "".
    pub fn failed_eye(&self) -> &'static str {
        match self.failed {
            [true, _] => "L",
            [_, true] => "R",
            _ => "",
        }
    }
}

/// A wear calibration with one eye allowed to fail (like a widen that was not caught): the eye that went through
/// gets its new levels, the failed one its `previous` ones (the last wear calibration's, if they were measured on
/// that eye: `CalibFile::measured_wear`), or without those provisional ones (`provisional_wear`). Both eyes
/// failing is the reason to redo it.
pub fn fit_wear_settled(
    samples: &[Vec<Sample>; 2],
    fallback_step: [f64; 2],
    previous: [Option<WearParams>; 2],
    hist: Option<[History; 2]>,
) -> Result<WearOutcome, String> {
    let eyes = fit_wear_eyes(samples, fallback_step);
    let ok = [0, 1].map(|e| eyes[e].as_ref().ok().copied());
    if ok == [None, None] {
        return Err(with_rate_note(wear_fail_message(&eyes), samples));
    }
    let wear = [0, 1].map(|e| match (ok[e], ok[1 - e]) {
        (Some((w, _)), _) => w,
        (None, Some((other, _))) => previous[e].unwrap_or_else(|| provisional_wear(&samples[e], &other, hist.map(|h| h[e]))),
        (None, None) => unreachable!("both eyes failing returned above"),
    });
    Ok(WearOutcome {
        wear,
        failed: ok.map(|x| x.is_none()),
        widen_measured: ok.iter().flatten().all(|x| x.1),
        message: wear_ok_message(&eyes, previous.map(|p| p.is_some())),
    })
}

/// The message of a wear calibration that went through: notes for an eye that failed (it uses its `previous`
/// values, or provisional ones) and for a widen not caught on an eye that passed; that eye's details at the end.
pub fn wear_ok_message(eyes: &[EyeWear; 2], previous: [bool; 2]) -> String {
    let mut notes = Vec::new();
    let mut details = String::new();
    for (e, x) in eyes.iter().enumerate() {
        if let Err(f) = x {
            let src = if previous[e] { "前の値" } else { "仮の値" };
            notes.push(format!("{}目は{}ので、{src}を使うよ", ["左", "右"][e], f.reason()));
            details = f.issue().details();
        }
    }
    if eyes.iter().any(|x| matches!(x, Ok((_, false)))) {
        notes.push(NOTE_WIDEN.to_string());
    }
    if notes.is_empty() { "校正できた（かぶり）".to_string() } else { format!("校正できた（{}）{details}", notes.join("。")) }
}

/// Pitch curve and squint depth (needs this wear's levels), with the sanity checks; pupil range kept from `old`.
/// With `strict` false the checks only produce warnings (for offline evaluation). Both eyes are checked before it
/// fails, and the message names both when both have a problem.
pub fn fit_user(
    samples: &[Vec<Sample>; 2],
    wear: &[WearParams; 2],
    old: Option<[UserParams; 2]>,
    strict: bool,
) -> Result<([UserParams; 2], Vec<String>), String> {
    fit_user_checked(samples, wear, old, strict).map_err(|m| with_rate_note(m, samples))
}

fn fit_user_checked(
    samples: &[Vec<Sample>; 2],
    wear: &[WearParams; 2],
    old: Option<[UserParams; 2]>,
    strict: bool,
) -> Result<([UserParams; 2], Vec<String>), String> {
    let mut out = [UserParams::default(); 2];
    let mut deltas = [(0.0, 0.0); 2];
    let mut warnings = Vec::new();
    let mut fails: [Option<EyeIssue>; 2] = [None, None];
    for (e, s) in samples.iter().enumerate() {
        let w = &wear[e];
        let need = UserNeeds::of(s);
        let s: Vec<Sample> = s.iter().map(|x| x.rescaled(w.r_px)).collect();
        let s = &s[..];
        let pts: Vec<(f64, f64)> =
            s.iter().filter(|x| neutral(x.label) && x.ok && x.pitch.is_finite() && x.ap.is_finite()).map(|x| (x.pitch, x.ap)).collect();
        if pts.len() < need.points {
            let count = |f: &dyn Fn(&Sample) -> bool| s.iter().filter(|x| neutral(x.label) && f(x)).count();
            let (frames, gaze, pupil) = (count(&|_| true), count(&|x| x.pitch.is_finite()), count(&|x| x.ok));
            if gaze < need.points {
                return Err(MSG_NO_GAZE.into());
            }
            let need_text = format!("{} 必要", need.points);
            fails[e] = Some(if pupil < need.points {
                EyeIssue::new(MSG_PUPIL, format!("{pupil}/{frames}"), &need_text)
            } else {
                EyeIssue::new(MSG_LIDS, format!("{}/{frames}", pts.len()), &need_text)
            });
            continue;
        }
        let xs: Vec<f64> = pts.iter().map(|p| p.0).collect();
        let ys: Vec<f64> = pts.iter().map(|p| p.1).collect();
        let q = Quad::fit(&xs, &ys, &vec![true; xs.len()]).ok_or("視線と目の開きの関係が求められなかった（もう一度）")?;
        let (c2, c1, c0, m) = (q.c[0], q.c[1], q.c[2], q.xm);
        let pitch_c = [c0 - c1 * m + c2 * m * m, c1 - 2.0 * c2 * m, c2];
        let mut sorted = xs.clone();
        let (lo, hi) = (vision::percentile(&mut sorted, 1.0), vision::percentile(&mut sorted, 99.0));
        let (ap_sq, nsq) = med_of(s, |x| (x.label == Label::Squint).then_some(x.ap));
        if nsq < need.squint {
            fails[e] = Some(EyeIssue::new(MSG_SQUINT_MISSING, String::new(), ""));
            continue;
        }
        let f_sq = (ap_sq - w.ap_cl) / (w.ap_n - w.ap_cl);
        let (ap_down, nd) = med_of(s, |x| (x.label == Label::LookDown).then_some(x.ap));
        let (lower_down, _) = med_of(s, |x| (x.label == Label::LookDown).then_some(x.lower_px));
        let checks = [
            (f_sq >= 0.85 || f_sq.is_nan(), EyeIssue::new(MSG_SQUINT_SHALLOW, format!("f_sq {f_sq:.2}"), "")),
            (nd < need.look_down || ap_down > 0.95 * w.ap_n, EyeIssue::new(MSG_LOOK_DOWN, format!("{ap_down:.2} / 普段 {:.2}", w.ap_n), "")),
            (lower_down < w.lower_px, EyeIssue::new(MSG_LOWER_UP, format!("{lower_down:.1} px / 普段 {:.1} px", w.lower_px), "")),
        ];
        for (bad, issue) in checks {
            if !bad {
                continue;
            }
            if strict {
                fails[e].get_or_insert(issue);
            } else {
                warnings.push(issue.message(e));
            }
        }
        deltas[e] = (ap_down - w.ap_n, ap_sq - w.ap_n);
        let keep = old.map(|o| o[e]).unwrap_or_default();
        out[e] = UserParams { pitch_c, pitch_lo: lo, pitch_hi: hi, f_sq, pd_min: keep.pd_min, pd_max: keep.pd_max };
    }
    if fails.iter().any(Option::is_some) {
        return Err(eyes_message(&fails));
    }
    // The left/right difference is only a warning: clean recordings trip it too (0.39 and 0.33 in two of them).
    let (dd, ds) = ((deltas[0].0 - deltas[1].0).abs(), (deltas[0].1 - deltas[1].1).abs());
    if dd >= 0.2 || ds >= 0.2 {
        warnings.push(format!("左右の差が大きい [下を見たとき {dd:.2}、細め {ds:.2}]"));
    }
    Ok((out, warnings))
}

/// Pupil range (P5 of bright, P95 of dark, in iris units) from a recording with bright and dark steps, in units
/// of this wear's calibrated radius when there is one.
pub fn fit_pupil(samples: &[Vec<Sample>; 2], r: Option<[f64; 2]>) -> Option<[(f64, f64); 2]> {
    let mut out = [(0.0, 0.0); 2];
    for (e, s) in samples.iter().enumerate() {
        let rr = r.map_or_else(|| calib_radius(s, &[Label::Normal, Label::Bright, Label::Dark]), |r| r[e]);
        let s: Vec<Sample> = s.iter().map(|x| x.rescaled(rr)).collect();
        let mut br: Vec<f64> = s.iter().filter(|x| x.label == Label::Bright && x.pd.is_finite()).map(|x| x.pd).collect();
        let mut dk: Vec<f64> = s.iter().filter(|x| x.label == Label::Dark && x.pd.is_finite()).map(|x| x.pd).collect();
        let fps = samples_fps(&s);
        let step = |l: Label| s.iter().filter(|x| x.label == l).count();
        if br.len() < NEED_PUPIL.frames(step(Label::Bright), fps) || dk.len() < NEED_PUPIL.frames(step(Label::Dark), fps) {
            return None;
        }
        let (lo, hi) = (vision::percentile(&mut br, 5.0), vision::percentile(&mut dk, 95.0));
        if hi - lo < 0.1 || (hi - lo).is_nan() {
            return None;
        }
        out[e] = (lo, hi);
    }
    Some(out)
}

// ------------------------------------------------------------------------------------------------- calib.json

/// One past wear calibration's sizes for one eye (iris radii, except `r_px`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct WearRecord {
    pub step: f64,
    pub gap: f64,
    pub r_px: f64,
}

impl WearRecord {
    pub fn of(w: &WearParams) -> Self {
        Self { step: w.b_w - w.b_n, gap: w.ap_n - w.ap_cl, r_px: w.r_px }
    }
}

/// What ~/.config/eyecam/calib.json holds: the user calibration, the last wear calibration and the history of wear
/// calibrations (their sizes only).
#[derive(Clone, Debug, Default)]
pub struct CalibFile {
    pub user: Option<[UserParams; 2]>,
    pub user_time: String,
    /// What the user calibration's checks warned about (not enforced).
    pub user_warnings: Vec<String>,
    pub pupil_measured: bool,
    pub wear: Option<[WearParams; 2]>,
    pub wear_time: String,
    /// Whether that wear calibration measured the widen (false: it used the history's or the default width; None:
    /// not known, a file from before this field). status.json's last_calib_widen after a restart.
    pub wear_widen_measured: Option<bool>,
    /// The eye whose part of that wear calibration failed ("L" / "R"; "" none): it kept its earlier levels (or got
    /// provisional ones). status.json's calib_failed_eye after a restart.
    pub wear_failed_eye: String,
    /// (time, L, R), oldest first.
    pub history: Vec<(String, [WearRecord; 2])>,
    /// A wear calibration has succeeded at least once (also one whose widen was not caught): the first-time setup
    /// is done. Files from before this field count as done when they have a history.
    pub setup_done: bool,
}

/// What calib.json and calib_result.json say about their eyes: L is the left eye. Files without it were written
/// before 2026-10-07, when eyecam called the right eye's camera L (see `ring`): their L and R are the other eye's.
pub const JSON_EYES_ANATOMICAL: &str = "\"eyes\": \"anatomical\"";

/// Whether a calib.json / calib_result.json says its L and R are the anatomical eyes (JSON_EYES_ANATOMICAL).
pub fn json_eyes_anatomical(j: &Json) -> bool {
    j.get("eyes").and_then(Json::str) == Some("anatomical")
}

/// The key ("L" / "R") holding an eye's values (0 = left) in a file whose eyes are `anatomical` or not (an old file
/// keeps the left eye's under "R").
pub fn json_eye_key(eye: usize, anatomical: bool) -> &'static str {
    ["L", "R"][eye ^ !anatomical as usize]
}

/// The other eye's name: "L" <-> "R" (anything else as it is).
pub fn other_eye_name(name: &str) -> String {
    match name {
        "L" => "R".into(),
        "R" => "L".into(),
        n => n.into(),
    }
}

/// A message of an old file (from before the eyes were anatomical) about the eyes it really meant: 左 and 右
/// exchanged ("左目" <-> "右目", "[左 a・右 b]" -> "[右 a・左 b]"), "左右" kept.
pub fn swap_eye_words(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    let mut chars = text.chars().peekable();
    while let Some(c) = chars.next() {
        match c {
            '左' if chars.peek() == Some(&'右') => {
                chars.next();
                out.push_str("左右");
            }
            '左' => out.push('右'),
            '右' => out.push('左'),
            c => out.push(c),
        }
    }
    out
}

fn median_of(v: impl Iterator<Item = f64>) -> f64 {
    let mut v: Vec<f64> = v.filter(|x| x.is_finite()).collect();
    vision::median(&mut v)
}

fn wear_json(w: &WearParams) -> String {
    format!(
        "{{\"r_px\": {}, \"b_n\": {}, \"b_w\": {}, \"ap_n\": {}, \"ap_cl\": {}, \"pitch_n\": {}, \"lower_px\": {}}}",
        json::num(w.r_px),
        json::num(w.b_n),
        json::num(w.b_w),
        json::num(w.ap_n),
        json::num(w.ap_cl),
        json::num(w.pitch_n),
        json::num(w.lower_px)
    )
}

fn user_json(u: &UserParams) -> String {
    format!(
        "{{\"pitch_c\": [{}, {}, {}], \"pitch_lo\": {}, \"pitch_hi\": {}, \"f_sq\": {}, \"pd_min\": {}, \"pd_max\": {}}}",
        json::num(u.pitch_c[0]),
        json::num(u.pitch_c[1]),
        json::num(u.pitch_c[2]),
        json::num(u.pitch_lo),
        json::num(u.pitch_hi),
        json::num(u.f_sq),
        json::num(u.pd_min),
        json::num(u.pd_max)
    )
}

fn get_num(j: &Json, k: &str) -> Result<f64, String> {
    Ok(j.get(k).and_then(Json::num).unwrap_or(f64::NAN)).and_then(|v| if v.is_nan() { Err(format!("missing {k}")) } else { Ok(v) })
}

fn parse_wear(j: &Json) -> Result<WearParams, String> {
    Ok(WearParams {
        r_px: get_num(j, "r_px")?,
        b_n: get_num(j, "b_n")?,
        b_w: get_num(j, "b_w")?,
        ap_n: get_num(j, "ap_n")?,
        ap_cl: get_num(j, "ap_cl")?,
        pitch_n: j.get("pitch_n").and_then(Json::num).unwrap_or(DEFAULT_PITCH_N),
        lower_px: j.get("lower_px").and_then(Json::num).unwrap_or(f64::NAN),
    })
}

fn parse_user(j: &Json) -> Result<UserParams, String> {
    let c = j.get("pitch_c").and_then(Json::arr).ok_or("missing pitch_c")?;
    let c: Vec<f64> = c.iter().map(|v| v.num().unwrap_or(0.0)).collect();
    if c.len() != 3 {
        return Err("pitch_c needs 3 numbers".into());
    }
    Ok(UserParams {
        pitch_c: [c[0], c[1], c[2]],
        pitch_lo: get_num(j, "pitch_lo")?,
        pitch_hi: get_num(j, "pitch_hi")?,
        f_sq: get_num(j, "f_sq")?,
        pd_min: get_num(j, "pd_min")?,
        pd_max: get_num(j, "pd_max")?,
    })
}

impl CalibFile {
    /// The last wear calibration's levels of each eye that were measured on that eye: not those of the eye that
    /// failed then (they were themselves carried over or provisional, and a calibration that fails on that eye
    /// again says so and starts it from provisional ones).
    pub fn measured_wear(&self) -> [Option<WearParams>; 2] {
        [0, 1].map(|e| self.wear.map(|w| w[e]).filter(|_| self.wear_failed_eye != ["L", "R"][e]))
    }

    /// The medians of the history (None when it is empty).
    pub fn history_params(&self) -> Option<[History; 2]> {
        if self.history.is_empty() {
            return None;
        }
        let h = |e: usize| History {
            step: median_of(self.history.iter().map(|x| x.1[e].step)),
            gap: median_of(self.history.iter().map(|x| x.1[e].gap)),
            r_px: median_of(self.history.iter().map(|x| x.1[e].r_px)),
        };
        Some([h(0), h(1)])
    }

    /// Add a successful wear calibration to the history (keeping the last HISTORY_MAX).
    pub fn push_history(&mut self, time: &str, w: &[WearParams; 2]) {
        self.history.push((time.to_string(), [WearRecord::of(&w[0]), WearRecord::of(&w[1])]));
        let extra = self.history.len().saturating_sub(HISTORY_MAX);
        self.history.drain(..extra);
    }

    pub fn to_json(&self) -> String {
        let mut s = format!("{{\n  \"version\": 2,\n  {JSON_EYES_ANATOMICAL},\n  \"setup_done\": {}", self.setup_done);
        if let Some(u) = &self.user {
            s += &format!(
                ",\n  \"user\": {{\"time\": {}, \"pupil_measured\": {}, \"warnings\": [{}], \"L\": {}, \"R\": {}}}",
                json::string(&self.user_time),
                self.pupil_measured,
                self.user_warnings.iter().map(|w| json::string(w)).collect::<Vec<_>>().join(", "),
                user_json(&u[0]),
                user_json(&u[1])
            );
        }
        if let Some(w) = &self.wear {
            let widen = match self.wear_widen_measured {
                Some(true) => ", \"widen\": \"measured\"",
                Some(false) => ", \"widen\": \"default\"",
                None => "",
            };
            let failed = if self.wear_failed_eye.is_empty() {
                String::new()
            } else {
                format!(", \"failed_eye\": {}", json::string(&self.wear_failed_eye))
            };
            s += &format!(
                ",\n  \"wear\": {{\"time\": {}{widen}{failed}, \"L\": {}, \"R\": {}}}",
                json::string(&self.wear_time),
                wear_json(&w[0]),
                wear_json(&w[1])
            );
        }
        if !self.history.is_empty() {
            let rec = |r: &WearRecord| {
                format!("{{\"step\": {}, \"gap\": {}, \"r_px\": {}}}", json::num(r.step), json::num(r.gap), json::num(r.r_px))
            };
            let items: Vec<String> = self
                .history
                .iter()
                .map(|(t, r)| format!("\n    {{\"time\": {}, \"L\": {}, \"R\": {}}}", json::string(t), rec(&r[0]), rec(&r[1])))
                .collect();
            s += &format!(",\n  \"history\": [{}\n  ]", items.join(","));
        }
        s + "\n}\n"
    }

    pub fn parse(text: &str) -> Result<Self, String> {
        Self::parse_converting(text).map(|(c, _)| c)
    }

    /// Parse, and say whether the file was written before the eyes were anatomical (JSON_EYES_ANATOMICAL): its
    /// per-eye values, failed eye and warnings were then read for the other eye, and the caller should save it
    /// again (with the marker) so it is converted once.
    pub fn parse_converting(text: &str) -> Result<(Self, bool), String> {
        let j = json::parse(text)?;
        let anatomical = json_eyes_anatomical(&j);
        let (l, r) = (json_eye_key(0, anatomical), json_eye_key(1, anatomical));
        let mut c = CalibFile::default();
        if let Some(u) = j.get("user") {
            c.user = Some([
                parse_user(u.get(l).ok_or_else(|| format!("user.{l}"))?)?,
                parse_user(u.get(r).ok_or_else(|| format!("user.{r}"))?)?,
            ]);
            c.user_time = u.get("time").and_then(Json::str).unwrap_or("").to_string();
            c.pupil_measured = matches!(u.get("pupil_measured"), Some(Json::Bool(true)));
            c.user_warnings = u
                .get("warnings")
                .and_then(Json::arr)
                .map(|a| {
                    a.iter()
                        .filter_map(|w| w.str().map(|w| if anatomical { w.to_string() } else { swap_eye_words(w) }))
                        .collect()
                })
                .unwrap_or_default();
        }
        if let Some(w) = j.get("wear") {
            c.wear = Some([
                parse_wear(w.get(l).ok_or_else(|| format!("wear.{l}"))?)?,
                parse_wear(w.get(r).ok_or_else(|| format!("wear.{r}"))?)?,
            ]);
            c.wear_time = w.get("time").and_then(Json::str).unwrap_or("").to_string();
            c.wear_widen_measured = match w.get("widen").and_then(Json::str) {
                Some("measured") => Some(true),
                Some("default") => Some(false),
                _ => None,
            };
            c.wear_failed_eye = match w.get("failed_eye").and_then(Json::str) {
                Some(e @ ("L" | "R")) if anatomical => e.to_string(),
                Some(e @ ("L" | "R")) => other_eye_name(e),
                _ => String::new(),
            };
        }
        if let Some(items) = j.get("history").and_then(Json::arr) {
            let rec = |e: Option<&Json>| -> Option<WearRecord> {
                let e = e?;
                Some(WearRecord { step: e.get("step")?.num()?, gap: e.get("gap")?.num()?, r_px: e.get("r_px")?.num()? })
            };
            for it in items {
                if let (Some(rl), Some(rr)) = (rec(it.get(l)), rec(it.get(r))) {
                    c.history.push((it.get("time").and_then(Json::str).unwrap_or("").to_string(), [rl, rr]));
                }
            }
        } else if let Some(w) = &c.wear {
            // Files from before the history: the last wear calibration is the history.
            c.history.push((c.wear_time.clone(), [WearRecord::of(&w[0]), WearRecord::of(&w[1])]));
        }
        c.setup_done = matches!(j.get("setup_done"), Some(Json::Bool(true))) || !c.history.is_empty();
        Ok((c, !anatomical))
    }

    /// History entries from saved calibration attempts (`<dir>/calib_*/calib_result.json`, successful wear ones),
    /// for a calib.json that has none yet. Old results (before the eyes were anatomical) are read the other way round.
    pub fn history_from_results(dir: &std::path::Path) -> Vec<(String, [WearRecord; 2])> {
        let Ok(rd) = std::fs::read_dir(dir) else { return Vec::new() };
        let mut names: Vec<String> =
            rd.filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().to_string()).filter(|n| n.starts_with("calib_")).collect();
        names.sort();
        let mut out = Vec::new();
        for n in names {
            let Ok(text) = std::fs::read_to_string(dir.join(&n).join("calib_result.json")) else { continue };
            let Ok(j) = json::parse(&text) else { continue };
            if j.get("kind").and_then(Json::str) != Some("wear") || !matches!(j.get("ok"), Some(Json::Bool(true))) {
                continue;
            }
            let rec = |e: &str| -> Option<WearRecord> {
                let v = j.get("values")?.get(e)?;
                Some(WearRecord { step: v.get("widen_step")?.num()?, gap: v.get("open_gap")?.num()?, r_px: v.get("r_px")?.num()? })
            };
            // A result from before the eyes were anatomical has the left eye's values under "R".
            let anatomical = json_eyes_anatomical(&j);
            if let (Some(l), Some(r)) = (rec(json_eye_key(0, anatomical)), rec(json_eye_key(1, anatomical))) {
                let time = j.get("time").and_then(Json::str).unwrap_or(&n).to_string();
                out.push((time, [l, r]));
            }
        }
        out
    }
}

// ------------------------------------------------------------------------------------------------- reports

/// Thresholds of the calibration checks (fit_wear / fit_user use the same numbers).
pub const MIN_WIDEN_STEP: f64 = 0.05;
pub const MIN_OPEN_GAP: f64 = 0.2;

/// How many frames a calibration check needs, relative to what the cameras delivered: at least `share` of the
/// step's frames, `seconds` worth at the cameras' frame rate, and `floor`. The cameras run at 90 frames a second on
/// most headsets but as slowly as 15 on some, where the old fixed counts could never be met (a 5.4 s normal step
/// has 486 frames at 90 fps and 81 at 15). At 90 fps the seconds decide and give the old counts.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Need {
    pub share: f64,
    pub seconds: f64,
    pub floor: usize,
}

impl Need {
    /// The frames needed out of a step's `step_frames` at `fps` (NaN: not known, the share and floor only).
    pub fn frames(&self, step_frames: usize, fps: f64) -> usize {
        let by_time = if fps.is_finite() { (self.seconds * fps).round() as usize } else { 0 };
        let by_share = (self.share * step_frames as f64).ceil() as usize;
        self.floor.max(by_time).max(by_share)
    }

    fn json(&self) -> String {
        format!("{{\"share\": {}, \"seconds\": {}, \"floor\": {}}}", json::num(self.share), json::num(self.seconds), self.floor)
    }
}

/// The pupil and the upper lid's line in the normal steps: about a second's worth (90 at 90 fps, 15 at 15).
pub const NEED_NORMAL: Need = Need { share: 0.18, seconds: 1.0, floor: 12 };
/// The pupil in the widen steps for the widen to count (60 at 90 fps).
pub const NEED_WIDEN: Need = Need { share: 0.14, seconds: 60.0 / FPS, floor: 8 };
/// Lids measured in the close step (20 at 90 fps).
pub const NEED_CLOSE: Need = Need { share: 0.15, seconds: 20.0 / FPS, floor: 3 };
/// User calibration: pupil, lids and Valve's gaze in the neutral steps (150 at 90 fps) ...
pub const NEED_USER_POINTS: Need = Need { share: 0.15, seconds: 150.0 / FPS, floor: 25 };
/// ... and the lids in the squint and look_down steps (30 at 90 fps).
pub const NEED_USER_STEP: Need = Need { share: 0.07, seconds: 30.0 / FPS, floor: 5 };
/// Pupil range: whole pupils in the bright and the dark steps (60 at 90 fps).
pub const NEED_PUPIL: Need = Need { share: 0.08, seconds: 60.0 / FPS, floor: 8 };
/// Below this many frames a second a failed calibration says how few frames came.
pub const LOW_FPS: f64 = 60.0;

/// The cameras' frame rate in a calibration's samples of one eye: the median interval between frames of the same
/// step, rounded to whole frames a second (as `FrameRate`); NaN without two frames in a row.
pub fn samples_fps(s: &[Sample]) -> f64 {
    let mut dt: Vec<f64> =
        s.windows(2).filter(|w| w[0].label == w[1].label).map(|w| w[1].t - w[0].t).filter(|d| *d > 0.0 && *d < 0.5).collect();
    if dt.is_empty() { f64::NAN } else { (1.0 / vision::median(&mut dt)).round().max(1.0) }
}

/// One eye's wear calibration needs (frames), and the frame rate they are for.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct WearNeeds {
    pub fps: f64,
    pub normal: usize,
    pub widen: usize,
    pub close: usize,
}

impl WearNeeds {
    pub fn of(s: &[Sample]) -> Self {
        let fps = samples_fps(s);
        let n = |l: Label| s.iter().filter(|x| x.label == l).count();
        Self {
            fps,
            normal: NEED_NORMAL.frames(n(Label::Normal), fps),
            widen: NEED_WIDEN.frames(n(Label::Widen), fps),
            close: NEED_CLOSE.frames(n(Label::Close), fps),
        }
    }
}

/// The steps a user calibration fits the pitch curve on.
fn neutral(l: Label) -> bool {
    matches!(l, Label::LeadIn | Label::Normal | Label::LookUp | Label::LookDown | Label::Bright | Label::Dark)
}

/// One eye's user calibration needs (frames), and the frame rate they are for.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct UserNeeds {
    pub fps: f64,
    pub points: usize,
    pub squint: usize,
    pub look_down: usize,
}

impl UserNeeds {
    pub fn of(s: &[Sample]) -> Self {
        let fps = samples_fps(s);
        let n = |f: &dyn Fn(Label) -> bool| s.iter().filter(|x| f(x.label)).count();
        Self {
            fps,
            points: NEED_USER_POINTS.frames(n(&neutral), fps),
            squint: NEED_USER_STEP.frames(n(&|l| l == Label::Squint), fps),
            look_down: NEED_USER_STEP.frames(n(&|l| l == Label::LookDown), fps),
        }
    }
}

/// "カメラの映像が毎秒 N 枚しか届いていない": added to a failed calibration's message when the cameras delivered
/// fewer than LOW_FPS frames a second, so that a screenshot says why.
pub fn rate_note(fps: f64) -> Option<String> {
    (fps.is_finite() && fps < LOW_FPS).then(|| format!("カメラの映像が毎秒 {} 枚しか届いていない", fps.round()))
}

/// `message`, with the rate note for the slower eye's frame rate if it is low.
fn with_rate_note(message: String, samples: &[Vec<Sample>; 2]) -> String {
    let fps = samples.iter().map(|s| samples_fps(s)).filter(|f| f.is_finite()).fold(f64::NAN, f64::min);
    match rate_note(fps) {
        Some(note) => format!("{message}。{note}"),
        None => message,
    }
}

impl Label {
    pub fn name(self) -> &'static str {
        match self {
            Label::LeadIn => "lead_in",
            Label::Normal => "normal",
            Label::Widen => "widen",
            Label::Close => "close",
            Label::Squint => "squint",
            Label::LookUp => "look_up",
            Label::LookDown => "look_down",
            Label::Bright => "bright",
            Label::Dark => "dark",
            Label::Other => "other",
        }
    }
}

/// The per-eye values behind a wear calibration, whether it passed or not: (JSON object, one log line per eye).
pub fn wear_report(samples: &[Vec<Sample>; 2]) -> (String, Vec<String>) {
    let mut eyes = Vec::new();
    let mut lines = Vec::new();
    let needs = [0, 1].map(|e| WearNeeds::of(&samples[e]));
    for (e, s) in samples.iter().enumerate() {
        let need = needs[e];
        let r_px = calib_radius(s, &[Label::Normal, Label::Widen]);
        let s: Vec<Sample> = s.iter().map(|x| x.rescaled(r_px)).collect();
        let n_pupil = s.iter().filter(|x| x.label == Label::Normal && x.ok).count();
        let (b_n, nn) = med_of(&s, |x| (x.label == Label::Normal && x.ok).then_some(x.skin));
        let (b_w, nw) = med_of(&s, |x| (x.label == Label::Widen && x.ok).then_some(x.skin));
        let (ap_n, _) = med_of(&s, |x| (x.label == Label::Normal && x.ok).then_some(x.ap));
        let (ap_cl, nc) = med_of(&s, |x| (x.label == Label::Close).then_some(x.ap));
        let all = |l: Label| s.iter().filter(|x| x.label == l).count();
        let ok_share = |l: Label| {
            let n = all(l);
            if n == 0 { f64::NAN } else { s.iter().filter(|x| x.label == l && x.ok).count() as f64 / n as f64 }
        };
        let d = diag_report(&s);
        eyes.push(format!(
            "\"{}\": {{\"r_px\": {}, \"b_n\": {}, \"b_w\": {}, \"widen_step\": {}, \"ap_n\": {}, \"ap_cl\": {}, \"open_gap\": {}, \
\"normal_frames_with_pupil\": {n_pupil}, \"normal_frames_with_lid_line\": {nn}, \"widen_frames_with_pupil\": {nw}, \"close_frames_with_lids\": {nc}, \
\"pupil_seen_share\": {{\"normal\": {}, \"widen\": {}, \"close\": {}}}, \"diag\": {}}}",
            ["L", "R"][e],
            json::num(r_px),
            json::num(b_n),
            json::num(b_w),
            json::num(b_w - b_n),
            json::num(ap_n),
            json::num(ap_cl),
            json::num(ap_n - ap_cl),
            json::num(ok_share(Label::Normal)),
            json::num(ok_share(Label::Widen)),
            json::num(ok_share(Label::Close)),
            d.json,
        ));
        lines.push(format!(
            "calib wear {}: widen step {:.3} (needs >= {MIN_WIDEN_STEP}; normal {:.3}, widen {:.3}), open gap {:.3} (needs >= {MIN_OPEN_GAP}), \
frames with pupil normal {n_pupil} of {}, with the upper lid line {nn} / widen {nw} (need {} / {}), close {nc} (need {}; at {:.0} fps), R {:.1} px; {}",
            ["L", "R"][e],
            b_w - b_n,
            b_n,
            b_w,
            ap_n - ap_cl,
            all(Label::Normal),
            need.normal,
            need.widen,
            need.close,
            need.fps,
            r_px,
            d.line
        ));
    }
    // The frame counts needed per eye ([L, R]), for the frame rate each eye's camera delivered, and the rule.
    let pair = |f: &dyn Fn(&WearNeeds) -> String| format!("[{}, {}]", f(&needs[0]), f(&needs[1]));
    let json = format!(
        "{{{}, \"thresholds\": {{\"widen_step_min\": {MIN_WIDEN_STEP}, \"open_gap_min\": {MIN_OPEN_GAP}, \"fps\": {}, \"normal_frames_min\": {}, \
\"widen_frames_min\": {}, \"close_frames_min\": {}, \"rule\": {{\"normal\": {}, \"widen\": {}, \"close\": {}}}}}}}",
        eyes.join(", "),
        pair(&|n| json::num(n.fps)),
        pair(&|n| n.normal.to_string()),
        pair(&|n| n.widen.to_string()),
        pair(&|n| n.close.to_string()),
        NEED_NORMAL.json(),
        NEED_WIDEN.json(),
        NEED_CLOSE.json(),
    );
    (json, lines)
}

/// One eye's search diagnostics over a calibration.
struct DiagReport {
    json: String,
    line: String,
}

/// How the pupil search went in one eye's calibration frames: frame rate, per step the frames, those with the pupil
/// and with the upper lid line, why the others had none, the prior resets; the window used, the median pupil
/// position and the window's contrast in the normal frames.
fn diag_report(s: &[Sample]) -> DiagReport {
    let mut labels: Vec<Label> = Vec::new();
    for x in s {
        if !labels.contains(&x.label) {
            labels.push(x.label);
        }
    }
    // Frame interval: the median gap between consecutive frames of a step.
    let mut dt: Vec<f64> = s.windows(2).filter(|w| w[0].label == w[1].label).map(|w| w[1].t - w[0].t).filter(|d| *d > 0.0 && *d < 0.5).collect();
    let fps = 1.0 / vision::median(&mut dt);
    let count = |l: Label, f: &dyn Fn(&Sample) -> bool| s.iter().filter(|x| x.label == l && f(x)).count();
    let no_blob = |x: &Sample, bits: u8| x.seen.miss == Miss::NoBlob && x.seen.rejected & bits != 0;
    let steps: Vec<String> = labels
        .iter()
        .map(|&l| {
            format!(
                "\"{}\": {{\"frames\": {}, \"pupil\": {}, \"lid_line\": {}, \"no_blob\": {}, \"no_blob_edge\": {}, \"no_blob_too_big\": {}, \
\"no_blob_shape\": {}, \"refine_failed\": {}, \"gated\": {}, \"left_edge_pupils\": {}, \"retried\": {}, \"prior_resets\": {}}}",
                l.name(),
                count(l, &|_| true),
                count(l, &|x| x.ok),
                count(l, &|x| x.ok && x.skin.is_finite()),
                count(l, &|x| x.seen.miss == Miss::NoBlob),
                count(l, &|x| no_blob(x, feat::REJ_LEFT | feat::REJ_TOP | feat::REJ_BOTTOM)),
                count(l, &|x| no_blob(x, feat::REJ_BIG)),
                count(l, &|x| no_blob(x, feat::REJ_SHAPE)),
                count(l, &|x| x.seen.miss == Miss::Refine),
                count(l, &|x| x.seen.miss == Miss::Gated),
                count(l, &|x| x.ok && x.seen.left_edge),
                count(l, &|x| x.seen.retried),
                count(l, &|x| x.seen.prior_reset),
            )
        })
        .collect();
    let normal = |x: &Sample| x.label == Label::Normal;
    let (px, _) = med_of(s, |x| (normal(x) && x.ok).then_some(x.seen.cx));
    let (py, _) = med_of(s, |x| (normal(x) && x.ok).then_some(x.seen.cy));
    let (x_min, _) = med_of(s, |x| Some(x.seen.x_min));
    let (xmax, _) = med_of(s, |x| Some(x.seen.xmax));
    let (contrast, _) = med_of(s, |x| normal(x).then_some(x.seen.contrast));
    let json = format!(
        "{{\"fps\": {}, \"search_x_min\": {}, \"search_xmax\": {}, \"normal_pupil_x\": {}, \"normal_pupil_y\": {}, \"normal_contrast\": {}, \
\"steps\": {{{}}}}}",
        json::num(fps),
        json::num(x_min),
        json::num(xmax),
        json::num(px),
        json::num(py),
        json::num(contrast),
        steps.join(", ")
    );
    let n = |f: &dyn Fn(&Sample) -> bool| s.iter().filter(|x| normal(x) && f(x)).count();
    let line = format!(
        "normal without pupil: no blob {} (edge {}, too big {}, shape {}), refine {}, gated {}; prior resets {}; window x {:.0}..{:.0}, \
pupil at ({:.0}, {:.0}), contrast {:.0}, {:.0} fps",
        n(&|x| x.seen.miss == Miss::NoBlob),
        n(&|x| no_blob(x, feat::REJ_LEFT | feat::REJ_TOP | feat::REJ_BOTTOM)),
        n(&|x| no_blob(x, feat::REJ_BIG)),
        n(&|x| no_blob(x, feat::REJ_SHAPE)),
        n(&|x| x.seen.miss == Miss::Refine),
        n(&|x| x.seen.miss == Miss::Gated),
        s.iter().filter(|x| x.seen.prior_reset).count(),
        x_min,
        xmax,
        px,
        py,
        contrast,
        fps
    );
    DiagReport { json, line }
}

/// The per-eye values behind a user calibration (needs this wear's levels): (JSON object, log lines).
pub fn user_report(samples: &[Vec<Sample>; 2], wear: &[WearParams; 2]) -> (String, Vec<String>) {
    let mut eyes = Vec::new();
    let mut lines = Vec::new();
    let mut deltas = [(f64::NAN, f64::NAN); 2];
    let needs = [0, 1].map(|e| UserNeeds::of(&samples[e]));
    for (e, s) in samples.iter().enumerate() {
        let w = &wear[e];
        let need = needs[e];
        let s: Vec<Sample> = s.iter().map(|x| x.rescaled(w.r_px)).collect();
        let (ap_sq, nsq) = med_of(&s, |x| (x.label == Label::Squint).then_some(x.ap));
        let (ap_down, nd) = med_of(&s, |x| (x.label == Label::LookDown).then_some(x.ap));
        let (lower_down, _) = med_of(&s, |x| (x.label == Label::LookDown).then_some(x.lower_px));
        let pitch_pts = s.iter().filter(|x| x.ok && x.pitch.is_finite() && x.ap.is_finite() && x.label != Label::Squint).count();
        let f_sq = (ap_sq - w.ap_cl) / (w.ap_n - w.ap_cl);
        deltas[e] = (ap_down - w.ap_n, ap_sq - w.ap_n);
        eyes.push(format!(
            "\"{}\": {{\"f_sq\": {}, \"ap_squint\": {}, \"ap_look_down\": {}, \"ap_normal\": {}, \"look_down_ratio\": {}, \
\"lower_look_down_px\": {}, \"lower_normal_px\": {}, \"squint_frames\": {nsq}, \"look_down_frames\": {nd}, \"pitch_points\": {pitch_pts}}}",
            ["L", "R"][e],
            json::num(f_sq),
            json::num(ap_sq),
            json::num(ap_down),
            json::num(w.ap_n),
            json::num(ap_down / w.ap_n),
            json::num(lower_down),
            json::num(w.lower_px),
        ));
        lines.push(format!(
            "calib user {}: squint depth {:.2} (needs < 0.85), look_down aperture {:.2} of normal (needs <= 0.95), lower lid {:.1} px vs normal {:.1} px (must not move up), pitch points {pitch_pts} (need {}), squint / look_down frames {nsq} / {nd} (need {} / {}; at {:.0} fps)",
            ["L", "R"][e],
            f_sq,
            ap_down / w.ap_n,
            lower_down,
            w.lower_px,
            need.points,
            need.squint,
            need.look_down,
            need.fps
        ));
    }
    let (dd, ds) = ((deltas[0].0 - deltas[1].0).abs(), (deltas[0].1 - deltas[1].1).abs());
    lines.push(format!("calib user L/R difference: look_down {dd:.2}, squint {ds:.2} (warning at >= 0.2)"));
    let pair = |f: &dyn Fn(&UserNeeds) -> String| format!("[{}, {}]", f(&needs[0]), f(&needs[1]));
    let json = format!(
        "{{{}, \"left_right_difference\": {{\"look_down\": {}, \"squint\": {}}}, \"thresholds\": {{\"f_sq_max\": 0.85, \"look_down_ratio_max\": 0.95, \"left_right_warn\": 0.2, \
\"fps\": {}, \"pitch_points_min\": {}, \"squint_frames_min\": {}, \"look_down_frames_min\": {}, \"rule\": {{\"pitch_points\": {}, \"squint_look_down\": {}}}}}}}",
        eyes.join(", "),
        json::num(dd),
        json::num(ds),
        pair(&|n| json::num(n.fps)),
        pair(&|n| n.points.to_string()),
        pair(&|n| n.squint.to_string()),
        pair(&|n| n.look_down.to_string()),
        NEED_USER_POINTS.json(),
        NEED_USER_STEP.json(),
    );
    (json, lines)
}

/// The collected samples as CSV (one row per eye and frame).
pub fn samples_csv(samples: &[Vec<Sample>; 2]) -> String {
    let mut out = String::from("eye,t,label,ok,skin_up,aperture,pupil_ratio,pitch,lower_px,r,iris_r,pupil_x,pupil_y,miss,rejected,contrast,x_min,prior_reset\n");
    let f = |v: f64| if v.is_finite() { format!("{v:.5}") } else { String::new() };
    for (e, s) in samples.iter().enumerate() {
        for x in s {
            out += &format!(
                "{},{:.6},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
                ["L", "R"][e],
                x.t,
                x.label.name(),
                x.ok as u8,
                f(x.skin),
                f(x.ap),
                f(x.pd),
                f(x.pitch),
                f(x.lower_px),
                f(x.r),
                f(x.iris_r),
                f(x.seen.cx),
                f(x.seen.cy),
                x.seen.miss.name(),
                x.seen.rejected,
                f(x.seen.contrast),
                f(x.seen.x_min),
                x.seen.prior_reset as u8
            );
        }
    }
    out
}

/// A wear calibration as JSON (for calib_result.json).
pub fn wear_params_json(w: &[WearParams; 2]) -> String {
    format!("{{\"L\": {}, \"R\": {}}}", wear_json(&w[0]), wear_json(&w[1]))
}

/// A user calibration as JSON (for calib_result.json).
pub fn user_params_json(u: &[UserParams; 2]) -> String {
    format!("{{\"L\": {}, \"R\": {}}}", user_json(&u[0]), user_json(&u[1]))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn wide_gate_needs_100_ms_above_half_and_stays_on_down_to_the_off_level() {
        let mut g = WideGate::default();
        let n = frames_in(WIDE_ON_S, FPS, 2);
        assert_eq!(n, 9, "9 frames at 90 fps");
        // A spike shorter than 100 ms is capped at the off level.
        let spike: Vec<f64> = (0..8).map(|_| g.step(0.9, n)).collect();
        assert!(spike.iter().all(|&v| v == WIDE_OFF), "{spike:?}");
        assert_eq!(g.step(0.2, n), 0.2);
        // A held widen comes through on its 9th frame and stays while above the off level.
        let held: Vec<f64> = (0..9).map(|_| g.step(0.8, n)).collect();
        assert_eq!(held[7], WIDE_OFF);
        assert_eq!(held[8], 0.8);
        assert_eq!(g.step(0.4, n), 0.4);
        assert_eq!(g.step(0.29, n), 0.29);
        // Below the off level it has to wait again.
        assert_eq!(g.step(0.9, n), WIDE_OFF);
        // At 15 fps the 100 ms are 2 frames (a single frame is still capped).
        let n = frames_in(WIDE_ON_S, 15.0, 2);
        let mut g = WideGate::default();
        assert_eq!([g.step(0.8, n), g.step(0.8, n)], [WIDE_OFF, 0.8]);
    }

    #[test]
    fn durations_turn_into_the_old_frame_counts_at_90_fps() {
        let at = |s: f64, min: u32| [90.0, 72.0, 45.0, 18.0, 15.0].map(|fps| frames_in(s, fps, min));
        assert_eq!(at(HOLD_S, 1), [6, 5, 3, 1, 1]);
        assert_eq!(at(PUPIL_SETTLE_S, 1), [27, 22, 14, 5, 5]);
        assert_eq!(at(PRIOR_RESET_S, 2), [10, 8, 5, 2, 2]);
        assert_eq!(at(CLOSED_NO_PUPIL_S, 2), [3, 2, 2, 2, 2]);
        assert_eq!(at(WIDE_ON_S, 2), [9, 7, 5, 2, 2]);
        assert_eq!(at(MEDIAN_S, 3).map(|n| n | 1), [5, 5, 3, 3, 3]);
        assert_eq!(at(0.5, 1), [45, 36, 23, 9, 8]);
    }

    #[test]
    fn frame_rate_comes_from_the_frame_times() {
        let rate = |fps: f64, n: usize, lost: &[usize]| {
            let mut r = FrameRate::default();
            for k in (0..n).filter(|k| !lost.contains(k)) {
                r.push(1000.0 + k as f64 / fps + (k % 3) as f64 * 3e-5);
            }
            r.fps()
        };
        assert_eq!(rate(15.0, 4, &[]), FPS, "too few frames yet: the default");
        assert_eq!(rate(15.0, 40, &[]), 15.0);
        assert_eq!(rate(72.0, 40, &[]), 72.0);
        assert_eq!(rate(90.0, 100, &[50, 51, 70]), 90.0, "lost frames do not change it");
        assert_eq!(rate(120.0, 100, &[]), 120.0);
        // A long gap (frames stopped) is not an interval.
        let mut r = FrameRate::default();
        for k in 0..40 {
            r.push(if k < 20 { k as f64 / 15.0 } else { 100.0 + k as f64 / 15.0 });
        }
        assert_eq!(r.fps(), 15.0);
    }

    fn samples(label: Label, n: usize, skin: f64, ap: f64, pitch: f64, lower: f64, ok: bool) -> Vec<Sample> {
        (0..n)
            .map(|i| Sample {
                label,
                ok,
                skin,
                ap: ap + (i % 3) as f64 * 0.001,
                pd: 0.9,
                pitch: pitch + (i % 5) as f64,
                lower_px: lower,
                r: 55.0,
                iris_r: if ok { 55.0 } else { f64::NAN },
                t: i as f64 / 90.0,
                seen: Seen::default(),
            })
            .collect()
    }

    #[test]
    fn fits_wear_and_user_and_round_trips_json() {
        let mut eye = Vec::new();
        eye.extend(samples(Label::Normal, 300, 0.73, 1.44, -14.0, 250.0, true));
        eye.extend(samples(Label::Widen, 200, 0.93, 1.6, -14.0, 252.0, true));
        eye.extend(samples(Label::Close, 80, f64::NAN, 0.56, -14.0, 240.0, false));
        let both = [eye.clone(), eye];
        let (w, measured) = fit_wear(&both, [0.19; 2]).unwrap();
        assert!(measured);
        assert!((w[0].b_w - w[0].b_n - 0.2).abs() < 1e-9 && (w[0].ap_n - 1.441).abs() < 0.002);
        // No widen caught on the right eye: still a calibration, with the fallback step there and nothing measured.
        let mut flat = both[1].clone();
        for x in flat.iter_mut().filter(|x| x.label == Label::Widen) {
            x.skin = 0.74;
        }
        let (wp, measured) = fit_wear(&[both[0].clone(), flat.clone()], [0.19, 0.21]).unwrap();
        assert!(!measured && (wp[1].b_w - wp[1].b_n - 0.21).abs() < 1e-9 && (wp[0].b_w - wp[0].b_n - 0.2).abs() < 1e-9);
        // Without the closed level it still fails.
        flat.retain(|x| x.label != Label::Close);
        assert!(fit_wear(&[both[0].clone(), flat], [0.19; 2]).unwrap_err().contains("閉じた"));
        let mut u = Vec::new();
        u.extend(samples(Label::LeadIn, 200, 0.73, 1.44, -14.0, 250.0, true));
        u.extend(samples(Label::LookUp, 200, 0.73, 1.6, 5.0, 248.0, true));
        u.extend(samples(Label::LookDown, 200, 0.6, 1.1, -35.0, 256.0, true));
        u.extend(samples(Label::Squint, 200, 0.6, 0.83, -14.0, 249.0, true));
        let both_u = [u.clone(), u.clone()];
        let (user, warn) = fit_user(&both_u, &w, None, true).unwrap();
        assert!(warn.is_empty());
        // The reports carry the same numbers the checks use.
        let (report, lines) = wear_report(&both);
        assert!(crate::json::parse(&report).is_ok(), "{report}");
        assert!(lines[0].contains("widen step 0.200"), "{}", lines[0]);
        let (ureport, ulines) = user_report(&both_u, &w);
        assert!(crate::json::parse(&ureport).is_ok(), "{ureport}");
        assert_eq!(ulines.len(), 3);
        assert_eq!(samples_csv(&both).lines().count(), 1 + 2 * 580);
        assert!((user[0].f_sq - (0.831 - 0.561) / (1.441 - 0.561)).abs() < 0.01, "{:?}", user[0]);
        // A look_down that did not lower the lid is refused.
        let mut bad = u.clone();
        bad.retain(|s| s.label != Label::LookDown);
        bad.extend(samples(Label::LookDown, 200, 0.73, 1.43, -35.0, 256.0, true));
        assert!(fit_user(&[bad.clone(), bad.clone()], &w, None, true).unwrap_err().contains("下を見ても"));
        assert_eq!(fit_user(&[bad.clone(), bad], &w, None, false).unwrap().1.len(), 2);
        let file = CalibFile {
            user: Some(user),
            user_time: "t".into(),
            user_warnings: vec!["左右の差が大きい \"x\"".into()],
            pupil_measured: false,
            wear: Some(w),
            wear_time: "w".into(),
            wear_widen_measured: Some(false),
            wear_failed_eye: String::new(),
            history: Vec::new(),
            setup_done: false,
        };
        let back = CalibFile::parse(&file.to_json()).unwrap();
        assert_eq!(back.user, file.user);
        assert_eq!(back.wear_widen_measured, Some(false));
        let measured = CalibFile { wear: Some(w), wear_widen_measured: Some(true), ..CalibFile::default() };
        assert_eq!(CalibFile::parse(&measured.to_json()).unwrap().wear_widen_measured, Some(true));
        let older = CalibFile { wear_widen_measured: None, ..measured };
        assert_eq!(CalibFile::parse(&older.to_json()).unwrap().wear_widen_measured, None, "a file from before it");
        // A file without a history takes its last wear calibration as the history.
        assert_eq!(back.history.len(), 1);
        assert!(back.setup_done, "a history means the setup was done");
        assert!((back.history_params().unwrap()[0].step - 0.2).abs() < 1e-9);
        assert_eq!(back.user_warnings, file.user_warnings);
        // A left/right difference is a warning, not a failure; a lower lid moving up still fails.
        let mut lr = u.clone();
        lr.retain(|s| s.label != Label::LookDown);
        lr.extend(samples(Label::LookDown, 200, 0.6, 0.8, -35.0, 256.0, true));
        let (_, warn) = fit_user(&[u.clone(), lr], &w, None, true).unwrap();
        assert!(warn.iter().any(|m| m.contains("左右の差")), "{warn:?}");
        let mut up = u.clone();
        up.retain(|s| s.label != Label::LookDown);
        up.extend(samples(Label::LookDown, 200, 0.6, 1.1, -35.0, 240.0, true));
        assert!(fit_user(&[up.clone(), up], &w, None, true).unwrap_err().contains("下まぶたが上に"));
        assert_eq!(back.wear, file.wear);
    }

    /// A wear calibration's samples for one eye: `pupil` of the 300 normal frames with the pupil, `line` of those
    /// also with the upper lid's skin line.
    fn wear_eye(pupil: usize, line: usize) -> Vec<Sample> {
        let mut eye = Vec::new();
        eye.extend(samples(Label::Normal, 300, 0.73, 1.44, -14.0, 250.0, true));
        eye.extend(samples(Label::Widen, 200, 0.93, 1.6, -14.0, 252.0, true));
        eye.extend(samples(Label::Close, 80, f64::NAN, 0.56, -14.0, 240.0, false));
        for (i, x) in eye.iter_mut().filter(|x| x.label == Label::Normal).enumerate() {
            x.ok = i < pupil;
            if i >= line {
                x.skin = f64::NAN;
            }
        }
        eye
    }

    #[test]
    fn wear_calibration_checks_both_eyes_and_names_them() {
        let good = wear_eye(300, 300);
        // Both eyes without the pupil: "both", with each eye's count.
        let err = fit_wear(&[wear_eye(12, 12), wear_eye(30, 30)], [0.19; 2]).unwrap_err();
        assert_eq!(err, format!("両目{MSG_PUPIL}[左 12/300・右 30/300、90 必要]"));
        assert!(fit_wear_settled(&[wear_eye(12, 12), wear_eye(30, 30)], [0.19; 2], [None; 2], None).is_err());
        // One eye: named, and the right eye is not hidden behind the left one any more.
        assert_eq!(fit_wear(&[good.clone(), wear_eye(40, 40)], [0.19; 2]).unwrap_err(), format!("右目{MSG_PUPIL}[40/300、90 必要]"));
        assert_eq!(fit_wear(&[wear_eye(89, 89), good.clone()], [0.19; 2]).unwrap_err(), format!("左目{MSG_PUPIL}[89/300、90 必要]"));
        // The pupil was seen but the upper lid's line was not: its own message.
        let eyes = fit_wear_eyes(&[wear_eye(300, 50), good.clone()], [0.19; 2]);
        assert_eq!(eyes[0], Err(WearFail::LidLine { seen: 50, frames: 300, need: 90 }));
        assert_eq!(wear_fail_message(&eyes), format!("左目{MSG_LID_LINE}[50/300、90 必要]"));
        // Different problems on the two eyes: both messages.
        let mut no_close = good.clone();
        no_close.retain(|x| x.label != Label::Close);
        let err = fit_wear(&[wear_eye(0, 0), no_close], [0.19; 2]).unwrap_err();
        assert_eq!(err, format!("左目{MSG_PUPIL}[0/300、90 必要]。右目{MSG_CLOSE}"));
        // Enough frames on both: the levels.
        let (w, measured) = fit_wear(&[good.clone(), wear_eye(90, 90)], [0.19; 2]).unwrap();
        assert!(measured && (w[1].b_n - 0.73).abs() < 1e-9);
    }

    /// A wear calibration's samples for one eye at `fps`: the protocol's steps (5.4 s normal, 4.4 s widen, 1.2 s
    /// close) with the pupil in `pupil` of the normal frames.
    fn wear_eye_at(fps: f64, pupil: usize) -> Vec<Sample> {
        let n = |s: f64| (s * fps).round() as usize;
        let mut eye = Vec::new();
        eye.extend(samples(Label::Normal, n(5.4), 0.73, 1.44, -14.0, 250.0, true));
        eye.extend(samples(Label::Widen, n(4.4), 0.93, 1.6, -14.0, 252.0, true));
        eye.extend(samples(Label::Close, n(1.2), f64::NAN, 0.56, -14.0, 240.0, false));
        for (i, x) in eye.iter_mut().enumerate() {
            x.t = i as f64 / fps;
        }
        for (i, x) in eye.iter_mut().filter(|x| x.label == Label::Normal).enumerate() {
            x.ok = i < pupil;
        }
        eye
    }

    #[test]
    fn calibration_needs_follow_the_frames_the_cameras_delivered() {
        // At 90 fps the old counts: 90 of 486 normal frames, 60 widen, 20 close.
        let at90 = wear_eye_at(90.0, 486);
        assert_eq!(WearNeeds::of(&at90), WearNeeds { fps: 90.0, normal: 90, widen: 60, close: 20 });
        // At 15 fps (issue #23: "[L 81/81, R 81/81, 90 needed]"): about a second's worth.
        let at15 = wear_eye_at(15.0, 81);
        assert_eq!(WearNeeds::of(&at15), WearNeeds { fps: 15.0, normal: 15, widen: 10, close: 3 });
        let (w, measured) = fit_wear(&[at15.clone(), at15.clone()], [0.19; 2]).unwrap();
        assert!(measured && (w[0].b_w - w[0].b_n - 0.2).abs() < 1e-9 && (w[1].ap_n - w[1].ap_cl - 0.88).abs() < 0.01);
        // Too few: the real need, and the frame rate when it is low.
        let err = fit_wear(&[wear_eye_at(15.0, 5), wear_eye_at(15.0, 3)], [0.19; 2]).unwrap_err();
        assert_eq!(err, format!("両目{MSG_PUPIL}[左 5/81・右 3/81、15 必要]。カメラの映像が毎秒 15 枚しか届いていない"));
        let err = fit_wear(&[wear_eye_at(72.0, 30), wear_eye_at(72.0, 20)], [0.19; 2]).unwrap_err();
        assert_eq!(err, format!("両目{MSG_PUPIL}[左 30/389・右 20/389、72 必要]"), "72 fps is not low");
        // Eyes needing different counts (their cameras' rates differ): one message each.
        let err = fit_wear(&[wear_eye_at(15.0, 5), wear_eye_at(18.0, 3)], [0.19; 2]).unwrap_err();
        assert_eq!(err, format!("左目{MSG_PUPIL}[5/81、15 必要]。右目{MSG_PUPIL}[3/97、18 必要]。カメラの映像が毎秒 15 枚しか届いていない"));
        // The share keeps a check from passing on a few frames when the rate is not known.
        assert_eq!(NEED_NORMAL.frames(486, f64::NAN), 88);
        assert_eq!(NEED_NORMAL.frames(30, f64::NAN), 12);
        // The report has the needs per eye and the rate.
        let (report, lines) = wear_report(&[at15.clone(), at90.clone()]);
        let j = crate::json::parse(&report).unwrap();
        let th = j.get("thresholds").unwrap();
        let arr = |k: &str| th.get(k).and_then(Json::arr).unwrap().iter().map(|v| v.num().unwrap()).collect::<Vec<_>>();
        assert_eq!(arr("fps"), [15.0, 90.0]);
        assert_eq!(arr("normal_frames_min"), [15.0, 90.0]);
        assert_eq!((arr("widen_frames_min"), arr("close_frames_min")), (vec![10.0, 60.0], vec![3.0, 20.0]));
        assert!(lines[0].contains("(need 15 / 10), close 18 (need 3; at 15 fps)"), "{}", lines[0]);
        // User calibration: 150 points at 90 fps, 25 at 15.
        let user_eye = |fps: f64| {
            let n = |s: f64| (s * fps).round() as usize;
            let mut u = Vec::new();
            u.extend(samples(Label::LeadIn, n(2.2), 0.73, 1.44, -14.0, 250.0, true));
            u.extend(samples(Label::Squint, n(4.2), 0.6, 0.83, -14.0, 249.0, true));
            u.extend(samples(Label::LookUp, n(4.2), 0.73, 1.6, 5.0, 248.0, true));
            u.extend(samples(Label::LookDown, n(4.2), 0.6, 1.1, -35.0, 256.0, true));
            for (i, x) in u.iter_mut().enumerate() {
                x.t = i as f64 / fps;
            }
            u
        };
        assert_eq!(UserNeeds::of(&user_eye(90.0)), UserNeeds { fps: 90.0, points: 150, squint: 30, look_down: 30 });
        assert_eq!(UserNeeds::of(&user_eye(15.0)), UserNeeds { fps: 15.0, points: 25, squint: 5, look_down: 5 });
        assert!(fit_user(&[user_eye(15.0), user_eye(15.0)], &w, None, true).is_ok());
        let mut blind = user_eye(15.0);
        blind.iter_mut().for_each(|x| x.ok = false);
        assert_eq!(
            fit_user(&[blind, user_eye(15.0)], &w, None, true).unwrap_err(),
            format!("左目{MSG_PUPIL}[0/159、25 必要]。カメラの映像が毎秒 15 枚しか届いていない")
        );
    }

    #[test]
    fn one_eye_failing_keeps_its_earlier_levels() {
        let good = wear_eye(300, 300);
        let old = WearParams { r_px: 60.0, b_n: 0.7, b_w: 0.9, ap_n: 1.5, ap_cl: 0.6, pitch_n: -12.0, lower_px: 245.0 };
        // The right eye failed: the left one's new levels, the right one's previous ones.
        let out = fit_wear_settled(&[good.clone(), wear_eye(12, 12)], [0.19; 2], [Some(old); 2], None).unwrap();
        assert_eq!(out.failed, [false, true]);
        assert_eq!(out.failed_eye(), "R");
        assert_eq!(out.wear[1], old);
        assert!((out.wear[0].b_n - 0.73).abs() < 1e-9 && out.widen_measured && !out.for_history());
        assert_eq!(out.message, "校正できた（右目は瞳がうまく見えなかったので、前の値を使うよ）[12/300、90 必要]");
        // No earlier levels: provisional ones, the other eye's with the history's step, gap and radius.
        let hist = History { step: 0.25, gap: 0.8, r_px: 66.0 };
        let out = fit_wear_settled(&[wear_eye(300, 20), good.clone()], [0.19; 2], [None; 2], Some([hist, hist])).unwrap();
        assert_eq!(out.failed_eye(), "L");
        let (l, r) = (out.wear[0], out.wear[1]);
        assert_eq!((l.r_px, l.b_n, l.ap_n), (66.0, r.b_n, r.ap_n));
        assert!((l.b_w - l.b_n - 0.25).abs() < 1e-9 && (l.ap_n - l.ap_cl - 0.8).abs() < 1e-9 && l.lower_px.is_nan());
        assert_eq!(out.message, "校正できた（左目は上まぶたの線が見つからなかったので、仮の値を使うよ）[20/300、90 必要]");
        // Without a history either: the defaults, and the radius in use during the calibration.
        let w = provisional_wear(&wear_eye(0, 0), &r, None);
        assert_eq!(w.r_px, 55.0);
        assert!((w.b_w - w.b_n - DEFAULT_WIDEN_STEP).abs() < 1e-9 && (w.ap_n - w.ap_cl - DEFAULT_OPEN_GAP).abs() < 1e-9);
        // With the widen not caught on the eye that went through, both notes.
        let mut flat = good.clone();
        for x in flat.iter_mut().filter(|x| x.label == Label::Widen) {
            x.skin = 0.74;
        }
        let out = fit_wear_settled(&[flat, wear_eye(0, 0)], [0.19; 2], [Some(old); 2], None).unwrap();
        assert!(!out.widen_measured);
        assert_eq!(
            out.message,
            "校正できた（右目は瞳がうまく見えなかったので、前の値を使うよ。見開きは取れなかったので、いつもの幅を使うよ）[0/300、90 必要]"
        );
        // Both eyes fine: as before.
        let out = fit_wear_settled(&[good.clone(), good.clone()], [0.19; 2], [Some(old); 2], None).unwrap();
        assert!(out.for_history() && out.failed_eye().is_empty() && out.message == "校正できた（かぶり）");
        // The same eye failing again: what it has was not measured on it, so provisional ones from this wear.
        let file = CalibFile { wear: Some([old, old]), wear_failed_eye: "R".into(), ..CalibFile::default() };
        assert_eq!(file.measured_wear(), [Some(old), None]);
        let out = fit_wear_settled(&[good.clone(), wear_eye(12, 12)], [0.19; 2], file.measured_wear(), None).unwrap();
        assert_eq!((out.wear[1].b_n, out.wear[1].ap_n), (out.wear[0].b_n, out.wear[0].ap_n));
        assert_eq!(out.message, "校正できた（右目は瞳がうまく見えなかったので、仮の値を使うよ）[12/300、90 必要]");
        // The other eye failing now: its levels from then were measured.
        let out = fit_wear_settled(&[wear_eye(12, 12), good.clone()], [0.19; 2], file.measured_wear(), None).unwrap();
        assert_eq!(out.wear[0], old);
        assert!(out.message.contains("左目は瞳がうまく見えなかったので、前の値を使うよ"), "{}", out.message);
        // calib.json keeps which eye failed; older files have none.
        let file = CalibFile { wear: Some(out.wear), wear_failed_eye: "R".into(), ..CalibFile::default() };
        assert!(file.to_json().contains("\"failed_eye\": \"R\""));
        assert_eq!(CalibFile::parse(&file.to_json()).unwrap().wear_failed_eye, "R");
        let older = CalibFile { wear_failed_eye: String::new(), ..file };
        assert!(!older.to_json().contains("failed_eye"));
        assert_eq!(CalibFile::parse(&older.to_json()).unwrap().wear_failed_eye, "");
    }

    #[test]
    fn wear_report_says_why_frames_had_no_pupil() {
        let mut eye = wear_eye(300, 250);
        for (i, x) in eye.iter_mut().enumerate() {
            x.seen = Seen { cx: 240.0, cy: 190.0, x_min: 186.0, xmax: 346.0, contrast: 90.0, ..Seen::default() };
            if x.label == Label::Normal && i % 10 == 0 {
                (x.ok, x.seen.miss, x.seen.rejected) = (false, Miss::NoBlob, feat::REJ_TOP | feat::REJ_SHAPE);
            }
            if x.label == Label::Normal && i % 10 == 5 {
                (x.ok, x.seen.miss) = (false, Miss::Refine);
            }
            x.seen.prior_reset = i == 7;
        }
        let (report, lines) = wear_report(&[eye.clone(), eye]);
        let j = crate::json::parse(&report).unwrap();
        let l = j.get("L").unwrap();
        let num = |v: &Json, k: &str| v.get(k).and_then(Json::num).unwrap();
        assert_eq!(num(l, "normal_frames_with_pupil"), 240.0);
        assert_eq!(num(l, "normal_frames_with_lid_line"), 200.0);
        let d = l.get("diag").unwrap();
        assert!((num(d, "fps") - 90.0).abs() < 1e-6);
        assert_eq!((num(d, "search_x_min"), num(d, "search_xmax"), num(d, "normal_pupil_x")), (186.0, 346.0, 240.0));
        assert_eq!(num(d, "normal_contrast"), 90.0);
        let normal = d.get("steps").unwrap().get("normal").unwrap();
        assert_eq!(
            ["frames", "pupil", "lid_line", "no_blob", "no_blob_edge", "no_blob_shape", "no_blob_too_big", "refine_failed", "prior_resets"]
                .map(|k| num(normal, k)),
            [300.0, 240.0, 200.0, 30.0, 30.0, 30.0, 0.0, 30.0, 1.0]
        );
        assert_eq!(num(d.get("steps").unwrap().get("close").unwrap(), "frames"), 80.0);
        assert!(lines[0].contains("frames with pupil normal 240 of 300, with the upper lid line 200"), "{}", lines[0]);
        assert!(lines[0].contains("no blob 30 (edge 30, too big 0, shape 30), refine 30"), "{}", lines[0]);
    }

    #[test]
    fn user_calibration_checks_both_eyes() {
        let w = WearParams { r_px: 55.0, b_n: 0.73, b_w: 0.93, ap_n: 1.441, ap_cl: 0.561, pitch_n: -14.0, lower_px: 250.0 };
        let user = |squint_ap: f64, ok: bool| {
            let mut u = Vec::new();
            u.extend(samples(Label::LeadIn, 200, 0.73, 1.44, -14.0, 250.0, ok));
            u.extend(samples(Label::LookUp, 200, 0.73, 1.6, 5.0, 248.0, ok));
            u.extend(samples(Label::LookDown, 200, 0.6, 1.1, -35.0, 256.0, ok));
            u.extend(samples(Label::Squint, 200, 0.6, squint_ap, -14.0, 249.0, true));
            u
        };
        // A shallow squint on both eyes: one message for both, with both values.
        let err = fit_user(&[user(1.4, true), user(1.38, true)], &[w, w], None, true).unwrap_err();
        assert_eq!(err, format!("両目{MSG_SQUINT_SHALLOW}[左 f_sq 0.95・右 f_sq 0.93]"));
        // Only the right eye: the right eye (it used to be found only after the left one passed).
        assert_eq!(
            fit_user(&[user(0.83, true), user(1.4, true)], &[w, w], None, true).unwrap_err(),
            format!("右目{MSG_SQUINT_SHALLOW}[f_sq 0.95]")
        );
        // The pupil not seen in the neutral steps: the pupil, not Valve's gaze.
        assert_eq!(
            fit_user(&[user(0.83, false), user(0.83, true)], &[w, w], None, true).unwrap_err(),
            format!("左目{MSG_PUPIL}[0/600、150 必要]")
        );
        // Valve's gaze missing: that message, as before.
        let mut no_gaze = user(0.83, true);
        no_gaze.iter_mut().for_each(|x| x.pitch = f64::NAN);
        assert_eq!(fit_user(&[no_gaze.clone(), no_gaze], &[w, w], None, true).unwrap_err(), MSG_NO_GAZE);
        assert!(fit_user(&[user(0.83, true), user(0.83, true)], &[w, w], None, true).is_ok());
    }

    /// A grey frame with the lens-edge band, and a dark disc (the pupil) if `pupil`.
    fn disc_frame(pupil: Option<(f64, f64, f64)>) -> Vec<u8> {
        let mut img = vec![120u8; vision::W * vision::H];
        for y in 0..vision::H {
            for x in 0..vision::W {
                let v = &mut img[y * vision::W + x];
                if x >= 350 {
                    *v = 4;
                } else if let Some((px, py, r)) = pupil {
                    let d = (x as f64 - px).hypot(y as f64 - py);
                    if d < r {
                        *v = 12;
                    } else if d < 2.2 * r {
                        *v = 70;
                    }
                }
            }
        }
        img
    }

    #[test]
    fn the_search_prior_is_forgotten_after_ten_frames_without_a_pupil() {
        let mut e = EyeEngine::default();
        let p = Params::default();
        let eye = disc_frame(Some((250.0, 200.0, 25.0)));
        let blank = disc_frame(None);
        let o = e.process(&eye, f64::NAN, 0.0, &p);
        assert!(o.f.ok_pupil && e.prev.is_some());
        assert_eq!(o.x_min, feat::X_MIN_LO, "no dark shading: the window opens all the way");
        let misses = frames_in(PRIOR_RESET_S, FPS, 2);
        assert_eq!(misses, 10);
        for k in 1..=misses {
            let o = e.process(&blank, f64::NAN, k as f64 / FPS, &p);
            assert!(!o.f.ok_pupil);
            assert_eq!(o.f.diag.prior_reset, k == misses, "frame {k}");
            assert_eq!(e.prev.is_some(), k < misses);
        }
        assert!(e.prev_p.is_some(), "the lids are still measured where the pupil was");
        let o = e.process(&blank, f64::NAN, 1.0, &p);
        assert!(!o.f.diag.prior_reset, "once");
        assert!(e.process(&eye, f64::NAN, 1.1, &p).f.ok_pupil && e.prev.is_some());
        e.reset_tracking();
        assert!(e.prev.is_none() && e.prev_p.is_none() && e.nopupil == 0);
        assert!(e.r_est.len() > 0 || e.cm_n > 0, "what belongs to the wear stays");
    }

    #[test]
    fn at_15_fps_the_prior_goes_after_the_same_time() {
        let mut e = EyeEngine::default();
        let p = Params::default();
        let eye = disc_frame(Some((250.0, 200.0, 25.0)));
        let blank = disc_frame(None);
        for k in 0..10 {
            assert!(e.process(&eye, f64::NAN, k as f64 / 15.0, &p).f.ok_pupil);
        }
        assert_eq!(e.fps(), 15.0);
        // 2 frames at 15 fps (133 ms; 10 frames at 90 fps are 111 ms).
        assert!(!e.process(&blank, f64::NAN, 10.0 / 15.0, &p).f.diag.prior_reset);
        assert!(e.process(&blank, f64::NAN, 11.0 / 15.0, &p).f.diag.prior_reset);
        assert!(e.prev.is_none());
    }

    /// A closing eye: a dark band of lashes over the box, and a small dark blob on it.
    fn lash_frame(blob: (f64, f64, f64)) -> Vec<u8> {
        let mut img = disc_frame(None);
        for y in 120..280 {
            for x in 150..350 {
                let d = (x as f64 - blob.0).hypot(y as f64 - blob.1);
                img[y * vision::W + x] = if d < blob.2 { 10 } else { 55 };
            }
        }
        img
    }

    #[test]
    fn a_small_blob_found_without_the_prior_must_look_like_the_open_pupil() {
        let p = Params::default();
        let eye = disc_frame(Some((250.0, 200.0, 25.0)));
        let lash = lash_frame((282.0, 254.0, 10.5));
        let trained = || {
            let mut e = EyeEngine::default();
            for k in 0..80 {
                assert!(e.process(&eye, f64::NAN, k as f64 / FPS, &p).f.ok_pupil);
            }
            e
        };
        // Next to the prior, the lash blob's box is not bright: the prototype's rule takes it.
        let mut anchored = trained();
        anchored.prev = Some((280.0, 250.0));
        let o = anchored.process(&lash, f64::NAN, 1.0, &p);
        assert!(o.f.ok_pupil && !o.f.diag.gated, "{:?}", o.f.diag);
        // Without the prior it is refused: under half the open pupil's size.
        let mut e = trained();
        e.prev = None;
        let o = e.process(&lash, f64::NAN, 1.0, &p);
        assert!(!o.f.ok_pupil && o.f.diag.gated, "{:?}", o.f.diag);
        assert!(e.prev.is_none());
        // The real pupil is still taken without the prior.
        let o = e.process(&eye, f64::NAN, 1.1, &p);
        assert!(o.f.ok_pupil && !o.f.diag.gated);
    }

    #[test]
    fn widen_sensitivity_spans_the_two_tested_curves() {
        let close = |a: (f64, f64), b: (f64, f64)| (a.0 - b.0).abs() < 1e-12 && (a.1 - b.1).abs() < 1e-12;
        assert!(close(wide_curve(0.0), (0.4, 0.6)));
        assert!(close(wide_curve(1.0), (0.25, 0.5)));
        let (s, w) = wide_curve(DEFAULT_WIDEN_SENSITIVITY);
        assert!((s + 0.5 * w - 0.6).abs() < 1e-12, "0.5 at 60% of the step");
        assert!(close(wide_curve(7.0), wide_curve(1.0)) && close(wide_curve(f64::NAN), wide_curve(DEFAULT_WIDEN_SENSITIVITY)));
    }

    #[test]
    fn history_round_trips_and_gives_medians() {
        let mut file = CalibFile::default();
        assert!(file.history_params().is_none());
        let rec = |step, gap, r| WearRecord { step, gap, r_px: r };
        file.history = vec![
            ("a".into(), [rec(0.20, 0.9, 50.0), rec(0.18, 0.8, 60.0)]),
            ("b".into(), [rec(0.30, 1.0, 52.0), rec(0.22, 0.9, 62.0)]),
            ("c".into(), [rec(0.22, 1.1, 54.0), rec(0.20, 1.0, 64.0)]),
        ];
        let back = CalibFile::parse(&file.to_json()).unwrap();
        assert_eq!(back.history, file.history);
        let partial = CalibFile { setup_done: true, ..CalibFile::default() };
        assert!(CalibFile::parse(&partial.to_json()).unwrap().setup_done);
        assert!(!CalibFile::parse(&CalibFile::default().to_json()).unwrap().setup_done);
        let h = back.history_params().unwrap();
        assert_eq!((h[0].step, h[0].gap, h[0].r_px), (0.22, 1.0, 52.0));
        assert_eq!((h[1].step, h[1].gap, h[1].r_px), (0.20, 0.9, 62.0));
        let w = WearParams { r_px: 51.0, b_n: 0.8, b_w: 1.0, ap_n: 1.5, ap_cl: 0.6, pitch_n: -14.0, lower_px: 240.0 };
        for _ in 0..HISTORY_MAX + 5 {
            file.push_history("x", &[w, w]);
        }
        assert_eq!(file.history.len(), HISTORY_MAX);
        assert!((file.history.last().unwrap().1[0].step - 0.2).abs() < 1e-9);
    }

    #[test]
    fn calib_files_from_before_anatomical_eyes_are_read_for_the_other_eye() {
        let rec = |step, gap, r| WearRecord { step, gap, r_px: r };
        let wl = WearParams { r_px: 51.0, b_n: 0.8, b_w: 1.0, ap_n: 1.5, ap_cl: 0.6, pitch_n: -14.0, lower_px: 240.0 };
        let wr = WearParams { r_px: 61.0, b_n: 0.7, b_w: 0.9, ap_n: 1.4, ap_cl: 0.5, pitch_n: -12.0, lower_px: 230.0 };
        let ul = UserParams { f_sq: 0.31, pd_min: 2.0, pd_max: 6.0, ..UserParams::default() };
        let ur = UserParams { f_sq: 0.42, pd_min: 2.5, pd_max: 6.5, ..UserParams::default() };
        let file = CalibFile {
            user: Some([ul, ur]),
            user_time: "u".into(),
            user_warnings: vec![format!("左目{MSG_SQUINT_SHALLOW}[f_sq 0.95]"), "左右の差が大きい [下を見たとき 0.39、細め 0.08]".into()],
            pupil_measured: true,
            wear: Some([wl, wr]),
            wear_time: "w".into(),
            wear_widen_measured: Some(true),
            wear_failed_eye: "L".into(),
            history: vec![("a".into(), [rec(0.20, 0.9, 50.0), rec(0.18, 0.8, 60.0)])],
            setup_done: true,
        };
        // Written now: marked, read back as it is.
        let text = file.to_json();
        assert!(text.contains(&format!("\n  {JSON_EYES_ANATOMICAL},\n")), "{text}");
        let (back, converted) = CalibFile::parse_converting(&text).unwrap();
        assert!(!converted);
        assert_eq!((back.user, back.wear, &back.history), (file.user, file.wear, &file.history));
        assert_eq!((back.wear_failed_eye.as_str(), &back.user_warnings), ("L", &file.user_warnings));
        // The same file as builds before 2026-10-07 wrote it (no marker): its L was the right eye.
        let old = text.replace(&format!("  {JSON_EYES_ANATOMICAL},\n"), "").replace("\"version\": 2", "\"version\": 1");
        assert!(!old.contains("\"eyes\""), "{old}");
        let (conv, converted) = CalibFile::parse_converting(&old).unwrap();
        assert!(converted);
        assert_eq!(conv.user, Some([ur, ul]));
        assert_eq!(conv.wear, Some([wr, wl]));
        assert_eq!(conv.history, vec![("a".to_string(), [rec(0.18, 0.8, 60.0), rec(0.20, 0.9, 50.0)])]);
        assert_eq!(conv.wear_failed_eye, "R");
        assert_eq!(conv.user_warnings, [format!("右目{MSG_SQUINT_SHALLOW}[f_sq 0.95]"), "左右の差が大きい [下を見たとき 0.39、細め 0.08]".into()]);
        assert_eq!((conv.pupil_measured, conv.setup_done, conv.wear_widen_measured), (true, true, Some(true)));
        // Saved again it is marked, and read as it is from then on.
        let (again, converted) = CalibFile::parse_converting(&conv.to_json()).unwrap();
        assert!(!converted);
        assert_eq!((again.user, again.wear, again.wear_failed_eye.as_str()), (Some([ur, ul]), Some([wr, wl]), "R"));
        // An old file from before the history: its last wear calibration becomes the history, swapped too.
        let older = CalibFile { history: Vec::new(), ..file.clone() }.to_json().replace(&format!("  {JSON_EYES_ANATOMICAL},\n"), "");
        let (conv, _) = CalibFile::parse_converting(&older).unwrap();
        assert_eq!(conv.history[0].1, [WearRecord::of(&wr), WearRecord::of(&wl)]);
    }

    #[test]
    fn eye_words_of_old_messages_trade_places() {
        assert_eq!(
            swap_eye_words("校正できた（右目は瞳がうまく見えなかったので、前の値を使うよ）[0/486、90 必要]"),
            "校正できた（左目は瞳がうまく見えなかったので、前の値を使うよ）[0/486、90 必要]"
        );
        assert_eq!(
            swap_eye_words(&format!("両目{MSG_PUPIL}[左 12/486・右 30/486、90 必要]。左右の差が大きい")),
            format!("両目{MSG_PUPIL}[右 12/486・左 30/486、90 必要]。左右の差が大きい")
        );
        assert_eq!(other_eye_name("L"), "R");
        assert_eq!(other_eye_name("LR"), "LR");
    }

    #[test]
    fn history_from_old_results_is_read_for_the_other_eye() {
        let dir = std::env::temp_dir().join(format!("eyecam-history-results-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let values = r#""values": {"L": {"widen_step": 0.2, "open_gap": 0.9, "r_px": 50}, "R": {"widen_step": 0.3, "open_gap": 1.1, "r_px": 60}}"#;
        for (name, marker) in [("calib_2026-10-05_19-51-03", String::new()), ("calib_2026-10-08_09-00-00", format!("{JSON_EYES_ANATOMICAL}, "))] {
            std::fs::create_dir_all(dir.join(name)).unwrap();
            let text = format!("{{\"kind\": \"wear\", {marker}\"time\": \"{name}\", \"ok\": true, {values}}}");
            std::fs::write(dir.join(name).join("calib_result.json"), text).unwrap();
        }
        let h = CalibFile::history_from_results(&dir);
        let (a, b) = (rec_of(0.2, 0.9, 50.0), rec_of(0.3, 1.1, 60.0));
        assert_eq!(h, vec![("calib_2026-10-05_19-51-03".to_string(), [b, a]), ("calib_2026-10-08_09-00-00".to_string(), [a, b])]);
        std::fs::remove_dir_all(&dir).unwrap();
    }

    fn rec_of(step: f64, gap: f64, r_px: f64) -> WearRecord {
        WearRecord { step, gap, r_px }
    }

    #[test]
    fn auto_baseline_is_the_mode_of_the_warm_up() {
        // A wearer who mostly looks straight (0.80 px-units) but glances and widens a lot (tails): the mode, not the
        // mean, is the open level.
        // 30 s of usable frames: 2700 at 90 fps, 450 at 15.
        for (fps, frames) in [(FPS, 2700), (15.0, 450)] {
            let mut b = AutoBase::default();
            assert_eq!(b.warmup_remaining_s(), WARMUP_S);
            for i in 0..frames {
                let skin = match i % 10 {
                    0..=5 => 40.0 + (i % 3) as f64 * 0.1,
                    6 | 7 => 46.0,
                    _ => 30.0 + (i % 7) as f64,
                };
                assert!(!b.ready, "{fps} fps, frame {i}");
                b.warm(skin, 80.0, 0.5, 1.0 / fps);
                if i == frames / 2 - 1 {
                    assert!((b.warmup_remaining_s() - 15.0).abs() < 1e-6, "{}", b.warmup_remaining_s());
                }
            }
            assert!(b.ready, "{fps} fps");
            // Within the 5-bin smoothing window (2 bins of 0.5) of the true level.
            assert!((b.b - 40.1).abs() <= 1.25, "{}", b.b);
            assert!((b.ap_n - 80.0).abs() <= 1.25, "{}", b.ap_n);
            assert_eq!(b.warmup_remaining_s(), 0.0);
        }
        let mut c = AutoBase::default();
        c.seed(41.0, 79.0);
        assert!(c.ready && c.b == 41.0);
    }
}
