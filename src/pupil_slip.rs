//! The headset's slip taken out of the pupil gaze (`pupil_slip_correction`, off by default; a test). The pupil gaze
//! (pupil.rs) maps where each pupil is in the eye camera's picture, so a headset pushed or re-seated a few millimetres
//! moves it as if the eye had turned (1 px of camera picture is about 1°). eyecam-rec tracks the eye-tracking LEDs'
//! reflections (glints) near each pupil (tools/eyecam glint.rs): they move with the camera-to-eye shift as the pupil
//! does, but hardly with the eye turning, and it publishes their offset from a template taken at a still moment
//! (the live file's glint part, eyecam_live.rs). Here, per eye:
//!   - the offset is followed across templates (a new template starts from 0 again: the one before's last offset is
//!     carried, so the slip goes on where it was) and smoothed over SLIP_TAU_S, held while not tracked;
//!   - its change since the eye's map and centre were last set (the re-center; also this start) is the slip; a new
//!     template within REZERO_S after that (the panel asks for one after each fit and re-center) is the zero instead:
//!     carried over, the old template's offset at that moment holds how the glints sit at the eye's gaze then
//!     against the old template's gaze, not a slip;
//!   - it is converted into the eye server's pupil pair units by a linear fit of that pair against eyecam-rec's pupil
//!     centre in the same picture (both see the same pupil), learnt as the eyes move (FIT_TAU_S; used once it has seen
//!     the pupil move FIT_MIN_SD_PX each way and fits with at least FIT_MIN_R2), and taken off the pair before the map.
//! Without eyecam-rec's glint part (an older eyecam-rec, none, its values stale) for SLIP_LOST_S the eye's
//! correction eases out over EASE_S, and back in when it comes back (its slip going on from where it was: a new
//! template is carried like any other). A slip further than MAX_SLIP_PX (a wrong lock) gives no correction. Whether a
//! pair is fresh still goes by the eye server's own pairs (pupil.rs step_with); turned off, it all starts over. On the 2026-11-14 slip tests replayed this way
//! (eyecam-rec's live path, then this) the pupil gaze's median error per step went from 9.0° / 8.3° to 5.1° / 4.0°.

use crate::eyecam_live::{CamSlip, Live};
use crate::pupil::EyeMap;

/// The slip is smoothed over this long (s), as replayed.
pub const SLIP_TAU_S: f64 = 0.3;
/// Without eyecam-rec's slip for this long (s), the eye's correction eases out...
pub const SLIP_LOST_S: f64 = 2.0;
/// ...over this long (s), and back in as long when it is back.
pub const EASE_S: f64 = 1.0;
/// A slip further than this (px) is taken for a wrong lock, not a slip: no correction while it is (the pushes in the
/// 2026-11-14 slip tests reached about 55 px).
pub const MAX_SLIP_PX: f32 = 60.0;
/// The px-to-pair fit forgets over this long (s)...
pub const FIT_TAU_S: f64 = 120.0;
/// ...and is used once it has this many frames' weight...
pub const FIT_MIN_WEIGHT: f64 = 300.0;
/// ...the camera pupil has moved at least this much each way (its standard deviation, px; about 3°)...
pub const FIT_MIN_SD_PX: f64 = 3.0;
/// ...and it explains at least this share of the pair's spread (each coordinate).
pub const FIT_MIN_R2: f64 = 0.8;
/// A frame counts for the fit only while the camera pupil moved less than this since the frame before (px): the eye
/// server's pair is of a frame or two before, which matters only while the eye moves.
pub const FIT_STEADY_PX: f32 = 2.0;
/// The log line comes once per this long (s).
pub const LOG_S: f64 = 60.0;
/// A template with the same id is the same one while its time is within this of the one seen (ns): a restarted
/// eyecam-rec numbers from 1 again, seconds later; small changes are not a new template (eyecam-rec's 3f836aa
/// published a time that drifted by about 22 us every second, and each change carried the offset again: 2026-11-14
/// slip test 3).
pub const TEMPLATE_SAME_NS: u64 = 500_000_000;
/// A new template this soon (s) after a map or centre change starts the slip from 0 (see the module comment).
pub const REZERO_S: f64 = 30.0;

/// The eye server's pupil pair (`extra`) from eyecam-rec's pupil centre (px): pair = a + k * px, least squares with
/// exponential forgetting (weights: each frame 1, decaying over FIT_TAU_S).
#[derive(Clone, Debug, Default)]
struct PairFit {
    /// Sums of w, w x, w x x' (x = 1, px, py), w x y (y = the pair's two values), w y y.
    w: f64,
    sx: [f64; 2],
    sxx: [[f64; 2]; 2],
    sy: [f64; 2],
    sxy: [[f64; 2]; 2],
    syy: [f64; 2],
    last_time: Option<f64>,
}

impl PairFit {
    fn add(&mut self, time: f64, px: [f32; 2], pair: [f32; 2]) {
        let decay = self.last_time.map_or(1.0, |last| (-(time - last).max(0.0) / FIT_TAU_S).exp());
        self.last_time = Some(time);
        let x = px.map(f64::from);
        let y = pair.map(f64::from);
        self.w = self.w * decay + 1.0;
        for i in 0..2 {
            self.sx[i] = self.sx[i] * decay + x[i];
            self.sy[i] = self.sy[i] * decay + y[i];
            self.syy[i] = self.syy[i] * decay + y[i] * y[i];
            for j in 0..2 {
                self.sxx[i][j] = self.sxx[i][j] * decay + x[i] * x[j];
                self.sxy[i][j] = self.sxy[i][j] * decay + x[i] * y[j];
            }
        }
    }

    /// k (k[pair coordinate][px axis]): the pair's change per px of the camera pupil, once the fit can be used.
    fn slope(&self) -> Option<[[f64; 2]; 2]> {
        if self.w < FIT_MIN_WEIGHT {
            return None;
        }
        let mean_x = self.sx.map(|s| s / self.w);
        let mean_y = self.sy.map(|s| s / self.w);
        // Covariances about the means
        let cxx = [0, 1].map(|i| [0, 1].map(|j| self.sxx[i][j] / self.w - mean_x[i] * mean_x[j]));
        let cxy = [0, 1].map(|i| [0, 1].map(|j| self.sxy[i][j] / self.w - mean_x[i] * mean_y[j]));
        if cxx[0][0] < FIT_MIN_SD_PX * FIT_MIN_SD_PX || cxx[1][1] < FIT_MIN_SD_PX * FIT_MIN_SD_PX {
            return None;
        }
        let det = cxx[0][0] * cxx[1][1] - cxx[0][1] * cxx[1][0];
        if det.abs() < 1e-9 {
            return None;
        }
        let inv = [[cxx[1][1] / det, -cxx[0][1] / det], [-cxx[1][0] / det, cxx[0][0] / det]];
        let mut k = [[0.0; 2]; 2];
        for (out, row) in k.iter_mut().enumerate() {
            for (axis, value) in row.iter_mut().enumerate() {
                *value = inv[axis][0] * cxy[0][out] + inv[axis][1] * cxy[1][out];
            }
            // The share of the pair's spread the fit explains
            let var_y = self.syy[out] / self.w - mean_y[out] * mean_y[out];
            let explained = row[0] * cxy[0][out] + row[1] * cxy[1][out];
            if var_y <= 0.0 || explained / var_y < FIT_MIN_R2 {
                return None;
            }
        }
        Some(k)
    }
}

/// One eye's slip.
#[derive(Clone, Debug, Default)]
struct EyeSlip {
    fit: PairFit,
    /// The last usable fit (kept when the fit can't be used for a while: the eyes still).
    k: Option<[[f64; 2]; 2]>,
    /// The camera frame last taken (its capture time, ns), its pupil and the pair with it.
    frame_ns: Option<u64>,
    pupil: Option<[f32; 2]>,
    pair: Option<[f32; 2]>,
    /// The template followed (its id and when it was taken: a restarted eyecam-rec counts from 1 again), the offsets of
    /// the ones before it carried, and its own last offset (px).
    template: Option<(u32, u64)>,
    carried: [f32; 2],
    offset: [f32; 2],
    /// The slip smoothed (px), and when the last camera frame was (the eye server's time).
    smooth: Option<[f32; 2]>,
    last_frame: Option<f64>,
    /// The smoothed slip when the map and centre were last set (or at the start).
    reference: Option<[f32; 2]>,
    /// For the log: frames taken, frames tracked.
    frames: u64,
    tracked: u64,
    /// How much of the correction is applied (0..1): eased out while eyecam-rec's slip is lost, back in once it is back.
    weight: f32,
    /// The sample time last stepped (for the easing).
    last_step: Option<f64>,
    /// Until when a new template is the slip's zero (REZERO_S after a map or centre change), and that one came.
    rezero_until: Option<f64>,
    rezero_now: bool,
}

impl EyeSlip {
    /// A sample: the eye server's time and pupil pair, eyecam-rec's slip for this eye if fresh.
    fn step(&mut self, time: f64, pair: [f32; 2], cam: Option<&CamSlip>) {
        // The correction eased out while eyecam-rec's slip is lost, back in while it is there
        let dt = self.last_step.map_or(0.0, |last| (time - last).max(0.0));
        self.last_step = Some(time);
        let lost = self.last_frame.is_none_or(|last| time - last > SLIP_LOST_S);
        let by = (dt / EASE_S) as f32;
        self.weight = if lost && cam.is_none() { (self.weight - by).max(0.0) } else { (self.weight + by).min(1.0) };
        let Some(cam) = cam else {
            return;
        };
        if self.frame_ns == Some(cam.time_ns) {
            return;
        }
        self.frame_ns = Some(cam.time_ns);
        self.frames += 1;
        // The fit: a frame with a pupil, still since the last, and a pair that is there
        let pair_ok = pair.iter().all(|v| v.is_finite()) && pair != [0.0; 2];
        if let (Some(p), Some(last), true) = (cam.pupil, self.pupil, pair_ok) {
            let moved = (p[0] - last[0]).hypot(p[1] - last[1]);
            if moved < FIT_STEADY_PX && self.pair != Some(pair) {
                self.fit.add(time, p, pair);
                if let Some(k) = self.fit.slope() {
                    self.k = Some(k);
                }
            }
        }
        self.pupil = cam.pupil;
        self.pair = pair_ok.then_some(pair);
        // A new template (also one from a restarted eyecam-rec, after a loss): the one before's last offset is carried
        if let Some(template) = cam.template {
            let same = self
                .template
                .is_some_and(|(id, at)| id == template.0 && at.abs_diff(template.1) <= TEMPLATE_SAME_NS);
            if !same {
                if self.template.is_some() {
                    self.carried = [self.carried[0] + self.offset[0], self.carried[1] + self.offset[1]];
                    // (just after a map or centre change: this template is the zero)
                    if self.rezero_until.take().is_some_and(|until| time <= until) {
                        self.rezero_now = true;
                    }
                }
                self.template = Some(template);
                self.offset = [0.0; 2];
            }
            if cam.tracked && cam.offset.iter().all(|v| v.is_finite()) {
                self.tracked += 1;
                self.offset = cam.offset;
                let total = [self.carried[0] + self.offset[0], self.carried[1] + self.offset[1]];
                let smooth = match (self.smooth, self.last_frame) {
                    (Some(s), Some(last)) => {
                        let a = (1.0 - (-(time - last).max(0.0) / SLIP_TAU_S).exp()) as f32;
                        [s[0] + a * (total[0] - s[0]), s[1] + a * (total[1] - s[1])]
                    }
                    _ => total,
                };
                // (never a NaN kept)
                if smooth.iter().all(|v| v.is_finite()) {
                    self.smooth = Some(smooth);
                    if std::mem::take(&mut self.rezero_now) {
                        self.reference = Some(smooth);
                    }
                }
            }
        }
        self.last_frame = Some(time);
        if self.reference.is_none() {
            self.reference = self.smooth;
        }
    }

    /// The slip since the reference (px), if there is one.
    fn slip(&self) -> Option<[f32; 2]> {
        let (s, r) = (self.smooth?, self.reference?);
        Some([s[0] - r[0], s[1] - r[1]])
    }

    /// The pair with the slip taken off, as much of it as `weight` says (unchanged without a slip or a fit, or with a
    /// slip beyond MAX_SLIP_PX).
    fn corrected(&self, pair: [f32; 2]) -> [f32; 2] {
        match (self.slip(), self.k) {
            (Some(s), Some(k)) if s[0].hypot(s[1]) <= MAX_SLIP_PX => [0, 1].map(|out| {
                pair[out] - self.weight * (k[out][0] * f64::from(s[0]) + k[out][1] * f64::from(s[1])) as f32
            }),
            _ => pair,
        }
    }
}

/// Both eyes' slip, and the maps it was last referred to.
#[derive(Clone, Debug, Default)]
pub struct SlipCorrection {
    eyes: [EyeSlip; 2],
    maps: Option<[EyeMap; 2]>,
    next_log: Option<f64>,
}

impl SlipCorrection {
    /// Start over (pupil_slip_correction turned off), if it has been used.
    pub fn reset_if_used(&mut self) {
        if self.maps.is_some() {
            *self = Self::default();
        }
    }

    /// One sample: the eye server's estimate_extra with each eye's pupil pair corrected for the slip (0/1 left, 2/3
    /// right; the rest as it is). A new map or centre (a re-center, the eye fit) makes the slip now its zero.
    pub fn correct(&mut self, time: f64, maps: &[EyeMap; 2], extra: &[f32; 8], camera: Option<&Live>) -> [f32; 8] {
        if self.maps.is_some_and(|old| old != *maps) {
            for eye in &mut self.eyes {
                eye.reference = eye.smooth;
                eye.rezero_until = Some(time + REZERO_S);
            }
        }
        self.maps = Some(*maps);
        let mut out = *extra;
        for (i, eye) in self.eyes.iter_mut().enumerate() {
            let pair = [extra[2 * i], extra[2 * i + 1]];
            let cam = camera.and_then(|live| live.slip[i].as_ref());
            eye.step(time, pair, cam);
            let fixed = eye.corrected(pair);
            out[2 * i] = fixed[0];
            out[2 * i + 1] = fixed[1];
        }
        out
    }

    /// The journal line, once per LOG_S: per eye the share of camera frames tracked, the slip now (px) and whether the
    /// conversion is learnt.
    pub fn log_line(&mut self, time: f64) -> Option<String> {
        let due = *self.next_log.get_or_insert(time + LOG_S);
        if time < due {
            return None;
        }
        self.next_log = Some(time + LOG_S);
        let parts: Vec<String> = ["left", "right"]
            .iter()
            .zip(&mut self.eyes)
            .map(|(name, eye)| {
                let share = if eye.frames > 0 { 100.0 * eye.tracked as f64 / eye.frames as f64 } else { 0.0 };
                let slip = eye.slip().map_or("none".to_owned(), |s| format!("({:+.1}, {:+.1}) px", s[0], s[1]));
                let fit = if eye.k.is_some() { "learnt" } else { "not learnt yet" };
                let line = format!(
                    "{name} tracked {share:.0}% of {} frames, slip {slip}, conversion {fit}",
                    eye.frames
                );
                eye.frames = 0;
                eye.tracked = 0;
                line
            })
            .collect();
        Some(format!("Pupil slip correction: {}", parts.join("; ")))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn maps(center: f32) -> [EyeMap; 2] {
        [EyeMap { yaw: [0.0, 100.0, 0.0], pitch: [0.0, 0.0, 100.0], center: [center, 0.0] }; 2]
    }

    /// A camera frame for one eye: its time, pupil centre, slip offset (tracked or held) and template.
    fn cam(time_ns: u64, pupil: [f32; 2], offset: [f32; 2], tracked: bool, template: u32) -> CamSlip {
        CamSlip {
            time_ns,
            pupil: Some(pupil),
            tracked,
            template: Some((template, u64::from(template))),
            offset,
            matches: if tracked { 4 } else { 0 },
            since_good: 0,
            glints: 4,
        }
    }

    /// The eye server's pair for a camera pupil (px): 0.01 per px, x and y swapped and one flipped, as a real camera's
    /// axes may be.
    fn pair_of(px: [f32; 2]) -> [f32; 2] {
        [0.3 - 0.01 * px[1], 0.1 + 0.01 * px[0]]
    }

    #[test]
    fn the_fit_learns_the_pair_per_px_once_the_eye_has_moved() {
        let mut fit = PairFit::default();
        // Still: never usable
        for i in 0..400 {
            fit.add(i as f64 / 90.0, [200.0, 150.0], pair_of([200.0, 150.0]));
        }
        assert!(fit.slope().is_none());
        // Looking around (a circle of 8 px): k = [[0, -0.01], [0.01, 0]]
        for i in 0..400 {
            let a = i as f32 * 0.05;
            let px = [200.0 + 8.0 * a.cos(), 150.0 + 8.0 * a.sin()];
            fit.add((400 + i) as f64 / 90.0, px, pair_of(px));
        }
        let k = fit.slope().expect("usable");
        assert!(k[0][0].abs() < 1e-6 && (k[0][1] + 0.01).abs() < 1e-6, "{k:?}");
        assert!((k[1][0] - 0.01).abs() < 1e-6 && k[1][1].abs() < 1e-6, "{k:?}");
        // A pair that has nothing to do with the pupil doesn't fit
        let mut noise = PairFit::default();
        for i in 0..800 {
            let a = i as f32 * 0.05;
            let px = [200.0 + 8.0 * a.cos(), 150.0 + 8.0 * a.sin()];
            noise.add(i as f64 / 90.0, px, [((i * 7919) % 101) as f32 * 0.001, ((i * 104729) % 97) as f32 * 0.001]);
        }
        assert!(noise.slope().is_none());
    }

    /// Runs `frames` camera frames at 90 a second through a correction, the eye looking around a little (so the fit
    /// learns) unless `still`, the headset slipped by `slip(i)` px (the camera pupil and the glints alike); returns
    /// the corrected pairs.
    fn run(
        corr: &mut SlipCorrection,
        start: usize,
        frames: usize,
        still: bool,
        slip: impl Fn(usize) -> [f32; 2],
        tracked: impl Fn(usize) -> bool,
        template: impl Fn(usize) -> u32,
        template_start: impl Fn(usize) -> [f32; 2],
    ) -> Vec<[f32; 8]> {
        let maps = maps(0.0);
        let mut out = Vec::new();
        for i in start..start + frames {
            let a = i as f32 * 0.05;
            let look = if still { [0.0; 2] } else { [8.0 * a.cos(), 8.0 * a.sin()] };
            let s = slip(i);
            let px = [200.0 + look[0] + s[0], 150.0 + look[1] + s[1]];
            let pair = pair_of(px);
            let t0 = template_start(i);
            let c = cam(i as u64 * 11_111_111, px, [s[0] - t0[0], s[1] - t0[1]], tracked(i), template(i));
            let live = Live { slip: [Some(c), Some(c)], ..crate::tests::camera(0.75, 0.0) };
            let extra = [pair[0], pair[1], pair[0], pair[1], 0.0, 0.0, 0.0, 0.0];
            out.push(corr.correct(i as f64 / 90.0, &maps, &extra, Some(&live)));
        }
        out
    }

    #[test]
    fn a_slip_is_taken_out_of_the_pair_once_learnt() {
        let mut corr = SlipCorrection::default();
        // Learning while looking around, no slip: nothing changes
        let out = run(&mut corr, 0, 900, false, |_| [0.0; 2], |_| true, |_| 1, |_| [0.0; 2]);
        assert!(corr.eyes[0].k.is_some());
        let last = out.last().unwrap();
        let px = [200.0 + 8.0 * (899.0f32 * 0.05).cos(), 150.0 + 8.0 * (899.0f32 * 0.05).sin()];
        assert!((last[0] - pair_of(px)[0]).abs() < 1e-5 && (last[1] - pair_of(px)[1]).abs() < 1e-5);
        // The headset slips 10 px down at once, the eye still: within a second the pair is back where it was
        let out = run(&mut corr, 900, 180, true, |_| [0.0, 10.0], |_| true, |_| 1, |_| [0.0; 2]);
        let before = pair_of([200.0, 150.0]);
        let end = out.last().unwrap();
        assert!((end[0] - before[0]).abs() < 1e-3 && (end[1] - before[1]).abs() < 1e-3, "{end:?} vs {before:?}");
        // ...smoothed: right after the slip, most of it still there (tau 0.3 s)
        let first = out[0];
        assert!((first[0] - before[0]).abs() > 0.05, "{first:?}");
        // Not tracked for a while (a blink, a squint): held, nothing jumps
        let held = run(&mut corr, 1080, 60, true, |_| [0.0, 10.0], |_| false, |_| 1, |_| [0.0; 2]);
        assert!(held.iter().all(|p| (p[0] - end[0]).abs() < 1e-4 && (p[1] - end[1]).abs() < 1e-4));
        // A new template while slipped: carried on (no jump)
        let next = run(&mut corr, 1140, 90, true, |_| [0.0, 10.0], |_| true, |_| 2, |_| [0.0, 10.0]);
        // (to within the smoothing's last thousandth of the slip)
        assert!(next.iter().all(|p| (p[0] - end[0]).abs() < 1e-3 && (p[1] - end[1]).abs() < 1e-3));
        // A re-center (a new centre): the slip now is the new zero; the pair goes through as it is
        let maps2 = maps(1.0);
        let px = [200.0, 160.0];
        let c = cam(5_000_000_000_000, px, [0.0, 0.0], true, 2);
        let live = Live { slip: [Some(c), Some(c)], ..crate::tests::camera(0.75, 0.0) };
        let p = pair_of(px);
        let out = corr.correct(1240.0 / 90.0, &maps2, &[p[0], p[1], p[0], p[1], 0.0, 0.0, 0.0, 0.0], Some(&live));
        assert!((out[0] - p[0]).abs() < 1e-5 && (out[1] - p[1]).abs() < 1e-5, "{out:?}");
    }

    #[test]
    fn without_eyecam_or_a_fit_nothing_changes() {
        let maps = maps(0.0);
        let extra = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8];
        let mut corr = SlipCorrection::default();
        assert_eq!(corr.correct(0.0, &maps, &extra, None), extra);
        // A slip but no fit yet (the eye still): unchanged
        let mut corr = SlipCorrection::default();
        let out = run(&mut corr, 0, 300, true, |i| [0.0, if i > 100 { 10.0 } else { 0.0 }], |_| true, |_| 1, |_| [0.0; 2]);
        let p = pair_of([200.0, 160.0]);
        assert!((out.last().unwrap()[0] - p[0]).abs() < 1e-6);
        // Learnt, then eyecam-rec gone for longer than SLIP_LOST_S: the correction eases out over EASE_S (the fit and
        // the reference kept), and back in once it is back, a new template carried on from where it was
        let mut corr = SlipCorrection::default();
        run(&mut corr, 0, 900, false, |_| [0.0; 2], |_| true, |_| 1, |_| [0.0; 2]);
        run(&mut corr, 900, 90, true, |_| [0.0, 10.0], |_| true, |_| 1, |_| [0.0; 2]);
        let p = pair_of([200.0, 160.0]);
        let full = pair_of([200.0, 150.0]);
        let input = [p[0], p[1], p[0], p[1], 0.0, 0.0, 0.0, 0.0];
        let t0 = 989.0 / 90.0;
        let held = corr.correct(t0 + 1.0, &maps, &input, None);
        assert!((held[0] - full[0]).abs() < 0.01, "held through a short gap: {held:?}");
        let mut out = Vec::new();
        let mut t = t0 + 1.0;
        while t < t0 + SLIP_LOST_S + EASE_S + 0.5 {
            t += 1.0 / 90.0;
            out.push(corr.correct(t, &maps, &input, None)[0]);
        }
        // (no step bigger than the easing's)
        assert!(out.windows(2).all(|w| (w[1] - w[0]).abs() < 0.1 / 90.0 * 1.5), "eased");
        assert!((out.last().unwrap() - p[0]).abs() < 1e-6, "out: {:?}", out.last());
        assert!(corr.eyes[0].k.is_some() && corr.eyes[0].reference.is_some(), "the fit and the reference kept");
        // Back with a restarted eyecam-rec (template 1 again, taken seconds later), where it was: eased back in
        let mut back = Vec::new();
        for i in 0..200 {
            let c = CamSlip { template: Some((1, 5_000_000_000)), ..cam(9_000_000_000_000 + i * 11_111_111, [200.0, 160.0], [0.0; 2], true, 1) };
            let live = Live { slip: [Some(c), Some(c)], ..crate::tests::camera(0.75, 0.0) };
            back.push(corr.correct(t + (i + 1) as f64 / 90.0, &maps, &input, Some(&live))[0]);
        }
        assert!(back.windows(2).all(|w| (w[1] - w[0]).abs() < 0.1 / 90.0 * 1.5), "eased back");
        assert!((back.last().unwrap() - full[0]).abs() < 1e-3, "back: {:?}", back.last());
    }

    #[test]
    fn a_slip_beyond_the_limit_or_not_a_number_gives_no_correction() {
        let maps = maps(0.0);
        let mut corr = SlipCorrection::default();
        run(&mut corr, 0, 900, false, |_| [0.0; 2], |_| true, |_| 1, |_| [0.0; 2]);
        // A wrong lock 70 px away: nothing taken off
        let out = run(&mut corr, 900, 180, true, |_| [0.0, 70.0], |_| true, |_| 1, |_| [0.0; 2]);
        let p = pair_of([200.0, 220.0]);
        assert!((out.last().unwrap()[0] - p[0]).abs() < 1e-6);
        // An offset that isn't a number: not taken, the smoothing stays a number
        let mut corr = SlipCorrection::default();
        run(&mut corr, 0, 900, false, |_| [0.0; 2], |_| true, |_| 1, |_| [0.0; 2]);
        let c = CamSlip { offset: [f32::NAN, 1.0], ..cam(99_000_000_000, [200.0, 150.0], [0.0; 2], true, 1) };
        let live = Live { slip: [Some(c), Some(c)], ..crate::tests::camera(0.75, 0.0) };
        let q = pair_of([200.0, 150.0]);
        let out = corr.correct(11.0, &maps, &[q[0], q[1], q[0], q[1], 0.0, 0.0, 0.0, 0.0], Some(&live));
        assert!(out.iter().all(|v| v.is_finite()) && corr.eyes[0].smooth.unwrap().iter().all(|v| v.is_finite()));
        // Turned off: starts over
        corr.reset_if_used();
        assert!(corr.eyes[0].k.is_none() && corr.maps.is_none());
    }

    #[test]
    fn a_template_time_drifting_a_little_is_the_same_template() {
        // eyecam-rec 3f836aa: the template's time drifted by 22 us every second; each change was taken for a new
        // template and the offset carried again, the slip growing by the offset every second (hundreds of px a
        // minute). The same template, the headset 10 px down and still, for 3 minutes: the slip stays 10 px
        let mut corr = SlipCorrection::default();
        run(&mut corr, 0, 900, false, |_| [0.0; 2], |_| true, |_| 1, |_| [0.0; 2]);
        let maps = maps(0.0);
        let mut out = [0.0f32; 8];
        for i in 900..900 + 90 * 180 {
            let drift = (i / 90) as u64 * 22_000;
            let c = CamSlip { template: Some((1, 1 + drift)), ..cam(i as u64 * 11_111_111, [200.0, 160.0], [0.0, 10.0], true, 1) };
            let live = Live { slip: [Some(c), Some(c)], ..crate::tests::camera(0.75, 0.0) };
            let p = pair_of([200.0, 160.0]);
            out = corr.correct(i as f64 / 90.0, &maps, &[p[0], p[1], p[0], p[1], 0.0, 0.0, 0.0, 0.0], Some(&live));
        }
        let slip = corr.eyes[0].slip().unwrap();
        assert!(slip[0].abs() < 1e-3 && (slip[1] - 10.0).abs() < 1e-3, "{slip:?}");
        let full = pair_of([200.0, 150.0]);
        assert!((out[0] - full[0]).abs() < 1e-3, "{out:?}");
        // A restarted eyecam-rec (template 1 again, its time seconds away, offset from 0): carried on, still 10 px
        let mut i = 900 + 90 * 180;
        for _ in 0..90 {
            let c = CamSlip { template: Some((1, 9_000_000_000_000)), ..cam(i as u64 * 11_111_111, [200.0, 160.0], [0.0, 0.0], true, 1) };
            let live = Live { slip: [Some(c), Some(c)], ..crate::tests::camera(0.75, 0.0) };
            let p = pair_of([200.0, 160.0]);
            corr.correct(i as f64 / 90.0, &maps, &[p[0], p[1], p[0], p[1], 0.0, 0.0, 0.0, 0.0], Some(&live));
            i += 1;
        }
        let slip = corr.eyes[0].slip().unwrap();
        assert!((slip[1] - 10.0).abs() < 1e-3, "{slip:?}");
    }

    #[test]
    fn a_new_template_right_after_a_recenter_is_the_zero() {
        // A re-center, then (as the panel asks) a new template 10 s later, taken while the old template's offset reads
        // 12 px (the glints at this gaze against the old template's gaze, no slip): the slip stays 0, nothing taken off
        let mut corr = SlipCorrection::default();
        run(&mut corr, 0, 900, false, |_| [0.0; 2], |_| true, |_| 1, |_| [0.0; 2]);
        let maps2 = maps(1.0);
        let mut last = [0.0f32; 8];
        for i in 900..900 + 90 * 20 {
            let (template, offset) = if i < 900 + 900 { (1, [12.0, 0.0]) } else { (2, [0.0, 0.0]) };
            let c = cam(i as u64 * 11_111_111, [200.0, 150.0], offset, true, template);
            let live = Live { slip: [Some(c), Some(c)], ..crate::tests::camera(0.75, 0.0) };
            let p = pair_of([200.0, 150.0]);
            last = corr.correct(i as f64 / 90.0, &maps2, &[p[0], p[1], p[0], p[1], 0.0, 0.0, 0.0, 0.0], Some(&live));
            if i == 900 {
                // (the re-center: the 12 px reading so far is the zero)
                assert!(corr.eyes[0].slip().unwrap().iter().all(|v| v.abs() < 12.0));
            }
        }
        let slip = corr.eyes[0].slip().unwrap();
        assert!(slip.iter().all(|v| v.abs() < 1e-3), "{slip:?}");
        let p = pair_of([200.0, 150.0]);
        assert!((last[0] - p[0]).abs() < 1e-5 && (last[1] - p[1]).abs() < 1e-5, "{last:?}");
        // Later than REZERO_S after the re-center, a new template carries on as before (a new wear, a restarted
        // eyecam-rec)
        for i in 900 + 90 * 20..900 + 90 * 60 {
            let c = cam(i as u64 * 11_111_111, [200.0, 155.0], [0.0, 5.0], true, 2);
            let live = Live { slip: [Some(c), Some(c)], ..crate::tests::camera(0.75, 0.0) };
            let p = pair_of([200.0, 155.0]);
            corr.correct(i as f64 / 90.0, &maps2, &[p[0], p[1], p[0], p[1], 0.0, 0.0, 0.0, 0.0], Some(&live));
        }
        for i in 900 + 90 * 60..900 + 90 * 62 {
            let c = cam(i as u64 * 11_111_111, [200.0, 155.0], [0.0, 0.0], true, 3);
            let live = Live { slip: [Some(c), Some(c)], ..crate::tests::camera(0.75, 0.0) };
            let p = pair_of([200.0, 155.0]);
            corr.correct(i as f64 / 90.0, &maps2, &[p[0], p[1], p[0], p[1], 0.0, 0.0, 0.0, 0.0], Some(&live));
        }
        let slip = corr.eyes[0].slip().unwrap();
        assert!((slip[1] - 5.0).abs() < 1e-3, "{slip:?}");
    }

    #[test]
    fn the_log_line_comes_once_a_minute() {
        let mut corr = SlipCorrection::default();
        assert!(corr.log_line(0.0).is_none());
        run(&mut corr, 0, 900, false, |_| [0.0; 2], |i| i % 4 != 0, |_| 1, |_| [0.0; 2]);
        assert!(corr.log_line(30.0).is_none());
        let line = corr.log_line(60.0).unwrap();
        assert!(line.starts_with("Pupil slip correction: left tracked 75% of 900 frames, slip (+0.0, +0.0) px, conversion learnt"), "{line}");
        assert!(corr.log_line(61.0).is_none());
    }
}
