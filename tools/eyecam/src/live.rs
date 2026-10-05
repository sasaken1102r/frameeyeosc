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

pub const FPS: f64 = 90.0;
/// Assumed iris radius in mm (HVID 11.8 mm); only scales pupil_mm.
pub const R_MM: f64 = 5.9;
/// Frames that wide/squint stay 0 after the eye reopens: int(0.08 s * 90).
const HOLD_FRAMES: u32 = 7;
/// A pupil counts as seen whole when at least this share of the 64 rays reached its edge ...
const PUPIL_MIN_VIS: f64 = 0.75;
/// ... and its roundness (b/a) is within this of the usual one.
const PUPIL_MAX_BA_DEV: f64 = 0.08;
/// Frames after a blink before the pupil size is trusted again (300 ms).
const PUPIL_SETTLE_FRAMES: u32 = 27;
/// After this many frames in a row without a pupil, the search forgets where the pupil was (its prior): a wrong
/// blob caught once (during a blink, say) would otherwise keep the real pupil out of reach for seconds.
pub const PRIOR_RESET_MISSES: u32 = 10;
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
/// EyeWide shows above `WIDE_OFF` only once it has stayed above 0.5 for this many frames (100 ms), and then until
/// it falls below `WIDE_OFF` (hysteresis): short spikes of the skin line stay at most `WIDE_OFF`.
const WIDE_ON_FRAMES: u32 = 9;
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

/// A NaN-aware causal running median over the last `n` frames.
struct Win {
    v: VecDeque<f64>,
    n: usize,
}

impl Win {
    fn new(n: usize) -> Self {
        Self { v: VecDeque::with_capacity(n), n }
    }

    fn push(&mut self, x: f64) -> f64 {
        if self.v.len() == self.n {
            self.v.pop_front();
        }
        self.v.push_back(x);
        let mut f: Vec<f64> = self.v.iter().copied().filter(|x| x.is_finite()).collect();
        vision::median(&mut f)
    }

}

/// A long-run median: keeps the last `cap` samples, recomputed every `every` pushes.
struct LongMedian {
    v: VecDeque<f64>,
    cap: usize,
    every: usize,
    since: usize,
    pub value: f64,
}

impl LongMedian {
    fn new(cap: usize, every: usize) -> Self {
        Self { v: VecDeque::with_capacity(cap), cap, every, since: 0, value: f64::NAN }
    }

    fn push(&mut self, x: f64) {
        if !x.is_finite() {
            return;
        }
        if self.v.len() == self.cap {
            self.v.pop_front();
        }
        self.v.push_back(x);
        self.since += 1;
        // Recompute often while warming up, then once per `every` samples.
        if self.since >= self.every || self.v.len() < 64 {
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
    cm_sum: Vec<f64>,
    cm_n: u32,
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
    closed_hist: u32,
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
    since: usize,
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
            since: 0,
            b: f64::NAN,
            ap_n: f64::NAN,
            ready: false,
            pitch_typ: LongMedian::new(2700, 45),
        }
    }
}

const WARMUP_FRAMES: usize = (WARMUP_S * FPS) as usize;

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
        if self.ready { 0.0 } else { (WARMUP_FRAMES - self.skin.len().min(WARMUP_FRAMES)) as f64 / FPS }
    }

    /// Add one usable frame's levels during warm-up (`bw`: histogram bin, px).
    fn warm(&mut self, skin: f64, ap: f64, bw: f64) {
        if self.ready {
            return;
        }
        self.skin.push(skin);
        self.ap.push(ap);
        self.since += 1;
        if (self.since >= 45 && self.skin.len() >= 20) || self.skin.len() >= WARMUP_FRAMES {
            self.since = 0;
            self.b = mode_est(&self.skin, bw);
            let a = mode_est(&self.ap, bw);
            if a.is_finite() {
                self.ap_n = a;
            }
        }
        if self.skin.len() >= WARMUP_FRAMES && self.b.is_finite() {
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
            cm_sum: vec![0.0; vision::W],
            cm_n: 0,
            xmax: 346.0,
            x_min: feat::X_MIN,
            // One sample every 2nd frame: 30 s for R, 60 s for the open box level.
            r_est: LongMedian::new(1350, 45),
            skin: Win::new(5),
            ap: Win::new(5),
            lo: Win::new(5),
            bx: Win::new(5),
            pd: Win::new(5),
            nopupil: 0,
            box_open: LongMedian::new(2700, 45),
            a_open: LongMedian::new(900, 45),
            inner_open: LongMedian::new(900, 45),
            ba_open: LongMedian::new(900, 45),
            since_closed: u32::MAX / 2,
            closed_hist: 0,
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

/// Hysteresis on EyeWide (see `WIDE_ON_FRAMES`).
#[derive(Clone, Copy, Debug, Default)]
struct WideGate {
    on: bool,
    run: u32,
}

impl WideGate {
    /// The value to report for this frame's ungated EyeWide `w`.
    fn step(&mut self, w: f64) -> f64 {
        self.run = if w > 0.5 { self.run + 1 } else { 0 };
        if w < WIDE_OFF {
            self.on = false;
        } else if self.run >= WIDE_ON_FRAMES {
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
        // Lens edge: the first frame, then the mean of one frame every 0.5 s for the first 20 s.
        if self.frame_no == 1 || (self.cm_n < 40 && self.frame_no.is_multiple_of(45)) {
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
        let (mut f, mut p) = self.ex.extract(img, self.xmax, self.prev, r_ref, self.prev_p.as_ref());
        // A "pupil" found while the iris box is as bright as lid skin must look like the open eye's pupil (as dark, mostly
        // visible, 0.45-1.3 x its size); otherwise it is a dark crease or lash line on the closed lid. Bright light also
        // lights the box this much, and the pupil then shrinks to 0.55-0.65 x its usual size but stays as dark and whole
        // (creases on the closed lid: interior 24-28 levels brighter, under half visible, 0.23-0.42 x the size).
        let box_bright = self.box_open.len() >= 30 && f.box_mean > 1.3 * self.box_open.value;
        if let Some(pp) = p
            && box_bright
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
            if !box_bright && self.frame_no.is_multiple_of(2) {
                self.a_open.push(pp.a);
                self.inner_open.push(pp.inner);
                if pp.vis >= PUPIL_MIN_VIS {
                    self.ba_open.push(pp.b / pp.a);
                }
            }
        }
        if f.ok_pupil && f.iris_r.is_finite() && f.iris_n >= 4 && self.frame_no.is_multiple_of(2) {
            self.r_est.push(f.iris_r);
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
        // Lid geometry in pixels (5-frame causal medians); the outputs below are in iris radii.
        let skin_px = self.skin.push(f.pupil_cy - f.upper_skin_y);
        let ap_px = self.ap.push(f.lower_y - f.upper_y);
        let lo_px = self.lo.push(f.lower_y - f.pupil_cy);
        let (skin, ap, lo) = (skin_px / r, ap_px / r, lo_px / r);
        let box_f = self.bx.push(f.box_mean);
        // The pupil's size is only taken from a pupil seen whole: most rays reach its edge, its roundness is the
        // usual one for this camera angle (a lid cutting into it flattens the fit), and not while the lids are
        // still opening after a blink (300 ms). Otherwise the last good value is kept.
        let roundness_ok = self.ba_open.len() < 30 || (f.pupil_b / f.pupil_a - self.ba_open.value).abs() <= PUPIL_MAX_BA_DEV;
        let pupil_whole = f.ok_pupil && f.pupil_vis >= PUPIL_MIN_VIS && roundness_ok && self.since_closed > PUPIL_SETTLE_FRAMES;
        let pd = self.pd.push(if pupil_whole { 2.0 * f.pupil_a / r } else { f64::NAN });
        self.nopupil = if f.ok_pupil { 0 } else { self.nopupil + 1 };
        if self.nopupil >= PRIOR_RESET_MISSES && self.prev.is_some() {
            // prev_p stays: the lids and the box are still measured where the pupil last was.
            self.prev = None;
            f.diag.prior_reset = true;
        }
        if f.ok_pupil && self.frame_no.is_multiple_of(2) {
            self.box_open.push(box_f);
        }

        // Closed: lid skin covers the box (brighter than the open-eye level) and no pupil for 3 frames.
        let closed = self.box_open.len() >= 30 && box_f > 1.3 * self.box_open.value && self.nopupil >= 3;
        self.closed_hist = ((self.closed_hist << 1) | closed as u32) & ((1 << HOLD_FRAMES) - 1);
        let hold = self.closed_hist != 0;
        self.since_closed = if closed { 0 } else { self.since_closed.saturating_add(1) };

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
        if open_seen && pitch.is_finite() && self.frame_no.is_multiple_of(2) {
            self.base.pitch_typ.push(pitch);
        }
        let p_typ = self.base.pitch_typ.value;
        let usable = open_seen && (!pitch.is_finite() || (p_typ.is_finite() && (pitch - p_typ).abs() < PITCH_GATE));
        if usable && !self.base.ready {
            self.base.warm(skin_px, ap_px, 0.01 * r);
        } else if usable && params.tune.track && step_px > 0.0 {
            let k = 1.0 / (TRACK_TAU_S * FPS);
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
        let wide = self.wide_gate.step(wide);

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
        }
    }
}

fn med_of(s: &[Sample], pick: impl Fn(&Sample) -> Option<f64>) -> (f64, usize) {
    let mut v: Vec<f64> = s.iter().filter_map(pick).filter(|x| x.is_finite()).collect();
    let n = v.len();
    (vision::median(&mut v), n)
}

/// Levels from normal / widen / close frames. Both eyes need the normal and closed levels; an eye whose widen was not
/// caught gets `fallback_step` (iris radii) as its widen step instead of failing the calibration. Also returns whether
/// the widen step was measured on both eyes (only then does it belong in the history).
pub fn fit_wear(samples: &[Vec<Sample>; 2], fallback_step: [f64; 2]) -> Result<([WearParams; 2], bool), String> {
    let mut out = [None, None];
    let mut measured = true;
    for (e, s) in samples.iter().enumerate() {
        // One radius for the whole calibration (as the prototype's block calibration does): the running estimate
        // may still be settling while the steps are recorded.
        let r_px = calib_radius(s, &[Label::Normal, Label::Widen]);
        let s: Vec<Sample> = s.iter().map(|x| x.rescaled(r_px)).collect();
        let s = &s;
        let is = |l: Label| move |x: &Sample| x.label == l;
        let normal: Vec<Sample> = s.iter().copied().filter(is(Label::Normal)).collect();
        let widen: Vec<Sample> = s.iter().copied().filter(is(Label::Widen)).collect();
        let close: Vec<Sample> = s.iter().copied().filter(is(Label::Close)).collect();
        let (b_n, nn) = med_of(&normal, |x| x.ok.then_some(x.skin));
        let (b_w, nw) = med_of(&widen, |x| x.ok.then_some(x.skin));
        let (ap_n, _) = med_of(&normal, |x| x.ok.then_some(x.ap));
        let (ap_cl, nc) = med_of(&close, |x| Some(x.ap));
        let eye = ["左", "右"][e];
        if nn < MIN_NORMAL_FRAMES {
            return Err(format!("{eye}目の瞳がうまく見えなかった（HMD のかぶり方を直して、もう一度）"));
        }
        let widen_ok = nw >= MIN_WIDEN_FRAMES && b_w - b_n >= MIN_WIDEN_STEP;
        let b_w = if widen_ok { b_w } else { b_n + fallback_step[e] };
        measured &= widen_ok;
        if nc < MIN_CLOSE_FRAMES || ap_n - ap_cl < MIN_OPEN_GAP || (ap_n - ap_cl).is_nan() {
            return Err(format!("{eye}目を閉じたのが検出できなかった（もう一度、しっかり閉じてね）"));
        }
        let (pitch_n, _) = med_of(&normal, |x| Some(x.pitch));
        let (lower_px, _) = med_of(&normal, |x| x.ok.then_some(x.lower_px));
        out[e] = Some(WearParams { r_px, b_n, b_w, ap_n, ap_cl, pitch_n, lower_px });
    }
    Ok(([out[0].unwrap(), out[1].unwrap()], measured))
}

/// Pitch curve and squint depth (needs this wear's levels), with the sanity checks; pupil range kept from `old`.
/// With `strict` false the checks only produce warnings (for offline evaluation).
pub fn fit_user(
    samples: &[Vec<Sample>; 2],
    wear: &[WearParams; 2],
    old: Option<[UserParams; 2]>,
    strict: bool,
) -> Result<([UserParams; 2], Vec<String>), String> {
    let mut out = [UserParams::default(); 2];
    let mut deltas = [(0.0, 0.0); 2];
    let mut warnings = Vec::new();
    let mut check = |bad: bool, msg: String| -> Result<(), String> {
        if !bad {
            Ok(())
        } else if strict {
            Err(msg)
        } else {
            warnings.push(msg);
            Ok(())
        }
    };
    for (e, s) in samples.iter().enumerate() {
        let eye = ["左", "右"][e];
        let w = &wear[e];
        let s: Vec<Sample> = s.iter().map(|x| x.rescaled(w.r_px)).collect();
        let s = &s[..];
        let neutral = |l: Label| matches!(l, Label::LeadIn | Label::Normal | Label::LookUp | Label::LookDown | Label::Bright | Label::Dark);
        let pts: Vec<(f64, f64)> =
            s.iter().filter(|x| neutral(x.label) && x.ok && x.pitch.is_finite() && x.ap.is_finite()).map(|x| (x.pitch, x.ap)).collect();
        if pts.len() < 150 {
            return Err("視線の向き（Valve）が取れなかった。frameeyeosc が動いているか確かめて、もう一度".into());
        }
        let xs: Vec<f64> = pts.iter().map(|p| p.0).collect();
        let ys: Vec<f64> = pts.iter().map(|p| p.1).collect();
        let q = Quad::fit(&xs, &ys, &vec![true; xs.len()]).ok_or("視線と目の開きの関係が求められなかった（もう一度）")?;
        let (c2, c1, c0, m) = (q.c[0], q.c[1], q.c[2], q.xm);
        let pitch_c = [c0 - c1 * m + c2 * m * m, c1 - 2.0 * c2 * m, c2];
        let mut sorted = xs.clone();
        let (lo, hi) = (vision::percentile(&mut sorted, 1.0), vision::percentile(&mut sorted, 99.0));
        let (ap_sq, nsq) = med_of(s, |x| (x.label == Label::Squint).then_some(x.ap));
        if nsq < 30 {
            return Err(format!("{eye}目の細めが測れなかった（もう一度）"));
        }
        let f_sq = (ap_sq - w.ap_cl) / (w.ap_n - w.ap_cl);
        check(f_sq >= 0.85 || f_sq.is_nan(), format!("{eye}目の細めが浅かった（もう一度、しっかり細めてね）[f_sq {f_sq:.2}]"))?;
        let (ap_down, nd) = med_of(s, |x| (x.label == Label::LookDown).then_some(x.ap));
        check(
            nd < 30 || ap_down > 0.95 * w.ap_n,
            format!("{eye}目: 下を見ても目の開きが変わっていない（もう一度、しっかり下を見てね）[{ap_down:.2} / 普段 {:.2}]", w.ap_n),
        )?;
        let (lower_down, _) = med_of(s, |x| (x.label == Label::LookDown).then_some(x.lower_px));
        check(
            lower_down < w.lower_px,
            format!("{eye}目: 下を見たら下まぶたが上に動いた（検出の失敗かも。もう一度）[{lower_down:.1} px / 普段 {:.1} px]", w.lower_px),
        )?;
        deltas[e] = (ap_down - w.ap_n, ap_sq - w.ap_n);
        let keep = old.map(|o| o[e]).unwrap_or_default();
        out[e] = UserParams { pitch_c, pitch_lo: lo, pitch_hi: hi, f_sq, pd_min: keep.pd_min, pd_max: keep.pd_max };
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
        if br.len() < 60 || dk.len() < 60 {
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
    /// (time, L, R), oldest first.
    pub history: Vec<(String, [WearRecord; 2])>,
    /// A wear calibration has succeeded at least once (also one whose widen was not caught): the first-time setup
    /// is done. Files from before this field count as done when they have a history.
    pub setup_done: bool,
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
        let mut s = format!("{{\n  \"version\": 1,\n  \"setup_done\": {}", self.setup_done);
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
            s += &format!(
                ",\n  \"wear\": {{\"time\": {}{widen}, \"L\": {}, \"R\": {}}}",
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
        let j = json::parse(text)?;
        let mut c = CalibFile::default();
        if let Some(u) = j.get("user") {
            c.user = Some([parse_user(u.get("L").ok_or("user.L")?)?, parse_user(u.get("R").ok_or("user.R")?)?]);
            c.user_time = u.get("time").and_then(Json::str).unwrap_or("").to_string();
            c.pupil_measured = matches!(u.get("pupil_measured"), Some(Json::Bool(true)));
            c.user_warnings = u
                .get("warnings")
                .and_then(Json::arr)
                .map(|a| a.iter().filter_map(|w| w.str().map(str::to_string)).collect())
                .unwrap_or_default();
        }
        if let Some(w) = j.get("wear") {
            c.wear = Some([parse_wear(w.get("L").ok_or("wear.L")?)?, parse_wear(w.get("R").ok_or("wear.R")?)?]);
            c.wear_time = w.get("time").and_then(Json::str).unwrap_or("").to_string();
            c.wear_widen_measured = match w.get("widen").and_then(Json::str) {
                Some("measured") => Some(true),
                Some("default") => Some(false),
                _ => None,
            };
        }
        if let Some(items) = j.get("history").and_then(Json::arr) {
            let rec = |e: Option<&Json>| -> Option<WearRecord> {
                let e = e?;
                Some(WearRecord { step: e.get("step")?.num()?, gap: e.get("gap")?.num()?, r_px: e.get("r_px")?.num()? })
            };
            for it in items {
                if let (Some(l), Some(r)) = (rec(it.get("L")), rec(it.get("R"))) {
                    c.history.push((it.get("time").and_then(Json::str).unwrap_or("").to_string(), [l, r]));
                }
            }
        } else if let Some(w) = &c.wear {
            // Files from before the history: the last wear calibration is the history.
            c.history.push((c.wear_time.clone(), [WearRecord::of(&w[0]), WearRecord::of(&w[1])]));
        }
        c.setup_done = matches!(j.get("setup_done"), Some(Json::Bool(true))) || !c.history.is_empty();
        Ok(c)
    }

    /// History entries from saved calibration attempts (`<dir>/calib_*/calib_result.json`, successful wear ones),
    /// for a calib.json that has none yet.
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
            if let (Some(l), Some(r)) = (rec("L"), rec("R")) {
                let time = j.get("time").and_then(Json::str).unwrap_or(&n).to_string();
                out.push((time, [l, r]));
            }
        }
        out
    }
}

// ------------------------------------------------------------------------------------------------- reports

/// Thresholds of the calibration checks (fit_wear / fit_user use the same numbers).
pub const MIN_NORMAL_FRAMES: usize = 90;
pub const MIN_WIDEN_FRAMES: usize = 60;
pub const MIN_CLOSE_FRAMES: usize = 20;
pub const MIN_WIDEN_STEP: f64 = 0.05;
pub const MIN_OPEN_GAP: f64 = 0.2;

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
    for (e, s) in samples.iter().enumerate() {
        let r_px = calib_radius(s, &[Label::Normal, Label::Widen]);
        let s: Vec<Sample> = s.iter().map(|x| x.rescaled(r_px)).collect();
        let (b_n, nn) = med_of(&s, |x| (x.label == Label::Normal && x.ok).then_some(x.skin));
        let (b_w, nw) = med_of(&s, |x| (x.label == Label::Widen && x.ok).then_some(x.skin));
        let (ap_n, _) = med_of(&s, |x| (x.label == Label::Normal && x.ok).then_some(x.ap));
        let (ap_cl, nc) = med_of(&s, |x| (x.label == Label::Close).then_some(x.ap));
        let all = |l: Label| s.iter().filter(|x| x.label == l).count();
        let ok_share = |l: Label| {
            let n = all(l);
            if n == 0 { f64::NAN } else { s.iter().filter(|x| x.label == l && x.ok).count() as f64 / n as f64 }
        };
        eyes.push(format!(
            "\"{}\": {{\"r_px\": {}, \"b_n\": {}, \"b_w\": {}, \"widen_step\": {}, \"ap_n\": {}, \"ap_cl\": {}, \"open_gap\": {}, \
\"normal_frames_with_pupil\": {nn}, \"widen_frames_with_pupil\": {nw}, \"close_frames_with_lids\": {nc}, \
\"pupil_seen_share\": {{\"normal\": {}, \"widen\": {}, \"close\": {}}}}}",
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
        ));
        lines.push(format!(
            "calib wear {}: widen step {:.3} (needs >= {MIN_WIDEN_STEP}; normal {:.3}, widen {:.3}), open gap {:.3} (needs >= {MIN_OPEN_GAP}), \
frames with pupil normal {nn} / widen {nw} (need {MIN_NORMAL_FRAMES} / {MIN_WIDEN_FRAMES}), close {nc} (need {MIN_CLOSE_FRAMES}), R {:.1} px",
            ["L", "R"][e],
            b_w - b_n,
            b_n,
            b_w,
            ap_n - ap_cl,
            r_px
        ));
    }
    let json = format!(
        "{{{}, \"thresholds\": {{\"widen_step_min\": {MIN_WIDEN_STEP}, \"open_gap_min\": {MIN_OPEN_GAP}, \"normal_frames_min\": {MIN_NORMAL_FRAMES}, \
\"widen_frames_min\": {MIN_WIDEN_FRAMES}, \"close_frames_min\": {MIN_CLOSE_FRAMES}}}}}",
        eyes.join(", ")
    );
    (json, lines)
}

/// The per-eye values behind a user calibration (needs this wear's levels): (JSON object, log lines).
pub fn user_report(samples: &[Vec<Sample>; 2], wear: &[WearParams; 2]) -> (String, Vec<String>) {
    let mut eyes = Vec::new();
    let mut lines = Vec::new();
    let mut deltas = [(f64::NAN, f64::NAN); 2];
    for (e, s) in samples.iter().enumerate() {
        let w = &wear[e];
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
            "calib user {}: squint depth {:.2} (needs < 0.85), look_down aperture {:.2} of normal (needs <= 0.95), lower lid {:.1} px vs normal {:.1} px (must not move up), pitch points {pitch_pts} (need 150)",
            ["L", "R"][e],
            f_sq,
            ap_down / w.ap_n,
            lower_down,
            w.lower_px
        ));
    }
    let (dd, ds) = ((deltas[0].0 - deltas[1].0).abs(), (deltas[0].1 - deltas[1].1).abs());
    lines.push(format!("calib user L/R difference: look_down {dd:.2}, squint {ds:.2} (warning at >= 0.2)"));
    let json = format!(
        "{{{}, \"left_right_difference\": {{\"look_down\": {}, \"squint\": {}}}, \"thresholds\": {{\"f_sq_max\": 0.85, \"look_down_ratio_max\": 0.95, \"left_right_warn\": 0.2, \"pitch_points_min\": 150}}}}",
        eyes.join(", "),
        json::num(dd),
        json::num(ds)
    );
    (json, lines)
}

/// The collected samples as CSV (one row per eye and frame).
pub fn samples_csv(samples: &[Vec<Sample>; 2]) -> String {
    let mut out = String::from("eye,t,label,ok,skin_up,aperture,pupil_ratio,pitch,lower_px,r,iris_r\n");
    let f = |v: f64| if v.is_finite() { format!("{v:.5}") } else { String::new() };
    for (e, s) in samples.iter().enumerate() {
        for x in s {
            out += &format!(
                "{},{:.6},{},{},{},{},{},{},{},{},{}\n",
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
                f(x.iris_r)
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
        // A spike shorter than 100 ms is capped at the off level.
        let spike: Vec<f64> = (0..8).map(|_| g.step(0.9)).collect();
        assert!(spike.iter().all(|&v| v == WIDE_OFF), "{spike:?}");
        assert_eq!(g.step(0.2), 0.2);
        // A held widen comes through on its 9th frame and stays while above the off level.
        let held: Vec<f64> = (0..9).map(|_| g.step(0.8)).collect();
        assert_eq!(held[7], WIDE_OFF);
        assert_eq!(held[8], 0.8);
        assert_eq!(g.step(0.4), 0.4);
        assert_eq!(g.step(0.29), 0.29);
        // Below the off level it has to wait again.
        assert_eq!(g.step(0.9), WIDE_OFF);
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
        for k in 1..=PRIOR_RESET_MISSES {
            let o = e.process(&blank, f64::NAN, k as f64 / FPS, &p);
            assert!(!o.f.ok_pupil);
            assert_eq!(o.f.diag.prior_reset, k == PRIOR_RESET_MISSES, "frame {k}");
            assert_eq!(e.prev.is_some(), k < PRIOR_RESET_MISSES);
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
    fn auto_baseline_is_the_mode_of_the_warm_up() {
        // A wearer who mostly looks straight (0.80 px-units) but glances and widens a lot (tails): the mode, not the
        // mean, is the open level.
        let mut b = AutoBase::default();
        assert_eq!(b.warmup_remaining_s(), WARMUP_S);
        for i in 0..WARMUP_FRAMES {
            let skin = match i % 10 {
                0..=5 => 40.0 + (i % 3) as f64 * 0.1,
                6 | 7 => 46.0,
                _ => 30.0 + (i % 7) as f64,
            };
            assert!(!b.ready);
            b.warm(skin, 80.0, 0.5);
        }
        assert!(b.ready);
        // Within the 5-bin smoothing window (2 bins of 0.5) of the true level.
        assert!((b.b - 40.1).abs() <= 1.25, "{}", b.b);
        assert!((b.ap_n - 80.0).abs() <= 1.25, "{}", b.ap_n);
        assert_eq!(b.warmup_remaining_s(), 0.0);
        let mut c = AutoBase::default();
        c.seed(41.0, 79.0);
        assert!(c.ready && c.b == 41.0);
    }
}
