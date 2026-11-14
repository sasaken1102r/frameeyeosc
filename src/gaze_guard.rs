//! Valve's gaze with a guard from the pupils (`gaze_guard`, off by default; a test). The rule: there is nothing Valve's
//! eye tracker can do that frameeyeosc can't, so its gaze is the floor. It goes out as gaze_source "valve" sends it (no
//! pupil map needed for that, Valve's own self-calibration, each eye's own gaze), and the pupil gaze (each eye's pupil
//! pair mapped as pupil.rs maps it, with the headset's slip taken out as pupil_slip.rs does) runs underneath as a
//! witness.
//!
//! SteamOS's self-calibration (Usercal, 0.5.8 on) re-solves a rotation and gain now and then, which can move Valve's
//! gaze several degrees at once and keep it there (in run 1 of the developer's 2026-11-14 slip tests Valve stayed 16°
//! to the side); the pupil pairs don't move with it. So per eye, d = Valve's combined gaze - that eye's pupil gaze is
//! followed:
//!   - only while the witness can be trusted: eyecam-rec follows both eyes' glints and they show no step (SLIP_QUIET_*:
//!     the headset didn't just move), both pupils are fresh, open and in range and move together (AGREE_DEG), the
//!     pupils and Valve's gaze are steady (STEADY_*), and Valve's gaze is not held (eyes shut, an eye unusable, looking
//!     far down where its sideways gaze is held);
//!   - fast (FAST_TAU_S, on every steady sample with both pupils) and slow (the baseline, BASE_TAU_S, learnt only from
//!     trusted samples near it);
//!   - when both eyes' fast d stand on average more than STEP_DEG off the baseline for STEP_HOLD_S of trusted samples:
//!     with the glints where they were while the baseline was learnt (SLIP_MOVED_PX) and the eyes looking where they
//!     looked while it was learnt (GAZE_SAME_DEG: the pupil map's own error changes with the gaze, alike in both eyes,
//!     so a look to the side would read as a step), Valve moved by itself, and the step goes into an offset added to
//!     Valve's gaze, eased in at EASE_DEG_S (never a jump); with the glints moved (wherever the eyes look), the headset
//!     moved, and Valve is trusted (it follows a slip far better than the pupils do, and it usually solves its
//!     self-calibration again after a re-seat): the offset is let go (eased out at EASE_DEG_S) and the baseline takes the
//!     new difference.
//! The offset is at most MAX_OFFSET_DEG, eases out at EASE_OUT_DEG_S once the witness has been lost for WITNESS_LOST_S,
//! and is zero again at every eye fit and re-center (a new pupil map or centre, or a new correction of Valve's gaze:
//! the fit measures Valve's own gaze, so it takes the step in itself), when the eye tracking stops (the headset taken
//! off; main.rs on_lost) and when the key is turned off.
//!
//! d is taken from Valve's combined gaze, not from each eye's own reading: the Frame's per-eye sideways gaze swings by
//! several degrees on its own (main.rs VERGENCE_DEG), which would look like steps, and the combined gaze is what goes
//! out unless independent_eyes says otherwise. Each eye's offset is added to its own gaze and their mean to the
//! combined gaze. All angles here are degrees (yaw right +, pitch up +).

use crate::pupil::{self, EyeMap, Fresh};

/// d is smoothed over this long (s) for the step test: pupil noise averages out, a step shows within a few tenths.
pub const FAST_TAU_S: f64 = 0.3;
/// The baseline of d is learnt over this long (s): slow enough that a step is not learnt away while it is tested,
/// fast enough to follow the pupil map's slow drift over a wear.
pub const BASE_TAU_S: f64 = 20.0;
/// A step is looked for only once the baseline has this many seconds of trusted samples (after a start, a fit or a
/// re-center); until then the baseline is the plain mean.
pub const BASE_MIN_S: f64 = 3.0;
/// d standing further than this from its baseline (degrees, the mean of both eyes) is a step. Valve's ratchets seen
/// so far were several degrees to 16°; the pupil gaze's own error over a minute is 1-2°.
pub const STEP_DEG: f32 = 3.0;
/// ...and it has to stay for this many seconds of trusted samples before anything is done (blinks and short losses in
/// between pause it: about 2 s of wall time).
pub const STEP_HOLD_S: f64 = 1.5;
/// A step is taken for Valve's only while the pupils' combined gaze is at most this far (degrees) from where it was,
/// on average, while the baseline was learnt: the corrected pupil gaze errs differently at different gazes (in the
/// 2026-11-14 slip tests up to about 12° at the side dots, 20° out), alike in both eyes, so d away from there differs
/// without Valve moving. Within it the map's error changes by less than STEP_DEG. Away from it such a step is neither
/// tested nor learnt; one that is Valve's is still there when the eyes come back. (A step with the glints moved is the
/// headset's wherever the eyes look.)
pub const GAZE_SAME_DEG: f32 = 5.0;
/// A step being tested is dropped when no sample has been trusted for this long (s): a blink pauses it, a longer
/// loss starts it over.
pub const CANDIDATE_GAP_S: f64 = 0.5;
/// Both eyes' fast d must differ by at most this much more than their baselines do (degrees): Valve's step moves
/// both alike, a pupil lost or relocked in one eye moves only that eye's.
pub const AGREE_DEG: f32 = 2.0;
/// Steady: the gaze smoothed over STEADY_FAST_TAU_S and over STEADY_SLOW_TAU_S (s) at most STEADY_DEG apart
/// (degrees), for Valve's gaze and both pupils. During a saccade they differ by many degrees, and Valve's gaze and the
/// pupil pair are not of exactly the same moment.
pub const STEADY_FAST_TAU_S: f64 = 0.03;
pub const STEADY_SLOW_TAU_S: f64 = 0.15;
pub const STEADY_DEG: f32 = 1.5;
/// The glints show no step while each eye's slip is within SLIP_QUIET_PX (px; 1 px is about 1° of pupil gaze) of
/// itself smoothed over SLIP_QUIET_TAU_S (s): a push or a re-seat moves them tens of px within a second (up to about
/// 55 px in the 2026-11-14 slip tests), while the glint offset eyecam-rec follows also moves a few px as the eyes look
/// around, which must not count.
pub const SLIP_QUIET_TAU_S: f64 = 2.0;
pub const SLIP_QUIET_PX: f32 = 3.0;
/// The headset moved when a step is due: an eye's slip more than this (px) from where it was while the baseline was
/// learnt. Then Valve is trusted and nothing is absorbed. A slip this small that the pupil slip correction leaves
/// partly in the pupils stays under STEP_DEG.
pub const SLIP_MOVED_PX: f32 = 5.0;
/// The offset eases to a new value at most this fast (degrees per second): 16° in 0.8 s, 0.22° a sample at 90 Hz.
pub const EASE_DEG_S: f32 = 20.0;
/// ...counting at most this long (s) of a sample's interval, so a gap in the samples does not make a jump.
pub const MAX_DT_S: f64 = 0.05;
/// The offset is never further than this from Valve's own gaze (degrees, each eye).
pub const MAX_OFFSET_DEG: f32 = 20.0;
/// No trusted sample for this long (s): what the offset was learnt for can't be checked any more, so it eases out...
pub const WITNESS_LOST_S: f64 = 30.0;
/// ...at this rate (degrees per second): slow enough not to be seen as a move.
pub const EASE_OUT_DEG_S: f32 = 1.0;
/// The journal line comes once per this long (s).
pub const LOG_S: f64 = 60.0;

fn add(a: [f32; 2], b: [f32; 2]) -> [f32; 2] {
    [a[0] + b[0], a[1] + b[1]]
}

fn sub(a: [f32; 2], b: [f32; 2]) -> [f32; 2] {
    [a[0] - b[0], a[1] - b[1]]
}

fn length(a: [f32; 2]) -> f32 {
    a[0].hypot(a[1])
}

/// Move a smoothed value the share `by` of the way to `to` (the first one taken as it is).
fn ease(value: &mut Option<[f32; 2]>, to: [f32; 2], by: f32) {
    *value = Some(match *value {
        Some(old) => add(old, [(to[0] - old[0]) * by, (to[1] - old[1]) * by]),
        None => to,
    });
}

/// The share of the way an exponential smoothing over `tau` moves in `dt`.
fn share(dt: f64, tau: f64) -> f32 {
    (1.0 - (-dt / tau).exp()) as f32
}

/// `from` moved toward `to` by at most `step` (degrees).
fn toward(from: [f32; 2], to: [f32; 2], step: f32) -> [f32; 2] {
    let gap = sub(to, from);
    let size = length(gap);
    if size <= step || size == 0.0 { to } else { add(from, [gap[0] * step / size, gap[1] * step / size]) }
}

/// `value` with its length at most `limit`.
fn bounded(value: [f32; 2], limit: f32) -> [f32; 2] {
    let size = length(value);
    if size <= limit { value } else { [value[0] * limit / size, value[1] * limit / size] }
}

/// What the eye fit and re-center set, for both sources: the pupil maps with their centres, and Valve's correction
/// (gaze_offset_x / y, gains, roll, each eye's sideways zero point and gain). Any change zeroes the offset.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Fit {
    pub maps: [EyeMap; 2],
    pub valve: [Option<f32>; 10],
}

/// Whether a gaze has stood still lately (see STEADY_*): it smoothed fast and slow.
#[derive(Clone, Copy, Debug, Default)]
struct Steady {
    fast: Option<[f32; 2]>,
    slow: Option<[f32; 2]>,
    since: Option<f64>,
}

impl Steady {
    /// Take the gaze at `time` (None: not there; that starts it over); whether it is steady.
    fn step(&mut self, time: f64, dt: f64, gaze: Option<[f32; 2]>) -> bool {
        let Some(gaze) = gaze else {
            *self = Self::default();
            return false;
        };
        let since = *self.since.get_or_insert(time);
        ease(&mut self.fast, gaze, share(dt, STEADY_FAST_TAU_S));
        ease(&mut self.slow, gaze, share(dt, STEADY_SLOW_TAU_S));
        // (just come back: not known to be steady until the slow one has had a little time)
        time - since >= STEADY_SLOW_TAU_S
            && self.fast.zip(self.slow).is_some_and(|(fast, slow)| length(sub(fast, slow)) <= STEADY_DEG)
    }
}

/// One eye's d and its glints.
#[derive(Clone, Copy, Debug, Default)]
struct EyeTrack {
    steady: Steady,
    /// d smoothed over FAST_TAU_S (with the offset as it is meant to be: the target).
    fast: Option<[f32; 2]>,
    /// d's baseline, and how many seconds of trusted samples it has.
    base: Option<[f32; 2]>,
    base_s: f64,
    /// The slip smoothed over SLIP_QUIET_TAU_S, and as it was while the baseline was learnt (px).
    slip_slow: Option<[f32; 2]>,
    slip_base: Option<[f32; 2]>,
}

/// A step being tested: since when, and how many seconds of trusted samples it has stood.
#[derive(Clone, Copy, Debug)]
struct Candidate {
    trusted_s: f64,
    last_trusted: f64,
}

/// One sample's guard: the offset added to each eye's gaze (degrees), whether the witness was trusted, and the pupil
/// gaze it saw (the mean of both eyes' mapped directions, when both were there).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct GuardStep {
    pub offset: [[f32; 2]; 2],
    pub trusted: bool,
    pub witness: Option<[f32; 2]>,
}

/// What happened to a step, for the journal.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Event {
    /// Valve moved by itself: this much went into the offset (each eye).
    Absorbed([[f32; 2]; 2]),
    /// The headset moved: Valve kept (the offset let go), the baseline took the new difference.
    KeptValve,
}

/// The guard between samples.
#[derive(Clone, Debug, Default)]
pub struct GazeGuard {
    fresh: Fresh,
    used: bool,
    last_time: Option<f64>,
    fit: Option<Fit>,
    valve_steady: Steady,
    eyes: [EyeTrack; 2],
    /// The pupils' combined gaze while the baseline was learnt, smoothed like it (see GAZE_SAME_DEG).
    base_gaze: Option<[f32; 2]>,
    candidate: Option<Candidate>,
    /// The offset as applied, and where it is going (each eye, degrees).
    offset: [[f32; 2]; 2],
    target: [[f32; 2]; 2],
    last_trusted: Option<f64>,
    /// The steps decided since the last journal line, and the samples stepped and trusted since then.
    events: Vec<(f64, Event)>,
    samples: u64,
    trusted: u64,
    next_log: Option<f64>,
}

impl GazeGuard {
    /// Start over (the key turned off, or gaze_source "pupil"), if it has been used; true if it had.
    pub fn reset_if_used(&mut self) -> bool {
        let used = self.used;
        if used {
            *self = Self::default();
        }
        used
    }

    /// The eye tracking stopped (main.rs on_lost: the headset taken off, or the eye server gone): the next wear starts
    /// from zero, as after a re-center. After a re-seat Valve solves its self-calibration again, so what was absorbed
    /// may be gone.
    pub fn wear_ended(&mut self) {
        self.rezero();
    }

    /// The offset back to zero and everything learnt with it forgotten (an eye fit or re-center); the journal's counts
    /// go on.
    fn rezero(&mut self) {
        self.valve_steady = Steady::default();
        self.eyes = [EyeTrack::default(); 2];
        self.base_gaze = None;
        self.candidate = None;
        self.offset = [[0.0; 2]; 2];
        self.target = [[0.0; 2]; 2];
        self.last_trusted = None;
    }

    /// Each eye's pupil direction for the witness (None while not usable): its pair (`mapped`, the slip taken out)
    /// mapped while the eye server's own pair (`extra`) is fresh (pupil.rs FRESH_S), the eye open (`open`) and the
    /// direction inside the fit's range; both left out when they are further apart than pupil.rs EYES_APART_DEG.
    pub fn witness(
        &mut self,
        time: f64,
        maps: &[EyeMap; 2],
        extra: &[f32; 8],
        mapped: &[f32; 8],
        open: [bool; 2],
    ) -> [Option<[f32; 2]>; 2] {
        let fresh = self.fresh.update(time, extra);
        let eyes: [Option<[f32; 2]>; 2] = [0, 1].map(|eye| {
            let pair = [mapped[2 * eye], mapped[2 * eye + 1]];
            fresh[eye]?;
            if !open[eye] || !pair.iter().all(|value| value.is_finite()) {
                return None;
            }
            Some(maps[eye].apply(pair)).filter(|direction| pupil::in_range(*direction))
        });
        match eyes {
            [Some(left), Some(right)] if length(sub(left, right)) > pupil::EYES_APART_DEG => [None, None],
            eyes => eyes,
        }
    }

    /// One sample: its time, Valve's combined gaze as it would go out without the guard (degrees) and whether it is
    /// usable (not held), both pupils' directions (witness), each eye's glint slip (px; None while eyecam-rec doesn't
    /// follow the glints) and the fit both sources have now. The offset to add to each eye's gaze.
    pub fn step(
        &mut self,
        time: f64,
        valve: [f32; 2],
        valve_ok: bool,
        pupils: [Option<[f32; 2]>; 2],
        slip: [Option<[f32; 2]>; 2],
        fit: &Fit,
    ) -> GuardStep {
        self.used = true;
        // Time going back (the eye server restarted its clock): everything starts over
        if self.last_time.is_some_and(|last| time < last) {
            self.rezero();
            self.fresh = Fresh::default();
            self.fit = None;
            self.next_log = None;
        }
        let dt = self.last_time.map_or(0.0, |last| time - last);
        self.last_time = Some(time);
        if self.fit.is_some_and(|old| old != *fit) {
            self.rezero();
        }
        self.fit = Some(*fit);
        self.samples += 1;

        let valve_steady = self.valve_steady.step(time, dt, valve_ok.then_some(valve));
        let pupils_steady = [0, 1].map(|eye| self.eyes[eye].steady.step(time, dt, pupils[eye]));
        let steady = valve_steady && pupils_steady == [true; 2];
        // d with the offset as it is meant to be, so a step already being eased in does not count again
        let d: Option<[[f32; 2]; 2]> = match (valve_ok, pupils) {
            (true, [Some(left), Some(right)]) => {
                Some([sub(add(valve, self.target[0]), left), sub(add(valve, self.target[1]), right)])
            }
            _ => None,
        };
        if let (Some(d), true) = (d, steady) {
            for (eye, d) in self.eyes.iter_mut().zip(d) {
                ease(&mut eye.fast, d, share(dt, FAST_TAU_S));
            }
        }
        // The glints: followed and without a step, both eyes
        let mut quiet = true;
        for (eye, slip) in self.eyes.iter_mut().zip(slip) {
            match slip {
                Some(slip) => {
                    ease(&mut eye.slip_slow, slip, share(dt, SLIP_QUIET_TAU_S));
                    quiet &= eye.slip_slow.is_some_and(|slow| length(sub(slip, slow)) <= SLIP_QUIET_PX);
                }
                None => quiet = false,
            }
        }
        let fast = self.eyes[0].fast.zip(self.eyes[1].fast);
        let base = self.eyes[0].base.zip(self.eyes[1].base);
        let agree = match (fast, base) {
            (Some((left, right)), Some((base_left, base_right))) => {
                length(sub(sub(left, right), sub(base_left, base_right))) <= AGREE_DEG
            }
            (Some(_), None) => true,
            _ => false,
        };
        let witness = match pupils {
            [Some(left), Some(right)] => Some([(left[0] + right[0]) / 2.0, (left[1] + right[1]) / 2.0]),
            _ => None,
        };
        let trusted = d.is_some() && steady && quiet && agree;
        if let (true, Some(gaze)) = (trusted, witness) {
            self.trusted += 1;
            self.last_trusted = Some(time);
            self.trusted_step(time, dt.min(MAX_DT_S), slip, gaze);
        } else if self.candidate.is_some_and(|candidate| time - candidate.last_trusted > CANDIDATE_GAP_S) {
            self.candidate = None;
        }
        // Lost for long: eased out
        let dt_ease = dt.clamp(0.0, MAX_DT_S) as f32;
        if self.last_trusted.is_none_or(|last| time - last > WITNESS_LOST_S) {
            self.target = self.target.map(|target| toward(target, [0.0; 2], EASE_OUT_DEG_S * dt_ease));
        }
        for eye in 0..2 {
            self.offset[eye] = toward(self.offset[eye], self.target[eye], EASE_DEG_S * dt_ease);
        }
        GuardStep { offset: self.offset, trusted, witness }
    }

    /// A trusted sample (`dt` its interval, capped; `gaze` the pupils' combined gaze): the baseline learnt, or a step
    /// tested and decided.
    fn trusted_step(&mut self, time: f64, dt: f64, slip: [Option<[f32; 2]>; 2], gaze: [f32; 2]) {
        let (Some(fast_left), Some(fast_right), Some(slip_left), Some(slip_right)) =
            (self.eyes[0].fast, self.eyes[1].fast, slip[0], slip[1])
        else {
            return;
        };
        let (fast, slip) = ([fast_left, fast_right], [slip_left, slip_right]);
        let ready = self.eyes.iter().all(|eye| eye.base.is_some() && eye.base_s >= BASE_MIN_S);
        let devs = [0, 1].map(|eye| self.eyes[eye].base.map_or([0.0; 2], |base| sub(fast[eye], base)));
        let step = [(devs[0][0] + devs[1][0]) / 2.0, (devs[0][1] + devs[1][1]) / 2.0];
        if !ready || length(step) <= STEP_DEG {
            // Near the baseline (or still learning it): learnt, the glints with it
            self.candidate = None;
            let mut by = 0.0;
            for (eye, (fast, slip)) in self.eyes.iter_mut().zip(fast.iter().zip(&slip)) {
                eye.base_s += dt;
                // The plain mean until it has BASE_TAU_S of samples, then smoothed over BASE_TAU_S
                by = (dt / eye.base_s.min(BASE_TAU_S).max(f64::EPSILON)).min(1.0) as f32;
                ease(&mut eye.base, *fast, by);
                ease(&mut eye.slip_base, *slip, by);
            }
            ease(&mut self.base_gaze, gaze, by);
            return;
        }
        // Off the baseline: tested only with the glints away from where they were (the headset's, wherever the eyes
        // look) or with the eyes where they looked while it was learnt (see GAZE_SAME_DEG); otherwise left alone, and a
        // step being tested is dropped once none has counted for CANDIDATE_GAP_S
        let moved = self.eyes.iter().zip(&slip).any(|(eye, slip)| {
            eye.slip_base.is_none_or(|base| length(sub(*slip, base)) > SLIP_MOVED_PX)
        });
        let same_gaze = self.base_gaze.is_some_and(|base| length(sub(gaze, base)) <= GAZE_SAME_DEG);
        if !moved && !same_gaze {
            if self.candidate.is_some_and(|candidate| time - candidate.last_trusted > CANDIDATE_GAP_S) {
                self.candidate = None;
            }
            return;
        }
        let candidate = self.candidate.get_or_insert(Candidate { trusted_s: 0.0, last_trusted: time });
        candidate.trusted_s += dt;
        candidate.last_trusted = time;
        if candidate.trusted_s < STEP_HOLD_S {
            return;
        }
        self.candidate = None;
        if moved {
            // The headset moved: Valve follows that, the pupils don't quite, and after a re-seat Valve usually solves
            // its self-calibration again, so what was absorbed may be gone: the offset is let go (eased out) and the
            // new difference, without it, is the baseline, at the gaze the pupils read now
            for eye in 0..2 {
                let back = sub([0.0; 2], self.target[eye]);
                self.target[eye] = [0.0; 2];
                let track = &mut self.eyes[eye];
                track.fast = Some(add(fast[eye], back));
                track.base = track.fast;
                track.slip_base = Some(slip[eye]);
            }
            self.base_gaze = Some(gaze);
            self.events.push((time, Event::KeptValve));
            return;
        }
        // Valve moved by itself: the step into the offset (bounded; what doesn't fit is taken as the new baseline)
        let mut absorbed = [[0.0; 2]; 2];
        for eye in 0..2 {
            let wanted = sub(self.target[eye], devs[eye]);
            let target = bounded(wanted, MAX_OFFSET_DEG);
            let moved_by = sub(target, self.target[eye]);
            self.target[eye] = target;
            absorbed[eye] = moved_by;
            let track = &mut self.eyes[eye];
            track.fast = track.fast.map(|fast| add(fast, moved_by));
            track.base = track.fast;
        }
        self.events.push((time, Event::Absorbed(absorbed)));
    }

    /// The journal line, once per LOG_S while the key is on (`active`: with a pupil map for both eyes and gaze_source
    /// "valve"; otherwise the line says the guard can't work).
    pub fn log_line(&mut self, time: f64, active: bool) -> Option<String> {
        let due = *self.next_log.get_or_insert(time + LOG_S);
        if time < due {
            return None;
        }
        self.next_log = Some(time + LOG_S);
        if !active {
            self.samples = 0;
            self.trusted = 0;
            return Some("Gaze guard: no pupil map for both eyes (or gaze_source \"pupil\"): Valve's gaze as it is".into());
        }
        let degrees = |value: [f32; 2]| format!("({:+.1}, {:+.1})°", value[0], value[1]);
        let new = std::mem::take(&mut self.events);
        let absorbed: Vec<String> = new
            .iter()
            .filter_map(|(_, event)| match event {
                Event::Absorbed([left, right]) => Some(format!("L {} R {}", degrees(*left), degrees(*right))),
                Event::KeptValve => None,
            })
            .collect();
        let kept = new.iter().filter(|(_, event)| *event == Event::KeptValve).count();
        let share = if self.samples > 0 { 100.0 * self.trusted as f64 / self.samples as f64 } else { 0.0 };
        let steps = match absorbed.len() {
            0 => "no step absorbed".to_owned(),
            n => format!("{n} step{} absorbed ({})", if n == 1 { "" } else { "s" }, absorbed.join("; ")),
        };
        let line = format!(
            "Gaze guard: offset L {} R {}, {steps}, {kept} kept Valve's (the headset moved; offset let go), witness trusted {share:.1}% of {} samples",
            degrees(self.offset[0]),
            degrees(self.offset[1]),
            self.samples
        );
        self.samples = 0;
        self.trusted = 0;
        Some(line)
    }

    /// The steps decided since the last journal line.
    #[cfg(test)]
    pub fn events(&self) -> &[(f64, Event)] {
        &self.events
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const DT: f64 = 1.0 / 90.0;

    fn fit(center: f32) -> Fit {
        Fit {
            maps: [EyeMap { yaw: [0.0, 100.0, 0.0], pitch: [0.0, 0.0, 100.0], center: [center, 0.0] }; 2],
            valve: [Some(0.0); 10],
        }
    }

    /// A made-up wear: per sample, Valve's gaze, both pupils, both slips. The eyes look at `look` (degrees); each
    /// pupil reads it plus its own bias (the left eye 1° right of the right one: vergence and the map's own error) and
    /// a little noise; Valve reads it plus `valve_error`.
    struct Wear {
        noise: u32,
    }

    impl Wear {
        /// Noise in -amount..amount, the same sequence every run.
        fn noise(&mut self, amount: f32) -> f32 {
            self.noise = self.noise.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            ((self.noise >> 8) as f32 / (1u32 << 24) as f32 * 2.0 - 1.0) * amount
        }
    }

    struct Out {
        sent: Vec<[f32; 2]>,
        steps: Vec<GuardStep>,
    }

    /// Runs `seconds` of samples from `start`: `valve_error(t)`, `pupil_error(eye, t)`, `slip(t)` (both eyes alike) and
    /// the fit (`center`); the gaze sent is Valve's plus the mean offset.
    #[allow(clippy::too_many_arguments)]
    fn run(
        guard: &mut GazeGuard,
        wear: &mut Wear,
        start: f64,
        seconds: f64,
        look: [f32; 2],
        valve_error: impl Fn(f64) -> [f32; 2],
        pupil_error: impl Fn(usize, f64) -> [f32; 2],
        slip: impl Fn(f64) -> Option<[f32; 2]>,
        center: f32,
    ) -> Out {
        let mut out = Out { sent: Vec::new(), steps: Vec::new() };
        let count = (seconds / DT).round() as usize;
        for i in 0..count {
            let t = start + i as f64 * DT;
            let valve = add(look, valve_error(t));
            let pupils = [0, 1].map(|eye| {
                let bias = if eye == 0 { [0.5, 0.0] } else { [-0.5, 0.0] };
                let noise = [wear.noise(0.5), wear.noise(0.5)];
                Some(add(add(add(look, bias), pupil_error(eye, t)), noise))
            });
            let slip = [slip(t); 2];
            let step = guard.step(t, valve, true, pupils, slip, &fit(center));
            let mean = [(step.offset[0][0] + step.offset[1][0]) / 2.0, (step.offset[0][1] + step.offset[1][1]) / 2.0];
            out.sent.push(add(valve, mean));
            out.steps.push(step);
        }
        out
    }

    fn largest_step(gazes: &[[f32; 2]]) -> f32 {
        gazes.windows(2).map(|pair| length(sub(pair[1], pair[0]))).fold(0.0, f32::max)
    }

    fn no_error(_: f64) -> [f32; 2] {
        [0.0; 2]
    }

    fn pupils_right(_: usize, _: f64) -> [f32; 2] {
        [0.0; 2]
    }

    fn still_slip(_: f64) -> Option<[f32; 2]> {
        Some([3.0, -2.0])
    }

    #[test]
    fn a_valve_ratchet_without_slip_is_absorbed_within_3_s_and_eased_in() {
        for ramp in [0.0, 0.3] {
            let mut guard = GazeGuard::default();
            let mut wear = Wear { noise: 1 };
            // 20 s to learn the baseline, then Valve moves 16° to the right (at once, or over 0.3 s) and stays
            let at = 20.0;
            let error = move |t: f64| {
                let share = if ramp == 0.0 { f32::from(t >= at) } else { ((t - at) / ramp).clamp(0.0, 1.0) as f32 };
                [16.0 * share, 0.0]
            };
            let out = run(&mut guard, &mut wear, 0.0, 30.0, [2.0, -1.0], error, pupils_right, still_slip, 0.0);
            let index = |t: f64| (t / DT).round() as usize;
            // Before: nothing added
            assert!(out.steps[..index(at)].iter().all(|s| s.offset == [[0.0; 2]; 2]), "{:?}", out.steps[index(at) - 1]);
            // Within 3 s the gaze sent is back where the eyes look (to within a degree), and stays there
            let back = index(at + 3.0);
            assert!(out.sent[back..].iter().all(|g| length(sub(*g, [2.0, -1.0])) < 1.0), "{:?}", out.sent[back]);
            assert_eq!(guard.events().len(), 1, "{:?}", guard.events());
            let Event::Absorbed(absorbed) = guard.events()[0].1 else { panic!("{:?}", guard.events()) };
            assert!(absorbed.iter().all(|a| (a[0] + 16.0).abs() < 0.5 && a[1].abs() < 0.5), "{absorbed:?}");
            // Eased in: the guard never moves the gaze sent by more than a degree a sample (Valve's own step aside)
            let added: Vec<[f32; 2]> = out.steps.iter().map(|s| s.offset[0]).collect();
            assert!(largest_step(&added) < 1.0, "{}", largest_step(&added));
            if ramp > 0.0 {
                assert!(largest_step(&out.sent) < 1.0, "{}", largest_step(&out.sent));
            }
            // ...and not at all before it has stood for STEP_HOLD_S
            assert!(out.steps[..index(at + STEP_HOLD_S)].iter().all(|s| s.offset == [[0.0; 2]; 2]));
        }
    }

    #[test]
    fn the_headset_pushed_with_valve_steady_gives_no_offset() {
        // The headset pushed 10 px at 20 s (over 0.3 s) and held there; the pupils read up to 10° off as long as it is
        // (the slip correction taking none, half or all of it out), then let go at 40 s; Valve stays where the eyes
        // look
        for corrected_share in [0.0, 0.5, 1.0] {
            let mut guard = GazeGuard::default();
            let mut wear = Wear { noise: 7 };
            let pushed = |t: f64| ((t - 20.0) / 0.3).clamp(0.0, 1.0) as f32 * (1.0 - ((t - 40.0) / 0.3).clamp(0.0, 1.0) as f32);
            let pupil = move |_: usize, t: f64| [10.0 * (1.0 - corrected_share) * pushed(t), 0.0];
            let slip = move |t: f64| Some([3.0 + 10.0 * pushed(t), -2.0]);
            let out = run(&mut guard, &mut wear, 0.0, 60.0, [0.0, 0.0], no_error, pupil, slip, 0.0);
            assert!(out.steps.iter().all(|s| s.offset == [[0.0; 2]; 2]), "{corrected_share}: {:?}", guard.events());
            assert!(guard.events().iter().all(|(_, e)| *e == Event::KeptValve), "{:?}", guard.events());
            // (the pupils off by more than STEP_DEG: tested, and each time taken for the headset; the witness trusted
            // again once the glints are still)
            if corrected_share < 0.6 {
                assert_eq!(guard.events().len(), 2, "{corrected_share}: {:?}", guard.events());
            }
            let trusted = out.steps.iter().filter(|s| s.trusted).count();
            assert!(trusted > out.steps.len() * 3 / 4, "{trusted} of {}", out.steps.len());
            // Nothing changes what is sent
            assert!(out.sent.iter().all(|g| length(*g) < 1e-6));
        }
        // Pushed while Valve also moved 8° (a solve right then): still Valve's, the headset moved
        let mut guard = GazeGuard::default();
        let mut wear = Wear { noise: 9 };
        let error = |t: f64| [if t >= 20.0 { 8.0 } else { 0.0 }, 0.0];
        let slip = |t: f64| Some([3.0 + if t >= 20.0 { 10.0 } else { 0.0 }, -2.0]);
        let out = run(&mut guard, &mut wear, 0.0, 40.0, [0.0, 0.0], error, pupils_right, slip, 0.0);
        assert!(out.steps.iter().all(|s| s.offset == [[0.0; 2]; 2]), "{:?}", guard.events());
    }

    #[test]
    fn pupil_noise_and_relocks_give_no_offset() {
        let mut guard = GazeGuard::default();
        let mut wear = Wear { noise: 3 };
        // A minute: one pupil now and then relocked 8° or 12° off for 0.5 to 4 s, the eyes taking turns; each pupil
        // also with 1.5° of noise on top of the usual; Valve steady
        let relock = |eye: usize, t: f64| -> [f32; 2] {
            let spans = [(12.0, 15.0, 0, 8.0), (25.0, 25.5, 1, -12.0), (33.0, 37.0, 0, 12.0), (45.0, 47.0, 1, 8.0)];
            let off = spans.iter().find(|(from, to, which, _)| *which == eye && (*from..*to).contains(&t));
            [off.map_or(0.0, |s| s.3), 0.0]
        };
        let noisy = std::cell::RefCell::new(Wear { noise: 11 });
        let pupil = |eye: usize, t: f64| {
            let mut noise = noisy.borrow_mut();
            add(relock(eye, t), [noise.noise(1.5), noise.noise(1.5)])
        };
        let out = run(&mut guard, &mut wear, 0.0, 60.0, [-5.0, 3.0], no_error, pupil, still_slip, 0.0);
        assert!(out.steps.iter().all(|s| s.offset == [[0.0; 2]; 2]), "{:?}", guard.events());
        assert!(guard.events().is_empty(), "{:?}", guard.events());
        // ...while it was trusted most of the rest of the time
        let trusted = out.steps.iter().filter(|s| s.trusted).count();
        assert!(trusted > out.steps.len() / 2, "{trusted} of {}", out.steps.len());
        // Both pupils relocked the same way at the same moment would read like Valve moving; without the glints
        // nothing is trusted at all
        let mut blind = GazeGuard::default();
        let out = run(&mut blind, &mut wear, 0.0, 30.0, [0.0; 2], |t| [if t > 10.0 { 16.0 } else { 0.0 }, 0.0], pupils_right, |_| None, 0.0);
        assert!(out.steps.iter().all(|s| s.offset == [[0.0; 2]; 2] && !s.trusted));
    }

    #[test]
    fn a_recenter_or_a_new_fit_zeroes_the_offset_and_it_is_bounded() {
        let mut guard = GazeGuard::default();
        let mut wear = Wear { noise: 5 };
        let error = |t: f64| [if t >= 20.0 { 16.0 } else { 0.0 }, 0.0];
        let out = run(&mut guard, &mut wear, 0.0, 30.0, [0.0; 2], error, pupils_right, still_slip, 0.0);
        assert!(out.steps.last().unwrap().offset.iter().all(|o| (o[0] + 16.0).abs() < 0.5));
        // A re-center (a new pupil centre): zero at once, and nothing absorbed again from the old baseline
        let out = run(&mut guard, &mut wear, 30.0, 20.0, [0.0; 2], error, pupils_right, still_slip, 1.0);
        assert!(out.steps.iter().all(|s| s.offset == [[0.0; 2]; 2]), "{:?}", out.steps[0]);
        // Valve's correction changed (the fit wrote a new zero point): zero too
        let mut guard = GazeGuard::default();
        run(&mut guard, &mut wear, 0.0, 30.0, [0.0; 2], error, pupils_right, still_slip, 0.0);
        let other = Fit { valve: [Some(0.1); 10], ..fit(0.0) };
        let step = guard.step(30.0, [16.0, 0.0], true, [Some([0.5, 0.0]), Some([-0.5, 0.0])], [Some([3.0, -2.0]); 2], &other);
        assert_eq!(step.offset, [[0.0; 2]; 2]);
        // Turned off: nothing kept
        assert!(guard.reset_if_used() && !guard.reset_if_used());
        // A 30° ratchet: at most MAX_OFFSET_DEG of it, eased, and the rest not tested again and again
        let mut guard = GazeGuard::default();
        let error = |t: f64| [if t >= 20.0 { 30.0 } else { 0.0 }, 0.0];
        let out = run(&mut guard, &mut wear, 0.0, 40.0, [0.0; 2], error, pupils_right, still_slip, 0.0);
        let last = out.steps.last().unwrap().offset;
        assert!(last.iter().all(|o| (length(*o) - MAX_OFFSET_DEG).abs() < 1e-3), "{last:?}");
        assert_eq!(guard.events().len(), 1, "{:?}", guard.events());
    }

    #[test]
    fn the_offset_eases_out_when_the_witness_is_lost_for_long() {
        let mut guard = GazeGuard::default();
        let mut wear = Wear { noise: 13 };
        let error = |t: f64| [if t >= 20.0 { 10.0 } else { 0.0 }, 0.0];
        run(&mut guard, &mut wear, 0.0, 30.0, [0.0; 2], error, pupils_right, still_slip, 0.0);
        // The glints gone for a minute and a half: held for WITNESS_LOST_S, then eased out at EASE_OUT_DEG_S
        let out = run(&mut guard, &mut wear, 30.0, 90.0, [0.0; 2], error, pupils_right, |_| None, 0.0);
        let at = |t: f64| out.steps[((t - 30.0) / DT).round() as usize].offset[0][0];
        assert!((at(30.0 + WITNESS_LOST_S - 1.0) + 10.0).abs() < 0.5, "{}", at(30.0 + WITNESS_LOST_S - 1.0));
        assert!(at(30.0 + WITNESS_LOST_S + 5.0) > -10.0 + 4.0 && at(30.0 + WITNESS_LOST_S + 5.0) < -10.0 + 6.0);
        assert!(at(119.0).abs() < 1e-3);
        let added: Vec<[f32; 2]> = out.steps.iter().map(|s| s.offset[0]).collect();
        assert!(largest_step(&added) <= EASE_OUT_DEG_S * DT as f32 + 1e-4);
        // Back with Valve still 10° off and the headset where it was: absorbed again
        run(&mut guard, &mut wear, 120.0, 10.0, [0.0; 2], error, pupils_right, still_slip, 0.0);
        let last = guard.offset[0];
        assert!((last[0] + 10.0).abs() < 0.5, "{last:?}");
    }

    #[test]
    fn looking_aside_and_back_with_valve_and_the_glints_steady_gives_no_offset() {
        // 20 s straight ahead (the baseline), then a look 20° to the side where the corrected pupil map reads 4° (or
        // 8°) further out in both eyes alike (its own error there), held for 3 s (or 10 s), then back; Valve right all
        // along, the glints still
        for (aside_s, error) in [(3.0, 4.0), (3.0, 8.0), (10.0, 4.0), (10.0, -6.0)] {
            let mut guard = GazeGuard::default();
            let mut wear = Wear { noise: 19 };
            let mut steps = run(&mut guard, &mut wear, 0.0, 20.0, [0.0; 2], no_error, pupils_right, still_slip, 0.0).steps;
            let map_error = move |_: usize, _: f64| [error, 0.0];
            steps.extend(run(&mut guard, &mut wear, 20.0, aside_s, [20.0, 0.0], no_error, map_error, still_slip, 0.0).steps);
            let back = 20.0 + aside_s;
            steps.extend(run(&mut guard, &mut wear, back, 10.0, [0.0; 2], no_error, pupils_right, still_slip, 0.0).steps);
            assert!(steps.iter().all(|s| s.offset == [[0.0; 2]; 2]), "{aside_s} {error}: {:?}", guard.events());
            assert!(guard.events().is_empty(), "{:?}", guard.events());
            // (the witness trusted aside too: it is the gaze, not the trust, that keeps the step out)
            let aside = &steps[(20.0 / DT) as usize + 60..((20.0 + aside_s) / DT) as usize];
            assert!(aside.iter().filter(|s| s.trusted).count() > aside.len() / 2);
        }
        // Valve's step while the eyes look aside is taken out once they are back where the baseline was learnt
        let mut guard = GazeGuard::default();
        let mut wear = Wear { noise: 23 };
        run(&mut guard, &mut wear, 0.0, 20.0, [0.0; 2], no_error, pupils_right, still_slip, 0.0);
        let ratchet = |_: f64| [10.0, 0.0];
        let map_error = |_: usize, _: f64| [4.0, 0.0];
        let aside = run(&mut guard, &mut wear, 20.0, 5.0, [20.0, 0.0], ratchet, map_error, still_slip, 0.0);
        assert!(aside.steps.iter().all(|s| s.offset == [[0.0; 2]; 2]));
        let out = run(&mut guard, &mut wear, 25.0, 5.0, [0.0; 2], ratchet, pupils_right, still_slip, 0.0);
        assert!(out.sent.last().is_some_and(|g| length(*g) < 1.0), "{:?}", out.sent.last());
    }

    #[test]
    fn a_reseat_lets_the_offset_go_and_it_does_not_come_back() {
        // Valve's 16° step taken out, then at 30 s the headset re-seated: the glints 10 px away, the pupils 6° off
        // (what the slip correction leaves), and Valve, solving its self-calibration again, right once more
        let mut guard = GazeGuard::default();
        let mut wear = Wear { noise: 29 };
        let valve = |t: f64| [if (20.0..30.0).contains(&t) { 16.0 } else { 0.0 }, 0.0];
        let reseat = |t: f64| Some([3.0 + if t >= 30.0 { 10.0 } else { 0.0 }, -2.0]);
        let pupils = |_: usize, t: f64| [if t >= 30.0 { 6.0 } else { 0.0 }, 0.0];
        let out = run(&mut guard, &mut wear, 0.0, 70.0, [0.0; 2], valve, pupils, reseat, 0.0);
        let index = |t: f64| (t / DT).round() as usize;
        assert!((out.steps[index(29.9)].offset[0][0] + 16.0).abs() < 0.5, "{:?}", out.steps[index(29.9)]);
        // Let go within about 5 s of the re-seat (the glints settle, the step stands for STEP_HOLD_S), eased, and
        // not taken in again
        assert!(out.steps[index(36.0)..].iter().all(|s| s.offset == [[0.0; 2]; 2]), "{:?}", out.steps[index(36.0)]);
        assert!(out.sent[index(36.0)..].iter().all(|g| length(*g) < 1e-6));
        let added: Vec<[f32; 2]> = out.steps.iter().map(|s| s.offset[0]).collect();
        assert!(largest_step(&added) < 1.0, "{}", largest_step(&added));
        let events: Vec<bool> = guard.events().iter().map(|(_, e)| matches!(e, Event::Absorbed(_))).collect();
        assert_eq!(events, [true, false], "{:?}", guard.events());
        // The eye tracking stopping (the headset taken off) zeroes it at once too
        let mut guard = GazeGuard::default();
        let valve = |t: f64| [if t >= 20.0 { 16.0 } else { 0.0 }, 0.0];
        run(&mut guard, &mut wear, 0.0, 30.0, [0.0; 2], valve, pupils_right, still_slip, 0.0);
        assert!((guard.offset[0][0] + 16.0).abs() < 0.5);
        guard.wear_ended();
        let step = guard.step(40.0, [16.0, 0.0], true, [Some([0.5, 0.0]), Some([-0.5, 0.0])], [Some([3.0, -2.0]); 2], &fit(0.0));
        assert_eq!(step.offset, [[0.0; 2]; 2]);
    }

    #[test]
    fn the_witness_takes_fresh_open_pupils_in_range() {
        let maps = fit(0.0).maps;
        let mut guard = GazeGuard::default();
        let pair = |yaw: f32, pitch: f32| [yaw / 100.0, pitch / 100.0];
        let extra = |l: [f32; 2], r: [f32; 2]| [l[0], l[1], r[0], r[1], 0.0, 0.0, 0.0, 0.0];
        // The first pair is not known to be fresh; the next, changed, is
        let e = extra(pair(2.0, 1.0), pair(1.0, 1.0));
        assert_eq!(guard.witness(0.0, &maps, &e, &e, [true; 2]), [None, None]);
        let e = extra(pair(2.1, 1.0), pair(1.1, 1.0));
        let both = guard.witness(DT, &maps, &e, &e, [true; 2]);
        assert!(both.iter().all(Option::is_some));
        // The slip-corrected pairs are what is mapped
        let moved = extra(pair(3.0, 1.0), pair(1.1, 1.0));
        let e2 = extra(pair(2.2, 1.0), pair(1.2, 1.0));
        let eyes = guard.witness(2.0 * DT, &maps, &e2, &moved, [true; 2]);
        assert!((eyes[0].unwrap()[0] - 3.0).abs() < 1e-4);
        // A shut eye; one out of range; both too far apart
        let e3 = extra(pair(2.3, 1.0), pair(1.3, 1.0));
        assert_eq!(guard.witness(3.0 * DT, &maps, &e3, &e3, [false, true])[0], None);
        let far = extra(pair(60.0, 1.0), pair(1.4, 1.0));
        assert_eq!(guard.witness(4.0 * DT, &maps, &far, &far, [true; 2])[0], None);
        let apart = extra(pair(-12.0, 1.0), pair(12.0, 1.0));
        assert_eq!(guard.witness(5.0 * DT, &maps, &apart, &apart, [true; 2]), [None, None]);
    }

    #[test]
    fn the_journal_line_comes_once_a_minute() {
        let mut guard = GazeGuard::default();
        let mut wear = Wear { noise: 17 };
        assert!(guard.log_line(0.0, true).is_none());
        let error = |t: f64| [if t >= 20.0 { 16.0 } else { 0.0 }, 0.0];
        run(&mut guard, &mut wear, 0.0, 30.0, [0.0; 2], error, pupils_right, still_slip, 0.0);
        assert!(guard.log_line(30.0, true).is_none());
        let line = guard.log_line(60.0, true).unwrap();
        assert!(line.starts_with("Gaze guard: offset L (-15.9, ") || line.starts_with("Gaze guard: offset L (-16.0, "), "{line}");
        assert!(line.contains("°, 1 step absorbed (L (-1"), "{line}");
        assert!(line.contains("0 kept Valve's") && line.contains("witness trusted "), "{line}");
        assert!(guard.log_line(61.0, true).is_none());
        let line = guard.log_line(120.0, true).unwrap();
        assert!(line.contains("no step absorbed") && line.contains("trusted 0.0% of 0 samples"), "{line}");
        assert!(guard.log_line(180.0, false).unwrap().contains("no pupil map"));
    }
}
