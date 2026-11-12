//! Gaze from the pupil position (gaze_source "pupil"; a test). The eye server publishes, next to its gaze, each eye's
//! pupil centre in the eye camera's picture (estimate_extra 0/1 for the left eye, 2/3 for the right). Its
//! self-calibration (SteamOS 0.5.8 on) keeps re-solving a rotation and gain that it applies to the gaze, which moves
//! the gaze several degrees at a time; it does not move the pupil pair. The panel's eye fit maps each eye's pair to
//! that eye's direction (an affine map, config.rs pupil_*), and each re-center sets that eye's centre for the wear.
//!
//! The gaze sent is the combined one: the mean of both eyes' mapped directions. Each eye's direction is used while its
//! pair is fresh (FRESH_S), the eye is not shut (the core's own decision, which goes by the eye camera too where the
//! eye server's openness is stuck at 0), and it is inside the fit's range (RANGE_MARGIN_DEG); two eyes further apart
//! than EYES_APART_DEG are both left out (one of them is wrong). One eye missing: the other plus the last known
//! difference between them. Both missing: the last gaze held for HOLD_S, then the eye
//! server's gaze shifted by the last known difference between it and the pupil gaze. Every change of where it comes
//! from eases over BLEND_S from the gaze as it was, so nothing jumps.

use crate::config::{GazeSource, Settings};

/// A pupil pair counts as fresh while it last changed at most this long ago (s). The eye server repeats its last
/// estimate while it has no new one (an eye it lost, a blink), and a repeated pair is not where the eye looks now.
pub const FRESH_S: f64 = 0.1;
/// The eye fit's dots are this far to the sides and up and down (degrees, gaze_fit.h kSideDeg / kUpDownDeg)...
const FIT_YAW_DEG: f32 = 20.0;
const FIT_PITCH_DEG: f32 = 15.0;
/// ...and a mapped eye more than this beyond them is not used (degrees): the map is a straight line fitted there.
pub const RANGE_MARGIN_DEG: f32 = 15.0;
/// Both eyes' directions further apart than this (degrees) are not used: the eyes look about the same way (4° apart
/// for a dot 0.9 m away, up to about 17° with each eye's straight-line map at the side dots), so one of the two is
/// wrong (the pairs while the headset is being put on read 40° and more apart). Intentional cross-eye gets the eye
/// server's gaze then.
pub const EYES_APART_DEG: f32 = 20.0;
/// Both eyes missing: the last combined gaze is held this long (s), then the eye server's gaze takes over.
pub const HOLD_S: f64 = 0.3;
/// A change of where the gaze comes from eases over this long (s) from the gaze as it was.
pub const BLEND_S: f64 = 0.25;
/// The differences that bridge a missing eye (left - right) and both (eye server - pupil) are smoothed over this (s).
pub const DIFF_TAU_S: f64 = 1.0;
/// The log line for a change of source is at most one per this long (s); the changes in between are counted in it.
pub const LOG_CHANGE_S: f64 = 5.0;
/// ...and the shares of the time come once per this long (s).
pub const LOG_SHARES_S: f64 = 60.0;

/// One eye's map from its pupil pair to its direction (degrees), and its centre for this wear.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct EyeMap {
    /// yaw = yaw[0] + yaw[1] * u + yaw[2] * v (+ center[0]); pitch likewise.
    pub yaw: [f32; 3],
    pub pitch: [f32; 3],
    pub center: [f32; 2],
}

impl EyeMap {
    /// The eye's direction (yaw, pitch in degrees) for its pupil pair.
    pub fn apply(&self, [u, v]: [f32; 2]) -> [f32; 2] {
        [
            self.yaw[0] + self.yaw[1] * u + self.yaw[2] * v + self.center[0],
            self.pitch[0] + self.pitch[1] * u + self.pitch[2] * v + self.center[1],
        ]
    }
}

/// Both eyes' maps, when gaze_source is "pupil" and both eyes have one; None otherwise (the eye server's gaze, as
/// always).
pub fn maps(settings: &Settings) -> Option<[EyeMap; 2]> {
    if settings.gaze_source != GazeSource::Pupil {
        return None;
    }
    let values = settings.pupil_map_values();
    let centers = settings.pupil_centers();
    let eye = |index: usize| -> Option<EyeMap> {
        let [a, b, c, d, e, f] = values[index];
        Some(EyeMap {
            yaw: [a?, b?, c?],
            pitch: [d?, e?, f?],
            center: centers[index].map(|center| center.unwrap_or(0.0)),
        })
    };
    Some([eye(0)?, eye(1)?])
}

/// Whether a mapped direction is inside the fit's range plus the margin.
fn in_range([yaw, pitch]: [f32; 2]) -> bool {
    yaw.abs() <= FIT_YAW_DEG + RANGE_MARGIN_DEG && pitch.abs() <= FIT_PITCH_DEG + RANGE_MARGIN_DEG
}

/// Each eye's pupil pair from the eye server's estimate_extra, and whether it is fresh (see FRESH_S).
#[derive(Clone, Debug, Default)]
pub struct Fresh {
    last: [Option<[f32; 2]>; 2],
    changed: [f64; 2],
}

impl Fresh {
    /// Take a sample's estimate_extra at `time`; each eye's pair while fresh, None otherwise (or not finite).
    pub fn update(&mut self, time: f64, extra: &[f32; 8]) -> [Option<[f32; 2]>; 2] {
        [0, 1].map(|eye| {
            let pair = [extra[2 * eye], extra[2 * eye + 1]];
            if !pair.iter().all(|value| value.is_finite()) {
                self.last[eye] = None;
                return None;
            }
            if self.last[eye] != Some(pair) {
                self.last[eye] = Some(pair);
                self.changed[eye] = time;
            }
            (time - self.changed[eye] <= FRESH_S).then_some(pair)
        })
    }
}

/// Where the sent gaze came from for a sample (with gaze_source "pupil" and a map).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum InUse {
    /// both eyes' pupils
    Pupil,
    /// the left eye's pupil alone (the right one missing), and the right's
    Left,
    Right,
    /// neither: the last gaze held (HOLD_S)
    Hold,
    /// neither for longer: the eye server's gaze, shifted to where the pupils last were
    Valve,
}

impl InUse {
    pub fn name(self) -> &'static str {
        match self {
            InUse::Pupil => "pupil",
            InUse::Left => "left",
            InUse::Right => "right",
            InUse::Hold => "hold",
            InUse::Valve => "valve",
        }
    }

    fn index(self) -> usize {
        self as usize
    }
}

fn add(a: [f32; 2], b: [f32; 2]) -> [f32; 2] {
    [a[0] + b[0], a[1] + b[1]]
}

fn sub(a: [f32; 2], b: [f32; 2]) -> [f32; 2] {
    [a[0] - b[0], a[1] - b[1]]
}

fn scaled(a: [f32; 2], by: f32) -> [f32; 2] {
    [a[0] * by, a[1] * by]
}

/// Move a smoothed value `by` of the way to `to` (the first one taken as it is).
fn ease(value: &mut Option<[f32; 2]>, to: [f32; 2], by: f32) {
    *value = Some(match *value {
        Some(old) => add(old, scaled(sub(to, old), by)),
        None => to,
    });
}

/// One sample's pupil gaze.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Step {
    /// The combined gaze (yaw, pitch in degrees).
    pub gaze: [f32; 2],
    pub in_use: InUse,
}

/// The pupil gaze between samples.
#[derive(Clone, Debug, Default)]
pub struct PupilGaze {
    fresh: Fresh,
    last_time: Option<f64>,
    // Left - right of the mapped eyes (degrees), smoothed over DIFF_TAU_S while both are used.
    diff: Option<[f32; 2]>,
    // The eye server's gaze - the pupil gaze (degrees), smoothed while a pupil is used.
    valve_minus: Option<[f32; 2]>,
    // When a pupil was last used, and the gaze it gave.
    last_good: Option<(f64, [f32; 2])>,
    // The gaze as last given, and where it came from.
    out: Option<[f32; 2]>,
    in_use: Option<InUse>,
    // Since when a change of source eases, and the difference it eases away.
    blend: Option<(f64, [f32; 2])>,
}

impl PupilGaze {
    /// Start over (a gap in the samples).
    pub fn reset(&mut self) {
        *self = Self::default();
    }

    /// One sample: its time, both eyes' maps, the eye server's estimate_extra, which eyes are open (not shut for the
    /// gaze), and the eye server's combined gaze as it would go out (degrees).
    pub fn step(&mut self, time: f64, maps: &[EyeMap; 2], extra: &[f32; 8], open: [bool; 2], valve: [f32; 2]) -> Step {
        let dt = self.last_time.map_or(0.0, |last| (time - last).max(0.0));
        self.last_time = Some(time);
        let by = (1.0 - (-dt / DIFF_TAU_S).exp()) as f32;
        let pairs = self.fresh.update(time, extra);
        let mut eyes: [Option<[f32; 2]>; 2] = [0, 1].map(|eye| {
            let pair = pairs[eye].filter(|_| open[eye])?;
            Some(maps[eye].apply(pair)).filter(|mapped| in_range(*mapped))
        });
        // Two eyes far apart: one of them is wrong (a pupil lost at the lens edge, the headset being put on), and which
        // can't be told, so neither is used
        if let [Some(left), Some(right)] = eyes {
            let apart = sub(left, right);
            if (apart[0] * apart[0] + apart[1] * apart[1]).sqrt() > EYES_APART_DEG {
                eyes = [None, None];
            }
        }
        let half_diff = scaled(self.diff.unwrap_or([0.0; 2]), 0.5);
        let (target, in_use) = match eyes {
            [Some(left), Some(right)] => {
                ease(&mut self.diff, sub(left, right), by);
                (scaled(add(left, right), 0.5), InUse::Pupil)
            }
            [Some(left), None] => (sub(left, half_diff), InUse::Left),
            [None, Some(right)] => (add(right, half_diff), InUse::Right),
            [None, None] => match self.last_good {
                Some((at, gaze)) if time - at <= HOLD_S => (gaze, InUse::Hold),
                _ => (sub(valve, self.valve_minus.unwrap_or([0.0; 2])), InUse::Valve),
            },
        };
        if matches!(in_use, InUse::Pupil | InUse::Left | InUse::Right) {
            self.last_good = Some((time, target));
            ease(&mut self.valve_minus, sub(valve, target), by);
        }
        // A change of source eases from the gaze as it was; the first sample (gaze_source just set to "pupil", or after
        // a gap) from the eye server's gaze, which went out until now
        let (before, out) = match (self.in_use, self.out) {
            (Some(before), Some(out)) => (before, out),
            _ => (InUse::Valve, valve),
        };
        if before != in_use {
            self.blend = Some((time, sub(out, target)));
        }
        let gaze = match self.blend {
            Some((since, offset)) => {
                let left = 1.0 - ((time - since) / BLEND_S) as f32;
                if left > 0.0 {
                    add(target, scaled(offset, left))
                } else {
                    self.blend = None;
                    target
                }
            }
            None => target,
        };
        self.out = Some(gaze);
        self.in_use = Some(in_use);
        Step { gaze, in_use }
    }
}

/// The journal's lines about where the gaze comes from: a line when it changes (at most one per LOG_CHANGE_S, counting
/// the changes in between), and the share of each source once per LOG_SHARES_S.
#[derive(Clone, Debug, Default)]
pub struct SourceLog {
    last: Option<InUse>,
    seen: bool,
    last_line: Option<f64>,
    pending: u32,
    counts: [u32; 5],
    since: Option<f64>,
}

impl SourceLog {
    /// Take a sample's source (None: the eye server's gaze as always, gaze_source "valve" or no map); the lines to log.
    pub fn note(&mut self, time: f64, in_use: Option<InUse>) -> Vec<String> {
        let mut lines = Vec::new();
        if !self.seen || in_use != self.last {
            let due = self.last_line.is_none_or(|last| time - last >= LOG_CHANGE_S || time < last);
            // Not the start with the eye server's gaze: that is how it always is
            if self.seen || in_use.is_some() {
                if due {
                    let now = in_use.map_or("valve (no pupil map, or gaze_source valve)", InUse::name);
                    let more = match self.pending {
                        0 => String::new(),
                        1 => " (1 more change since the last line)".into(),
                        n => format!(" ({n} more changes since the last line)"),
                    };
                    lines.push(format!("Gaze source: {now}{more}"));
                    self.last_line = Some(time);
                    self.pending = 0;
                } else {
                    self.pending += 1;
                }
            }
            self.seen = true;
            self.last = in_use;
        }
        if let Some(in_use) = in_use {
            self.counts[in_use.index()] += 1;
            let since = *self.since.get_or_insert(time);
            if time - since >= LOG_SHARES_S || time < since {
                lines.push(self.shares_line());
                self.counts = [0; 5];
                self.since = Some(time);
            }
        } else {
            self.counts = [0; 5];
            self.since = None;
        }
        lines
    }

    fn shares_line(&self) -> String {
        let total = self.counts.iter().sum::<u32>().max(1) as f32;
        let share = |source: InUse| 100.0 * self.counts[source.index()] as f32 / total;
        format!(
            "Gaze source over the last {LOG_SHARES_S:.0} s: pupil {:.1}%, one eye {:.1}% (L {:.1}%, R {:.1}%), \
             held {:.1}%, eye server {:.1}% of {} samples",
            share(InUse::Pupil),
            share(InUse::Left) + share(InUse::Right),
            share(InUse::Left),
            share(InUse::Right),
            share(InUse::Hold),
            share(InUse::Valve),
            total
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const DT: f64 = 1.0 / 90.0;

    // A made-up map: 200 degrees per unit, the pair (0.2, 0.1) straight ahead for both eyes, v up as pitch down.
    fn map() -> EyeMap {
        EyeMap {
            yaw: [-40.0, 200.0, 0.0],
            pitch: [20.0, 0.0, -200.0],
            center: [0.0, 0.0],
        }
    }

    // The pair that maps to (yaw, pitch).
    fn pair(yaw: f32, pitch: f32) -> [f32; 2] {
        [(yaw + 40.0) / 200.0, (20.0 - pitch) / 200.0]
    }

    fn extra(left: [f32; 2], right: [f32; 2]) -> [f32; 8] {
        [left[0], left[1], right[0], right[1], 0.0, 0.0, 0.0, 0.0]
    }

    fn settings_with_map() -> Settings {
        Settings {
            gaze_source: GazeSource::Pupil,
            pupil_yaw_left: Some(-40.0),
            pupil_yaw_u_left: Some(200.0),
            pupil_yaw_v_left: Some(0.0),
            pupil_pitch_left: Some(20.0),
            pupil_pitch_u_left: Some(0.0),
            pupil_pitch_v_left: Some(-200.0),
            pupil_yaw_right: Some(-40.0),
            pupil_yaw_u_right: Some(200.0),
            pupil_yaw_v_right: Some(0.0),
            pupil_pitch_right: Some(20.0),
            pupil_pitch_u_right: Some(0.0),
            pupil_pitch_v_right: Some(-200.0),
            ..Settings::default()
        }
    }

    #[test]
    fn a_map_is_used_only_with_gaze_source_pupil_and_both_eyes_mapped() {
        let settings = settings_with_map();
        assert_eq!(maps(&settings), Some([map(), map()]));
        assert_eq!(maps(&Settings { gaze_source: GazeSource::Valve, ..settings.clone() }), None);
        assert_eq!(maps(&Settings { pupil_pitch_v_right: None, ..settings.clone() }), None);
        assert_eq!(maps(&Settings::default()), None);
        // The centre is added after the map, null as 0
        let centred = Settings { pupil_center_yaw_left: Some(1.5), pupil_center_pitch_right: Some(-2.0), ..settings };
        let [left, right] = maps(&centred).unwrap();
        assert_eq!(left.apply(pair(3.0, 4.0)).map(|v| (v * 1000.0).round() / 1000.0), [4.5, 4.0]);
        assert_eq!(right.apply(pair(3.0, 4.0)).map(|v| (v * 1000.0).round() / 1000.0), [3.0, 2.0]);
    }

    #[test]
    fn a_pair_is_fresh_while_it_keeps_changing_and_for_fresh_s_after() {
        let mut fresh = Fresh::default();
        let mut time = 0.0;
        for i in 0..10 {
            let value = 0.2 + i as f32 * 0.001;
            assert!(fresh.update(time, &extra([value, 0.1], [value, 0.1])).iter().all(Option::is_some));
            time += DT;
        }
        // Repeated: fresh for FRESH_S after it last changed, then not
        let last = time - DT;
        let mut seen = Vec::new();
        for _ in 0..20 {
            seen.push((time - last, fresh.update(time, &extra([0.209, 0.1], [0.209, 0.1]))[0].is_some()));
            time += DT;
        }
        assert!(seen.iter().all(|(age, ok)| *ok == (*age <= FRESH_S)));
        // Not finite: not fresh
        assert_eq!(fresh.update(time, &extra([f32::NAN, 0.1], [0.3, 0.1]))[0], None);
    }

    // Runs a stream of (left, right) eye directions (None: the pair stops changing, as when the eye server loses the
    // eye) with both eyes open, and returns the steps.
    fn run(eyes: &[(Option<[f32; 2]>, Option<[f32; 2]>)], valve: [f32; 2]) -> Vec<Step> {
        let mut gaze = PupilGaze::default();
        let maps = [map(), map()];
        let mut held = [pair(0.0, 0.0); 2];
        let mut out = Vec::new();
        for (i, (left, right)) in eyes.iter().enumerate() {
            // A missing eye repeats its last pair; a present one changes by a hair each sample, as real pairs do
            let jitter = (i % 2) as f32 * 1e-6;
            if let Some(left) = left {
                held[0] = add(pair(left[0], left[1]), [jitter, 0.0]);
            }
            if let Some(right) = right {
                held[1] = add(pair(right[0], right[1]), [jitter, 0.0]);
            }
            out.push(gaze.step(i as f64 * DT, &maps, &extra(held[0], held[1]), [true; 2], valve));
        }
        out
    }

    fn largest_step(steps: &[Step]) -> f32 {
        steps
            .windows(2)
            .map(|pair| {
                let d = sub(pair[1].gaze, pair[0].gaze);
                (d[0] * d[0] + d[1] * d[1]).sqrt()
            })
            .fold(0.0, f32::max)
    }

    #[test]
    fn both_eyes_give_their_mean() {
        let steps = run(&[(Some([4.0, 2.0]), Some([2.0, 1.0])); 30], [9.0, 9.0]);
        assert!(steps.iter().all(|step| step.in_use == InUse::Pupil));
        let last = steps.last().unwrap().gaze;
        assert!((last[0] - 3.0).abs() < 1e-3 && (last[1] - 1.5).abs() < 1e-3, "{last:?}");
    }

    #[test]
    fn one_eye_missing_takes_the_other_plus_their_last_difference_without_a_jump() {
        // Left reads 2 deg further right than the right eye; then the right eye's pair freezes
        let mut eyes = vec![(Some([4.0, 0.0]), Some([2.0, 0.0])); 300];
        eyes.extend(vec![(Some([4.0, 0.0]), None); 120]);
        let steps = run(&eyes, [0.0, 0.0]);
        let after: Vec<&Step> = steps[300..].iter().collect();
        // Fresh for FRESH_S more, then the left eye alone
        assert!(after.iter().skip(12).all(|step| step.in_use == InUse::Left));
        let last = steps.last().unwrap().gaze;
        assert!((last[0] - 3.0).abs() < 0.05, "{last:?}");
        // From the eye server's gaze at the start to the pupils, and on, without a jump
        let (start, then) = (largest_step(&steps[..60]), largest_step(&steps[60..]));
        assert!(start < 3.0 / 20.0 && then < 0.05, "{start} {then}");
    }

    #[test]
    fn both_missing_hold_then_the_eye_server_shifted_and_back_without_a_jump() {
        // The eye server reads 5 deg right and 3 down of the pupils; both pairs freeze for a second, then come back
        let valve = [8.0, -2.0];
        let mut eyes = vec![(Some([3.0, 1.0]), Some([3.0, 1.0])); 300];
        eyes.extend(vec![(None, None); 90]);
        eyes.extend(vec![(Some([3.0, 1.0]), Some([3.0, 1.0])); 90]);
        let steps = run(&eyes, valve);
        let names: Vec<InUse> = steps.iter().map(|step| step.in_use).collect();
        // Fresh for FRESH_S, held for HOLD_S, then the eye server
        assert_eq!(names[300 + 5], InUse::Pupil);
        assert!(names[300 + 15..300 + 35].iter().all(|n| *n == InUse::Hold));
        assert!(names[300 + 45..390].iter().all(|n| *n == InUse::Valve));
        assert!(names[395..].iter().all(|n| *n == InUse::Pupil));
        // The eye server shifted by its learned difference lands where the pupils were
        let valve_gaze = steps[385].gaze;
        assert!((valve_gaze[0] - 3.0).abs() < 0.05 && (valve_gaze[1] - 1.0).abs() < 0.05, "{valve_gaze:?}");
        let (start, then) = (largest_step(&steps[..60]), largest_step(&steps[60..]));
        assert!(start < 6.0 / 20.0 && then < 0.05, "{start} {then}");
    }

    #[test]
    fn a_change_of_source_eases_from_where_the_gaze_was() {
        // No pupils at first (the eye server gives none): its gaze as it is; then the pupils, 6 deg away, ease in
        let mut eyes = vec![(None, None); 50];
        eyes.extend(vec![(Some([6.0, 0.0]), Some([6.0, 0.0])); 60]);
        let mut gaze = PupilGaze::default();
        let maps = [map(), map()];
        let mut steps = Vec::new();
        for (i, (left, _)) in eyes.iter().enumerate() {
            let p = left.map_or([f32::NAN; 2], |l| add(pair(l[0], l[1]), [(i % 2) as f32 * 1e-6, 0.0]));
            steps.push(gaze.step(i as f64 * DT + 1.0, &maps, &extra(p, p), [true; 2], [0.0, 0.0]));
        }
        assert_eq!(steps[30].in_use, InUse::Valve);
        assert!(steps[30].gaze[0].abs() < 1e-6);
        let first = steps.iter().position(|step| step.in_use == InUse::Pupil).unwrap();
        assert!(steps[first].gaze[0] < 1.0, "{:?}", steps[first]);
        let eased = first + (BLEND_S / DT).ceil() as usize + 1;
        assert!((steps[eased].gaze[0] - 6.0).abs() < 1e-3, "{:?}", steps[eased]);
        // At most a twentieth of the 6 deg a sample (BLEND_S is 22.5 samples)
        assert!(largest_step(&steps) < 6.0 / 20.0, "{}", largest_step(&steps));
    }

    #[test]
    fn a_shut_eye_or_one_out_of_range_is_not_used() {
        let maps = [map(), map()];
        let mut gaze = PupilGaze::default();
        let open = gaze.step(0.0, &maps, &extra(pair(2.0, 0.0), pair(2.0, 0.0)), [true, false], [0.0; 2]);
        assert_eq!(open.in_use, InUse::Left);
        let mut gaze = PupilGaze::default();
        let far = pair(FIT_YAW_DEG + RANGE_MARGIN_DEG + 1.0, 0.0);
        let step = gaze.step(0.0, &maps, &extra(far, pair(2.0, 0.0)), [true; 2], [0.0; 2]);
        assert_eq!(step.in_use, InUse::Right);
        let mut gaze = PupilGaze::default();
        let edge = pair(FIT_YAW_DEG + RANGE_MARGIN_DEG - 1.0, FIT_PITCH_DEG + RANGE_MARGIN_DEG - 1.0);
        assert_eq!(gaze.step(0.0, &maps, &extra(edge, edge), [true; 2], [0.0; 2]).in_use, InUse::Pupil);
        // Both in range but far apart: neither (one of them is wrong)
        let mut gaze = PupilGaze::default();
        let apart = extra(pair(-EYES_APART_DEG / 2.0 - 0.5, 0.0), pair(EYES_APART_DEG / 2.0 + 0.5, 0.0));
        assert_eq!(gaze.step(0.0, &maps, &apart, [true; 2], [0.0; 2]).in_use, InUse::Valve);
        let close = extra(pair(-EYES_APART_DEG / 2.0 + 0.5, 0.0), pair(EYES_APART_DEG / 2.0 - 0.5, 0.0));
        assert_eq!(gaze.step(DT, &maps, &close, [true; 2], [0.0; 2]).in_use, InUse::Pupil);
    }

    #[test]
    fn the_log_says_each_change_at_most_every_few_seconds_and_the_shares_each_minute() {
        let mut log = SourceLog::default();
        // The eye server's gaze from the start says nothing
        assert!(log.note(0.0, None).is_empty());
        assert_eq!(log.note(0.1, Some(InUse::Pupil)), vec!["Gaze source: pupil".to_owned()]);
        // Changes within LOG_CHANGE_S are counted, and told with the next line
        assert!(log.note(1.0, Some(InUse::Hold)).is_empty());
        assert!(log.note(1.2, Some(InUse::Pupil)).is_empty());
        let lines = log.note(6.0, Some(InUse::Left));
        assert_eq!(lines, vec!["Gaze source: left (2 more changes since the last line)".to_owned()]);
        // A minute of samples: the shares
        let mut lines = Vec::new();
        let mut time = 6.0;
        while lines.is_empty() || !lines.last().unwrap_or(&String::new()).contains("over the last") {
            time += DT;
            lines = log.note(time, Some(InUse::Pupil));
        }
        let shares = lines.last().unwrap();
        assert!(shares.starts_with("Gaze source over the last 60 s: pupil "), "{shares}");
        assert!(shares.contains("eye server 0.0%"), "{shares}");
    }
}
