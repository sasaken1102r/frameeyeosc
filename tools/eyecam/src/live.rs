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
//! typical pitch), and from then on they follow slowly (skin line: 30 s time constant, every usable frame, see
//! TRACK_TAU_S; aperture: 20 s, frames with EyeWide and EyeSquint < 0.6, see TRACK_AP_TAU_S). The widen
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
/// A pupil lost while the last one seen had its small-x edge within this many px of the search window's left edge ran
/// out of the lit part of the picture, not under a lid. The small-x side of both upright pictures is the temporal one
/// (toward the ear; the 2026-10-09 recordings: an eye turning outward moves its pupil there, one turning inward toward
/// the lens-edge band at large x), and it is dark: an eye turned 25 degrees or more outward takes its pupil into that
/// shading, where it merges with it (a blob too big to be one), and the iris box, cut by the window's edge, takes in
/// sclera and skin and reads 1.31-1.50 x its open level. Such an eye is not closed; its eyelid, widen and squint keep
/// their last values (but widen 0 and the eyelid at most 0.75) until the pupil is seen again, as long as the lids
/// measured there are still apart (`EDGE_OPEN_AP`): an eye that really closes after an outward glance (the last pupil
/// stays at the edge) still closes.
/// 12 px missed a right look of 35 degrees on 2026-10-09 15:23 (the right eye's last pupil 15 px from the edge, 9 s
/// falsely closed). On the features of the 2026-10-03/04 and 10-08 eye videos: 53 of the 55 frames falsely closed
/// looking outward (Valve's openness 0.9 or more) no longer closed, none of the 6118 closed frames (Valve's below 0.5);
/// replays of the 10-07 to 10-09 videos: 3 of 10515 closed frames lost (blinks' last frames; the blinks still closed).
const EDGE_LOST_PX: f64 = 16.0;
/// The aperture (iris radii, 5-frame median) down to which an eye lost at the window's edge is held open. Closed eyes
/// measure 0.3-0.8 (99% below 1.03 on 2026-10-03/08, below 1.2 on 10-09), the eyes falsely closed looking outward
/// 1.06-1.59 (95% above 1.27).
const EDGE_OPEN_AP: f64 = 1.2;
/// The extended pupil search (`feat::Search::find_x_min`, `search_x_min` in the drift log): how far (px) its left edge
/// may go past `x_min` toward the ear. `x_min` is where the temporal shading ends minus 10 px, fixed in the first 20 s of
/// a wear; lying down darkens the frame, so it moves in while the pupil moves out (2026-10-12 guided recordings, the
/// right eye's pupil found in 0.66 of the open steps sitting, 0.39 on the back and 0.09 on the right side; on the side
/// its temporal edge sat a few px past x_min for the whole minute). Moving x_min itself 10 px further out (shading
/// end - 20) found it lying down (0.78 / 0.65) but lost blinks sitting (1042 -> 1002 short closes over the free
/// recordings, one eye 69 -> 48, and a new 7 s false close): x_min also bounds the iris box, the lids and the edge
/// hold, and a box with shading in it no longer brightens enough over a closed lid. So x_min stays, and only the
/// pupil search reaches further, and only when the search inside x_min finds nothing. Replays (2026-10-12): the right
/// eye's pupil in 0.73 / 0.70 / 0.65 of the open steps sitting / on the back / on the side, the side's close steps
/// all read closed (none before) and its 6 s false close gone; the 7 free recordings (3 min sitting each) caught 320 of
/// Valve's 341 closes (321 before) with 504 short closes where Valve closed too (506), false closes 21.3 s (23.7, none
/// new); the 10 wear calibrations found the pupil in as many normal-step frames or more (one eye 276 -> 465 of 486)
/// with the same fitted levels (within 0.008). The pupils it found had their temporal edge up to 16 px past x_min
/// (medians 13 on the side, 8 on the back); where the shading reaches the pupil itself (the iris around it in the
/// dark), no edge finds it, and the edge hold keeps the eye open.
pub const EXT_MAX_PX: f64 = 24.0;
/// The extended search's edge keeps this many px (`feat::EXT_EDGE_PX`) left of a pupil found near it, and opens to
/// EXT_MAX_PX when the pupil is lost with its last small-x edge within this of it (or when the search inside x_min
/// refused a blob cut by its edge that refined into an ellipse: the first pupil of a wear may never be seen inside).
const EXT_NEAR_PX: f64 = feat::EXT_EDGE_PX;
/// It comes back toward x_min this fast (px/s) while the pupil is seen with its small-x edge EXT_NEAR_PX or more
/// inside x_min (24 px in 6 s): a glance back in does not close it at once.
const EXT_BACK_PX_S: f64 = 4.0;
/// A pupil from the extended search is taken only when it looks like the open eye's pupil: at least this share of
/// its rays reached its edge, this round (b/a), 0.6-1.3 x the open pupil's size (the crease check takes 0.45, for a
/// pupil in bright light at 0.55-0.65 x; stricter here, where the shading's blobs are as dark), at most EXT_DARKER
/// levels lighter inside
/// (before the open pupil is known, the first 30 samples of a wear: a semi-axis within EXT_A_PX); and while Valve's
/// openness for the eye is known, it is at least EXT_VALVE_OPEN. Without Valve's openness, it must be within
/// EXT_TRACK_PX of the last pupil when that was seen within EXT_TRACK_S. Both guard against what a blink leaves near
/// where the pupil was: the pupil half under the closing lid, lashes. The extended search runs on every frame without
/// a pupil while it is open, and such a "pupil" moves the place the iris box is measured at to the shaded edge, where a
/// closed lid no longer reads bright enough: before the Valve check, 2026-10-09 16:37 lost a 0.4 s close (Valve's
/// openness 0) to a pupil found at 0.26, and the 10-09 user calibration a blink's start to one found at 0.
/// The track alone was too strict and too weak at once: a glance back from looking down (10-12 10:02, 70 px in a
/// frame) was refused for 1.4 s and read closed, and it does not tell a lid-cut pupil from the pupil.
const EXT_MIN_VIS: f64 = 0.5;
const EXT_MIN_BA: f64 = 0.5;
const EXT_SIZE: (f64, f64) = (0.6, 1.3);
const EXT_DARKER: f64 = 10.0;
const EXT_TRACK_S: f64 = 1.0;
const EXT_TRACK_PX: f64 = 40.0;
const EXT_A_PX: (f64, f64) = (8.0, 45.0);
const EXT_VALVE_OPEN: f64 = 0.6;
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
/// After a manual relearn's warm-up (`relearn` with open-eye levels already known), the levels in use go from the old
/// ones to the new ones over this many seconds (a straight line), so the eyelid, EyeWide and squint don't jump.
pub const RELEARN_RAMP_S: f64 = 2.0;
/// Usable frames for the baseline: Valve pitch within this many degrees of the wearer's typical pitch.
const PITCH_GATE: f64 = 15.0;
/// After warm-up the skin line's baseline follows the skin line with this time constant (s), at most half a widen step
/// per time constant, through every usable frame. It used to take only frames with EyeWide below 0.2 (60 s): a
/// baseline learnt low (2026-10-09 19:42, after an eyecam restart during the gaze fit: the right eye's 17 px under its
/// skin line) read EyeWide 1 from then on and never moved, the right eye widened over 70% of the normal part (59%
/// live) while the frames show it as open as the left one. Through every frame at 30 s: 25% (left 52 -> 10%), the
/// other sessions' normal parts the same or less widened, and the widen steps of the protocol sessions (5 s each)
/// kept: medians 0.75-1.00 (0.79-1.00 before). A widen of one full step held reads 0.5 or more for about 22 s (a
/// bigger one longer: 1.4 steps about 50 s), then fades into the baseline. 20 s took the 19:42 eye down to 15%
/// but faded some widen steps to 0.3 by their end; 0.6 as the limit (as for the aperture) left it at 66%.
const TRACK_TAU_S: f64 = 30.0;
/// The aperture's baseline follows faster (`TRACK_AP_TAU_S`), through frames with EyeSquint and EyeWide below
/// `TRACK_AP_MAX`. It used to follow only frames below 0.2: an upper lid that settled lower after the warm-up
/// (2026-10-09 16:37, both eyes 13-14 px lower from 40 s on, at the same pupil position and gaze; the frames show
/// the lids relaxed) read as squint 0.4-0.5 from then on, and as squinting it never taught the baseline (113 -> 112 px
/// over 2 minutes): eyelid 0.55 / 0.53 for good, while the skin line's baseline followed. Through frames up to 0.6 at
/// 20 s: 0.72 / 0.73 (squint 0.07 / 0.03) over that part, the other sessions' normal parts the same or nearer 0.75,
/// and the squint steps of the protocol sessions unchanged (2026-10-07 squint 1.00 / 1.00, 10-08 0.50 / 0.79). A
/// squint held longer than that, at under 0.6, fades into the baseline.
const TRACK_AP_TAU_S: f64 = 20.0;
const TRACK_AP_MAX: f64 = 0.6;
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

/// Looking down narrows an open eye: the upper lid follows the eye down faster than the pupil moves in the picture
/// (1.3 x, the lower lid 0.7 x), so the aperture shrinks with the gaze, and a pitch correction that is too shallow
/// reads an open eye looking down as half shut. The user calibration's curve (`calib user`) often is: in 6 of the 11
/// left-eye looks down on record the lower lid was found as far below the pupil as straight ahead (within 0.1 iris
/// radii; 0.16-0.47 nearer in every right-eye one), so the aperture seemed to narrow too little (2026-10-14: 0.18 iris
/// radii at -37 degrees, where an open eye's narrows by 0.39 in the median over the recordings, 27 degrees below its
/// typical pitch). So below the wearer's typical pitch the curve counts for at least LID_PITCH_PER_DEG iris radii a
/// degree beyond LID_PITCH_FROM_DEG (up to LID_PITCH_MAX_DEG beyond) and at most LID_PITCH_CAP times that, and for
/// nothing nearer; looking up it may only say narrower. On the recordings of 2026-10-07 to 10-14 (one wearer) the
/// aperture stays within 0.05 down to 10-15 degrees below the typical pitch and then narrows by 0.028 a degree (the
/// median of 16 guided looks down; 0.016-0.057 in the six user calibrations). The hinge sits at 8, not 10, so that
/// glances 3-13 degrees down read about as with the calibration's own curve (free recordings, eyelid below 0.6 in
/// 10.4% of those frames, 9.9% with the curve, 11.2% with the hinge at 10). Replayed with the 2026-10-14 calibration
/// (before in brackets): the look-down steps of 5 guided recordings, both eyes, read 0.75 in the median (0.72), below
/// 0.6 in 12% of the frames (30%), the worst eye 0.51 (0.27); the squint steps 0.32 (0.32), below 0.6 in 97% (98%);
/// EyeSquint looking down 0.00 (0.06); free recordings looking more than 25 degrees down below 0.6 in 6.2% (9.4%),
/// looking up in 12.7% (14.6%); the calibrations themselves come out the same. Looking up, the curve could take the
/// eyelid down where the aperture is as wide or wider (one of six calibrations, fitted through its three steps, to
/// 0.59 in the look-up steps).
const LID_PITCH_FROM_DEG: f64 = 8.0;
const LID_PITCH_PER_DEG: f64 = 0.028;
const LID_PITCH_MAX_DEG: f64 = 30.0;
const LID_PITCH_CAP: f64 = 1.5;

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
    /// Aperture vs Valve pitch (degrees): a0 + a1 p + a2 p^2; only differences are used, and only within the
    /// downgaze bounds (`pitch_drop`).
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

    /// How much narrower (iris radii) an open eye's aperture is at `pitch` than at the wearer's typical pitch `p0`,
    /// from the curve. Below `p0` it is held between LID_PITCH_PER_DEG a degree beyond LID_PITCH_FROM_DEG and
    /// LID_PITCH_CAP times that (so nothing nearer `p0`); above it, looking up, the curve may say narrower (up to
    /// LID_PITCH_CAP x LID_PITCH_PER_DEG a degree) but never wider.
    fn pitch_drop(&self, pitch: f64, p0: f64) -> f64 {
        let drop = self.pitch_curve(p0) - self.pitch_curve(pitch);
        if pitch > p0 {
            return drop.clamp(0.0, LID_PITCH_CAP * LID_PITCH_PER_DEG * (pitch - p0).min(LID_PITCH_MAX_DEG));
        }
        let floor = LID_PITCH_PER_DEG * (p0 - pitch - LID_PITCH_FROM_DEG).clamp(0.0, LID_PITCH_MAX_DEG);
        drop.clamp(floor, LID_PITCH_CAP * floor)
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
    /// Not closed while the pupil is lost at the window's (temporal) edge (see EDGE_LOST_PX).
    pub edge_hold: bool,
    /// Look for a pupil the window's left edge cuts past it (see EXT_MAX_PX).
    pub ext_search: bool,
}

impl Default for Tune {
    fn default() -> Self {
        let (wide_start, wide_width) = wide_curve(DEFAULT_WIDEN_SENSITIVITY);
        Self { wide_start, wide_width, track: true, gaze_drop: true, edge_hold: true, ext_search: true }
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
#[derive(Clone, Copy, Debug, Default)]
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
    /// The pupil search's left edge (`feat::search_x_min`): also the lid and iris-box regions' and the edge hold's.
    pub x_min: f64,
    /// The extended pupil search's left edge for this frame (`x_min` when it is not extended; see EXT_MAX_PX).
    pub search_x_min: f64,
    /// px from the last pupil seen's small-x (temporal) edge to `x_min` (NaN before the first pupil).
    pub edge_gap: f64,
    /// No pupil, the last one at the window's (temporal) edge: not closed, the eyelid values held (see EDGE_LOST_PX).
    pub edge_held: bool,
    /// The levels in use for this frame (calibrated, or the automatic baseline + stored step).
    pub b_n: f64,
    pub b_w: f64,
}

/// The widen step and open gap (iris radii) the engine uses, and the confidence factor that goes with them: the
/// history's, or the population defaults.
pub fn levels(params: &Params) -> (f64, f64, f64) {
    params.hist.map_or((DEFAULT_WIDEN_STEP, DEFAULT_OPEN_GAP, 0.7), |h| (h.step, h.gap, 1.0))
}

/// What the engine holds for the current wear, read only (the drift log, `driftlog`): the references fixed early in
/// a wear and the ones that follow.
#[derive(Clone, Copy, Debug)]
pub struct WearState {
    /// The time (s) of this wear's first frame (NaN before one).
    pub t_first: f64,
    /// The pupil search window: x from `x_min` to `xmax`, rows feat::SEARCH_Y0..SEARCH_Y1.
    pub x_min: f64,
    pub xmax: f64,
    /// The extended pupil search's left edge after the last frame (EXT_MAX_PX).
    pub search_x_min: f64,
    pub x_min_from: feat::XMinFrom,
    /// The frame time (s) x_min and xmax were last worked out on, and from how many frames' column means (they stop
    /// after 40, about 20 s into a wear).
    pub x_min_at: f64,
    pub x_min_frames: u32,
    /// The open eye's long-run box brightness (the closed rule's reference) and how many samples it has (it is used
    /// from 30 on).
    pub box_open: f64,
    pub box_open_n: usize,
    /// The auto baseline (px): skin line above the pupil and aperture, and whether it is ready (warm-up over or
    /// seeded). Before it is ready these are the warm-up's running modes (NaN at first).
    pub base_skin_px: f64,
    pub base_ap_px: f64,
    pub ready: bool,
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
    /// Where x_min came from, and the time (s) of the frame it was last worked out on (the drift log).
    x_min_from: feat::XMinFrom,
    x_min_at: f64,
    /// The extended pupil search's left edge (EXT_MAX_PX), and the time (s) the last pupil was seen.
    find_x_min: f64,
    prev_p_t: f64,
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
    /// The last eyelid, widen and squint reported for an open eye, kept while the pupil is lost at the window's
    /// (temporal) edge.
    last_out: (f64, f64, f64),
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
    /// A manual relearn's new warm-up (`relearn` while the levels above were known): the levels above stay in use
    /// (and keep being followed) until it is ready, then its levels replace them through `ramp`.
    next: Option<Box<AutoBase>>,
    /// The switch to a relearn's new levels: the old skin line and aperture levels (px) and the seconds since the
    /// switch; the levels in use go from those to `b` / `ap_n` over RELEARN_RAMP_S.
    ramp: Option<(f64, f64, f64)>,
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
            next: None,
            ramp: None,
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
    /// Seconds of the warm-up still to go: this wear's first one, or a relearn's while the old levels stay in use.
    fn warmup_remaining_s(&self) -> f64 {
        if let Some(next) = &self.next {
            return next.warmup_remaining_s();
        }
        if self.ready { 0.0 } else { (WARMUP_S - self.seconds).clamp(0.0, WARMUP_S) }
    }

    /// The open-eye levels in use (skin line height, aperture; px): `b` / `ap_n`, or on the way to them from the old
    /// ones for RELEARN_RAMP_S after a relearn (an old level that was never known is not ramped from).
    fn levels(&self) -> (f64, f64) {
        match self.ramp {
            Some((b0, ap0, s)) => {
                let k = (s / RELEARN_RAMP_S).clamp(0.0, 1.0);
                let lerp = |from: f64, to: f64| if from.is_finite() && to.is_finite() { from + (to - from) * k } else { to };
                (lerp(b0, self.b), lerp(ap0, self.ap_n))
            }
            None => (self.b, self.ap_n),
        }
    }

    /// One frame (`dt` s) further into the ramp after a relearn, if one runs.
    fn tick(&mut self, dt: f64) {
        if let Some((_, _, s)) = &mut self.ramp {
            *s += dt;
            if *s >= RELEARN_RAMP_S {
                self.ramp = None;
            }
        }
    }

    /// One usable frame for a relearn's warm-up (as `warm`), if one runs; once it is ready its levels replace the old
    /// ones (through the ramp; an aperture level it could not work out keeps the old one).
    fn warm_next(&mut self, skin: f64, ap: f64, bw: f64, dt: f64) {
        let Some(next) = &mut self.next else { return };
        next.warm(skin, ap, bw, dt);
        if next.ready {
            let (b, ap_n) = (next.b, next.ap_n);
            self.next = None;
            let (b0, ap0) = self.levels();
            self.ramp = Some((b0, ap0, 0.0));
            self.b = b;
            if ap_n.is_finite() {
                self.ap_n = ap_n;
            }
        }
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

    /// Learn the open-eye levels again from a fresh warm-up. With levels already known, they stay in use (and keep
    /// being followed) while the new warm-up runs in `next`, and are replaced once it is ready; a relearn during a
    /// relearn starts the new warm-up over and keeps the levels in use (a ramp still running ends where it is).
    /// Without levels yet, the warm-up just starts over, as at the start of a wear. The wearer's typical pitch stays
    /// either way: it is about where they look, not how open their eyes are, and it decides which frames the warm-up
    /// may use (cleared, the first frames with a pitch would wait for it to fill again).
    fn relearn(&mut self) {
        if self.ready {
            (self.b, self.ap_n) = self.levels();
            self.ramp = None;
            self.next = Some(Box::new(Self { pitch_typ: LongMedian::new(0.0), ..Self::default() }));
            return;
        }
        let pitch_typ = std::mem::replace(&mut self.pitch_typ, LongMedian::new(0.0));
        *self = Self { pitch_typ, ..Self::default() };
    }

    fn seed(&mut self, b: f64, ap_n: f64) {
        if b.is_finite() && ap_n.is_finite() {
            (self.b, self.ap_n, self.ready) = (b, ap_n, true);
            self.skin = Vec::new();
            self.ap = Vec::new();
            // A calibration's levels win over a relearn still warming up, at once
            self.next = None;
            self.ramp = None;
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
            x_min_from: feat::XMinFrom::Fixed,
            x_min_at: f64::NAN,
            find_x_min: feat::X_MIN,
            prev_p_t: f64::NAN,
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
            last_out: (0.75, 0.0, 0.0),
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

    /// Learn this wear's open-eye levels again (`relearn` on ctl.sock): a new warm-up of the auto baseline. With
    /// levels already known (warmed up or seeded), the eyelid, EyeWide and squint keep working from them meanwhile,
    /// and the new levels take over once the warm-up is done (over RELEARN_RAMP_S); without them yet, the warm-up
    /// starts over as at the start of a wear and EyeWide waits for it. Unlike `reset`, everything else learnt about
    /// the wear stays (lens edge, search window, iris radius, the open eye's pupil size, darkness and iris-box
    /// brightness that tell a closed eye, the wearer's typical pitch): only the levels the wearer's eyes rest at are
    /// relearnt, and blinks keep working meanwhile.
    pub fn relearn(&mut self) {
        self.base.relearn();
    }

    /// A relearn's warm-up runs while the old open-eye levels stay in use (`warmup_remaining_s` counts it down).
    pub fn relearning(&self) -> bool {
        self.base.next.is_some()
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
        self.find_x_min = self.x_min;
    }

    /// What a pupil from the extended search (`feat::Search::find_x_min`) must be like on a frame at `t` (see
    /// EXT_MIN_VIS). `valve_open`: Valve's openness for this eye (NaN without it). Once the open pupil is known these
    /// are stricter than the closed-lid crease check, which then never drops such a pupil (and its box: the lids and
    /// the box of a pupil the check refuses are measured where the last pupil was).
    fn ext_check(&self, t: f64, valve_open: f64) -> feat::ExtCheck {
        let known = self.a_open.len() >= 30;
        feat::ExtCheck {
            min_vis: EXT_MIN_VIS,
            min_ba: EXT_MIN_BA,
            a: if known { (EXT_SIZE.0 * self.a_open.value, EXT_SIZE.1 * self.a_open.value) } else { EXT_A_PX },
            inner_max: if known { self.inner_open.value + EXT_DARKER } else { f64::INFINITY },
            near: self
                .prev_p
                .filter(|_| !valve_open.is_finite() && t - self.prev_p_t < EXT_TRACK_S)
                .map(|q| (q.cx, q.cy, EXT_TRACK_PX)),
            closing: valve_open < EXT_VALVE_OPEN,
        }
    }

    /// What the engine holds for this wear (read only; the drift log).
    pub fn wear_state(&self) -> WearState {
        WearState {
            t_first: self.t_first,
            x_min: self.x_min,
            xmax: self.xmax,
            search_x_min: self.find_x_min,
            x_min_from: self.x_min_from,
            x_min_at: self.x_min_at,
            x_min_frames: self.cm_n,
            box_open: self.box_open.value,
            box_open_n: self.box_open.len(),
            base_skin_px: self.base.b,
            base_ap_px: self.base.ap_n,
            ready: self.base.ready,
        }
    }

    /// The lens edge and iris radius the engine currently uses.
    pub fn geometry(&self) -> (f64, f64) {
        (self.xmax, self.r_est.value)
    }

    /// The camera's frame rate as measured from the frame times (FPS until a few frames are in).
    pub fn fps(&self) -> f64 {
        self.rate.fps()
    }

    /// The open-eye levels are known (warm-up over, or seeded by a calibration; also while a relearn's warm-up runs
    /// with the old ones in use).
    pub fn baseline_ready(&self) -> bool {
        self.base.ready
    }

    /// Seconds of usable frames still needed before EyeWide is enabled (0 when ready), or, during a relearn, before
    /// its new levels take over.
    pub fn warmup_remaining_s(&self) -> f64 {
        self.base.warmup_remaining_s()
    }

    /// The open-eye levels in use (skin line height, aperture; px) once ready.
    pub fn baseline_px(&self) -> Option<(f64, f64)> {
        self.base.ready.then(|| self.base.levels())
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
        self.process_open(img, pitch, f64::NAN, t, params)
    }

    /// `process` with Valve's openness for this eye (0..1, NaN without it): only a hint for the extended pupil search
    /// (EXT_VALVE_OPEN), nothing else uses it.
    pub fn process_open(&mut self, img: &[u8], pitch: f64, valve_open: f64, t: f64, params: &Params) -> Out {
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
            (self.x_min, self.x_min_from) = feat::search_x_min_from(&m);
            self.x_min_at = t;
        }
        // The extended search's edge: within EXT_MAX_PX past x_min (and not past X_MIN_LO), x_min itself when off.
        let ext_lo = if params.tune.ext_search { (self.x_min - EXT_MAX_PX).max(feat::X_MIN_LO) } else { self.x_min };
        self.find_x_min = self.find_x_min.clamp(ext_lo.min(self.x_min), self.x_min);
        let search_x_min = self.find_x_min;
        self.ex.search = feat::Search::live_ext(self.x_min, search_x_min, self.ext_check(t, valve_open));
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
        // The extended search's edge follows the pupil: EXT_NEAR_PX left of one found near it, all the way out when it
        // is lost near it (or a blob cut by x_min looked like an ellipse), slowly back while it is well inside x_min.
        let lost_near = self.prev_p.is_some_and(|q| q.cx - q.a - self.find_x_min < EXT_NEAR_PX);
        match p {
            Some(pp) if pp.cx - pp.a - self.find_x_min < EXT_NEAR_PX => {
                self.find_x_min = (pp.cx - pp.a - EXT_NEAR_PX).max(ext_lo).min(self.find_x_min);
            }
            Some(pp) if pp.cx - pp.a >= self.x_min + EXT_NEAR_PX => {
                self.find_x_min = (self.find_x_min + EXT_BACK_PX_S / fps).min(self.x_min);
            }
            Some(_) => {}
            None if lost_near || f.diag.left_refused => self.find_x_min = ext_lo.min(self.x_min),
            None => {}
        }
        if let Some(pp) = p {
            self.prev = Some((pp.cx, pp.cy));
            self.prev_p = Some(pp);
            self.prev_p_t = t;
            if !box_bright && sampled {
                self.a_open.push(pp.a, long_hz);
                self.inner_open.push(pp.inner, long_hz);
                if pp.vis >= PUPIL_MIN_VIS {
                    self.ba_open.push(pp.b / pp.a, long_hz);
                }
            }
        }
        // Not from the extended search: early in a wear the iris radius's median still jumps between modes (56 and 83 px
        // on the 2026-10-09 18:16 user calibration's left eye), the box is sized by it, and two edge pupils found at
        // 0.9 s flipped it half a second early, so the open eye's box level was learnt 12% brighter and the blink at
        // 3.6 s (Valve's openness 0 for 0.2 s) never read 1.3 x. Their own box and lids still use the radius in use.
        if f.ok_pupil && !f.diag.extended && f.iris_r.is_finite() && f.iris_n >= 4 && sampled {
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
        // Not while the pupil is lost at the window's (temporal) edge with the lids still apart (EDGE_LOST_PX).
        let no_pupil_long = self.nopupil >= frames_in(CLOSED_NO_PUPIL_S, fps, 2);
        let edge_gap = self.prev_p.map_or(f64::NAN, |q| q.cx - q.a - self.x_min);
        let edge_held = params.tune.edge_hold && !f.ok_pupil && edge_gap < EDGE_LOST_PX && ap >= EDGE_OPEN_AP;
        let closed = self.box_open.len() >= 30 && box_f > 1.3 * self.box_open.value && no_pupil_long && !edge_held;
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
        let (step_r, gap_r, cal_q) = levels(params);
        let (step_px, gap_px) = (step_r * r, gap_r * r);
        let open_seen = f.ok_pupil && !closed && skin_px.is_finite();
        if open_seen && pitch.is_finite() && sampled {
            self.base.pitch_typ.push(pitch, long_hz);
        }
        let p_typ = self.base.pitch_typ.value;
        let usable = open_seen && (!pitch.is_finite() || (p_typ.is_finite() && (pitch - p_typ).abs() < PITCH_GATE));
        if usable && !self.base.ready {
            self.base.warm(skin_px, ap_px, 0.01 * r, 1.0 / fps);
        } else if usable {
            // A relearn's warm-up, if one runs (the levels below stay in use until it is ready)
            self.base.warm_next(skin_px, ap_px, 0.01 * r, 1.0 / fps);
            if params.tune.track && step_px > 0.0 {
                // One frame's share of the time constant.
                let k = 1.0 / (TRACK_TAU_S * fps);
                self.base.b += (skin_px - self.base.b).clamp(-0.5 * step_px, 0.5 * step_px) * k;
                let k_ap = 1.0 / (TRACK_AP_TAU_S * fps);
                if self.last_wide < TRACK_AP_MAX
                    && self.last_sq < TRACK_AP_MAX
                    && ap_px.is_finite()
                    && self.base.ap_n.is_finite()
                {
                    self.base.ap_n += (ap_px - self.base.ap_n).clamp(-0.25 * gap_px, 0.25 * gap_px) * k_ap;
                }
            }
        }
        self.base.tick(1.0 / fps);
        let ready = self.base.ready;
        let (b_px, ap_n_px) = self.base.levels();

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
        let apc = if pitch.is_finite() { ap_px + user.pitch_drop(pitch, p0) * r } else { ap_px };
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
        // Held at the last open values, but never widened: the pupil is often lost just as the eye widens to look aside
        // (2026-10-09 18:17: the right eye held at 0.93 / widen 0.71 for 10 s of a right look).
        let (eye_lid, wide, squint) =
            if edge_held { (self.last_out.0.min(0.75), 0.0, self.last_out.2) } else { (eye_lid, wide, squint) };
        if !closed {
            self.last_out = (eye_lid, wide, squint);
        }

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
            search_x_min,
            edge_gap,
            edge_held,
            b_n,
            b_w,
        }
    }
}

impl EyeEngine {
    /// A calibration's sample of the frame `process_open` just returned `o` for: `Sample::from_out`, and on normal
    /// and widen frames with the pupil the second, higher search for a widened upper lid (`widen_fit`), on the same
    /// frame (only during a calibration: live never runs it).
    pub fn calib_sample(&mut self, label: Label, o: &Out, t: f64) -> Sample {
        let mut s = Sample::from_out(label, o, t);
        if matches!(label, Label::Normal | Label::Widen)
            && o.f.ok_pupil
            && let Some(p) = self.prev_p
            && let Some((y, frac)) = self.ex.upper_high(&p, self.xmax)
        {
            (s.skin2, s.frac2) = ((p.cy - y) / o.r, frac);
        }
        s
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
    /// This frame's own upper lid line: its inlier share (NaN without one), and whether it was pushed against its
    /// band's edge (feat::LID_EDGE_SHARE: then `skin` and `ap` are NaN, the frame has no line).
    pub frac: f64,
    pub edge: bool,
    /// The second, higher search for a widened lid (feat::HIGH_BAND; normal and widen frames with the pupil, see
    /// `widen_fit`): its skin line (normalised by `r`, as `skin`, this frame alone) and inlier share; NaN when it
    /// did not run or found nothing.
    pub skin2: f64,
    pub frac2: f64,
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
        Sample { skin: self.skin * k, ap: self.ap * k, pd: self.pd * k, skin2: self.skin2 * k, r, ..*self }
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
        // A frame whose upper lid line was pushed against its band's edge is left out (its 5-frame medians may still
        // have a value from the frames around it).
        let edge = o.f.upper_edge;
        Sample {
            label,
            ok: o.f.ok_pupil,
            skin: if edge { f64::NAN } else { o.skin_up },
            ap: if edge { f64::NAN } else { o.aperture },
            pd: if o.f.ok_pupil { 2.0 * o.f.pupil_a / o.r } else { f64::NAN },
            pitch: o.pitch,
            lower_px: o.f.lower_y,
            r: o.r,
            iris_r: if o.f.ok_pupil && o.f.iris_n >= 4 { o.f.iris_r } else { f64::NAN },
            t,
            seen: Seen::of(o),
            frac: o.f.upper_frac,
            edge,
            skin2: f64::NAN,
            frac2: f64::NAN,
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
        let close: Vec<Sample> = s.iter().copied().filter(is(Label::Close)).collect();
        let n_pupil = normal.iter().filter(|x| x.ok).count();
        let (b_n, nn) = med_of(&normal, |x| x.ok.then_some(x.skin));
        let wf = widen_fit(s, b_n, need.widen);
        let (b_w, nw) = (wf.b_w, wf.n);
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
/// A widen frame whose upper lid line lies more than this (iris radii) below the normal level is left out of the
/// widen level: a widened lid is never lower than the normal one, so such a line is not the lid. Some wearers' lid
/// line is faint enough that the search sometimes takes another line in its band instead: the band's floor (about
/// 15 to 23 px under the pupil centre, -0.26 to -0.40 R) or the pupil's own upper rim (+0.45 R), both 0.5 R or more
/// under a normal level of about 1.0. A user's wear calibration of 2026-10-14 took the floor in more than half of its
/// widen frames and measured a widen level of -0.398 on both eyes ("見開きは取れなかった"); the same user's guided capture of
/// 2026-10-17 has the pupil rim in about a fifth of the right eye's widen frames (its third widen: 0.47, the real
/// lid 1.95). A real widen raises the line (the developer's by +0.25, that user's by +0.75 to 0.9), and 0.2 leaves room
/// for a widen held weakly around the normal level; frames lowered further than that are not a widen (a blink, a
/// look down), so leaving them out is right either way.
pub const WIDEN_MIN_BELOW_N: f64 = 0.2;

/// The widen level of the widen frames among `s`: the median upper lid line of those with the pupil and a line no
/// more than WIDEN_MIN_BELOW_N below the normal level `b_n`, and how many those are (they are what NEED_WIDEN counts,
/// so too few of them falls back to the usual widen step as before).
fn widen_level(s: &[Sample], b_n: f64) -> (f64, usize) {
    med_of(s, |x| (x.label == Label::Widen && x.ok && x.skin >= b_n - WIDEN_MIN_BELOW_N).then_some(x.skin))
}

/// When at least this share of an eye's widen frames with the pupil have no upper lid line within WIDEN_MIN_BELOW_N of
/// the normal level (one lower, or none: feat::LID_EDGE_SHARE), the widened lid has left the search band, and the
/// widen level comes from the higher band (feat::HIGH_BAND) instead. One wearer's 0.7.9 wear calibration
/// (2026-10-17): 789 of 792 widen frames (both eyes); the lid was seen at 1.87 R in the 3 others and at 2.00 to 2.04
/// (the band's top) in a guided capture. The developer's 84 calibrations: at most 15 of 396 (3.8%, half blinks).
pub const WIDEN_LOW_SHARE: f64 = 0.5;
/// A line from the higher band counts toward the widen level only with at least this inlier share (a lid line ~0.85,
/// another line the search settled on ~0.46: the same wearer's guided capture, 2026-10-17), ...
pub const WIDEN_HIGH_MIN_FRAC: f64 = 0.75;
/// ... at most this far (iris radii) above the normal level (a brow or a crease further up is not the lid), ...
pub const WIDEN_HIGH_MAX_STEP: f64 = 1.5;
/// ... within this of the line on the frame before or after it (a lid held wide does not jump), ...
pub const WIDEN_HIGH_NEIGHBOUR: f64 = 0.1;
/// ... and not within WIDEN_HIGH_FIXED of a line that the higher band also finds in at least WIDEN_HIGH_FIXED_SHARE
/// of the normal frames with the pupil (a crease or the brow, there whether the eye widens or not).
pub const WIDEN_HIGH_FIXED: f64 = 0.15;
pub const WIDEN_HIGH_FIXED_SHARE: f64 = 0.25;

/// Where a wear calibration's widen level came from (calib_result.json's `widen_source`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WidenSource {
    /// The usual band (`widen_level`): the widen frames mostly have their lid line in it.
    Main,
    /// The higher band: most widen frames had no lid line near the normal level in the usual one (WIDEN_LOW_SHARE).
    Upper,
    /// As for `Upper`, but the higher band had too few good lines: the usual band's level as before (`widen_level`;
    /// with too few frames there too, the usual step).
    Fallback,
}

impl WidenSource {
    pub fn name(self) -> &'static str {
        match self {
            WidenSource::Main => "main",
            WidenSource::Upper => "upper",
            WidenSource::Fallback => "fallback",
        }
    }
}

/// One eye's widen level (`widen_fit`): the level and how many frames it is from (NEED_WIDEN counts them), where it
/// came from, the widen frames with the pupil and those of them without a lid line near the normal level (the
/// trigger), the higher band's good lines and those taken, and the line it also finds in the normal frames (NaN: none).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct WidenFit {
    pub b_w: f64,
    pub n: usize,
    pub source: WidenSource,
    pub with_pupil: usize,
    pub low: usize,
    pub high_good: usize,
    pub high_used: usize,
    pub fixed: f64,
}

/// The widen level from one eye's samples `s` (all steps, in one radius) for normal level `b_n`, `need` frames
/// needed. Usually `widen_level`. When most widen frames with the pupil have no lid line near the normal level
/// (WIDEN_LOW_SHARE), the median of the higher band's lines (`Sample::skin2`) that pass the WIDEN_HIGH_* checks; with
/// fewer than `need` of those, `widen_level` as before. For a wearer whose lid stays in the usual band this is
/// `widen_level` exactly.
pub fn widen_fit(s: &[Sample], b_n: f64, need: usize) -> WidenFit {
    let widen: Vec<&Sample> = s.iter().filter(|x| x.label == Label::Widen).collect();
    let with_pupil = widen.iter().filter(|x| x.ok).count();
    let low = widen.iter().filter(|x| x.ok && (x.skin.is_nan() || x.skin < b_n - WIDEN_MIN_BELOW_N)).count();
    let main = |source| {
        let (b_w, n) = widen_level(s, b_n);
        WidenFit { b_w, n, source, with_pupil, low, high_good: 0, high_used: 0, fixed: f64::NAN }
    };
    if with_pupil == 0 || (low as f64) < WIDEN_LOW_SHARE * with_pupil as f64 {
        return main(WidenSource::Main);
    }
    let good =
        |x: &Sample| x.ok && x.skin2.is_finite() && x.frac2 >= WIDEN_HIGH_MIN_FRAC && x.skin2 - b_n <= WIDEN_HIGH_MAX_STEP;
    // A line the higher band finds in the normal frames too is not the widened lid.
    let normal_ok = s.iter().filter(|x| x.label == Label::Normal && x.ok).count();
    let (fixed, n_fixed) = med_of(s, |x| (x.label == Label::Normal && good(x)).then_some(x.skin2));
    let fixed = if n_fixed > 0 && n_fixed as f64 >= WIDEN_HIGH_FIXED_SHARE * normal_ok as f64 { fixed } else { f64::NAN };
    let cand: Vec<bool> = widen.iter().map(|x| good(x) && (fixed.is_nan() || (x.skin2 - fixed).abs() >= WIDEN_HIGH_FIXED)).collect();
    let high_good = widen.iter().filter(|x| good(x)).count();
    // Neighbours: the widen frames just before and after (the same step: under 0.1 s apart).
    let near = |i: usize, j: usize| {
        cand[j] && (widen[i].t - widen[j].t).abs() < 0.1 && (widen[i].skin2 - widen[j].skin2).abs() <= WIDEN_HIGH_NEIGHBOUR
    };
    let mut used: Vec<f64> = (0..widen.len())
        .filter(|&i| cand[i] && ((i > 0 && near(i, i - 1)) || (i + 1 < widen.len() && near(i, i + 1))))
        .map(|i| widen[i].skin2)
        .collect();
    let high_used = used.len();
    if high_used < need.max(1) {
        return WidenFit { high_good, high_used, fixed, ..main(WidenSource::Fallback) };
    }
    let b_w = vision::median(&mut used);
    WidenFit { b_w, n: high_used, source: WidenSource::Upper, with_pupil, low, high_good, high_used, fixed }
}

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
        let wf = widen_fit(&s, b_n, need.widen);
        let (b_w, nw) = (wf.b_w, wf.n);
        let below = s.iter().filter(|x| x.label == Label::Widen && x.ok && x.skin < b_n - WIDEN_MIN_BELOW_N).count();
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
\"normal_frames_with_pupil\": {n_pupil}, \"normal_frames_with_lid_line\": {nn}, \"widen_frames_with_pupil\": {nw}, \"widen_frames_below_normal\": {below}, \"close_frames_with_lids\": {nc}, \
\"pupil_seen_share\": {{\"normal\": {}, \"widen\": {}, \"close\": {}}}, \"diag\": {}, \
\"widen_source\": \"{}\", \"widen_frames_no_line_near_normal\": {}, \"widen_frames_upper_good\": {}, \"widen_frames_upper_used\": {}, \
\"widen_upper_fixed_line\": {}}}",
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
            wf.source.name(),
            wf.low,
            wf.high_good,
            wf.high_used,
            json::num(wf.fixed),
        ));
        lines.push(format!(
            "calib wear {}: widen step {:.3} (needs >= {MIN_WIDEN_STEP}; normal {:.3}, widen {:.3}), open gap {:.3} (needs >= {MIN_OPEN_GAP}), \
frames with pupil normal {n_pupil} of {}, with the upper lid line {nn} / widen {nw} (need {} / {}), close {nc} (need {}; at {:.0} fps), widen below normal left out {below}, widen from {} (no line near normal {} of {}, upper good {} / used {}), R {:.1} px; {}",
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
            wf.source.name(),
            wf.low,
            wf.with_pupil,
            wf.high_good,
            wf.high_used,
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
    let mut out = String::from("eye,t,label,ok,skin_up,aperture,pupil_ratio,pitch,lower_px,r,iris_r,pupil_x,pupil_y,miss,rejected,contrast,x_min,prior_reset,upper_frac,lid_edge,skin_up2,upper_frac2\n");
    let f = |v: f64| if v.is_finite() { format!("{v:.5}") } else { String::new() };
    for (e, s) in samples.iter().enumerate() {
        for x in s {
            out += &format!(
                "{},{:.6},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
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
                x.seen.prior_reset as u8,
                f(x.frac),
                x.edge as u8,
                f(x.skin2),
                f(x.frac2)
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
                frac: 1.0,
                edge: false,
                skin2: f64::NAN,
                frac2: f64::NAN,
            })
            .collect()
    }

    /// One eye's wear steps with a user's levels: normal 1.0 R, closed aperture 0.56, and the given widen frames.
    fn wear_eye_with_widen(widen: &[(usize, f64)]) -> Vec<Sample> {
        let mut eye = samples(Label::Normal, 300, 1.0, 1.7, -14.0, 250.0, true);
        for &(n, skin) in widen {
            eye.extend(samples(Label::Widen, n, skin, 2.4, -14.0, 252.0, true));
        }
        eye.extend(samples(Label::Close, 80, f64::NAN, 0.56, -14.0, 240.0, false));
        eye
    }

    #[test]
    fn widen_frames_with_the_lid_line_below_normal_are_left_out() {
        // A user's guided capture of 2026-10-17: in some widen frames the line found is the search band's floor
        // (-0.35 R) or the pupil's upper rim (+0.45 R), not the widened lid (1.85 R). Taken together their median
        // was the rim (0.45), below normal: no widen. Without them it is the lid.
        let faint = wear_eye_with_widen(&[(90, 1.85), (60, -0.35), (50, 0.45)]);
        let both = [faint.clone(), faint];
        let (w, measured) = fit_wear(&both, [0.19; 2]).unwrap();
        assert!(measured);
        assert!((w[0].b_w - 1.85).abs() < 1e-9 && (w[0].b_n - 1.0).abs() < 1e-9, "{:?}", w[0]);
        // The report has the same widen level, and how many frames were left out.
        let (report, lines) = wear_report(&both);
        assert!(report.contains("\"widen_step\": 0.85") && report.contains("\"widen_frames_below_normal\": 110"), "{report}");
        assert!(lines[0].contains("widen 90 (need ") && lines[0].contains("widen below normal left out 110,"), "{}", lines[0]);
        // The same user's wear calibration of 2026-10-14 (-0.398 on both eyes): mostly the floor, too few frames left for a
        // widen. It falls back to the usual step as it did before, and does not fail.
        let floor = wear_eye_with_widen(&[(50, 1.85), (150, -0.398)]);
        let (w, measured) = fit_wear(&[floor.clone(), floor], [0.19; 2]).unwrap();
        assert!(!measured && (w[0].b_w - w[0].b_n - 0.19).abs() < 1e-9, "{:?}", w[0]);
        // A wearer whose lid line is found: a few frames a little under normal (a weak start, a half blink) stay in,
        // and the widen level is the plain median as before.
        let usual = wear_eye_with_widen(&[(150, 1.25), (30, 1.2), (20, 0.85)]);
        let (w, measured) = fit_wear(&[usual.clone(), usual.clone()], [0.19; 2]).unwrap();
        let (plain, n) = med_of(&usual, |x| (x.label == Label::Widen && x.ok).then_some(x.skin));
        assert!(measured && n == 200 && w[0].b_w == plain && (plain - 1.25).abs() < 1e-9, "{:?}", w[0]);
        assert!(wear_report(&[usual.clone(), usual]).0.contains("\"widen_frames_below_normal\": 0"));
    }

    /// `n` widen frames whose usual band has no line near normal (skin NaN, as when the line was pushed against the
    /// band's edge) and whose higher band has the line at `skin2` (+ a little jitter), inlier share `frac2`.
    fn widen_high(n: usize, skin2: f64, frac2: f64) -> Vec<Sample> {
        let mut v = samples(Label::Widen, n, f64::NAN, 2.4, -14.0, 252.0, true);
        for (i, x) in v.iter_mut().enumerate() {
            (x.edge, x.skin2, x.frac2) = (true, skin2 + (i % 3) as f64 * 0.01, frac2);
        }
        v
    }

    /// One eye's wear steps: normal 1.0 R, closed 0.56, and the given widen frames.
    fn wear_widen(widen: Vec<Sample>) -> Vec<Sample> {
        let mut eye = samples(Label::Normal, 300, 1.0, 1.7, -14.0, 250.0, true);
        eye.extend(widen);
        eye.extend(samples(Label::Close, 80, f64::NAN, 0.56, -14.0, 240.0, false));
        eye
    }

    #[test]
    fn a_widened_lid_above_the_band_is_measured_in_the_higher_band() {
        // A user's 0.7.9 wear calibration (2026-10-17): during the widen the lid line never entered the band; the
        // search found its bottom edge (now no line) or the floor (-0.3). The higher band has the lid at 1.9.
        let mut widen = widen_high(250, 1.9, 0.85);
        widen.extend(samples(Label::Widen, 100, -0.3, 2.4, -14.0, 252.0, true));
        let eye = wear_widen(widen);
        let wf = widen_fit(&eye, 1.0, 60);
        assert_eq!((wf.source, wf.with_pupil, wf.low, wf.high_good, wf.high_used), (WidenSource::Upper, 350, 350, 250, 250));
        assert!((wf.b_w - 1.91).abs() < 1e-9, "{wf:?}");
        let both = [eye.clone(), eye];
        let (w, measured) = fit_wear(&both, [0.19; 2]).unwrap();
        assert!(measured && (w[0].b_w - 1.91).abs() < 1e-9, "{:?}", w[0]);
        let (report, lines) = wear_report(&both);
        assert!(report.contains("\"widen_source\": \"upper\"") && report.contains("\"widen_frames_upper_used\": 250"), "{report}");
        assert!(report.contains("\"widen_frames_below_normal\": 100"), "the existing key counts as before: {report}");
        assert!(lines[0].contains("widen from upper (no line near normal 350 of 350, upper good 250 / used 250)"), "{}", lines[0]);
        // The CSV has the new columns: this frame's line share, the edge flag, the higher band's line.
        let csv = samples_csv(&both);
        assert!(csv.lines().next().unwrap().ends_with(",prior_reset,upper_frac,lid_edge,skin_up2,upper_frac2"));
        assert!(csv.lines().any(|l| l.starts_with("L,") && l.contains(",widen,") && l.ends_with(",1.00000,1,1.90000,0.85000")), "{csv}");
    }

    #[test]
    fn the_higher_band_is_not_used_when_most_widen_frames_have_the_lid() {
        // The developer's calibrations: the lid in the band, a few frames of a half blink below normal (at most 15 of
        // 396). Whatever the higher band sees (here a brow at 2.4 in every frame), the widen level is 0.7.9's.
        let mut widen = samples(Label::Widen, 381, 1.25, 2.4, -14.0, 252.0, true);
        widen.extend(samples(Label::Widen, 15, 0.6, 1.2, -14.0, 252.0, true));
        for x in &mut widen {
            (x.skin2, x.frac2) = (2.4, 0.95);
        }
        let eye = wear_widen(widen);
        let wf = widen_fit(&eye, 1.0, 60);
        let (plain, n) = widen_level(&eye, 1.0);
        assert_eq!((wf.source, wf.low, wf.b_w, wf.n), (WidenSource::Main, 15, plain, n));
        assert!((plain - 1.25).abs() < 1e-9 && n == 381);
        let both = [eye.clone(), eye];
        assert!(wear_report(&both).0.contains("\"widen_source\": \"main\""));
        // Half of them low is enough to look higher (WIDEN_LOW_SHARE), less is not.
        let mut half = samples(Label::Widen, 100, 1.25, 2.4, -14.0, 252.0, true);
        half.extend(widen_high(99, 1.9, 0.85));
        assert_eq!(widen_fit(&wear_widen(half.clone()), 1.0, 60).source, WidenSource::Main);
        half.extend(widen_high(1, 1.9, 0.85));
        assert_eq!(widen_fit(&wear_widen(half), 1.0, 60).source, WidenSource::Upper);
    }

    #[test]
    fn without_a_good_line_in_the_higher_band_the_widen_falls_back_as_before() {
        // Nothing found up there: as 0.7.9 (too few frames near normal: the usual step).
        let mut widen = widen_high(250, f64::NAN, f64::NAN);
        widen.extend(samples(Label::Widen, 50, 1.85, 2.4, -14.0, 252.0, true));
        let eye = wear_widen(widen);
        let wf = widen_fit(&eye, 1.0, 60);
        assert_eq!((wf.source, wf.high_good, wf.n), (WidenSource::Fallback, 0, 50));
        let (w, measured) = fit_wear(&[eye.clone(), eye.clone()], [0.19; 2]).unwrap();
        assert!(!measured && (w[0].b_w - w[0].b_n - 0.19).abs() < 1e-9, "{:?}", w[0]);
        assert!(wear_report(&[eye.clone(), eye]).0.contains("\"widen_source\": \"fallback\""));
        let fit = |widen: Vec<Sample>| widen_fit(&wear_widen(widen), 1.0, 60);
        // Weak lines (another line the search settled on) do not count.
        assert_eq!(fit(widen_high(250, 1.9, 0.5)).source, WidenSource::Fallback);
        // Nor one more than 1.5 R above normal (a brow).
        assert_eq!(fit(widen_high(250, 2.6, 0.9)).source, WidenSource::Fallback);
        // Nor a line that jumps from frame to frame.
        let mut jumpy = widen_high(250, 1.9, 0.9);
        for (i, x) in jumpy.iter_mut().enumerate() {
            x.skin2 = 1.7 + 0.3 * (i % 3) as f64;
        }
        assert_eq!(fit(jumpy).source, WidenSource::Fallback);
        // Nor a line the higher band also finds in the normal frames (a crease): there whether widened or not.
        let mut eye = wear_widen(widen_high(250, 1.9, 0.9));
        for x in eye.iter_mut().filter(|x| x.label == Label::Normal).take(100) {
            (x.skin2, x.frac2) = (1.95, 0.9);
        }
        let wf = widen_fit(&eye, 1.0, 60);
        assert_eq!((wf.source, wf.high_good, wf.high_used), (WidenSource::Fallback, 250, 0), "{wf:?}");
        assert!((wf.fixed - 1.95).abs() < 1e-9);
        // In a few normal frames only, it is not a fixed line.
        let mut eye = wear_widen(widen_high(250, 1.9, 0.9));
        for x in eye.iter_mut().filter(|x| x.label == Label::Normal).take(50) {
            (x.skin2, x.frac2) = (1.95, 0.9);
        }
        assert_eq!(widen_fit(&eye, 1.0, 60).source, WidenSource::Upper);
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

    /// Lid skin (150) with the eye open between rows 130 and 270 (sclera 110), the iris (70) and pupil (12) at `pupil`
    /// (radius 25) if any, and the lens-edge band; `open` false: lid skin all over (a closed eye).
    fn lid_frame(pupil: Option<(f64, f64)>, open: bool) -> Vec<u8> {
        lid_frame_between(pupil, if open { 130..270 } else { 0..0 })
    }

    /// `lid_frame` with the eye open between the rows of `opening`.
    fn lid_frame_between(pupil: Option<(f64, f64)>, opening: std::ops::Range<usize>) -> Vec<u8> {
        let mut img = vec![150u8; vision::W * vision::H];
        for y in 0..vision::H {
            for x in 0..vision::W {
                let v = &mut img[y * vision::W + x];
                if x >= 350 {
                    *v = 4;
                } else if opening.contains(&y) {
                    *v = 110;
                    if let Some((px, py)) = pupil {
                        let d = (x as f64 - px).hypot(y as f64 - py);
                        if d < 25.0 {
                            *v = 12;
                        } else if d < 55.0 {
                            *v = 70;
                        }
                    }
                }
            }
        }
        img
    }

    /// An engine that has seen the open eye looking ahead for a second, then `at` (the pupil's centre) for 10 frames.
    fn seen_at(at: (f64, f64), tune: Tune) -> (EyeEngine, Params, f64) {
        let p = Params { tune, ..Params::default() };
        let mut e = EyeEngine::default();
        let mut t = 0.0;
        for k in 0..100 {
            let pupil = if k < 90 { (250.0, 200.0) } else { at };
            t = k as f64 / FPS;
            let o = e.process(&lid_frame(Some(pupil), true), f64::NAN, t, &p);
            assert!(o.f.ok_pupil && !o.closed, "frame {k}: {:?}", o.f.diag);
        }
        (e, p, t)
    }

    /// Whether the engine reports closed within 10 frames of `frame`, and the outputs of the last one.
    fn closes(e: &mut EyeEngine, p: &Params, t0: f64, frame: &[u8]) -> (bool, Out) {
        let mut any = false;
        let mut last = None;
        for k in 1..=10 {
            let o = e.process(frame, f64::NAN, t0 + k as f64 / FPS, p);
            assert!(!o.f.ok_pupil);
            any |= o.closed;
            last = Some(o);
        }
        (any, last.unwrap())
    }

    #[test]
    fn a_pupil_lost_at_the_temporal_edge_with_the_lids_apart_is_not_closed() {
        // The pupil gone, the iris box all sclera (brighter than the open eye's), the lids where they were.
        let lost = lid_frame(None, true);
        let shut = lid_frame(None, false);
        let edge = (128.0, 200.0);
        // Lost looking ahead: closed, as before.
        let (mut e, p, t) = seen_at((250.0, 200.0), Tune::default());
        let (closed, o) = closes(&mut e, &p, t, &lost);
        assert!(closed && !o.edge_held, "{o:?}");
        assert_eq!(o.x_min, feat::X_MIN_LO);
        // Lost with its small-x edge 3 px from the window's: not closed, the open eye's values held.
        let (mut e, p, t) = seen_at(edge, Tune::default());
        let before = e.process(&lid_frame(Some(edge), true), f64::NAN, t + 0.5 / FPS, &p);
        let (closed, o) = closes(&mut e, &p, t + 0.5 / FPS, &lost);
        assert!(!closed && o.edge_held && o.valid, "{o:?}");
        assert!((o.edge_gap - 3.0).abs() < 1.0 && o.aperture >= EDGE_OPEN_AP, "{o:?}");
        assert_eq!((o.eye_lid, o.eye_wide, o.eye_squint), (before.eye_lid, before.eye_wide, before.eye_squint));
        assert!(o.eye_lid > 0.5);
        // The pupil back: no longer held.
        let o = e.process(&lid_frame(Some(edge), true), f64::NAN, t + 1.0, &p);
        assert!(o.f.ok_pupil && !o.edge_held && !o.closed);
        // The eye closing after the glance (the last pupil still at the edge, the lids together): closed.
        let (closed, o) = closes(&mut e, &p, t + 1.0, &shut);
        assert!(closed && !o.edge_held, "{o:?}");
        assert_eq!(o.eye_lid, 0.0);
        // The pupil back after that close, then lost at the edge again: held at the open eye's values, not at closed.
        e.process(&lid_frame(Some(edge), true), f64::NAN, t + 1.5, &p);
        let (closed, o) = closes(&mut e, &p, t + 1.5, &lost);
        assert!(!closed && o.edge_held && o.eye_lid > 0.5, "{o:?}");
        // Held after a widened open eye: not widened (relaxed open at most).
        let (mut e, p, t) = seen_at(edge, Tune::default());
        e.last_out = (0.93, 0.71, 0.0);
        let (closed, o) = closes(&mut e, &p, t, &lost);
        assert!(!closed && o.edge_held && o.eye_lid == 0.75 && o.eye_wide == 0.0, "{o:?}");
        // 14 px from the edge (the right eye on 2026-10-09 15:23 was lost at 15): held too.
        let (mut e, p, t) = seen_at((139.0, 200.0), Tune::default());
        let (closed, o) = closes(&mut e, &p, t, &lost);
        assert!(!closed && o.edge_held && (o.edge_gap - 14.0).abs() < 1.0, "{o:?}");
        // The old rule (--no-edge-hold): closed.
        let (mut e, p, t) = seen_at(edge, Tune { edge_hold: false, ..Tune::default() });
        let (closed, o) = closes(&mut e, &p, t, &lost);
        assert!(closed && !o.edge_held, "{o:?}");
    }

    #[test]
    fn the_extended_search_follows_a_pupil_past_the_window_edge() {
        // The window's edge fixed at 186 (as if the shading ended at 196); the lid frames have no shading of their own.
        let run = |tune: Tune| {
            let p = Params { tune, ..Params::default() };
            let mut e = EyeEngine::default();
            let mut t = 0.0;
            let mut out = Vec::new();
            let mut feed = |e: &mut EyeEngine, at: Option<(f64, f64)>, open: f64, n: usize, out: &mut Vec<Out>| {
                for _ in 0..n {
                    out.push(e.process_open(&lid_frame(at, true), f64::NAN, open, t, &p));
                    if e.frame_no == 1 {
                        (e.cm_n, e.x_min, e.find_x_min) = (40, 186.0, 186.0);
                    }
                    t += 1.0 / FPS;
                }
            };
            // Ahead for a second: the extended edge stays at x_min
            feed(&mut e, Some((250.0, 200.0)), 1.0, 90, &mut out);
            let ahead = *out.last().unwrap();
            // Out toward the ear: the window still finds it cut by its edge (175..225), and the extended edge moves out
            feed(&mut e, Some((200.0, 200.0)), 1.0, 10, &mut out);
            let near = *out.last().unwrap();
            // Further out (135..185, left of the window): only the extended search can find it (and its iris fit
            // does not go into the iris radius)
            let r_n = e.r_est.len();
            feed(&mut e, Some((160.0, 200.0)), 1.0, 10, &mut out);
            let past = *out.last().unwrap();
            assert_eq!(e.r_est.len(), r_n);
            // ... not while Valve says the eye is closing
            feed(&mut e, Some((160.0, 200.0)), 0.2, 1, &mut out);
            let closing = *out.last().unwrap();
            // Lost out there: held at the edge, not closed
            feed(&mut e, None, 1.0, 10, &mut out);
            let lost = out[out.len() - 10..].to_vec();
            // Back ahead for 2 s, then 6 s more
            feed(&mut e, Some((250.0, 200.0)), 1.0, 180, &mut out);
            let back2 = *out.last().unwrap();
            feed(&mut e, Some((250.0, 200.0)), 1.0, 540, &mut out);
            (ahead, near, past, closing, lost, back2, *out.last().unwrap(), e.wear_state())
        };
        let (ahead, near, past, closing, lost, back2, back8, state) = run(Tune::default());
        assert!(ahead.f.ok_pupil && (ahead.x_min, ahead.search_x_min) == (186.0, 186.0), "{ahead:?}");
        assert!(near.f.ok_pupil && !near.f.diag.extended && near.f.diag.left_edge, "{:?}", near.f.diag);
        assert_eq!(near.search_x_min, 186.0 - EXT_MAX_PX, "its small-x edge (175) less EXT_NEAR_PX, capped");
        assert!(past.f.ok_pupil && past.f.diag.extended && !past.closed, "{:?}", past.f.diag);
        assert!((past.f.pupil_cx - 160.0).abs() < 1.5 && past.x_min == 186.0, "{past:?}");
        assert!(!closing.f.ok_pupil && !closing.f.diag.ext_tried, "{:?}", closing.f.diag);
        assert!(lost.iter().all(|o| !o.closed && !o.f.ok_pupil) && lost[9].edge_held && lost[9].edge_gap < 0.0, "{:?}", lost[9]);
        // Back: EXT_BACK_PX_S toward x_min, then there
        assert!((back2.search_x_min - (162.0 + 2.0 * EXT_BACK_PX_S)).abs() < 0.5, "{}", back2.search_x_min);
        assert_eq!((back8.search_x_min, state.search_x_min, state.x_min), (186.0, 186.0, 186.0));
        // Without it (--no-ext-search): the pupil out there is not found, the edge never moves
        let (_, near, past, _, _, _, _, _) = run(Tune { ext_search: false, ..Tune::default() });
        assert!(near.f.ok_pupil && near.search_x_min == 186.0);
        assert!(!past.f.ok_pupil && past.search_x_min == 186.0 && !past.f.diag.ext_tried, "{:?}", past.f.diag);
    }

    #[test]
    fn the_aperture_baseline_follows_lids_that_settle_lower_but_not_a_squint() {
        // A user calibration (squint and the eyelid from the aperture), 15 frames a second
        let p = Params { user: Some(UserParams::default()), ..Params::default() };
        let mut e = EyeEngine::default();
        let mut t = 0.0;
        let mut run = |e: &mut EyeEngine, opening: std::ops::Range<usize>, seconds: f64| {
            let mut last = None;
            for _ in 0..(seconds * 15.0) as usize {
                t += 1.0 / 15.0;
                last = Some(e.process(&lid_frame_between(Some((250.0, 200.0)), opening.clone()), f64::NAN, t, &p));
            }
            last.unwrap()
        };
        let o = run(&mut e, 130..270, 7.0);
        e.seed_px(o.skin_up * o.r, o.aperture * o.r);
        let (_, ap_open) = e.baseline_px().unwrap();
        let o = run(&mut e, 130..270, 2.0);
        assert!(o.eye_lid > 0.74 && o.eye_squint < 0.01, "{o:?}");
        // The upper lid settles 14 px lower: a mild squint at first, then the baseline follows (20 s time constant)
        let o = run(&mut e, 144..270, 1.0);
        assert!((0.2..0.6).contains(&o.eye_squint), "{o:?}");
        let o = run(&mut e, 144..270, 45.0);
        let (_, ap_low) = e.baseline_px().unwrap();
        assert!(ap_open - ap_low > 0.8 * 14.0, "{ap_open} -> {ap_low}");
        assert!(o.eye_lid > 0.7 && o.eye_squint < 0.1, "{o:?}");
        // A real squint (lids 50 px nearer) shows and does not move the baseline
        let o = run(&mut e, 160..250, 3.0);
        assert!(o.eye_squint > 0.9 && o.eye_lid < 0.3, "{o:?}");
        assert!((e.baseline_px().unwrap().1 - ap_low).abs() < 0.5);
    }

    #[test]
    fn the_pitch_correction_is_held_between_the_downgaze_floor_and_its_cap() {
        let flat = UserParams::default();
        let floor = |below: f64| LID_PITCH_PER_DEG * (below - LID_PITCH_FROM_DEG);
        // A curve flatter than the floor (or none) gets the floor, nothing within LID_PITCH_FROM_DEG of the typical pitch
        assert!((flat.pitch_drop(-37.0, -10.0) - floor(27.0)).abs() < 1e-12);
        assert_eq!((flat.pitch_drop(-10.0 - LID_PITCH_FROM_DEG, -10.0), flat.pitch_drop(-10.0, -10.0)), (0.0, 0.0));
        let steep = |slope: f64| UserParams { pitch_c: [1.5, slope, 0.0], ..UserParams::default() };
        assert!((steep(0.005).pitch_drop(-37.0, -10.0) - floor(27.0)).abs() < 1e-12);
        // A steeper one counts, up to LID_PITCH_CAP times the floor
        assert!((steep(0.025).pitch_drop(-37.0, -10.0) - 0.025 * 27.0).abs() < 1e-12);
        assert!((steep(0.06).pitch_drop(-37.0, -10.0) - LID_PITCH_CAP * floor(27.0)).abs() < 1e-12);
        // Far down it stops growing
        assert!((flat.pitch_drop(-90.0, -10.0) - LID_PITCH_PER_DEG * LID_PITCH_MAX_DEG).abs() < 1e-12);
        // Looking up: a curve that says narrower counts, one that says wider does not
        assert!((steep(-0.01).pitch_drop(20.0, -10.0) - 0.3).abs() < 1e-12);
        assert_eq!((steep(0.01).pitch_drop(20.0, -10.0), flat.pitch_drop(20.0, -10.0)), (0.0, 0.0));
    }

    #[test]
    fn an_open_eye_looking_down_reads_open_with_a_shallow_curve_and_a_squint_still_shows() {
        // A user calibration whose curve narrows the eye by only 0.01 iris radii a degree going down (and widens it
        // going up), 15 frames a second
        let p = Params {
            user: Some(UserParams { pitch_c: [1.5, 0.01, 0.0], ..UserParams::default() }),
            ..Params::default()
        };
        let mut e = EyeEngine::default();
        let mut t = 0.0;
        let mut run = |e: &mut EyeEngine, opening: std::ops::Range<usize>, pitch: f64, seconds: f64| {
            let mut last = None;
            for _ in 0..(seconds * 15.0) as usize {
                t += 1.0 / 15.0;
                last = Some(e.process(&lid_frame_between(Some((250.0, 200.0)), opening.clone()), pitch, t, &p));
            }
            last.unwrap()
        };
        let o = run(&mut e, 130..270, -10.0, 7.0);
        e.seed_px(o.skin_up * o.r, o.aperture * o.r);
        let o = run(&mut e, 130..270, -10.0, 2.0);
        assert!(o.eye_lid > 0.74 && o.eye_squint < 0.01, "{o:?}");
        // 27 degrees down the upper lid comes 25 px lower (0.45 iris radii; the floor there is 0.53): still open
        let o = run(&mut e, 155..270, -37.0, 3.0);
        assert!(25.0 / o.r < LID_PITCH_PER_DEG * (27.0 - LID_PITCH_FROM_DEG), "{}", o.r);
        assert!(o.eye_lid > 0.74 && o.eye_squint < 0.01, "{o:?}");
        // A squint down there still shows
        let o = run(&mut e, 165..240, -37.0, 3.0);
        assert!(o.eye_squint > 0.6 && o.eye_lid < 0.45, "{o:?}");
        // Looking up with the lids where they were: the curve's "wider up there" no longer takes the eyelid down
        let o = run(&mut e, 130..270, 20.0, 3.0);
        assert!(o.eye_lid > 0.74 && o.eye_squint < 0.01, "{o:?}");
    }

    #[test]
    fn the_skin_line_baseline_follows_a_higher_lid_and_a_held_widen_fades_slowly() {
        let p = Params::default();
        let mut e = EyeEngine::default();
        let mut t = 0.0;
        // At 15 frames a second; the outputs of every frame
        let mut run = |e: &mut EyeEngine, opening: std::ops::Range<usize>, seconds: f64| {
            (0..(seconds * 15.0) as usize)
                .map(|_| {
                    t += 1.0 / 15.0;
                    e.process(&lid_frame_between(Some((250.0, 200.0)), opening.clone()), f64::NAN, t, &p)
                })
                .collect::<Vec<_>>()
        };
        let o = run(&mut e, 130..270, 7.0).pop().unwrap();
        e.seed_px(o.skin_up * o.r, o.aperture * o.r);
        let (b_open, _) = e.baseline_px().unwrap();
        assert!(run(&mut e, 130..270, 2.0).iter().all(|o| o.eye_wide < 0.01));
        // The upper lid 10 px higher (about one widen step, 0.19 x the iris radius): widened at once, 0.5 or more for
        // about 22 s (0.7 x TRACK_TAU_S), and followed (EyeWide below 0.2) within a minute and a half
        let out = run(&mut e, 120..270, 90.0);
        let wide_for = out.iter().skip(5).position(|o| o.eye_wide < 0.5).map_or(f64::INFINITY, |k| (k + 5) as f64 / 15.0);
        assert!(out[5].eye_wide > 0.9 && (15.0..35.0).contains(&wide_for), "{} {wide_for}", out[5].eye_wide);
        assert!(out.last().unwrap().eye_wide < 0.2, "{:?}", out.last());
        assert!(e.baseline_px().unwrap().0 - b_open > 7.0);
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

    #[test]
    fn relearn_restarts_only_the_warm_up() {
        let mut e = EyeEngine::default();
        // A wear calibration's levels, and what the engine learnt about the wear meanwhile
        let w = WearParams { r_px: 50.0, b_n: 0.8, b_w: 1.0, ap_n: 1.6, ap_cl: 0.5, pitch_n: -12.0, lower_px: 240.0 };
        e.seed(&w);
        assert!(e.baseline_ready() && e.warmup_remaining_s() == 0.0);
        assert_eq!(e.baseline_px(), Some((40.0, 80.0)));
        e.xmax = 300.0;
        e.x_min = 120.0;
        e.find_x_min = 110.0;
        for k in 0..100 {
            e.base.pitch_typ.push(-10.0 + (k % 3) as f64, 45.0);
            e.r_est.push(50.0, 45.0);
            e.box_open.push(90.0, 45.0);
            e.a_open.push(12.0, 45.0);
        }
        e.t_first = 3.0;
        let pitch = e.base.pitch_typ.value;
        assert!(pitch.is_finite());
        e.relearn();
        // A new warm-up, counted down, while the old levels stay in use
        assert!(e.baseline_ready() && e.relearning());
        assert_eq!(e.warmup_remaining_s(), WARMUP_S);
        assert_eq!(e.baseline_px(), Some((40.0, 80.0)));
        let next = e.base.next.as_ref().unwrap();
        assert!(next.b.is_nan() && next.ap_n.is_nan() && next.skin.is_empty() && next.seconds == 0.0 && !next.ready);
        // ...the rest of the wear kept
        assert_eq!((e.xmax, e.x_min, e.find_x_min, e.t_first), (300.0, 120.0, 110.0, 3.0));
        assert_eq!((e.r_est.value, e.box_open.value, e.a_open.value), (50.0, 90.0, 12.0));
        assert_eq!((e.base.pitch_typ.len(), e.base.pitch_typ.value), (100, pitch));
        // A wear calibration seeds it again at once, as before (and ends the relearn)
        e.seed(&w);
        assert!(e.baseline_ready() && !e.relearning() && e.warmup_remaining_s() == 0.0);
        // reset (a new wear) forgets the wear too
        e.reset();
        assert!(e.base.pitch_typ.value.is_nan() && e.r_est.value.is_nan() && e.xmax == 346.0);
    }

    /// `n` seconds of usable frames at 90 fps with these levels (px) for a relearn's warm-up.
    fn warm_next_for(base: &mut AutoBase, seconds: f64, skin: f64, ap: f64) {
        for _ in 0..(seconds * FPS).round() as usize {
            base.warm_next(skin, ap, 0.5, 1.0 / FPS);
        }
    }

    #[test]
    fn relearn_keeps_the_old_levels_until_the_new_ones_are_learnt() {
        let mut e = EyeEngine::default();
        let w = WearParams { r_px: 50.0, b_n: 0.8, b_w: 1.0, ap_n: 1.6, ap_cl: 0.5, pitch_n: -12.0, lower_px: 240.0 };
        e.seed(&w);
        e.relearn();
        // Half the warm-up: the old levels in use, the new warm-up counting down
        warm_next_for(&mut e.base, 15.0, 46.0, 86.0);
        assert!(e.relearning() && e.baseline_ready());
        assert!((e.warmup_remaining_s() - 15.0).abs() < 0.02, "{}", e.warmup_remaining_s());
        assert_eq!(e.baseline_px(), Some((40.0, 80.0)));
        // process() uses them: it reports what an engine with the old levels and no relearn reports, and an engine with
        // the new ones something else (the same frames for all three, so the same iris radius; the levels are not
        // followed here, to compare them exactly)
        let mut p = Params::default();
        p.tune.track = false;
        let eye = disc_frame(Some((250.0, 200.0, 25.0)));
        let (mut old, mut new) = (EyeEngine::default(), EyeEngine::default());
        old.seed(&w);
        new.seed(&w);
        new.seed_px(46.0, 86.0);
        let o = e.process(&eye, f64::NAN, 0.0, &p);
        let (o_old, o_new) = (old.process(&eye, f64::NAN, 0.0, &p), new.process(&eye, f64::NAN, 0.0, &p));
        assert!(o.b_n.is_finite() && o.b_n == o_old.b_n && o.b_n < o_new.b_n, "{} {} {}", o.b_n, o_old.b_n, o_new.b_n);
        assert_eq!((o.b_w, o.eye_lid, o.eye_wide, o.eye_squint), (o_old.b_w, o_old.eye_lid, o_old.eye_wide, o_old.eye_squint));
        // Pressed again: the new warm-up starts over, the old levels still in use
        e.relearn();
        assert_eq!(e.warmup_remaining_s(), WARMUP_S);
        assert_eq!(e.baseline_px(), Some((40.0, 80.0)));
        warm_next_for(&mut e.base, 29.5, 46.0, 86.0);
        assert!(e.relearning() && e.baseline_px() == Some((40.0, 80.0)));
        // Ready: the new levels take over, from the old ones over RELEARN_RAMP_S
        warm_next_for(&mut e.base, 0.5, 46.0, 86.0);
        assert!(!e.relearning() && e.baseline_ready() && e.warmup_remaining_s() == 0.0);
        assert_eq!((e.base.b, e.base.ap_n), (46.0, 86.0));
        assert_eq!(e.baseline_px(), Some((40.0, 80.0)), "no jump at the switch");
        for _ in 0..(0.5 * RELEARN_RAMP_S * FPS).round() as usize {
            e.base.tick(1.0 / FPS);
        }
        let (b, ap) = e.baseline_px().unwrap();
        assert!((b - 43.0).abs() < 1e-6 && (ap - 83.0).abs() < 1e-6, "halfway: {b} {ap}");
        // ...also through process(): its level moves on with the frames from the old engine's to the new one's, and
        // stays at the new one's after the ramp
        let mut last = 0.5;
        for k in 1..=(RELEARN_RAMP_S * FPS) as usize {
            let t = k as f64 / FPS;
            let o = e.process(&eye, f64::NAN, t, &p);
            let (o_old, o_new) = (old.process(&eye, f64::NAN, t, &p), new.process(&eye, f64::NAN, t, &p));
            let f = (o.b_n - o_old.b_n) / (o_new.b_n - o_old.b_n);
            assert!(f >= last - 1e-9 && f <= 1.0 + 1e-9, "{k}: {f}");
            last = f;
        }
        assert!((last - 1.0).abs() < 1e-9, "{last}");
        assert!(e.base.ramp.is_none() && e.baseline_px() == Some((46.0, 86.0)));
        let (o, o_new) = (e.process(&eye, f64::NAN, 3.0, &p), new.process(&eye, f64::NAN, 3.0, &p));
        assert_eq!((o.b_n, o.b_w, o.eye_lid, o.eye_wide), (o_new.b_n, o_new.b_w, o_new.eye_lid, o_new.eye_wide));
        // A relearn during a ramp starts from where the ramp is
        e.base.ramp = Some((40.0, 80.0, 0.5 * RELEARN_RAMP_S));
        e.relearn();
        assert_eq!(e.baseline_px(), Some((43.0, 83.0)));
        assert!(e.base.ramp.is_none() && e.relearning());
    }

    #[test]
    fn relearn_without_levels_and_a_new_wear_start_the_warm_up_over() {
        // No levels yet (just put on): the warm-up starts over, nothing in use meanwhile, as before
        let mut e = EyeEngine::default();
        for _ in 0..(10.0 * FPS) as usize {
            e.base.warm(46.0, 86.0, 0.5, 1.0 / FPS);
        }
        assert!(!e.baseline_ready() && (e.warmup_remaining_s() - 20.0).abs() < 0.02);
        e.relearn();
        assert!(!e.baseline_ready() && !e.relearning() && e.baseline_px().is_none());
        assert_eq!(e.warmup_remaining_s(), WARMUP_S);
        assert!(e.base.b.is_nan() && e.base.skin.is_empty() && e.base.seconds == 0.0);
        let p = Params::default();
        let o = e.process(&disc_frame(Some((250.0, 200.0, 25.0))), f64::NAN, 0.0, &p);
        assert!(o.b_n.is_nan() && o.eye_wide == 0.0 && o.eye_squint == 0.0);
        // A new wear (reset) during a relearn: everything goes, the warm-up from nothing, as before
        let w = WearParams { r_px: 50.0, b_n: 0.8, b_w: 1.0, ap_n: 1.6, ap_cl: 0.5, pitch_n: -12.0, lower_px: 240.0 };
        e.seed(&w);
        e.relearn();
        warm_next_for(&mut e.base, 10.0, 46.0, 86.0);
        e.reset();
        assert!(!e.baseline_ready() && !e.relearning() && e.baseline_px().is_none());
        assert_eq!(e.warmup_remaining_s(), WARMUP_S);
        assert!(e.base.ramp.is_none() && e.base.b.is_nan() && e.base.seconds == 0.0);
    }
}
