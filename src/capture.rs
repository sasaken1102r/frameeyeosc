//! Averaging the raw gaze and openness for a moment while the user looks at a target (or keeps the
//! eyes shut), on the panel's request. The panel turns the averages into the gaze zero point and
//! gains and each eye's lid fit.

use crate::config::GazeCapture;
#[cfg(test)]
use crate::config::{CAPTURE_SECONDS, CAPTURE_SKIP};
use serde::Serialize;
use std::time::{Duration, Instant};

// Samples at the start are skipped while the eyes settle on the target (the request's `skip`), and the capture
// ends with the first sample its `seconds` after the first one (sample time). Without samples (tracking lost), it
// gives up this long on the clock after it should have ended.
const TIMEOUT_EXTRA: Duration = Duration::from_secs(3);
// The target that asks for the eyes-shut capture: every sample counts and the gaze is not used.
pub const CLOSED_TARGET: &str = "closed";
// A gaze sample counts when both eyes read at least this open (clearly open; a shut eye reads about
// 0.15-0.26), whatever gaze_hold_below is: an eye that reads less open looking down (0.45 for a face
// that reads 0.65 straight ahead) must still be measured there.
pub const OPEN_FLOOR: f32 = 0.3;

/// Whether a sample's gaze counts in a capture: both eyes open enough and at least one eye's gaze reliable. An eye the
/// eye server reads closing while the eye camera has seen it open for longer than a blink (`camera_open`: its eyelid
/// is the camera's, LidFrom::CameraOpen, or before the wear has a baseline for that, the camera has clearly seen its
/// pupil; see Sample::camera_open in main.rs) counts as open enough. The eye server can get stuck reading one eye at
/// 0.000 for a whole wear while that eye is open (2026-10-11: the right eye, the camera's eyelid about 0.75), its
/// combined gaze (the one measured) as steady as ever, and then every sample failed the floor and so did every eye fit;
/// after a night in VR it read both eyes 0.000 (issue 43), and every re-center timed out. Without such an eye, the
/// same as before.
pub fn usable(openness: [f32; 2], camera_open: [bool; 2], reliable: [bool; 2]) -> bool {
    let open = openness.map(|openness| openness >= OPEN_FLOOR);
    (0..2).all(|eye| open[eye] || camera_open[eye]) && reliable.contains(&true)
}

#[derive(Clone, Copy, Debug, PartialEq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum CaptureState {
    Running,
    Done,
}

/// What the status file reports about the latest capture. The gaze averages are on the -1..1 scale,
/// before the gaze zero point and gains are applied; openness is the Frame's, before any scale.
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct CaptureResult {
    pub id: i64,
    pub target: String,
    pub state: CaptureState,
    /// Averages and spread (standard deviation of both axes together); None without samples, and
    /// for the eyes-shut capture.
    pub x: Option<f32>,
    pub y: Option<f32>,
    pub spread: Option<f32>,
    /// Each eye's own sideways gaze averages (left, right); None like x.
    pub x_left: Option<f32>,
    pub x_right: Option<f32>,
    /// Each eye's average openness; None without samples.
    pub openness: Option<[f32; 2]>,
    /// Each eye that was camera-open (see usable) in most of the samples averaged: its openness above is the eye
    /// server's stuck reading, not the eye's, and the panel leaves that eye's lid fit as it was. False without samples.
    pub camera_open: [bool; 2],
    /// Samples that went into the average.
    pub samples: u32,
    /// Samples that came in after the skipped start, usable or not: the panel judges `samples`
    /// against this, as the tracker's rate varies (about 15 to 136 a second).
    pub received: u32,
    /// The tracker's rate over the capture (samples a second); None before two samples.
    pub rate: Option<f32>,
}

/// One capture in progress.
pub struct Capture {
    request: GazeCapture,
    closed: bool,
    started: Instant,
    first_time: Option<f64>,
    last_time: f64,
    // Every sample since the first, and those after the skipped start.
    all: u32,
    received: u32,
    sum: [f64; 2],
    sum_squares: [f64; 2],
    eye_x: [f64; 2],
    openness: [f64; 2],
    // Of the samples averaged, those with each eye camera-open.
    camera_open: [u32; 2],
    samples: u32,
}

impl Capture {
    pub fn new(request: GazeCapture) -> Self {
        Self {
            closed: request.target == CLOSED_TARGET,
            request,
            started: Instant::now(),
            first_time: None,
            last_time: 0.0,
            all: 0,
            received: 0,
            sum: [0.0; 2],
            sum_squares: [0.0; 2],
            eye_x: [0.0; 2],
            openness: [0.0; 2],
            camera_open: [0; 2],
            samples: 0,
        }
    }

    /// Take one sample's combined raw gaze, each eye's raw x and openness, and which eyes are camera-open (see
    /// usable); `usable` is false while the eyes are shut or the gaze is held (ignored by the eyes-shut capture).
    /// Returns true once the capture is over.
    pub fn add(
        &mut self,
        time: f64,
        gaze: [f32; 2],
        eye_x: [f32; 2],
        openness: [f32; 2],
        camera_open: [bool; 2],
        usable: bool,
    ) -> bool {
        let elapsed = time - *self.first_time.get_or_insert(time);
        if elapsed >= self.request.seconds {
            return true;
        }
        self.all += 1;
        self.last_time = time;
        if elapsed >= self.request.skip {
            self.received += 1;
        }
        let finite = gaze.iter().chain(&eye_x).chain(&openness).all(|value| value.is_finite());
        if elapsed >= self.request.skip && (usable || self.closed) && finite {
            for ((sum, squares), value) in self.sum.iter_mut().zip(&mut self.sum_squares).zip(gaze) {
                let value = f64::from(value);
                *sum += value;
                *squares += value * value;
            }
            for (sum, value) in self.eye_x.iter_mut().zip(eye_x) {
                *sum += f64::from(value);
            }
            for (sum, value) in self.openness.iter_mut().zip(openness) {
                *sum += f64::from(value);
            }
            for (count, camera_open) in self.camera_open.iter_mut().zip(camera_open) {
                *count += u32::from(camera_open);
            }
            self.samples += 1;
        }
        false
    }

    /// Whether it has waited too long for samples.
    pub fn timed_out(&self) -> bool {
        self.started.elapsed() >= Duration::from_secs_f64(self.request.seconds) + TIMEOUT_EXTRA
    }

    pub fn result(&self, state: CaptureState) -> CaptureResult {
        let count = f64::from(self.samples);
        let mean = self.sum.map(|sum| sum / count);
        let variance: f64 = (0..2)
            .map(|axis| (self.sum_squares[axis] / count - mean[axis] * mean[axis]).max(0.0))
            .sum();
        let known = state == CaptureState::Done && self.samples > 0;
        let gaze = known && !self.closed;
        CaptureResult {
            id: self.request.id,
            target: self.request.target.clone(),
            state,
            x: gaze.then_some(mean[0] as f32),
            y: gaze.then_some(mean[1] as f32),
            spread: gaze.then_some(variance.sqrt() as f32),
            x_left: gaze.then_some((self.eye_x[0] / count) as f32),
            x_right: gaze.then_some((self.eye_x[1] / count) as f32),
            openness: known.then(|| self.openness.map(|sum| (sum / count) as f32)),
            camera_open: self.camera_open.map(|camera_open| known && 2 * camera_open > self.samples),
            samples: if state == CaptureState::Done { self.samples } else { 0 },
            received: if state == CaptureState::Done { self.received } else { 0 },
            rate: self.rate(),
        }
    }

    /// Samples a second between the first and the last one.
    fn rate(&self) -> Option<f32> {
        let span = self.last_time - self.first_time?;
        (self.all > 1 && span > 0.0).then(|| (f64::from(self.all - 1) / span) as f32)
    }
}

impl CaptureResult {
    /// One line for the journal, so a calibration can be read back later.
    pub fn log_line(&self) -> String {
        let rate = self.rate.map_or_else(String::new, |rate| format!(" at {rate:.0} Hz"));
        let Some([left, right]) = self.openness else {
            return format!(
                "Gaze capture {} ({}): no usable samples ({} received{rate})",
                self.id, self.target, self.received
            );
        };
        let gaze = match (self.x, self.y, self.spread, self.x_left, self.x_right) {
            (Some(x), Some(y), Some(spread), Some(left), Some(right)) => {
                format!("x {x:+.4}, y {y:+.4}, spread {spread:.4}, eye x L {left:+.4} R {right:+.4}, ")
            }
            _ => String::new(),
        };
        // An eye whose openness is the eye server's stuck reading, while the camera saw it open
        let camera_open = match self.camera_open {
            [true, true] => " (L and R open on the eye camera)",
            [true, false] => " (L open on the eye camera)",
            [false, true] => " (R open on the eye camera)",
            [false, false] => "",
        };
        format!(
            "Gaze capture {} ({}): {gaze}openness L {left:.3} R {right:.3}{camera_open} from {} of {} samples{rate}",
            self.id, self.target, self.samples, self.received
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn capture() -> Capture {
        Capture::new(GazeCapture {
            id: 7,
            target: "up".into(),
            seconds: CAPTURE_SECONDS,
            skip: CAPTURE_SKIP,
        })
    }

    #[test]
    fn averages_only_the_settled_open_eyed_samples() {
        let mut capture = capture();
        let mut over = false;
        for i in 0..=180 {
            let time = 100.0 + f64::from(i) / 90.0;
            // Wild gaze while settling, alternating around (0.1, 0.3) after, and a blink in between.
            let settled = time - 100.0 >= CAPTURE_SKIP;
            let gaze = if !settled {
                [0.9, -0.9]
            } else if i % 2 == 0 {
                [0.11, 0.31]
            } else {
                [0.09, 0.29]
            };
            let blinking = (90..100).contains(&i);
            let (gaze, openness) = if blinking { ([-1.0, -1.0], [0.1, 0.1]) } else { (gaze, [0.9, 0.8]) };
            over = capture.add(time, gaze, [gaze[0] + 0.02, gaze[0] - 0.02], openness, [false; 2], !blinking);
            if over {
                break;
            }
        }
        assert!(over);
        let result = capture.result(CaptureState::Done);
        assert_eq!((result.id, result.target.as_str(), result.state), (7, "up", CaptureState::Done));
        // 1.5 s of samples at 90 Hz, less the 10 blinking ones.
        assert!((124..=126).contains(&result.samples), "{}", result.samples);
        assert!((result.x.unwrap() - 0.10).abs() < 1e-3 && (result.y.unwrap() - 0.30).abs() < 1e-3);
        let [left, right] = result.openness.unwrap();
        assert!((left - 0.9).abs() < 1e-4 && (right - 0.8).abs() < 1e-4);
        assert!((result.x_left.unwrap() - 0.12).abs() < 1e-3 && (result.x_right.unwrap() - 0.08).abs() < 1e-3);
        let spread = result.spread.unwrap();
        // Each axis is off by 0.01 either way.
        assert!((spread - 0.0002_f32.sqrt()).abs() < 1e-4, "{spread}");
        // Every sample after the skipped start came in (the blinking ones too), at 90 a second.
        assert!((134..=136).contains(&result.received), "{}", result.received);
        assert!((result.rate.unwrap() - 90.0).abs() < 0.5);
        assert_eq!(result.camera_open, [false; 2]);
        let line = result.log_line();
        let expected = format!("from {} of {} samples at 90 Hz", result.samples, result.received);
        assert!(line.starts_with("Gaze capture 7 (up): x +0.") && line.ends_with(&expected), "{line}");
    }

    #[test]
    fn eyes_that_read_less_open_are_still_measured() {
        // Whatever gaze_hold_below is: 0.45 looking down (0.65 straight ahead) counts.
        assert!(usable([0.45, 0.45], [false; 2], [true; 2]));
        assert!(usable([0.3, 0.9], [false; 2], [true, false]));
        // A shut or closing eye does not, nor a sample with no reliable eye.
        assert!(!usable([0.25, 0.6], [false; 2], [true; 2]));
        assert!(!usable([0.8, 0.8], [false; 2], [false; 2]));
        // A whole capture from such a face: every open sample counts.
        let mut capture = capture();
        for i in 0..=180 {
            let openness = [0.45, 0.42];
            let usable = usable(openness, [false; 2], [true; 2]);
            if capture.add(f64::from(i) / 90.0, [0.0, -0.33], [0.0, 0.0], openness, [false; 2], usable) {
                break;
            }
        }
        assert!(capture.result(CaptureState::Done).samples >= 134);
    }

    #[test]
    fn a_slow_tracker_is_measured_at_its_own_rate() {
        // 15 samples a second (a Frame not streaming): 2 s skipping the first 0.3 s gives about 26 samples,
        // all of them usable, and the result says so, so the panel can accept them.
        let mut capture = Capture::new(GazeCapture {
            id: 3,
            target: "center".into(),
            seconds: 2.0,
            skip: 0.3,
        });
        for i in 0..100 {
            if capture.add(50.0 + f64::from(i) / 15.0, [0.01, 0.02], [0.03, -0.01], [1.0, 0.95], [false; 2], true) {
                break;
            }
        }
        let result = capture.result(CaptureState::Done);
        assert_eq!((result.samples, result.received), (25, 25));
        assert!((result.rate.unwrap() - 15.0).abs() < 0.01);
        assert!(result.log_line().ends_with("from 25 of 25 samples at 15 Hz"), "{}", result.log_line());
    }

    #[test]
    fn nothing_usable_reports_no_average() {
        let mut capture = capture();
        for i in 0..200 {
            if capture.add(f64::from(i) / 90.0, [0.0, 0.0], [0.0, 0.0], [0.2, 0.2], [false; 2], false) {
                break;
            }
        }
        let result = capture.result(CaptureState::Done);
        assert_eq!((result.samples, result.x, result.spread, result.openness), (0, None, None, None));
        assert_eq!(result.camera_open, [false; 2]);
        assert_eq!(result.log_line(), "Gaze capture 7 (up): no usable samples (135 received at 90 Hz)");
        let running = capture.result(CaptureState::Running);
        assert_eq!((running.state, running.x, running.samples), (CaptureState::Running, None, 0));
    }

    #[test]
    fn the_eyes_shut_capture_keeps_every_sample_and_reports_only_openness() {
        let mut capture = Capture::new(GazeCapture {
            id: 9,
            target: CLOSED_TARGET.into(),
            seconds: 3.0,
            skip: 0.5,
        });
        let mut over_at = None;
        for i in 0..400 {
            // The gaze is held (not usable) all along, as it is with the eyes shut.
            if capture.add(f64::from(i) / 90.0, [0.5, -0.5], [0.5, 0.5], [0.15, 0.26], [false; 2], false) {
                over_at = Some(i);
                break;
            }
        }
        // As long as asked (3 s at 90 Hz), less the first half second
        assert_eq!(over_at, Some(270));
        let result = capture.result(CaptureState::Done);
        assert!((224..=226).contains(&result.samples), "{}", result.samples);
        assert_eq!((result.x, result.y, result.spread, result.x_left), (None, None, None, None));
        let [left, right] = result.openness.unwrap();
        assert!((left - 0.15).abs() < 1e-4 && (right - 0.26).abs() < 1e-4);
        assert_eq!(
            result.log_line(),
            format!("Gaze capture 9 (closed): openness L 0.150 R 0.260 from {0} of {0} samples at 90 Hz", result.samples)
        );
    }

    #[test]
    fn an_eye_the_camera_sees_open_stands_in_for_the_eye_server() {
        // The eye server stuck at 0.000 for the right eye, which the camera sees open: it counts, the left eye open.
        assert!(usable([0.95, 0.0], [false, true], [true, false]));
        assert!(usable([0.0, 0.95], [true, false], [true; 2]));
        // Both eyes stuck there and seen open by the camera (issue 43) count too.
        assert!(usable([0.0, 0.0], [true; 2], [true; 2]));
        // Still not without a reliable eye, nor for an eye the camera does not see open.
        assert!(!usable([0.95, 0.0], [false, true], [false; 2]));
        assert!(!usable([0.0, 0.0], [true, false], [true; 2]));
        assert!(!usable([0.95, 0.0], [true, false], [true; 2]));
        // Camera-open on an eye that is open anyway changes nothing.
        assert_eq!(usable([0.95, 0.9], [true, false], [true; 2]), usable([0.95, 0.9], [false; 2], [true; 2]));
        // A whole capture of 2026-10-11's wear: every sample counts and the right eye is reported camera-open, except
        // through a blink (the camera sees it closed, so it is the eye server's again, and the left eye shuts too).
        let mut capture = capture();
        for i in 0..=180 {
            let blinking = (90..100).contains(&i);
            let (openness, camera_open) = if blinking { ([0.1, 0.0], [false; 2]) } else { ([0.98, 0.0], [false, true]) };
            let usable = usable(openness, camera_open, [true, false]);
            if capture.add(f64::from(i) / 90.0, [0.01, -0.02], [0.0, 0.0], openness, camera_open, usable) {
                break;
            }
        }
        let result = capture.result(CaptureState::Done);
        assert!((124..=126).contains(&result.samples), "{}", result.samples);
        assert_eq!(result.camera_open, [false, true]);
        let [left, right] = result.openness.unwrap();
        assert!((left - 0.98).abs() < 1e-4 && right == 0.0);
        let line = result.log_line();
        assert!(line.contains("openness L 0.980 R 0.000 (R open on the eye camera) from "), "{line}");
        // Still running: nothing to report yet.
        assert_eq!(capture.result(CaptureState::Running).camera_open, [false; 2]);
    }

    #[test]
    fn the_eyes_shut_capture_reports_camera_open_eyes_by_the_majority() {
        // Every sample counts here (usable or not), and an eye is reported camera-open when it was in most of them.
        let mut capture = Capture::new(GazeCapture {
            id: 10,
            target: CLOSED_TARGET.into(),
            seconds: 3.0,
            skip: 0.5,
        });
        for i in 0..400 {
            let camera_open = [i % 3 == 0, i % 3 != 0];
            if capture.add(f64::from(i) / 90.0, [0.0, 0.0], [0.0, 0.0], [0.15, 0.0], camera_open, false) {
                break;
            }
        }
        let result = capture.result(CaptureState::Done);
        assert_eq!(result.camera_open, [false, true]);
        assert!(result.log_line().contains("openness L 0.150 R 0.000 (R open on the eye camera) from "));
    }
}
