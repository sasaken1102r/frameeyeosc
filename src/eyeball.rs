//! Gaze from the eye cameras' pupil ellipse, with each eye's centre of rotation kept as part of the state (gaze_source
//! "eyeball"; a test). The pupil gaze (pupil.rs) maps where the pupil is in a camera's picture, so a headset pushed up
//! or down a few millimetres moves it as if the eye had turned. Here each eye is a ball turning about its centre: the
//! pupil sits R_PUPIL in front of the centre along the gaze, and its outline is a circle, which the camera sees as an
//! ellipse whose flattening and long axis tell how the circle is tilted against the camera's ray to it. A turn moves
//! the pupil and tilts the ellipse; a shift of the headset moves the pupil but leaves the tilt, so the two can be told
//! apart and the centre follows the shift. Per eye an extended Kalman filter follows
//!   x = [yaw, pitch, yaw rate, pitch rate, cx, cy, cz]
//! yaw / pitch: the eye's direction in the head frame (+x right, +y up, -z forward); c: its centre in that eye
//! camera's frame (metres). Each frame of eyecam-rec (the live file's glint and shape parts) gives the pupil centre
//! (undistorted to the camera's normalised coordinates) and the ellipse as (1 - b/a)(cos 2phi, sin 2phi), phi its long
//! axis. The ellipse is used only while it can be trusted (the lids well clear of the pupil, nearly all of its rim
//! seen, and close to what the state expects); without it a pupil move is read as a turn and the centre is held. The
//! angles go through each eye's affine map from the eye fit (eyeball_map.rs), and both eyes are combined as the pupil
//! gaze combines them (pupil.rs: one eye, held, the eye server's; every change eased).
//!
//! The cameras' geometry comes from the headset's factory eye-camera calibration (/persist/eyetracking.json, only
//! read): each camera's focal lengths, centre and lens distortion, and how it is turned in the head. It is required:
//! the rays to the pupil, the ellipse's tilt against them and the head-frame angles all go by it, and a made-up
//! geometry would read part of every shift as a turn. Without it the source acts as "valve" (and says why).
//!
//! Model values are population anatomy (R_PUPIL) or were chosen offline on the developer's own recordings (2026-11-14
//! slip test 3: the noise levels, gates and the first pupil distance D0; tests 1, 2 and 4 and the re-seat tests were
//! not used for any choice).

use crate::eyecam_live::{CamShape, CamSlip, Live};
use crate::pupil::EyeMap;
use std::f64::consts::PI;
use std::path::Path;

/// The factory calibration file.
pub const CALIBRATION_PATH: &str = "/persist/eyetracking.json";

/// From the eye's centre of rotation to the (apparent) pupil centre, metres (anatomy: about 10.5 mm).
pub const R_PUPIL: f64 = 0.0105;
/// The first guess of the camera-to-pupil distance, metres (an effective value: the fitted geometry is not the
/// physical one, as the ratio of R_PUPIL to the depth acts as a gain that the eye fit's map takes up).
const D0: f64 = 0.012;
/// Noise of the pupil centre (normalised units, about 1.6 px) and of the ellipse's two numbers.
const SIG_POS: f64 = 0.006;
const SIG_ELL: f64 = 0.05;
/// The angles' acceleration noise (rad^2/s^3).
const Q_ACC: f64 = 1.0;
/// A frame whose pupil centre moved faster than this since the last frame with a pupil (normalised units a second: at 90
/// frames a second 0.02 a frame, about 5 px or a degree of turn; a speed, so slower cameras, as low as 15 frames a
/// second, need the same speed and not the same step) is a saccade: its acceleration noise is SACCADE_GAIN times Q_ACC,
/// so the filter follows the jump instead of easing into it. Chosen on the replays for the hop latency (the eyeball's lag behind the raw pupil to 90%
/// of a hop: a median 0.16 s without it, 0.07 s with it; p90 0.32 / 0.26 s), with the error kept: the slip tests' medians
/// within 0.1 degree, the re-seat tests' better (2026-11-15; a higher gain or a lower step is faster still but jumps more).
/// The ellipse's own gate sees the larger uncertainty too, so a turn's new ellipse is taken at once.
pub const SACCADE_SPEED: f64 = 1.8;
pub const SACCADE_GAIN: f64 = 100.0;
/// After a saccade frame, the eye counts as moving fast this long (s; pupil.rs leaves the eye server's difference alone
/// meanwhile).
pub const SACCADE_HOLD_S: f64 = 0.2;
/// The centre's random walk across the camera's axis and along it (m/sqrt(s)): the headset slipping.
const SIG_C: f64 = 0.0015;
const SIG_CZ: f64 = 0.0003;
/// A frame further than this (squared Mahalanobis distance) from what the state expects is refused.
const REJECT: f64 = 400.0;
/// The ellipse is left out of a frame beyond this (its own squared Mahalanobis distance)...
const ELL_GATE: f64 = 9.0;
/// ...and once it has been left out this many frames in a row it is believed: the angles are taken from it again;
/// within SACCADE_REACQ_S of a saccade frame after ELL_REACQ_MOVING frames already. A landing whose first frames went by
/// the pupil only (the ellipse blurred or gated as the eye stops) otherwise sat a fifth of a second on a pupil-only
/// angle (the model's pupil-place gain is small: a 16 px hop reads as about 3 degrees) before the new ellipse was
/// taken: on the re-seat replays with the outline (2026-11-15) the eyeball's lag behind the raw pupil to 90% of hops of
/// 60 px or more, p90 0.18 s -> 0.06 s (3 frames: a re-seat run's right eye turned the wrong way at two corners).
const ELL_REACQ: u32 = 20;
const ELL_REACQ_MOVING: u32 = 6;
const SACCADE_REACQ_S: f64 = 0.3;
/// Refused this many frames in a row: the angles are taken afresh from the next ellipse (the centre kept). If that
/// happens again within RESTART_S, or the angles sit at the limit (ANG_MAX_DEG), the centre kept is the wrong one: the
/// filter starts over from the frame. On the headset (2026-11-15, the eye log at 14:51 and its eye video) an eye
/// whose centre had gone 4-8 mm off was refused 20-41% of a minute with dozens of re-takes, each keeping that centre.
const REACQUIRE: u32 = 30;
const RESTART_S: f64 = 3.0;
/// Plausible camera-to-centre distances (m), angles (degrees), and the pupil must face the camera at least this much
/// (the cosine between the gaze and the ray to the pupil).
const DEPTH_MIN: f64 = 0.012;
const DEPTH_MAX: f64 = 0.060;
/// (65 degrees since 2026-11-15: one wearer's right eye reads 10-20 degrees down looking ahead, so looking down 15
/// degrees put it at the old 50 and every frame there was refused.)
const ANG_MAX_DEG: f64 = 65.0;
const FACING: f64 = -0.3;
/// The angle rates fade over this long without measurements (s).
const RATE_TAU: f64 = 0.15;
/// Time steps are taken as at most this long (s).
const DT_MAX: f64 = 0.1;
/// The ellipse is used only with the lids at least this far apart at the pupil (eyecam-rec's aperture, iris radii: a
/// lid near the pupil cuts its outline) and with at least PARTIAL_VIS_MIN of its rim seen. Below VIS_MIN its rim is
/// partly hidden (lashes, the eye's corner, at the far corners of the view): such an ellipse updates the state like any
/// other but never makes the filter take its angles from it (the re-takes), nor counts towards one (a run of them
/// must not let the next whole rim re-take at once), as a cut one can point the wrong way. Up to
/// 2026-11-15 the ellipse needed VIS_MIN: at the corners one eye then went by the pupil only for whole dots and stayed
/// near where it was (the re-seat replays: the left eye 17-21 degrees off at (-20, +15), on the headset the eyes then
/// "apart" and the eye server's gaze for the whole dot). With 0.88 (0.85-0.9 tried) both re-seat runs' corners improved.
const APERTURE_MIN: f64 = 1.3;
const VIS_MIN: f64 = 0.98;
const PARTIAL_VIS_MIN: f64 = 0.88;
/// An eye's direction is given while its pupil was seen at most this long ago (s, the eye server's time)...
pub const EYE_FRESH_S: f64 = 0.15;
/// ...and the filter took a frame (an update with the pupil, with or without its ellipse) at most this long ago: one
/// that keeps refusing its frames (lids over the rim at a corner, a squint) stands still at its last direction, which
/// is not where the eye looks; the other eye, the hold and the eye server's gaze take over then (pupil.rs).
pub const ACCEPTED_FRESH_S: f64 = 0.2;
/// An eye's directions count for the eye fit's captures (the map and the centre, eyeball_map.rs) once its filter has
/// updated with the ellipse this many frames since it started or last re-took its angles (about a second at 90 frames
/// a second)...
pub const SETTLED_ELLIPSE_FRAMES: u32 = 90;
/// ...and its centre is this sure across the camera's axis (m, the larger standard deviation of the two): before that
/// the centre is still settling, and a map fitted then holds its error (on the headset at 23:55 on 2026-11-14 an eye fit
/// 30 s after a start took eyes at the 2 mm cap with 2399 ellipse frames, and their centre moved 3 mm in depth by the
/// next re-center). Over the slip and re-seat replays a settled eye's sd is 0.3-0.45 mm (median), below 0.5 mm 1-15 s
/// after a start and a median 0.1-0.3 s after a re-seat (p90 about 1 s).
pub const SETTLED_CENTRE_SD: f64 = 0.0005;
/// While the ellipse is not used, the centre is held, and its uncertainty would grow without end: it is kept at most
/// this (m, each axis) or what it was before, if more, so the next ellipse cannot throw the centre millimetres in one
/// frame.
pub const CENTRE_SD_CAP: f64 = 0.002;
/// While an eye holds still, the direction given out is not the filter's own angles but the angles that put the pupil
/// where the filter has it about a centre held from when the eye came to rest. With the pupil still, the filter keeps
/// trading its centre against its angles on the ellipse alone, whose shape drifts with the pupil's size and the fit by
/// more than its place does: on the headset (2026-11-15, the gaze check after 955deec) the given gaze wandered up to a
/// degree or two over a dot's 3.5 s, 2-3 times Valve's spread on some dots, while the pupils stood within a few tenths
/// of a pixel; on the replays each such wander is the centre moving with the angles against it (correlation -1). The
/// filter itself is left alone (its centre and the hop latency are as before); the hold only changes what is given.
/// "Still": the pupil within HOLD_RADIUS (normalised units, about 1.6 px) of where it came to rest, for HOLD_AFTER_S,
/// without a saccade frame. Meanwhile the held centre follows the filter's with the time constant HOLD_CENTRE_TAU_S
/// (so a slow slip still comes through), and once the eye moves on or the pupil is lost the difference from the
/// filter's angles is eased out over HOLD_EASE_S, or dropped at once on a saccade frame. The held direction is never
/// more than HOLD_MAX_DEG from the filter's own: the cost of the hold is that a landing whose centre is still settling
/// is followed only at the hold's pace (on the model's own frames, where the filter ends exact, a turn to (-18, -10)
/// would otherwise be given 2.2 degrees off at first and 0.6 after 3 s).
/// Chosen on the replays (2026-11-15; slip tests 1-4 and both re-seat runs, all of them looked at): the spread of the
/// two eyes' mean over still stretches -39% (median 0.50 -> 0.31 deg, p90 0.93 -> 0.80), the hop latency the same
/// (median 0.067 s, p90 0.272 s behind the raw pupil), the slip and re-seat medians within about 0.1 degree (run 2
/// 0.5 better, run 4 0.11 worse). Holding from 0.1 s cut the spread further but cost run 3 1.7 degrees (it holds a
/// landing that is still settling); moving the filter's own centre less while still cut the spread a little less and
/// cost a frame or two of latency (the tighter filter refuses the next ellipse).
pub const HOLD_RADIUS: f64 = 0.006;
pub const HOLD_AFTER_S: f64 = 0.3;
pub const HOLD_CENTRE_TAU_S: f64 = 2.0;
pub const HOLD_EASE_S: f64 = 0.1;
pub const HOLD_MAX_DEG: f64 = 1.5;
/// The pictures are this many rows high when the calibration does not say.
const DEFAULT_ROWS: f64 = 400.0;

type V3 = [f64; 3];
const N: usize = 7;
type V7 = [f64; N];
type M7 = [[f64; N]; N];

fn dot(a: V3, b: V3) -> f64 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}

fn norm(a: V3) -> f64 {
    dot(a, a).sqrt()
}

fn scale(a: V3, by: f64) -> V3 {
    [a[0] * by, a[1] * by, a[2] * by]
}

fn add(a: V3, b: V3) -> V3 {
    [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
}

fn sub(a: V3, b: V3) -> V3 {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}

fn unit(a: V3) -> V3 {
    scale(a, 1.0 / norm(a))
}

fn cross(a: V3, b: V3) -> V3 {
    [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
}

/// The unit ray through a point in normalised camera coordinates.
fn ray(pos: [f64; 2]) -> V3 {
    unit([pos[0], pos[1], 1.0])
}

/// The direction of a gaze (yaw, pitch in radians) in the head frame: +x right, +y up, -z forward.
fn gaze_head(yaw: f64, pitch: f64) -> V3 {
    [yaw.sin() * pitch.cos(), pitch.sin(), -yaw.cos() * pitch.cos()]
}

/// One eye camera from the factory calibration.
#[derive(Clone, Debug, PartialEq)]
pub struct Camera {
    pub fx: f64,
    pub fy: f64,
    pub cx: f64,
    pub cy: f64,
    /// Lens distortion k1, k2, p1, p2, k3 (radial and tangential).
    pub dist: [f64; 5],
    /// The camera's x, y and z axes in the head frame (the columns of the head-from-camera rotation).
    pub axes: [V3; 3],
    /// The picture's height in rows (for turning a picture that was turned upright back to the sensor's), and its width.
    pub rows: f64,
    pub cols: f64,
}

impl Camera {
    /// A head-frame direction in the camera's frame.
    fn to_camera(&self, v: V3) -> V3 {
        [dot(self.axes[0], v), dot(self.axes[1], v), dot(self.axes[2], v)]
    }

    /// A camera-frame direction in the head frame.
    fn to_head(&self, v: V3) -> V3 {
        let [x, y, z] = self.axes;
        add(add(scale(x, v[0]), scale(y, v[1])), scale(z, v[2]))
    }

    /// A pixel of the sensor's picture in undistorted normalised coordinates: the distortion model inverted by
    /// fixed-point iteration (five steps; a pixel's error is then far below the pupil's own noise).
    pub fn undistort(&self, u: f64, v: f64) -> [f64; 2] {
        let [k1, k2, p1, p2, k3] = self.dist;
        let (x0, y0) = ((u - self.cx) / self.fx, (v - self.cy) / self.fy);
        let (mut x, mut y) = (x0, y0);
        for _ in 0..5 {
            let r2 = x * x + y * y;
            let radial = 1.0 + ((k3 * r2 + k2) * r2 + k1) * r2;
            if radial <= 0.0 {
                return [x0, y0];
            }
            let dx = 2.0 * p1 * x * y + p2 * (r2 + 2.0 * x * x);
            let dy = p1 * (r2 + 2.0 * y * y) + 2.0 * p2 * x * y;
            x = (x0 - dx) / radial;
            y = (y0 - dy) / radial;
        }
        [x, y]
    }

    /// The head's forward direction in the camera's frame.
    fn forward(&self) -> V3 {
        self.to_camera([0.0, 0.0, -1.0])
    }

    /// The gaze direction (yaw, pitch) of a camera-frame direction.
    fn angles_of(&self, g: V3) -> (f64, f64) {
        let h = self.to_head(g);
        (h[0].atan2(-h[2]), h[1].clamp(-1.0, 1.0).asin())
    }
}

/// Both eye cameras (0 = left eye, 1 = right, anatomical).
#[derive(Clone, Debug, PartialEq)]
pub struct Calibration {
    pub eyes: [Camera; 2],
}

/// The factory calibration from its JSON (`etCamCalib.left` / `.right`: fx, fy, cx, cy, dist[5], ny, headFromCamera
/// plus_x / plus_z). Nothing else in the file is read or kept.
pub fn parse_calibration(text: &str) -> Result<Calibration, String> {
    let root: serde_json::Value = serde_json::from_str(text).map_err(|e| format!("not JSON: {e}"))?;
    let cal = root.get("etCamCalib").ok_or("no etCamCalib")?;
    let camera = |side: &str| -> Result<Camera, String> {
        let c = cal.get(side).ok_or_else(|| format!("no etCamCalib.{side}"))?;
        let number = |key: &str| {
            c.get(key)
                .and_then(serde_json::Value::as_f64)
                .filter(|v| v.is_finite())
                .ok_or_else(|| format!("etCamCalib.{side}.{key} missing or not a number"))
        };
        let vector = |value: Option<&serde_json::Value>, name: &str, len: usize| -> Result<Vec<f64>, String> {
            let list = value.and_then(serde_json::Value::as_array).ok_or_else(|| format!("etCamCalib.{side}.{name} missing"))?;
            let numbers: Option<Vec<f64>> = list.iter().map(|v| v.as_f64().filter(|v| v.is_finite())).collect();
            numbers.filter(|n| n.len() == len).ok_or_else(|| format!("etCamCalib.{side}.{name}: not {len} numbers"))
        };
        let (fx, fy) = (number("fx")?, number("fy")?);
        if fx <= 0.0 || fy <= 0.0 {
            return Err(format!("etCamCalib.{side}: focal lengths must be positive"));
        }
        let dist = vector(c.get("dist"), "dist", 5)?;
        let pose = c.get("headFromCamera");
        let plus_x = vector(pose.and_then(|p| p.get("plus_x")), "headFromCamera.plus_x", 3)?;
        let plus_z = vector(pose.and_then(|p| p.get("plus_z")), "headFromCamera.plus_z", 3)?;
        let x: V3 = [plus_x[0], plus_x[1], plus_x[2]];
        let z: V3 = [plus_z[0], plus_z[1], plus_z[2]];
        if norm(x) < 1e-6 || norm(cross(x, z)) < 1e-6 {
            return Err(format!("etCamCalib.{side}.headFromCamera: not a rotation"));
        }
        // The axes made exactly orthonormal: x as given, z made square to it, y = z x x
        let x = unit(x);
        let z = unit(sub(z, scale(x, dot(x, z))));
        let y = cross(z, x);
        let rows = c.get("ny").and_then(serde_json::Value::as_f64).filter(|v| *v > 1.0).unwrap_or(DEFAULT_ROWS);
        let cols = c.get("nx").and_then(serde_json::Value::as_f64).filter(|v| *v > 1.0).unwrap_or(DEFAULT_ROWS);
        Ok(Camera {
            fx,
            fy,
            cx: number("cx")?,
            cy: number("cy")?,
            dist: [dist[0], dist[1], dist[2], dist[3], dist[4]],
            axes: [x, y, z],
            rows,
            cols,
        })
    };
    Ok(Calibration { eyes: [camera("left")?, camera("right")?] })
}

/// The factory calibration from its file.
pub fn load_calibration(path: &Path) -> Result<Calibration, String> {
    let text = std::fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?;
    parse_calibration(&text).map_err(|e| format!("{}: {e}", path.display()))
}

/// One eye frame as eyecam-rec gives it: the pupil centre (px, in its picture; None without a pupil), the ellipse's
/// semi-axes, long axis (degrees) and rim share seen, the lids' aperture (iris radii), and whether the picture was
/// turned upright from the sensor's. NaN for what it lacks.
#[derive(Clone, Copy, Debug)]
pub struct RawFrame {
    /// The frame's time (s, the camera's clock).
    pub time: f64,
    pub pupil: Option<[f64; 2]>,
    pub a: f64,
    pub b: f64,
    pub angle_deg: f64,
    pub vis: f64,
    pub aperture: f64,
    pub flipped: bool,
}

/// What the filter takes from a frame.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Obs {
    pub time: f64,
    /// The pupil centre, undistorted normalised camera coordinates.
    pub pos: Option<[f64; 2]>,
    /// The ellipse as (1 - b/a)(cos 2phi, sin 2phi), while it can be trusted...
    pub ell: Option<[f64; 2]>,
    /// ...and whether its rim was partly hidden (below VIS_MIN: it never makes the filter re-take its angles).
    pub partial: bool,
}

/// A frame in the sensor's picture, undistorted, with the ellipse kept only where the lids are clear of the pupil and
/// enough of its rim is seen (an unknown aperture or rim share does not stop it).
pub fn observe(camera: &Camera, frame: &RawFrame) -> Obs {
    // (a centre outside the picture is no pupil: the distortion can't be undone there, and the iteration would fold it
    // back to some point inside)
    let inside = |p: &[f64; 2]| (0.0..=camera.cols).contains(&p[0]) && (0.0..=camera.rows).contains(&p[1]);
    let Some([px, py]) = frame.pupil.filter(|p| p.iter().all(|v| v.is_finite()) && inside(p)) else {
        return Obs { time: frame.time, pos: None, ell: None, partial: false };
    };
    // A picture turned upright is flipped top to bottom back to the sensor's (and so is the ellipse's axis)
    let (py, angle) = if frame.flipped { (camera.rows - 1.0 - py, -frame.angle_deg) } else { (py, frame.angle_deg) };
    let pos = camera.undistort(px, py);
    let (a, b) = (frame.a.max(frame.b), frame.a.min(frame.b));
    let lids_clear = !frame.aperture.is_finite() || frame.aperture >= APERTURE_MIN;
    let rim_seen = !frame.vis.is_finite() || frame.vis >= PARTIAL_VIS_MIN;
    let partial = frame.vis.is_finite() && frame.vis < VIS_MIN;
    let ell = (angle.is_finite() && a > 0.0 && b.is_finite() && lids_clear && rim_seen).then(|| {
        let t = angle.to_radians();
        let flat = 1.0 - b / a;
        [flat * (2.0 * t).cos(), flat * (2.0 * t).sin()]
    });
    Obs { time: frame.time, pos: Some(pos), ell, partial }
}

/// What the state expects to see: the pupil centre, and with `ell` the ellipse; the count of numbers.
fn predict(x: &V7, camera: &Camera, ell: bool) -> ([f64; 4], usize) {
    let g = camera.to_camera(gaze_head(x[0], x[1]));
    let mut p = add([x[4], x[5], x[6]], scale(g, R_PUPIL));
    p[2] = p[2].max(1e-4);
    let z = [p[0] / p[2], p[1] / p[2]];
    if !ell {
        return ([z[0], z[1], 0.0, 0.0], 2);
    }
    // The circle is tilted against the ray to it by the angle between them; the image's short axis runs along the
    // gaze's own image direction at the pupil, the long axis square to it
    let cos_t = dot(g, unit(p)).abs();
    let along = [g[0] - z[0] * g[2], g[1] - z[1] * g[2]];
    let phi = along[1].atan2(along[0]) + PI / 2.0;
    let flat = 1.0 - cos_t;
    ([z[0], z[1], flat * (2.0 * phi).cos(), flat * (2.0 * phi).sin()], 4)
}

/// The prediction and its Jacobian (forward differences; the rates do not enter the observation).
fn jacobian(x: &V7, camera: &Camera, ell: bool) -> ([f64; 4], [[f64; N]; 4], usize) {
    let (z0, n) = predict(x, camera, ell);
    let mut j = [[0.0; N]; 4];
    for i in [0, 1, 4, 5, 6] {
        let step = if i < 4 { 1e-5 } else { 1e-6 };
        let mut x2 = *x;
        x2[i] += step;
        let (z, _) = predict(&x2, camera, ell);
        for (r, row) in j.iter_mut().enumerate().take(n) {
            row[i] = (z[r] - z0[r]) / step;
        }
    }
    (z0, j, n)
}

/// The angles that put the pupil at `target` (normalised) for an eye whose centre is at `centre`, from `start`
/// (radians; Newton's method on the prediction, three steps).
fn angles_about(camera: &Camera, target: [f64; 4], start: [f64; 2], centre: V3) -> [f64; 2] {
    let mut x = [start[0], start[1], 0.0, 0.0, centre[0], centre[1], centre[2]];
    for _ in 0..3 {
        let (z, _) = predict(&x, camera, false);
        let mut j = [[0.0; 2]; 2];
        for i in 0..2 {
            let mut x2 = x;
            x2[i] += 1e-5;
            let (z2, _) = predict(&x2, camera, false);
            for r in 0..2 {
                j[r][i] = (z2[r] - z[r]) / 1e-5;
            }
        }
        let det = j[0][0] * j[1][1] - j[0][1] * j[1][0];
        if det.abs() < 1e-12 {
            break;
        }
        let (r0, r1) = (target[0] - z[0], target[1] - z[1]);
        x[0] += (j[1][1] * r0 - j[0][1] * r1) / det;
        x[1] += (-j[1][0] * r0 + j[0][0] * r1) / det;
    }
    [x[0], x[1]]
}

/// The two circle normals an ellipse allows at a pupil seen at `pos` (both facing the camera).
fn normals(pos: [f64; 2], e: [f64; 2]) -> [V3; 2] {
    let tilt = (1.0 - e[0].hypot(e[1])).clamp(-1.0, 1.0).acos();
    let short = 0.5 * e[1].atan2(e[0]) - PI / 2.0;
    let d = ray(pos);
    [1.0, -1.0].map(|s| {
        let m = scale([short.cos(), short.sin(), 0.0], s);
        let m = unit(sub(m, scale(d, dot(m, d))));
        add(scale(d, -tilt.cos()), scale(m, tilt.sin()))
    })
}

/// Of the two normals, the one nearer the head's forward direction.
fn normal_ahead(pos: [f64; 2], e: [f64; 2], camera: &Camera) -> V3 {
    let [a, b] = normals(pos, e);
    let forward = camera.forward();
    if dot(b, forward) > dot(a, forward) { b } else { a }
}

/// Of the two normals, the one that puts the pupil nearest the sphere about `centre` along its seen ray.
fn normal_on_sphere(pos: [f64; 2], e: [f64; 2], centre: V3) -> V3 {
    let d = ray(pos);
    let miss = |g: V3| {
        let pupil = add(centre, scale(g, R_PUPIL));
        norm(sub(scale(d, dot(pupil, d)), pupil))
    };
    let [a, b] = normals(pos, e);
    if miss(b) < miss(a) { b } else { a }
}

/// The depth, angles and facing in range.
fn plausible(x: &V7, camera: &Camera) -> bool {
    let centre = [x[4], x[5], x[6]];
    let depth = norm(centre);
    let limit = ANG_MAX_DEG.to_radians();
    if !(DEPTH_MIN..=DEPTH_MAX).contains(&depth) || x[0].abs() > limit || x[1].abs() > limit {
        return false;
    }
    let g = camera.to_camera(gaze_head(x[0], x[1]));
    let pupil = add(centre, scale(g, R_PUPIL));
    dot(g, unit(pupil)) < FACING
}

fn diagonal(values: V7) -> M7 {
    let mut m = [[0.0; N]; N];
    for i in 0..N {
        m[i][i] = values[i];
    }
    m
}

fn mul(a: &M7, b: &M7) -> M7 {
    let mut m = [[0.0; N]; N];
    for i in 0..N {
        for k in 0..N {
            if a[i][k] != 0.0 {
                for j in 0..N {
                    m[i][j] += a[i][k] * b[k][j];
                }
            }
        }
    }
    m
}

fn transpose(a: &M7) -> M7 {
    let mut m = [[0.0; N]; N];
    for i in 0..N {
        for j in 0..N {
            m[i][j] = a[j][i];
        }
    }
    m
}

/// The inverse of the top-left n x n of `a` (Gauss-Jordan with partial pivoting); None when it is singular.
fn inverse(a: &[[f64; 4]; 4], n: usize) -> Option<[[f64; 4]; 4]> {
    let mut m = *a;
    let mut inv = [[0.0; 4]; 4];
    for (i, row) in inv.iter_mut().enumerate().take(n) {
        row[i] = 1.0;
    }
    for col in 0..n {
        let pivot = (col..n).max_by(|&p, &q| m[p][col].abs().total_cmp(&m[q][col].abs()))?;
        if m[pivot][col].abs() < 1e-300 {
            return None;
        }
        m.swap(col, pivot);
        inv.swap(col, pivot);
        let f = 1.0 / m[col][col];
        for k in 0..n {
            m[col][k] *= f;
            inv[col][k] *= f;
        }
        for row in 0..n {
            if row != col {
                let g = m[row][col];
                if g != 0.0 {
                    for k in 0..n {
                        m[row][k] -= g * m[col][k];
                        inv[row][k] -= g * inv[col][k];
                    }
                }
            }
        }
    }
    Some(inv)
}

/// The rows `rows` of J times P times their transpose, plus `noise` on the diagonal.
fn innovation(j: &[[f64; N]; 4], rows: std::ops::Range<usize>, p: &M7, noise: &[f64; 4]) -> [[f64; 4]; 4] {
    let mut s = [[0.0; 4]; 4];
    let base = rows.start;
    for r in rows.clone() {
        let mut jp = [0.0; N];
        for (k, value) in jp.iter_mut().enumerate() {
            *value = (0..N).map(|i| j[r][i] * p[i][k]).sum();
        }
        for c in rows.clone() {
            s[r - base][c - base] = (0..N).map(|k| jp[k] * j[c][k]).sum::<f64>();
        }
        s[r - base][r - base] += noise[r];
    }
    s
}

/// y' S^-1 y over the first n numbers.
fn distance(y: &[f64; 4], s_inv: &[[f64; 4]; 4], n: usize) -> f64 {
    (0..n).map(|r| y[r] * (0..n).map(|c| s_inv[r][c] * y[c]).sum::<f64>()).sum()
}

/// What a frame did to the filter.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Update {
    /// The filter started from this frame.
    Start,
    /// No pupil: the prediction.
    NoPupil,
    /// Too far from what was expected: the prediction.
    Rejected,
    /// The update would leave the plausible range: the prediction.
    Implausible,
    /// Updated with the pupil and its ellipse...
    Ellipse,
    /// ...or with the pupil only (the centre held).
    PupilOnly,
}

impl Update {
    pub fn name(self) -> &'static str {
        match self {
            Update::Start => "start",
            Update::NoPupil => "nopupil",
            Update::Rejected => "rejected",
            Update::Implausible => "implausible",
            Update::Ellipse => "ellipse",
            Update::PupilOnly => "pupil",
        }
    }
}

/// The filter's state after a frame.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Estimate {
    /// The eye's direction in the head frame (radians; yaw + right, pitch + up).
    pub yaw: f64,
    pub pitch: f64,
    /// Its centre of rotation in the camera's frame (m).
    pub centre: V3,
    pub update: Update,
}

/// One eye's filter.
#[derive(Clone, Debug, Default)]
pub struct EyeFilter {
    x: Option<V7>,
    p: M7,
    last: f64,
    // Frames refused in a row, and frames whose ellipse was left out in a row
    misses: u32,
    ell_off: u32,
    /// How often the angles were taken afresh from the ellipse.
    pub retakes: u32,
    /// Updates with the ellipse since the start or the last re-take.
    ellipse_frames: u32,
    // The pupil centre of the last frame that had one (normalised), for the saccade boost
    last_pos: Option<([f64; 2], f64)>,
    /// The last frame taken was a saccade (its acceleration noise raised).
    pub saccade: bool,
    // When the last saccade frame was (the camera's time)
    saccade_at: Option<f64>,
    reacquired_at: Option<f64>,
    // Holding still (HOLD_RADIUS): where the pupil came to rest and since when, whether it is still, the centre the
    // direction is given about meanwhile, the difference from the filter's own angles, the time of the last frame and
    // the direction given (radians)
    rest: Option<([f64; 2], f64)>,
    still: bool,
    held_centre: Option<V3>,
    held: [f64; 2],
    held_time: f64,
    given: Option<[f64; 2]>,
}

/// The centre's uncertainty while the ellipse is not used: no standard deviation above the larger of CENTRE_SD_CAP and
/// what it was before (`before`, variances), the covariances scaled with it.
fn cap_centre(p: &mut M7, before: [f64; 3]) {
    for (k, i) in (4..7).enumerate() {
        let limit = before[k].max(CENTRE_SD_CAP * CENTRE_SD_CAP);
        if p[i][i] > limit {
            let f = (limit / p[i][i]).sqrt();
            for j in 0..N {
                p[i][j] *= f;
                p[j][i] *= f;
            }
        }
    }
}

/// Whether an eye's filter gives its direction: the pupil seen within EYE_FRESH_S and a frame taken within
/// ACCEPTED_FRESH_S (times in whatever clock the caller uses).
#[derive(Clone, Copy, Debug, Default)]
pub struct Freshness {
    seen: Option<f64>,
    accepted: Option<f64>,
}

impl Freshness {
    /// A frame at `time`, and what the filter made of it.
    pub fn note(&mut self, time: f64, o: &Obs, estimate: Option<&Estimate>) {
        if o.pos.is_some() {
            self.seen = Some(time);
        }
        if estimate.is_some_and(|e| matches!(e.update, Update::Start | Update::Ellipse | Update::PupilOnly)) {
            self.accepted = Some(time);
        }
    }

    pub fn fresh(&self, time: f64) -> bool {
        self.seen.is_some_and(|seen| time - seen <= EYE_FRESH_S)
            && self.accepted.is_some_and(|accepted| time - accepted <= ACCEPTED_FRESH_S)
    }
}

impl EyeFilter {
    /// Whether its centre has settled enough for the eye fit (SETTLED_ELLIPSE_FRAMES and SETTLED_CENTRE_SD).
    pub fn settled(&self) -> bool {
        self.x.is_some()
            && self.ellipse_frames >= SETTLED_ELLIPSE_FRAMES
            && self.p[4][4].max(self.p[5][5]) < SETTLED_CENTRE_SD * SETTLED_CENTRE_SD
    }

    /// The centre (mm) and its standard deviations (mm), once started.
    pub fn centre_mm(&self) -> Option<(V3, V3)> {
        let x = self.x?;
        Some((scale([x[4], x[5], x[6]], 1e3), [4, 5, 6].map(|i| self.p[i][i].max(0.0).sqrt() * 1e3)))
    }
    /// Start over.
    pub fn reset(&mut self) {
        *self = Self::default();
    }

    /// Whether it has started.
    #[cfg(test)]
    pub fn started(&self) -> bool {
        self.x.is_some()
    }

    /// The direction given out (yaw, pitch in radians: the filter's own angles, or while the eye holds still the held
    /// ones, see HOLD_RADIUS); None until started.
    pub fn direction(&self) -> Option<[f64; 2]> {
        self.given
    }

    /// Take one frame; the state after it, its angles the direction given out (None until a frame with a pupil and its
    /// ellipse starts it).
    pub fn step(&mut self, camera: &Camera, o: &Obs) -> Option<Estimate> {
        let retakes = self.retakes;
        let mut e = self.filter(camera, o)?;
        let x = self.x?;
        let restarted = e.update == Update::Start || self.retakes != retakes;
        // Where the pupil came to rest: anew after a saccade frame, a move beyond HOLD_RADIUS, a frame without the pupil
        // (after a blink the lids may still be clearing and the centre moving: the hold waits HOLD_AFTER_S again) and
        // a start or re-take (the centre was just thrown)
        match o.pos {
            Some(p) if !restarted => match self.rest {
                Some((at, _)) if (p[0] - at[0]).hypot(p[1] - at[1]) <= HOLD_RADIUS && !self.saccade => {}
                _ => self.rest = Some((p, o.time)),
            },
            Some(p) => self.rest = Some((p, o.time)),
            None => self.rest = None,
        }
        self.still = o.pos.is_some()
            && !self.saccade
            && self.rest.is_some_and(|(_, since)| o.time - since >= HOLD_AFTER_S);
        let dt = (o.time - self.held_time).clamp(0.0, DT_MAX);
        self.held_time = o.time;
        let centre = [x[4], x[5], x[6]];
        if self.still && !restarted {
            let before = *self.held_centre.get_or_insert(centre);
            let held = add(before, scale(sub(centre, before), 1.0 - (-dt / HOLD_CENTRE_TAU_S).exp()));
            self.held_centre = Some(held);
            let (target, _) = predict(&x, camera, false);
            let [yaw, pitch] = angles_about(camera, target, [x[0], x[1]], held);
            self.held = [yaw - x[0], pitch - x[1]];
            let size = self.held[0].hypot(self.held[1]);
            let most = HOLD_MAX_DEG.to_radians();
            if size > most {
                self.held = self.held.map(|d| d * most / size);
            }
        } else {
            self.held_centre = None;
            let keep = if self.saccade || restarted { 0.0 } else { (-dt / HOLD_EASE_S).exp() };
            self.held = self.held.map(|d| d * keep);
        }
        // (a solve that went wrong gives nothing to hold; NaN times the ease would stay NaN)
        if !self.held.iter().all(|d| d.is_finite()) {
            self.held = [0.0; 2];
        }
        e.yaw = x[0] + self.held[0];
        e.pitch = x[1] + self.held[1];
        self.given = Some([e.yaw, e.pitch]);
        Some(e)
    }

    fn estimate(&self, update: Update) -> Option<Estimate> {
        let x = self.x?;
        Some(Estimate { yaw: x[0], pitch: x[1], centre: [x[4], x[5], x[6]], update })
    }

    /// A frame refused: counted towards REACQUIRE only with a whole rim (or no rim share known), so a run of partly
    /// hidden ones cannot make the first whole-rim frame re-take at once.
    fn miss(&mut self, partial: bool) {
        if !partial {
            self.misses += 1;
        }
    }

    /// The filter's own step: one frame; its state after it (None until a frame with a pupil and its ellipse starts it).
    fn filter(&mut self, camera: &Camera, o: &Obs) -> Option<Estimate> {
        let deg = |d: f64| d.to_radians();
        let Some(mut x) = self.x else {
            // Started from the first frame with an ellipse: the gaze from its normal nearer straight ahead, the pupil
            // D0 along its ray
            let (Some(pos), Some(ell)) = (o.pos, o.ell) else {
                return None;
            };
            let g = normal_ahead(pos, ell, camera);
            let (yaw, pitch) = camera.angles_of(g);
            let c = sub(scale(ray(pos), D0), scale(g, R_PUPIL));
            self.x = Some([yaw, pitch, 0.0, 0.0, c[0], c[1], c[2]]);
            self.p = diagonal([deg(10.0).powi(2), deg(10.0).powi(2), 1.0, 1.0, 0.005f64.powi(2), 0.005f64.powi(2), 0.008f64.powi(2)]);
            self.last = o.time;
            self.misses = 0;
            self.ellipse_frames = 0;
            return self.estimate(Update::Start);
        };
        let dt = (o.time - self.last).clamp(0.0, DT_MAX);
        self.last = o.time;
        let fade = (-dt / RATE_TAU).exp();
        let mut f = diagonal([1.0; N]);
        f[0][2] = RATE_TAU * (1.0 - fade);
        f[1][3] = RATE_TAU * (1.0 - fade);
        f[2][2] = fade;
        f[3][3] = fade;
        // Refused for a while again soon after a re-take (RESTART_S), or at the angle limit: start over from this frame
        if o.pos.is_some()
            && o.ell.is_some()
            && self.misses >= REACQUIRE
            && !o.partial
        {
            let limit = (ANG_MAX_DEG - 1.0).to_radians();
            let at_limit = x[0].abs() >= limit || x[1].abs() >= limit;
            let again = self.reacquired_at.is_some_and(|at| o.time - at <= RESTART_S);
            if at_limit || again {
                self.x = None;
                self.reacquired_at = None;
                self.retakes += 1;
                return self.filter(camera, o);
            }
        }
        // Refused for a while: the angles taken afresh from this frame's ellipse (the normal that puts the pupil on
        // the sphere about the centre kept), their uncertainty reset
        if let (Some(pos), Some(ell)) = (o.pos, o.ell)
            && self.misses >= REACQUIRE
            && !o.partial
        {
            self.reacquired_at = Some(o.time);
            let g = normal_on_sphere(pos, ell, [x[4], x[5], x[6]]);
            (x[0], x[1]) = camera.angles_of(g);
            x[2] = 0.0;
            x[3] = 0.0;
            for i in 0..4 {
                for k in 0..N {
                    self.p[i][k] = 0.0;
                    self.p[k][i] = 0.0;
                }
            }
            self.p[0][0] = deg(10.0).powi(2);
            self.p[1][1] = deg(10.0).powi(2);
            self.p[2][2] = 1.0;
            self.p[3][3] = 1.0;
            self.misses = 0;
            self.retakes += 1;
            self.ellipse_frames = 0;
        }
        // The prediction
        let mut xp = [0.0; N];
        for (i, value) in xp.iter_mut().enumerate() {
            *value = (0..N).map(|k| f[i][k] * x[k]).sum();
        }
        let limit = ANG_MAX_DEG.to_radians();
        xp[0] = xp[0].clamp(-limit, limit);
        xp[1] = xp[1].clamp(-limit, limit);
        // A frame whose pupil moved faster than SACCADE_SPEED since the last one with a pupil is a saccade: the angles'
        // acceleration noise is SACCADE_GAIN times higher for it, so the filter follows the jump instead of easing in
        // (confirming it with the next frame cut the jumps by a third but lost half the latency gain: not done)
        let fast = o.pos.zip(self.last_pos).is_some_and(|(p, (q, at))| {
            let interval = (o.time - at).clamp(1.0 / 120.0, DT_MAX);
            (p[0] - q[0]).hypot(p[1] - q[1]) > SACCADE_SPEED * interval
        });
        let saccade = fast;
        self.saccade = saccade;
        if saccade {
            self.saccade_at = Some(o.time);
        }
        if let Some(p) = o.pos {
            self.last_pos = Some((p, o.time));
        }
        let q_acc = if saccade { Q_ACC * SACCADE_GAIN } else { Q_ACC };
        let mut q = [[0.0; N]; N];
        for i in 0..2 {
            q[i][i] = q_acc * dt.powi(3) / 3.0;
            q[i][i + 2] = q_acc * dt.powi(2) / 2.0;
            q[i + 2][i] = q_acc * dt.powi(2) / 2.0;
            q[i + 2][i + 2] = q_acc * dt;
        }
        q[4][4] = SIG_C * SIG_C * dt;
        q[5][5] = SIG_C * SIG_C * dt;
        q[6][6] = SIG_CZ * SIG_CZ * dt;
        // (the centre's uncertainty before this frame, for the cap while the ellipse is not used)
        let before = [self.p[4][4], self.p[5][5], self.p[6][6]];
        let mut pp = mul(&mul(&f, &self.p), &transpose(&f));
        for i in 0..N {
            for k in 0..N {
                pp[i][k] += q[i][k];
            }
        }
        let Some(pos) = o.pos else {
            self.x = Some(xp);
            self.p = pp;
            cap_centre(&mut self.p, before);
            return self.estimate(Update::NoPupil);
        };
        let noise = [SIG_POS * SIG_POS, SIG_POS * SIG_POS, SIG_ELL * SIG_ELL, SIG_ELL * SIG_ELL];
        let mut with_ell = o.ell.is_some();
        let (mut zp, mut j, _) = jacobian(&xp, camera, with_ell);
        if let Some(ell) = o.ell {
            // The ellipse on its own against what is expected: left out when it disagrees, believed after it has
            // disagreed for ELL_REACQ whole-rim frames (ELL_REACQ_MOVING just after a saccade; a partly hidden one never
            // counts; the angles from its normal nearer straight ahead, the pupil kept on its seen ray at the distance
            // it was)
            let se = innovation(&j, 2..4, &pp, &noise);
            let ye = [ell[0] - zp[2], ell[1] - zp[3], 0.0, 0.0];
            let far = inverse(&se, 2).is_none_or(|inv| distance(&ye, &inv, 2) > ELL_GATE);
            if far {
                let moving = self.saccade_at.is_some_and(|at| o.time - at <= SACCADE_REACQ_S);
                let need = if moving { ELL_REACQ_MOVING } else { ELL_REACQ };
                // (only whole rims count towards believing it: a partly hidden one neither adds nor clears)
                if !o.partial {
                    self.ell_off += 1;
                }
                if self.ell_off >= need && !o.partial {
                    let g_old = camera.to_camera(gaze_head(xp[0], xp[1]));
                    let pupil_old = add([xp[4], xp[5], xp[6]], scale(g_old, R_PUPIL));
                    let g = normal_ahead(pos, ell, camera);
                    (xp[0], xp[1]) = camera.angles_of(g);
                    xp[2] = 0.0;
                    xp[3] = 0.0;
                    let c = sub(scale(ray(pos), norm(pupil_old)), scale(g, R_PUPIL));
                    (xp[4], xp[5], xp[6]) = (c[0], c[1], c[2]);
                    pp = diagonal([deg(5.0).powi(2), deg(5.0).powi(2), 1.0, 1.0, 0.003f64.powi(2), 0.003f64.powi(2), 0.004f64.powi(2)]);
                    (zp, j, _) = jacobian(&xp, camera, true);
                    self.ell_off = 0;
                    self.retakes += 1;
                    self.ellipse_frames = 0;
                } else {
                    with_ell = false;
                }
            } else {
                self.ell_off = 0;
            }
        }
        let n = if with_ell { 4 } else { 2 };
        let mut y = [0.0; 4];
        y[0] = pos[0] - zp[0];
        y[1] = pos[1] - zp[1];
        if let (true, Some(ell)) = (with_ell, o.ell) {
            y[2] = ell[0] - zp[2];
            y[3] = ell[1] - zp[3];
        }
        let s = innovation(&j, 0..n, &pp, &noise);
        let Some(s_inv) = inverse(&s, n) else {
            self.x = Some(xp);
            self.p = pp;
            self.miss(o.partial);
            cap_centre(&mut self.p, before);
            return self.estimate(Update::Rejected);
        };
        if distance(&y, &s_inv, n) > REJECT {
            self.x = Some(xp);
            self.p = pp;
            self.miss(o.partial);
            cap_centre(&mut self.p, before);
            return self.estimate(Update::Rejected);
        }
        // The gain K = P J' S^-1; without the ellipse the centre is held (its rows zero)
        let mut pjt = [[0.0; 4]; N];
        for i in 0..N {
            for r in 0..n {
                pjt[i][r] = (0..N).map(|k| pp[i][k] * j[r][k]).sum();
            }
        }
        let mut gain = [[0.0; 4]; N];
        for i in 0..N {
            if !with_ell && i >= 4 {
                continue;
            }
            for c in 0..n {
                gain[i][c] = (0..n).map(|r| pjt[i][r] * s_inv[r][c]).sum();
            }
        }
        let mut xn = xp;
        for (i, value) in xn.iter_mut().enumerate() {
            *value += (0..n).map(|r| gain[i][r] * y[r]).sum::<f64>();
        }
        if !plausible(&xn, camera) {
            self.x = Some(xp);
            self.p = pp;
            self.miss(o.partial);
            cap_centre(&mut self.p, before);
            return self.estimate(Update::Implausible);
        }
        // P = (I - K J) P (I - K J)' + K R K' (the Joseph form), kept symmetric
        let mut ikj = diagonal([1.0; N]);
        for i in 0..N {
            for k in 0..N {
                ikj[i][k] -= (0..n).map(|r| gain[i][r] * j[r][k]).sum::<f64>();
            }
        }
        let mut p = mul(&mul(&ikj, &pp), &transpose(&ikj));
        for i in 0..N {
            for k in 0..N {
                p[i][k] += (0..n).map(|r| gain[i][r] * noise[r] * gain[k][r]).sum::<f64>();
            }
        }
        for i in 0..N {
            for k in 0..i {
                let mean = (p[i][k] + p[k][i]) / 2.0;
                p[i][k] = mean;
                p[k][i] = mean;
            }
        }
        self.x = Some(xn);
        self.p = p;
        self.misses = 0;
        if with_ell {
            self.ellipse_frames += 1;
        } else {
            cap_centre(&mut self.p, before);
        }
        self.estimate(if with_ell { Update::Ellipse } else { Update::PupilOnly })
    }
}

/// One eye's frame from the live file's glint part (the pupil centre and the frame's time) and shape part.
pub fn raw_frame(slip: &CamSlip, shape: &CamShape) -> RawFrame {
    RawFrame {
        time: slip.time_ns as f64 / 1e9,
        pupil: slip.pupil.map(|p| p.map(f64::from)),
        a: f64::from(shape.a),
        b: f64::from(shape.b),
        angle_deg: f64::from(shape.angle_deg),
        vis: f64::from(shape.vis),
        aperture: f64::from(shape.aperture),
        flipped: shape.flipped,
    }
}

/// Both eyes' filters between samples, fed from eyecam-rec's live values (each new frame once).
#[derive(Clone, Debug, Default)]
pub struct EyeballGaze {
    calibration: Option<Calibration>,
    /// Each eye's map from the eye fit (eyeball_map.rs); None until it has one.
    pub maps: Option<[EyeMap; 2]>,
    filters: [EyeFilter; 2],
    // The frame last taken per eye (its time, ns), and when its pupil was last seen and a frame last taken (the eye
    // server's time)
    frame_ns: [Option<u64>; 2],
    fresh: [Freshness; 2],
    // When an eye's frame was last a saccade, and when each eye's filter last used its ellipse (the eye server's time)
    saccade_at: Option<f64>,
    ellipse_at: [Option<f64>; 2],
    last_time: Option<f64>,
    used: bool,
    /// Frames taken per eye, how they went (Update order), and the time they took (ns), for the journal.
    pub frames: [u64; 2],
    pub updates: [[u64; 6]; 2],
    pub busy_ns: u64,
}

impl EyeballGaze {
    /// Set the cameras' calibration (None: the source can't run).
    pub fn set_calibration(&mut self, calibration: Option<Calibration>) {
        self.calibration = calibration;
        self.restart();
    }

    pub fn has_calibration(&self) -> bool {
        self.calibration.is_some()
    }

    /// The filters start over (a gap in the samples, the source turned off).
    pub fn restart(&mut self) {
        for filter in &mut self.filters {
            filter.reset();
        }
        self.frame_ns = [None; 2];
        self.fresh = [Freshness::default(); 2];
        self.saccade_at = None;
        self.last_time = None;
        self.used = false;
    }

    /// Not in use: start over if it was.
    pub fn idle(&mut self) {
        if self.used {
            self.restart();
        }
    }

    /// Take a sample at `time` (the eye server's, s) with eyecam-rec's values as read for it: each eye's new frame
    /// into its filter. Each eye's direction (yaw, pitch in degrees, as its filter gives it, EyeFilter::direction,
    /// before the map) while it is fresh (Freshness: its pupil seen within EYE_FRESH_S and a frame taken within
    /// ACCEPTED_FRESH_S); None for both without the calibration.
    pub fn step(&mut self, time: f64, live: Option<&Live>) -> [Option<[f32; 2]>; 2] {
        if self.calibration.is_none() {
            return [None; 2];
        }
        if self.last_time.is_some_and(|last| time < last) {
            self.restart();
        }
        let Some(calibration) = &self.calibration else {
            return [None; 2];
        };
        self.last_time = Some(time);
        self.used = true;
        let started = std::time::Instant::now();
        let mut out = [None; 2];
        for eye in 0..2 {
            let camera = &calibration.eyes[eye];
            if let Some((slip, shape)) = live.and_then(|live| live.slip[eye].zip(live.shape[eye]))
                && self.frame_ns[eye] != Some(slip.time_ns)
            {
                self.frame_ns[eye] = Some(slip.time_ns);
                let o = observe(camera, &raw_frame(&slip, &shape));
                let estimate = self.filters[eye].step(camera, &o);
                self.frames[eye] += 1;
                if let Some(estimate) = estimate {
                    self.updates[eye][estimate.update as usize] += 1;
                    if matches!(estimate.update, Update::Start | Update::Ellipse) {
                        self.ellipse_at[eye] = Some(time);
                    }
                }
                self.fresh[eye].note(time, &o, estimate.as_ref());
                if self.filters[eye].saccade {
                    self.saccade_at = Some(time);
                }
            }
            if self.fresh[eye].fresh(time) && let Some([yaw, pitch]) = self.filters[eye].direction() {
                out[eye] = Some([yaw.to_degrees() as f32, pitch.to_degrees() as f32]);
            }
        }
        self.busy_ns += started.elapsed().as_nanos() as u64;
        out
    }

    /// Whether an eye moved fast (a saccade, EyeFilter::saccade) within SACCADE_HOLD_S of `time` (the eye server's):
    /// the eye server's difference is not learnt then (pupil.rs step_eyes).
    pub fn moving(&self, time: f64) -> bool {
        self.saccade_at.is_some_and(|at| time - at <= SACCADE_HOLD_S)
    }

    /// Whether each eye's filter used its ellipse within ACCEPTED_FRESH_S of `time` (the eye server's): an eye going by
    /// its pupil alone (no usable ellipse) is not trusted for the two eyes' difference (pupil.rs step_eyes).
    pub fn trusted(&self, time: f64) -> [bool; 2] {
        self.ellipse_at.map(|at| at.is_some_and(|at| time - at <= ACCEPTED_FRESH_S))
    }

    /// Each eye's centre's depth along its camera's axis (mm), once its filter has started (the eye fit's depth check,
    /// eyeball_map.rs).
    pub fn depths_mm(&self) -> [Option<f64>; 2] {
        self.filters.each_ref().map(|filter| filter.centre_mm().map(|(c, _)| c[2]))
    }

    /// Each filter's re-takes (for the journal).
    pub fn retakes(&self) -> [u32; 2] {
        self.filters.each_ref().map(|filter| filter.retakes)
    }

    /// The directions `step` gave, kept only for eyes whose filter has settled (EyeFilter::settled): what the eye
    /// fit's captures may count.
    pub fn settled_only(&self, eyes: [Option<[f32; 2]>; 2]) -> [Option<[f32; 2]>; 2] {
        [0, 1].map(|eye| eyes[eye].filter(|_| self.filters[eye].settled()))
    }

    /// Each eye's centre now, for the journal: where it is (mm, camera frame), how sure (standard deviations, mm),
    /// how many ellipse updates since the start or last re-take, and whether it counts for the eye fit.
    pub fn centre_line(&self) -> String {
        let eye = |e: usize| {
            let filter = &self.filters[e];
            match filter.centre_mm() {
                Some((c, sd)) => format!(
                    "{} ({:.1}, {:.1}, {:.1}) mm, sd ({:.2}, {:.2}, {:.2}), {} ellipse frames{}",
                    ["L", "R"][e],
                    c[0],
                    c[1],
                    c[2],
                    sd[0],
                    sd[1],
                    sd[2],
                    filter.ellipse_frames,
                    if filter.settled() {
                        String::new()
                    } else {
                        format!(
                            " (not settled: needs {SETTLED_ELLIPSE_FRAMES} ellipse frames and sd across below {:.2} mm)",
                            SETTLED_CENTRE_SD * 1e3
                        )
                    }
                ),
                None => format!("{} not started", ["L", "R"][e]),
            }
        };
        format!("Eyeball centre: {}; {}", eye(0), eye(1))
    }
}

/// Whether a file is eyecam-rec's replay output (`eyecam-rec --replay ... --out`): by its header.
pub fn is_eyecam_replay(header: &str) -> bool {
    header.starts_with("eye,eye_index,t_cam,")
}

/// The filter over an eyecam-rec replay output (its live CSV: eye, eye_index, t_cam, ok_pupil, aperture, pupil_vis,
/// pupil_cx / _cy / _a / _b and pupil_angle, or outline_a / _b / _angle / _vis for the shape where present), each eye on its own in camera-time order, the left eye's picture taken
/// as turned upright (as eyecam-rec turns it on the Frame). Writes one row per frame to `out` (eye, eye_index, t_cam,
/// update, yaw_deg, pitch_deg, centre_x_mm / _y_mm / _z_mm, empty before the filter starts; fresh as the live source
/// would use the eye (Freshness, on the camera's clock), settled for the eye fit, the centre's larger standard
/// deviation across the camera's axis and the one along it, in mm) and returns the report.
pub fn replay_eyecam(text: &str, calibration: &Calibration, out: Option<&Path>) -> Result<String, String> {
    let mut lines = text.lines();
    let header: Vec<&str> = lines.next().ok_or("the file is empty")?.split(',').collect();
    let column = |name: &str| header.iter().position(|c| *c == name).ok_or_else(|| format!("column {name} is missing"));
    let [eye_c, index_c, t_c, ok_c, ap_c, vis_c, cx_c, cy_c, a_c, b_c, ang_c] = [
        "eye", "eye_index", "t_cam", "ok_pupil", "aperture", "pupil_vis", "pupil_cx", "pupil_cy", "pupil_a", "pupil_b",
        "pupil_angle",
    ]
    .map(column);
    let (eye_c, index_c, t_c, ok_c, ap_c, vis_c) = (eye_c?, index_c?, t_c?, ok_c?, ap_c?, vis_c?);
    let (cx_c, cy_c, mut a_c, mut b_c, mut ang_c) = (cx_c?, cy_c?, a_c?, b_c?, ang_c?);
    // (the pupil's outline for this gaze, as the live file's shape part has it, where eyecam-rec writes it; older
    // outputs have the pupil's own ellipse only)
    let mut vis_c = vis_c;
    if let [Ok(a), Ok(b), Ok(ang), Ok(vis)] = ["outline_a", "outline_b", "outline_angle", "outline_vis"].map(column) {
        (a_c, b_c, ang_c, vis_c) = (a, b, ang, vis);
    }
    // Per eye: (t_cam, eye_index text, the frame)
    let mut frames: [Vec<(f64, String, RawFrame)>; 2] = [Vec::new(), Vec::new()];
    for (number, line) in lines.enumerate() {
        if line.trim().is_empty() {
            continue;
        }
        let cells: Vec<&str> = line.split(',').collect();
        let cell = |i: usize| cells.get(i).copied().unwrap_or("");
        let num = |i: usize| cell(i).parse::<f64>().unwrap_or(f64::NAN);
        let eye = match cell(eye_c) {
            "L" => 0,
            "R" => 1,
            other => return Err(format!("line {}: eye {other:?}", number + 2)),
        };
        let t = num(t_c);
        if !t.is_finite() {
            continue;
        }
        let ok = cell(ok_c) == "1";
        let centre = [num(cx_c), num(cy_c)];
        let frame = RawFrame {
            time: t,
            pupil: (ok && centre.iter().all(|v| v.is_finite())).then_some(centre),
            a: num(a_c),
            b: num(b_c),
            angle_deg: num(ang_c),
            vis: num(vis_c),
            aperture: num(ap_c),
            flipped: eye == 0,
        };
        frames[eye].push((t, cell(index_c).to_owned(), frame));
    }
    let mut writer = match out {
        Some(path) => Some(std::io::BufWriter::new(
            std::fs::File::create(path).map_err(|e| format!("{}: {e}", path.display()))?,
        )),
        None => None,
    };
    use std::io::Write as _;
    if let Some(w) = writer.as_mut() {
        writeln!(w, "eye,eye_index,t_cam,update,yaw_deg,pitch_deg,centre_x_mm,centre_y_mm,centre_z_mm,fresh,settled,centre_sd_mm,centre_sd_z_mm").map_err(|e| e.to_string())?;
    }
    let mut report = String::new();
    let mut total_ns = 0u128;
    let mut total = 0usize;
    for eye in 0..2 {
        let camera = &calibration.eyes[eye];
        frames[eye].sort_by(|a, b| a.0.total_cmp(&b.0));
        let mut filter = EyeFilter::default();
        let mut fresh = Freshness::default();
        let mut not_fresh = 0usize;
        let mut counts = [0usize; 6];
        let mut waiting = 0usize;
        let started = std::time::Instant::now();
        let mut rows = Vec::with_capacity(frames[eye].len());
        for (t, index, frame) in &frames[eye] {
            let o = observe(camera, frame);
            let estimate = filter.step(camera, &o);
            match estimate {
                Some(e) => counts[e.update as usize] += 1,
                None => waiting += 1,
            }
            // (fresh as the live glue judges it, on the camera's clock)
            fresh.note(*t, &o, estimate.as_ref());
            let now_fresh = fresh.fresh(*t);
            not_fresh += usize::from(estimate.is_some() && !now_fresh);
            let sd = filter.centre_mm().map_or([f64::NAN; 2], |(_, sd)| [sd[0].max(sd[1]), sd[2]]);
            rows.push((*t, index, estimate, now_fresh, filter.settled(), sd));
        }
        total_ns += started.elapsed().as_nanos();
        total += frames[eye].len();
        if let Some(w) = writer.as_mut() {
            for (t, index, estimate, now_fresh, settled, sd) in rows {
                let name = ["L", "R"][eye];
                match estimate {
                    Some(e) => writeln!(
                        w,
                        "{name},{index},{t:.6},{},{:.6},{:.6},{:.4},{:.4},{:.4},{},{},{:.3},{:.3}",
                        e.update.name(),
                        e.yaw.to_degrees(),
                        e.pitch.to_degrees(),
                        e.centre[0] * 1e3,
                        e.centre[1] * 1e3,
                        e.centre[2] * 1e3,
                        u8::from(now_fresh),
                        u8::from(settled),
                        sd[0],
                        sd[1]
                    ),
                    None => writeln!(w, "{name},{index},{t:.6},,,,,,,0,0,,"),
                }
                .map_err(|e| e.to_string())?;
            }
        }
        let names = [Update::Start, Update::NoPupil, Update::Rejected, Update::Implausible, Update::Ellipse, Update::PupilOnly];
        let counted: Vec<String> = names.iter().map(|u| format!("{} {}", u.name(), counts[*u as usize])).collect();
        report += &format!(
            "eyeball {}: {} frames ({} before the start): {}; {} re-takes; {} frames not given (no pupil seen, or no frame taken, lately)\n",
            ["left", "right"][eye],
            frames[eye].len(),
            waiting,
            counted.join(", "),
            filter.retakes,
            not_fresh
        );
    }
    if total > 0 {
        report += &format!("eyeball filter: {:.1} us per frame\n", total_ns as f64 / 1e3 / total as f64);
    }
    if let Some(mut w) = writer {
        w.flush().map_err(|e| e.to_string())?;
    }
    Ok(report)
}

#[cfg(test)]
pub mod tests {
    use super::*;

    /// A made-up camera (not any headset's numbers): 270 px focal length, centred, slight barrel distortion, looking
    /// back at the eye from in front, below and to the nose's side (its z axis towards the eye: head +z, turned up 40
    /// degrees and 15 to the side), its picture's x about along the head's +x.
    pub fn camera() -> Camera {
        let (up, side) = (40f64.to_radians(), 15f64.to_radians());
        let z = unit([-side.sin() * up.cos(), up.sin(), side.cos() * up.cos()]);
        let x = unit(sub([1.0, 0.0, 0.0], scale(z, z[0])));
        Camera { fx: 270.0, fy: 270.0, cx: 200.0, cy: 200.0, dist: [-0.05, 0.01, 0.0005, -0.0003, 0.0], axes: [x, cross(z, x), z], rows: 400.0, cols: 400.0 }
    }

    /// A made-up calibration JSON with two such cameras (and fields the loader leaves alone).
    pub fn calibration_json() -> String {
        json_of(&camera())
    }

    /// The calibration JSON of two cameras like `c` (its x and z axes as plus_x / plus_z).
    fn json_of(c: &Camera) -> String {
        let side = |c: &Camera| {
            format!(
                r#"{{"fx": {}, "fy": {}, "cx": {}, "cy": {}, "nx": 400, "ny": 400, "dist": [{}, {}, {}, {}, {}],
                "headFromCamera": {{"plus_x": [{}, {}, {}], "plus_z": [{}, {}, {}], "position": [0.03, -0.01, -0.02]}}}}"#,
                c.fx, c.fy, c.cx, c.cy, c.dist[0], c.dist[1], c.dist[2], c.dist[3], c.dist[4], c.axes[0][0], c.axes[0][1],
                c.axes[0][2], c.axes[2][0], c.axes[2][1], c.axes[2][2]
            )
        };
        format!(
            r#"{{"etCamCalib": {{"left": {}, "right": {}, "ipdSensor": {{"x": 0, "y": 0, "z": 0, "temp": 30}}}}, "ipdData": [{{}}, {{}}]}}"#,
            side(c),
            side(c)
        )
    }

    /// The sensor pixel where a camera-frame point is seen (the distortion applied).
    fn project(c: &Camera, p: V3) -> [f64; 2] {
        let (x, y) = (p[0] / p[2], p[1] / p[2]);
        let [k1, k2, p1, p2, k3] = c.dist;
        let r2 = x * x + y * y;
        let radial = 1.0 + ((k3 * r2 + k2) * r2 + k1) * r2;
        let xd = x * radial + 2.0 * p1 * x * y + p2 * (r2 + 2.0 * x * x);
        let yd = y * radial + p1 * (r2 + 2.0 * y * y) + 2.0 * p2 * x * y;
        [c.fx * xd + c.cx, c.fy * yd + c.cy]
    }

    /// The frame the model expects of an eye whose centre is at `centre` (camera frame, m) looking at (yaw, pitch)
    /// (degrees), as eyecam-rec would give it: the pupil centre in the sensor's picture (distorted) and an ellipse whose
    /// flattening and long axis are the model's. The filter's own behaviour is tested on these; `outline` gives what
    /// a camera sees.
    pub fn seen(c: &Camera, centre: V3, yaw: f64, pitch: f64, time: f64) -> RawFrame {
        let x = [yaw.to_radians(), pitch.to_radians(), 0.0, 0.0, centre[0], centre[1], centre[2]];
        let (z, _) = predict(&x, c, true);
        let pixel = project(c, [z[0], z[1], 1.0]);
        let flat = z[2].hypot(z[3]);
        let angle = 0.5 * z[3].atan2(z[2]);
        RawFrame {
            time,
            pupil: Some(pixel),
            a: 20.0,
            b: 20.0 * (1.0 - flat),
            angle_deg: angle.to_degrees().rem_euclid(180.0),
            vis: 1.0,
            aperture: 2.0,
            flipped: false,
        }
    }

    /// The frame a camera sees of such an eye: the pupil (a 1 mm circle) projected point by point through the lens and
    /// its ellipse fitted from the outline's moments, in the sensor's picture (not flipped). (A small pupil: at the
    /// filter's short distances a larger one's perspective makes its outline differ more from a tilted circle.)
    pub fn outline(c: &Camera, centre: V3, yaw: f64, pitch: f64, time: f64) -> RawFrame {
        let g = c.to_camera(gaze_head(yaw.to_radians(), pitch.to_radians()));
        let pupil = add(centre, scale(g, R_PUPIL));
        // Two directions in the pupil's plane
        let u = unit(cross(g, [0.0, 1.0, 0.0]));
        let v = cross(g, u);
        let n = 360;
        let points: Vec<[f64; 2]> = (0..n)
            .map(|k| {
                let t = 2.0 * PI * k as f64 / n as f64;
                project(c, add(pupil, add(scale(u, 0.001 * t.cos()), scale(v, 0.001 * t.sin()))))
            })
            .collect();
        // The ellipse of the outline: centre and second moments of the boundary (a closed curve sampled evenly in its
        // parameter: for a projected circle, close to the ellipse's own)
        let mean = [0, 1].map(|i| points.iter().map(|p| p[i]).sum::<f64>() / n as f64);
        let cov = |i: usize, j: usize| points.iter().map(|p| (p[i] - mean[i]) * (p[j] - mean[j])).sum::<f64>() / n as f64;
        let (sxx, syy, sxy) = (cov(0, 0), cov(1, 1), cov(0, 1));
        let half = (sxx + syy) / 2.0;
        let root = (half * half - (sxx * syy - sxy * sxy)).max(0.0).sqrt();
        let (l1, l2) = (half + root, half - root);
        let angle = 0.5 * (2.0 * sxy).atan2(sxx - syy);
        let centre_px = project(c, pupil);
        RawFrame {
            time,
            pupil: Some(centre_px),
            a: (2.0 * l1).sqrt(),
            b: (2.0 * l2.max(0.0)).sqrt(),
            angle_deg: angle.to_degrees().rem_euclid(180.0),
            vis: 1.0,
            aperture: 2.0,
            flipped: false,
        }
    }

    /// The eye's centre (camera frame) that puts its pupil D0 away on the camera's axis while it looks straight ahead
    /// (the distance the filter starts from: its geometry is an effective one, see D0).
    fn eye_at(c: &Camera) -> V3 {
        sub([0.0, 0.0, D0], scale(c.forward(), R_PUPIL))
    }

    /// What makes the frames: `seen` (the model's own) or `outline` (a camera's).
    type Frames = fn(&Camera, V3, f64, f64, f64) -> RawFrame;

    /// The filter's own angles (degrees), without the still-eye hold (HOLD_RADIUS, tested on its own).
    fn own(filter: &EyeFilter) -> (f64, f64) {
        let x = filter.x.unwrap();
        (x[0].to_degrees(), x[1].to_degrees())
    }

    /// Runs `seconds` at 90 frames a second from `start` of an eye whose centre and direction are given per time;
    /// returns the filter's own last angles (degrees, see `own`) and its centre (mm).
    fn run(filter: &mut EyeFilter, c: &Camera, frames: Frames, from: f64, seconds: f64, at: impl Fn(f64) -> (V3, f64, f64)) -> (f64, f64, V3) {
        let mut last = None;
        let n = (seconds * 90.0).round() as usize;
        for k in 0..n {
            let t = from + k as f64 / 90.0;
            let (centre, yaw, pitch) = at(t);
            let o = observe(c, &frames(c, centre, yaw, pitch, t));
            if let Some(e) = filter.step(c, &o) {
                last = Some(e);
            }
        }
        let e = last.unwrap();
        let (yaw, pitch) = own(filter);
        (yaw, pitch, scale(e.centre, 1e3))
    }

    /// Looks around for a while so the centre is learnt: the eye fit's five dots (as the left eye sees them), two
    /// seconds each, three times; then the map from the last round, as the eye fit makes it (eyeball_map.rs).
    fn learn(filter: &mut EyeFilter, c: &Camera, frames: Frames) -> (f64, EyeMap) {
        learn_from(filter, c, frames, 0.0)
    }

    /// The same from start (s).
    fn learn_from(filter: &mut EyeFilter, c: &Camera, frames: Frames, start: f64) -> (f64, EyeMap) {
        let mut t = start;
        let mut medians = [[0.0f32; 2]; 5];
        for _ in 0..3 {
            for (i, (_, yaw, pitch)) in crate::eyeball_map::DOTS.iter().enumerate() {
                let [yaw, pitch] = crate::eyeball_map::eye_direction(*yaw, *pitch, 0);
                run(filter, c, frames, t, 1.0, |_| (eye_at(c), yaw, pitch));
                let (y, p, _) = run(filter, c, frames, t + 1.0, 1.0, |_| (eye_at(c), yaw, pitch));
                medians[i] = [y as f32, p as f32];
                t += 2.0;
            }
        }
        let (map, _) = crate::eyeball_map::fit_eye(&medians, 0).unwrap();
        (t, map)
    }

    /// After learning: how far the mapped gaze strays (degrees) while the eye looks at each of `places` in turn (one
    /// second each, the last half counted), the worst.
    fn turned(frames: Frames, places: &[(f64, f64)]) -> f64 {
        let c = camera();
        let mut filter = EyeFilter::default();
        let (mut t, map) = learn(&mut filter, &c, frames);
        let mut worst = 0.0f64;
        for &(yaw, pitch) in places {
            run(&mut filter, &c, frames, t, 0.5, |_| (eye_at(&c), yaw, pitch));
            let (y, p, _) = run(&mut filter, &c, frames, t + 0.5, 0.5, |_| (eye_at(&c), yaw, pitch));
            let [y, p] = mapped(&map, y, p);
            worst = worst.max((y - yaw).hypot(p - pitch));
            t += 1.0;
        }
        worst
    }

    /// After learning, the eye looking straight ahead: the headset pushed by `shift` (mm) over half a second and left
    /// there for 2.5 s. How far the mapped gaze moved (degrees, at most and at the end), and the centre's move (mm).
    fn shifted(frames: Frames, shift: V3, lids_clear: bool) -> (f64, f64, V3) {
        let c = camera();
        let mut filter = EyeFilter::default();
        let (t, map) = learn(&mut filter, &c, frames);
        let (y0, p0, c0) = run(&mut filter, &c, frames, t, 1.0, |_| (eye_at(&c), 0.0, 0.0));
        let [y0, p0] = mapped(&map, y0, p0);
        let mut worst = 0.0f64;
        let mut last = (0.0, [0.0; 3]);
        for k in 0..270 {
            let time = t + 1.0 + k as f64 / 90.0;
            let share = (k as f64 / 45.0).min(1.0);
            let mut frame = frames(&c, add(eye_at(&c), scale(shift, 1e-3 * share)), 0.0, 0.0, time);
            if !lids_clear {
                frame.aperture = 1.0;
            }
            let e = filter.step(&c, &observe(&c, &frame)).unwrap();
            let (yaw, pitch) = own(&filter);
            let [y, p] = mapped(&map, yaw, pitch);
            worst = worst.max((y - y0).hypot(p - p0));
            last = ((y - y0).hypot(p - p0), sub(scale(e.centre, 1e3), c0));
        }
        (worst, last.0, last.1)
    }

    /// Places between and beyond the eye fit's dots.
    const PLACES: [(f64, f64); 6] = [(0.0, 0.0), (12.0, 8.0), (-15.0, -6.0), (5.0, 12.0), (25.0, -10.0), (-10.0, 18.0)];
    /// Pushes of the headset (mm, camera frame): up, sideways, a mix.
    const SHIFTS: [V3; 3] = [[0.0, 3.0, 0.0], [3.0, 0.0, 0.0], [2.0, -2.0, 1.0]];

    /// The map's direction for an estimate (degrees).
    fn mapped(map: &EyeMap, yaw: f64, pitch: f64) -> [f64; 2] {
        map.apply([yaw as f32, pitch as f32]).map(f64::from)
    }

    #[test]
    fn the_distortion_is_undone() {
        let c = camera();
        for p in [[0.0, 0.0, 1.0], [0.3, -0.2, 1.0], [-0.5, 0.4, 1.0]] {
            let [u, v] = project(&c, p);
            let [x, y] = c.undistort(u, v);
            assert!((x - p[0]).abs() < 1e-4 && (y - p[1]).abs() < 1e-4, "{p:?} -> {x} {y}");
        }
    }

    #[test]
    fn the_calibration_is_read_and_checked() {
        let cal = parse_calibration(&calibration_json()).unwrap();
        let c = camera();
        let got = &cal.eyes[0];
        assert_eq!((got.fx, got.cy, got.dist, got.rows), (c.fx, c.cy, c.dist, 400.0));
        for (a, b) in got.axes.iter().zip(&c.axes) {
            assert!(norm(sub(*a, *b)) < 1e-12, "{a:?} {b:?}");
        }
        // Axes given a little off square come out orthonormal, x kept
        let mut skew = camera();
        skew.axes[2] = add(skew.axes[2], [0.05, 0.0, 0.0]);
        let axes = parse_calibration(&json_of(&skew)).unwrap().eyes[0].axes;
        assert!(norm(sub(axes[0], c.axes[0])) < 1e-12);
        for i in 0..3 {
            assert!((norm(axes[i]) - 1.0).abs() < 1e-12);
            assert!(dot(axes[i], axes[(i + 1) % 3]).abs() < 1e-12);
        }
        // Without ny the picture is 400 rows
        assert_eq!(parse_calibration(&calibration_json().replace("\"ny\": 400, ", "")).unwrap().eyes[1].rows, 400.0);
        // What is missing or wrong is said
        let broken = [
            ("{}", "no etCamCalib"),
            ("not json", "not JSON"),
            (r#"{"etCamCalib": {"left": {}}}"#, "etCamCalib.left.fx"),
        ];
        for (text, why) in broken {
            let err = parse_calibration(text).unwrap_err();
            assert!(err.contains(why), "{err}");
        }
        let short = calibration_json().replacen("\"dist\": [-0.05, ", "\"dist\": [", 1);
        assert!(parse_calibration(&short).unwrap_err().contains("dist: not 5 numbers"));
        let flat = Camera { fx: 0.0, ..camera() };
        assert!(parse_calibration(&json_of(&flat)).unwrap_err().contains("positive"));
        let mut parallel = camera();
        parallel.axes[2] = scale(parallel.axes[0], 2.0);
        assert!(parse_calibration(&json_of(&parallel)).unwrap_err().contains("not a rotation"));
        // From a file, and a file that isn't there
        let path = std::env::temp_dir().join(format!("frameeyeosc-eyecal-{}.json", std::process::id()));
        std::fs::write(&path, calibration_json()).unwrap();
        assert_eq!(load_calibration(&path).unwrap(), cal);
        std::fs::remove_file(&path).unwrap();
        assert!(load_calibration(&path).unwrap_err().contains("frameeyeosc-eyecal"));
    }

    #[test]
    fn a_flipped_picture_is_turned_back_and_the_ellipse_needs_clear_lids_and_its_rim() {
        let c = camera();
        let frame = seen(&c, eye_at(&c), 10.0, 5.0, 0.0);
        let plain = observe(&c, &frame);
        assert!(plain.pos.is_some() && plain.ell.is_some());
        // The same frame in a picture turned upright: the same observation
        let [x, y] = frame.pupil.unwrap();
        let upright = RawFrame { pupil: Some([x, 399.0 - y]), angle_deg: (180.0 - frame.angle_deg) % 180.0, flipped: true, ..frame };
        let turned = observe(&c, &upright);
        for i in 0..2 {
            assert!((turned.pos.unwrap()[i] - plain.pos.unwrap()[i]).abs() < 1e-12);
            assert!((turned.ell.unwrap()[i] - plain.ell.unwrap()[i]).abs() < 1e-9);
        }
        // A lid near the pupil or part of the rim hidden: the pupil only; unknown values don't stop it
        assert!(observe(&c, &RawFrame { aperture: 1.2, ..frame }).ell.is_none());
        assert!(observe(&c, &RawFrame { vis: 0.85, ..frame }).ell.is_none());
        // ...a rim partly hidden (PARTIAL_VIS_MIN up to VIS_MIN): the ellipse, marked partial
        let part = observe(&c, &RawFrame { vis: 0.9, ..frame });
        assert!(part.ell.is_some() && part.partial && !plain.partial);
        assert!(observe(&c, &RawFrame { aperture: f64::NAN, vis: f64::NAN, ..frame }).ell.is_some());
        assert!(observe(&c, &RawFrame { angle_deg: f64::NAN, ..frame }).ell.is_none());
        // No pupil, or one outside the picture: nothing
        assert_eq!(observe(&c, &RawFrame { pupil: None, ..frame }).pos, None);
        assert_eq!(observe(&c, &RawFrame { pupil: Some([2200.0, -1300.0]), ..frame }).pos, None);
    }

    #[test]
    fn the_prediction_matches_what_the_camera_sees() {
        let c = camera();
        for (yaw, pitch) in [(0.0, 0.0), (15.0, -10.0), (-20.0, 12.0)] {
            let o = observe(&c, &outline(&c, eye_at(&c), yaw, pitch, 0.0));
            let x = [yaw.to_radians(), pitch.to_radians(), 0.0, 0.0, eye_at(&c)[0], eye_at(&c)[1], eye_at(&c)[2]];
            let (z, n) = predict(&x, &c, true);
            assert_eq!(n, 4);
            let pos = o.pos.unwrap();
            let ell = o.ell.unwrap();
            assert!((z[0] - pos[0]).abs() < 1e-3 && (z[1] - pos[1]).abs() < 1e-3, "{yaw} {pitch}: {z:?} {pos:?}");
            assert!((z[2] - ell[0]).abs() < 0.03 && (z[3] - ell[1]).abs() < 0.03, "{yaw} {pitch}: {z:?} {ell:?}");
            // One of the ellipse's two normals is the gaze
            let g = c.to_camera(gaze_head(x[0], x[1]));
            let best = normals(pos, ell).iter().map(|n| dot(*n, g)).fold(-1.0, f64::max);
            assert!(best > 0.99, "{yaw} {pitch}: {best}");
        }
    }

    #[test]
    fn a_turn_of_the_eye_is_followed() {
        // Frames as the model expects them: each place within half a degree once mapped
        let worst = turned(seen, &PLACES);
        eprintln!("turns, model's frames: at most {worst:.2} deg off");
        assert!(worst < 0.5, "{worst}");
        // A camera's picture of the pupil (perspective, the lens: the model's tilted circle is only near it): the turns
        // of 10-27 degrees, out to beyond the dots, followed within 6 degrees (between the dots within 3)
        let worst = turned(outline, &PLACES);
        eprintln!("turns, a camera's frames: at most {worst:.2} deg off");
        assert!(worst < 6.0, "{worst}");
        let worst = turned(outline, &PLACES[..3]);
        eprintln!("turns between the dots, a camera's frames: at most {worst:.2} deg off");
        assert!(worst < 3.0, "{worst}");
    }

    #[test]
    fn a_saccade_is_followed_within_a_few_frames() {
        // After the fit, the eye jumps from straight ahead to (15, 8) and to (-18, -10) from one frame to the next:
        // within 2 degrees of where it landed after a few frames (the saccade's acceleration noise, SACCADE_GAIN)
        let c = camera();
        let mut filter = EyeFilter::default();
        let (mut t, map) = learn(&mut filter, &c, seen);
        for (yaw, pitch) in [(15.0, 8.0), (-18.0, -10.0), (0.0, 0.0)] {
            let mut within = None;
            for k in 0..45 {
                let e = filter.step(&c, &observe(&c, &seen(&c, eye_at(&c), yaw, pitch, t))).unwrap();
                let [y, p] = mapped(&map, e.yaw.to_degrees(), e.pitch.to_degrees());
                if within.is_none() && (y - yaw).hypot(p - pitch) <= 2.0 {
                    within = Some(k);
                }
                t += 1.0 / 90.0;
            }
            eprintln!("saccade to ({yaw}, {pitch}): within 2 deg after {within:?} frames");
            assert!(within.is_some_and(|k| k <= 8), "({yaw}, {pitch}): {within:?}");
        }
    }

    #[test]
    fn a_saccade_goes_by_the_pupils_speed_whatever_the_cameras_frame_rate() {
        let c = camera();
        // A smooth pursuit at 30 degrees a second: no saccade at 90 frames a second, nor at 15 (where it moves 2 degrees
        // a frame, more than a 90-frame step); a 300 degree a second jump is one at either rate
        for fps in [90.0, 15.0] {
            for (speed, saccade) in [(30.0, false), (300.0, true)] {
                let mut filter = EyeFilter::default();
                let mut t = 0.0;
                learn_from(&mut filter, &c, seen, t);
                t += 30.0;
                let mut seen_saccade = false;
                for k in 0..((0.1 * fps) as usize).max(2) {
                    let yaw = -10.0 + speed * k as f64 / fps;
                    filter.step(&c, &observe(&c, &seen(&c, eye_at(&c), yaw.min(20.0), 0.0, t)));
                    // (the first frame after learning comes from where the learning ended)
                    if k > 0 {
                        seen_saccade |= filter.saccade;
                    }
                    t += 1.0 / fps;
                }
                assert_eq!(seen_saccade, saccade, "{fps} fps, {speed} deg/s");
            }
        }
    }

    #[test]
    fn a_headset_shift_with_the_eye_still_moves_the_gaze_less_than_a_degree() {
        for shift in SHIFTS {
            // The headset pushed 3 mm (the camera against the eye), the eye still looking straight ahead
            let (worst, after, centre) = shifted(seen, shift, true);
            eprintln!("shift {shift:?} mm, model's frames: gaze moved at most {worst:.2} deg ({after:.2} after), centre {centre:.2?} mm");
            assert!(worst < 1.0, "shift {shift:?}: {worst:.2} deg");
            // ...the centre followed (the model's geometry is not the physical one, so not by the same millimetres)
            assert!(norm(centre) > 1.0, "shift {shift:?}: centre moved {centre:?} mm");
            // A camera's picture of the pupil, 2 mm: under 1.5 degrees (the pupil's place alone would read about 11)
            let (worst, after, _) = shifted(outline, scale(shift, 2.0 / 3.0), true);
            eprintln!("shift {:?} mm, a camera's frames: gaze moved at most {worst:.2} deg ({after:.2} after)", scale(shift, 2.0 / 3.0));
            assert!(worst < 1.5, "shift {shift:?} x 2/3, outline: {worst:.2} deg");
            // Without the ellipse (a lid near the pupil) the same push is read as a turn: the ellipse is what tells
            let (_, blind, _) = shifted(seen, shift, false);
            assert!(blind > 5.0, "shift {shift:?} without the ellipse: {blind:.2} deg");
        }
    }

    /// The spread (degrees, the larger of yaw's and pitch's standard deviations) of a run of directions (radians).
    fn spread(d: &[[f64; 2]]) -> f64 {
        let sd = |i: usize| {
            let mean = d.iter().map(|v| v[i]).sum::<f64>() / d.len() as f64;
            (d.iter().map(|v| (v[i] - mean).powi(2)).sum::<f64>() / d.len() as f64).sqrt().to_degrees()
        };
        sd(0).max(sd(1))
    }

    #[test]
    fn a_still_eye_is_held_while_its_ellipse_drifts_and_let_go_when_it_moves() {
        // After the fit the eye looks at (8, 4) for 4 s, the pupil still, while the ellipse's flattening drifts by 5%
        // over 2 s and back (as with the pupil's size): the filter's own angles wander with it (its centre against
        // them), the direction given stays within a fraction of that
        let c = camera();
        let mut filter = EyeFilter::default();
        let (mut t, _) = learn(&mut filter, &c, seen);
        let (mut own, mut given) = (Vec::new(), Vec::new());
        for k in 0..360 {
            let mut frame = seen(&c, eye_at(&c), 8.0, 4.0, t);
            let flat = 1.0 - frame.b / frame.a;
            frame.b = frame.a * (1.0 - flat * (1.0 + 0.05 * (2.0 * PI * k as f64 / 360.0).sin()));
            let e = filter.step(&c, &observe(&c, &frame)).unwrap();
            assert_eq!(filter.direction(), Some([e.yaw, e.pitch]));
            if k >= 45 {
                let x = filter.x.unwrap();
                own.push([x[0], x[1]]);
                given.push([e.yaw, e.pitch]);
            }
            t += 1.0 / 90.0;
        }
        let (own, given) = (spread(&own), spread(&given));
        eprintln!("still eye, drifting ellipse: the filter's own angles spread {own:.2} deg, the given {given:.2} deg");
        assert!(own > 0.3 && given < own / 2.0, "own {own:.2}, given {given:.2}");
        assert!(filter.still);
        // A saccade lets go at once: on its first frame the filter's own angles are given
        let e = filter.step(&c, &observe(&c, &seen(&c, eye_at(&c), -12.0, -6.0, t))).unwrap();
        let x = filter.x.unwrap();
        assert!(filter.saccade && !filter.still);
        assert_eq!((e.yaw, e.pitch), (x[0], x[1]));
        t += 1.0 / 90.0;
        // Held again after HOLD_AFTER_S there; a lost pupil lets go over HOLD_EASE_S
        run(&mut filter, &c, seen, t, 0.5, |_| (eye_at(&c), -12.0, -6.0));
        t += 0.5;
        assert!(filter.still);
        filter.held = [0.01, -0.01];
        let mut lost = seen(&c, eye_at(&c), -12.0, -6.0, t);
        lost.pupil = None;
        let e = filter.step(&c, &observe(&c, &lost)).unwrap();
        let x = filter.x.unwrap();
        let keep = (-1.0 / 90.0 / HOLD_EASE_S).exp();
        assert!(!filter.still);
        assert!((e.yaw - x[0] - 0.01 * keep).abs() < 1e-12 && (e.pitch - x[1] + 0.01 * keep).abs() < 1e-12);
    }

    /// Learns, looks at (8, 4) for a second so the eye is held; returns the time of the next frame.
    fn held_at(filter: &mut EyeFilter, c: &Camera) -> f64 {
        let (t, _) = learn(filter, c, seen);
        run(filter, c, seen, t, 1.0, |_| (eye_at(c), 8.0, 4.0));
        assert!(filter.still);
        t + 1.0
    }

    /// Frames at (8, 4) from `t` for `seconds`: from which frame on the eye is held again (None: not within them).
    fn held_again(filter: &mut EyeFilter, c: &Camera, t: f64, seconds: f64) -> Option<usize> {
        let n = (seconds * 90.0).round() as usize;
        (0..n).find(|&k| {
            filter.step(c, &observe(c, &seen(c, eye_at(c), 8.0, 4.0, t + k as f64 / 90.0)));
            filter.still
        })
    }

    #[test]
    fn after_a_blink_or_a_re_take_the_hold_waits_again() {
        let c = camera();
        // A blink (the pupil lost for 0.1 s) and back at the same place: not held on the frame it is back, only
        // HOLD_AFTER_S later (the lids may still be clearing, the centre still moving)
        let mut filter = EyeFilter::default();
        let mut t = held_at(&mut filter, &c);
        for _ in 0..9 {
            let mut lost = seen(&c, eye_at(&c), 8.0, 4.0, t);
            lost.pupil = None;
            filter.step(&c, &observe(&c, &lost));
            assert!(!filter.still);
            t += 1.0 / 90.0;
        }
        let again = held_again(&mut filter, &c, t, 1.0);
        eprintln!("after a blink: held again from frame {again:?}");
        assert_eq!(again, Some((HOLD_AFTER_S * 90.0).round() as usize));
        // A re-take (frames refused REACQUIRE times in a row, the angles taken afresh): the same wait
        let mut filter = EyeFilter::default();
        let t = held_at(&mut filter, &c);
        filter.misses = REACQUIRE;
        let retakes = filter.retakes;
        let again = held_again(&mut filter, &c, t, 1.0);
        assert_eq!(filter.retakes, retakes + 1);
        eprintln!("after a re-take: held again from frame {again:?}");
        assert_eq!(again, Some((HOLD_AFTER_S * 90.0).round() as usize));
        // Whatever the held difference was, it is dropped on the re-take, and a NaN one is never given
        let mut filter = EyeFilter::default();
        let t = held_at(&mut filter, &c);
        filter.held = [f64::NAN, 0.0];
        filter.rest = None;
        let e = filter.step(&c, &observe(&c, &seen(&c, eye_at(&c), 8.0, 4.0, t))).unwrap();
        assert!(e.yaw.is_finite() && e.pitch.is_finite() && filter.held == [0.0; 2]);
    }

    #[test]
    fn the_hold_follows_the_filters_settling_and_a_slow_slip_at_its_pace() {
        // The cost of the hold: after a landing the filter's centre still settles, and the hold (from HOLD_AFTER_S)
        // follows it only at HOLD_CENTRE_TAU_S. On the model's frames the filter's own angles end exact; the given
        // direction is off by at most HOLD_MAX_DEG just after a turn and within 0.75 degree of them after 3 s
        let c = camera();
        let mut filter = EyeFilter::default();
        let (mut t, _) = learn(&mut filter, &c, seen);
        for (yaw, pitch) in [(15.0, 8.0), (-18.0, -10.0), (0.0, 0.0)] {
            let (mut worst, mut last) = (0.0f64, 0.0);
            for _ in 0..270 {
                let e = filter.step(&c, &observe(&c, &seen(&c, eye_at(&c), yaw, pitch, t))).unwrap();
                let (y, p) = own(&filter);
                last = (e.yaw.to_degrees() - y).hypot(e.pitch.to_degrees() - p);
                worst = worst.max(last);
                t += 1.0 / 90.0;
            }
            eprintln!("hold after a turn to ({yaw}, {pitch}): at most {worst:.2} deg from the filter's own, {last:.2} after 3 s");
            assert!(worst <= HOLD_MAX_DEG + 1e-9 && last < 0.75, "({yaw}, {pitch}): {worst:.2} / {last:.2}");
        }
        // The headset sliding 2 mm over 4 s with the eye still: the given direction never strays from the filter's own
        // by more than a degree (the pupil leaves the hold's radius every second or so and the hold starts anew)
        let mut worst = 0.0f64;
        for k in 0..450 {
            let share = (k as f64 / 360.0).min(1.0);
            let centre = add(eye_at(&c), [0.0, 0.002 * share, 0.0]);
            let e = filter.step(&c, &observe(&c, &seen(&c, centre, 0.0, 0.0, t))).unwrap();
            let (y, p) = own(&filter);
            worst = worst.max((e.yaw.to_degrees() - y).hypot(e.pitch.to_degrees() - p));
            t += 1.0 / 90.0;
        }
        eprintln!("hold through a slow 2 mm slip: at most {worst:.2} deg from the filter's own");
        assert!(worst < 1.0, "{worst:.2}");
    }

    #[test]
    fn it_waits_for_an_ellipse_holds_through_lost_frames_and_restarts() {
        let c = camera();
        let mut filter = EyeFilter::default();
        // No ellipse yet: not started
        let frame = RawFrame { aperture: 1.0, ..seen(&c, eye_at(&c), 0.0, 0.0, 0.0) };
        assert!(filter.step(&c, &observe(&c, &frame)).is_none() && !filter.started());
        let e = filter.step(&c, &observe(&c, &seen(&c, eye_at(&c), 0.0, 0.0, 0.01))).unwrap();
        assert_eq!(e.update, Update::Start);
        // No pupil: the prediction
        let none = Obs { time: 0.02, pos: None, ell: None, partial: false };
        assert_eq!(filter.step(&c, &none).unwrap().update, Update::NoPupil);
        filter.reset();
        assert!(!filter.started());
    }

    #[test]
    fn an_eye_is_fresh_only_while_its_pupil_is_seen_and_its_filter_takes_frames() {
        let o = |pos: bool| Obs { time: 0.0, pos: pos.then_some([0.0, 0.0]), ell: None, partial: false };
        let e = |update: Update| Estimate { yaw: 0.0, pitch: 0.0, centre: [0.0; 3], update };
        let mut fresh = Freshness::default();
        assert!(!fresh.fresh(0.0));
        fresh.note(1.0, &o(true), Some(&e(Update::Ellipse)));
        assert!(fresh.fresh(1.0) && fresh.fresh(1.0 + EYE_FRESH_S));
        // The pupil seen but every frame refused: given until ACCEPTED_FRESH_S after the last one taken, then not
        for k in 1..40 {
            let t = 1.0 + f64::from(k) / 90.0;
            fresh.note(t, &o(true), Some(&e(if k % 2 == 0 { Update::Rejected } else { Update::Implausible })));
            assert_eq!(fresh.fresh(t), t - 1.0 <= ACCEPTED_FRESH_S, "{t}");
        }
        // Taken again (the pupil only): fresh at once
        fresh.note(2.0, &o(true), Some(&e(Update::PupilOnly)));
        assert!(fresh.fresh(2.0));
        // No pupil: the prediction is not a frame taken, and the pupil is not seen
        fresh.note(2.1, &o(false), Some(&e(Update::NoPupil)));
        assert!(fresh.fresh(2.1) && !fresh.fresh(2.0 + EYE_FRESH_S + 0.01));
    }

    #[test]
    fn a_filter_that_keeps_refusing_its_frames_gives_no_direction() {
        let c = camera();
        let mut gaze = EyeballGaze::default();
        gaze.set_calibration(Some(Calibration { eyes: [c.clone(), c.clone()] }));
        let mut live = crate::tests::camera(0.75, 0.0);
        let mut out = Vec::new();
        let mut accepted = 0.0;
        for k in 0..400u64 {
            let time = k as f64 / 90.0;
            // From frame 200 the "pupil" jumps about the picture from frame to frame (something else taken for it at a
            // corner, the lids over the real one): the filter refuses nearly all of these frames
            let (mut slip, shape) = live_frame(&c, 1_000_000_000 + k * 11_111_111, 5.0);
            if k >= 200 {
                let r = (k.wrapping_mul(2_654_435_761) >> 7) % 10_000;
                slip.pupil = Some([20.0 + (r % 100) as f32 * 3.6, 20.0 + (r / 100) as f32 * 3.6]);
            }
            live.slip = [Some(slip), Some(slip)];
            live.shape = [Some(shape), Some(shape)];
            let before = gaze.updates[0];
            let eyes = gaze.step(time, Some(&live));
            let took = [Update::Start, Update::Ellipse, Update::PupilOnly]
                .iter()
                .any(|u| gaze.updates[0][*u as usize] != before[*u as usize]);
            if took {
                accepted = time;
            }
            // Given only while a frame was taken within ACCEPTED_FRESH_S (the pupil is seen in every frame)
            assert_eq!(eyes[0].is_some(), time - accepted <= ACCEPTED_FRESH_S, "frame {k}");
            out.push(eyes);
        }
        assert!(out[199][0].is_some());
        let refused = gaze.updates[0][Update::Rejected as usize] + gaze.updates[0][Update::Implausible as usize];
        assert!(refused > 100, "{:?}", gaze.updates[0]);
        // ...so for stretches of it no direction: the one-eye / hold / eye server fallbacks take over in pupil.rs
        let none = out[200..].iter().filter(|eyes| eyes[0].is_none()).count();
        eprintln!("a jumping pupil: {refused} frames refused, no direction for {none} of 200 samples");
        assert!(none > 20, "{none}");
    }

    #[test]
    fn the_centre_settles_for_the_eye_fit_and_its_uncertainty_stays_capped_without_the_ellipse() {
        let c = camera();
        let mut filter = EyeFilter::default();
        // Not settled before SETTLED_ELLIPSE_FRAMES ellipse updates...
        run(&mut filter, &c, seen, 0.0, 0.5, |_| (eye_at(&c), 0.0, 0.0));
        assert!(!filter.settled());
        // ...nor while the centre is unsure across the camera's axis, however many there were
        let mut unsure = filter.clone();
        unsure.ellipse_frames = 10 * SETTLED_ELLIPSE_FRAMES;
        unsure.p[4][4] = (2.0 * SETTLED_CENTRE_SD).powi(2);
        assert!(!unsure.settled());
        // Looking about (the eye fit's dots): settled, the centre within SETTLED_CENTRE_SD
        let (_, _) = learn(&mut filter, &c, seen);
        let (_, sd) = filter.centre_mm().unwrap();
        assert!(filter.settled(), "{sd:?}");
        let t = 30.0;
        run(&mut filter, &c, seen, t, 0.5, |_| (eye_at(&c), 0.0, 0.0));
        let (_, before) = filter.centre_mm().unwrap();
        // Ten seconds with a lid near the pupil (no ellipse): the centre held, its uncertainty no larger than the cap
        // (or what it was); without the cap it would grow by SIG_C a second's root to about 4.7 mm
        let mut t = 30.5;
        for _ in 0..900 {
            let frame = RawFrame { aperture: 1.0, ..seen(&c, eye_at(&c), 3.0, 2.0, t) };
            let e = filter.step(&c, &observe(&c, &frame)).unwrap();
            assert_eq!(e.update, Update::PupilOnly);
            t += 1.0 / 90.0;
        }
        let (_, after) = filter.centre_mm().unwrap();
        for i in 0..3 {
            assert!(after[i] <= before[i].max(CENTRE_SD_CAP * 1e3) + 1e-9, "{before:?} -> {after:?}");
        }
        // Unsure again by then (above SETTLED_CENTRE_SD): such an eye does not count for the eye fit until the ellipse
        // has made it sure again, looking about
        assert!(!filter.settled(), "{after:?}");
        learn_from(&mut filter, &c, seen, t);
        assert!(filter.settled(), "{:?}", filter.centre_mm());
        // A re-take starts the count over: a filter started anew is not settled
        filter.reset();
        assert!(!filter.settled() && filter.centre_mm().is_none());
    }

    /// A live file's values for one eye frame (glint and shape parts).
    fn live_frame(c: &Camera, time_ns: u64, yaw: f64) -> (CamSlip, CamShape) {
        let f = seen(c, eye_at(c), yaw, 0.0, time_ns as f64 / 1e9);
        let slip = CamSlip { time_ns, pupil: f.pupil.map(|p| p.map(|v| v as f32)), ..CamSlip::default() };
        let shape = CamShape {
            a: f.a as f32,
            b: f.b as f32,
            angle_deg: f.angle_deg as f32,
            vis: 1.0,
            aperture: 2.0,
            flipped: false,
        };
        (slip, shape)
    }

    #[test]
    fn the_live_glue_takes_each_frame_once_and_gives_an_eye_while_its_pupil_is_fresh() {
        let c = camera();
        let mut gaze = EyeballGaze::default();
        // Without the calibration: nothing
        assert_eq!(gaze.step(0.0, None), [None, None]);
        gaze.set_calibration(Some(Calibration { eyes: [c.clone(), c.clone()] }));
        let mut live = crate::tests::camera(0.75, 0.0);
        let mut out = [None; 2];
        for k in 0..200u64 {
            let time = k as f64 / 90.0;
            let (slip, shape) = live_frame(&c, 1_000_000_000 + k * 11_111_111, 10.0);
            live.slip = [Some(slip), None];
            live.shape = [Some(shape), None];
            // Two samples per frame: the frame taken once
            gaze.step(time, Some(&live));
            out = gaze.step(time + 0.001, Some(&live));
        }
        assert_eq!(gaze.frames, [200, 0]);
        assert!(out[1].is_none());
        let [yaw, _] = out[0].unwrap();
        assert!(yaw > 5.0, "{yaw}");
        // The pupil gone for longer than EYE_FRESH_S: no direction
        let mut time = 200.0 / 90.0;
        live.slip = [None; 2];
        while time < 200.0 / 90.0 + EYE_FRESH_S + 0.05 {
            out = gaze.step(time, Some(&live));
            time += 1.0 / 90.0;
        }
        assert_eq!(out, [None, None]);
        // Time going back starts over
        gaze.step(0.0, Some(&live));
        assert!(!gaze.filters[0].started());
    }

    #[test]
    fn an_eyecam_replay_csv_runs_through_the_filter() {
        let c = camera();
        let cal = Calibration { eyes: [c.clone(), c.clone()] };
        let mut text = String::from("eye,eye_index,t_cam,ok_pupil,aperture,pupil_vis,pupil_cx,pupil_cy,pupil_a,pupil_b,pupil_angle\n");
        for k in 0..90 {
            let t = 100.0 + k as f64 / 90.0;
            for (eye, name) in ["L", "R"].iter().enumerate() {
                let f = seen(&c, eye_at(&c), 8.0, -4.0, t);
                let [x, y] = f.pupil.unwrap();
                // The left eye's picture is turned upright, as eyecam-rec gives it
                let (y, angle) = if eye == 0 { (399.0 - y, (180.0 - f.angle_deg) % 180.0) } else { (y, f.angle_deg) };
                text += &format!("{name},{k},{t:.6},1,2.0,1.0,{x:.4},{y:.4},{:.4},{:.4},{angle:.4}\n", f.a, f.b);
            }
        }
        assert!(is_eyecam_replay(&text));
        let out = std::env::temp_dir().join(format!("frameeyeosc-eyeball-replay-{}.csv", std::process::id()));
        let report = replay_eyecam(&text, &cal, Some(&out)).unwrap();
        assert!(report.contains("eyeball left: 90 frames (0 before the start): start 1, nopupil 0"), "{report}");
        let rows = std::fs::read_to_string(&out).unwrap();
        std::fs::remove_file(&out).unwrap();
        let last: Vec<&str> = rows.lines().filter(|l| l.starts_with("L,")).last().unwrap().split(',').collect();
        let (yaw, pitch) = (last[4].parse::<f64>().unwrap(), last[5].parse::<f64>().unwrap());
        assert!((yaw - 8.0).abs() < 1.5 && (pitch + 4.0).abs() < 1.5, "{last:?}");
        // Both eyes the same: their outputs the same
        let right: Vec<&str> = rows.lines().filter(|l| l.starts_with("R,")).last().unwrap().split(',').collect();
        assert_eq!(last[3..], right[3..]);
        assert!(replay_eyecam("eye,eye_index,t_cam\n", &cal, None).unwrap_err().contains("ok_pupil"));
        // With eyecam-rec's outline columns the shape comes from them: the same frames with the pupil's own ellipse
        // spoilt (cut flat) and the outline as it was give the same rows
        let mut header = text.lines().next().unwrap().to_owned();
        header += ",outline_a,outline_b,outline_angle,outline_vis";
        let mut both = header + "\n";
        for line in text.lines().skip(1) {
            let cells: Vec<&str> = line.split(',').collect();
            let spoilt = [&cells[..8], &["20.0", "11.0", "90.0"]].concat().join(",");
            both += &format!("{spoilt},{},{},{},{}\n", cells[8], cells[9], cells[10], cells[5]);
        }
        let out2 = std::env::temp_dir().join(format!("frameeyeosc-eyeball-replay2-{}.csv", std::process::id()));
        replay_eyecam(&both, &cal, Some(&out2)).unwrap();
        let rows2 = std::fs::read_to_string(&out2).unwrap();
        std::fs::remove_file(&out2).unwrap();
        assert_eq!(rows2, rows);
    }

    /// Learns, then looks at (yaw, pitch) for a second; returns the time of the next frame.
    fn settled_at(filter: &mut EyeFilter, c: &Camera, yaw: f64, pitch: f64) -> f64 {
        let (t, _) = learn(filter, c, seen);
        run(filter, c, seen, t, 1.0, |_| (eye_at(c), yaw, pitch));
        t + 1.0
    }

    /// The model's frame at (yaw, pitch) with its ellipse turned a quarter and flattened (as a lid or glints cut it),
    /// its rim share `vis`.
    fn cut(c: &Camera, yaw: f64, pitch: f64, t: f64, vis: f64) -> RawFrame {
        let f = seen(c, eye_at(c), yaw, pitch, t);
        RawFrame { b: 0.6 * f.a, angle_deg: (f.angle_deg + 90.0) % 180.0, vis, ..f }
    }

    #[test]
    fn a_partly_hidden_rim_is_used_but_never_makes_the_filter_take_its_angles() {
        let c = camera();
        // A rim 90% seen: its ellipse is used (an update with it)...
        let mut filter = EyeFilter::default();
        let mut t = settled_at(&mut filter, &c, 8.0, 4.0);
        let mut frame = seen(&c, eye_at(&c), 8.0, 4.0, t);
        frame.vis = 0.9;
        assert_eq!(filter.step(&c, &observe(&c, &frame)).unwrap().update, Update::Ellipse);
        t += 1.0 / 90.0;
        // ...but one that disagrees, for a second, never re-takes the angles (the pupil keeps them)...
        let retakes = filter.retakes;
        for _ in 0..90 {
            filter.step(&c, &observe(&c, &cut(&c, 8.0, 4.0, t, 0.9)));
            t += 1.0 / 90.0;
        }
        assert_eq!(filter.retakes, retakes);
        let (yaw, pitch) = own(&filter);
        assert!((yaw - 8.0).abs() < 2.0 && (pitch - 4.0).abs() < 2.0, "{yaw} {pitch}");
        // ...nor counts towards it: right after them a whole rim that disagrees is believed only after ELL_REACQ frames
        // of its own (not at once)
        for k in 0..ELL_REACQ {
            filter.step(&c, &observe(&c, &cut(&c, 8.0, 4.0, t, 1.0)));
            t += 1.0 / 90.0;
            assert_eq!(filter.retakes, retakes + u32::from(k + 1 == ELL_REACQ), "frame {k}");
        }
    }

    #[test]
    fn refused_again_soon_or_at_the_angle_limit_the_filter_starts_over() {
        let c = camera();
        // At the angle limit: the next re-take is a fresh start from the frame
        let mut filter = EyeFilter::default();
        let t = settled_at(&mut filter, &c, 8.0, 4.0);
        let mut x = filter.x.unwrap();
        x[1] = -(ANG_MAX_DEG - 0.5).to_radians();
        filter.x = Some(x);
        filter.misses = REACQUIRE;
        let retakes = filter.retakes;
        let e = filter.step(&c, &observe(&c, &seen(&c, eye_at(&c), 8.0, 4.0, t))).unwrap();
        assert_eq!((e.update, filter.retakes), (Update::Start, retakes + 1));
        // Refused for REACQUIRE frames right after a re-take: started over; the first re-take only re-takes the angles
        let mut filter = EyeFilter::default();
        let mut t = settled_at(&mut filter, &c, 8.0, 4.0);
        filter.misses = REACQUIRE;
        let e = filter.step(&c, &observe(&c, &seen(&c, eye_at(&c), 8.0, 4.0, t))).unwrap();
        assert_ne!(e.update, Update::Start);
        t += 1.0 / 90.0;
        filter.misses = REACQUIRE;
        let e = filter.step(&c, &observe(&c, &seen(&c, eye_at(&c), 8.0, 4.0, t))).unwrap();
        assert_eq!(e.update, Update::Start);
        // ...but not once RESTART_S has gone by
        t += RESTART_S + 0.1;
        run(&mut filter, &c, seen, t, 0.5, |_| (eye_at(&c), 8.0, 4.0));
        t += 0.5;
        filter.misses = REACQUIRE;
        filter.step(&c, &observe(&c, &seen(&c, eye_at(&c), 8.0, 4.0, t)));
        t += RESTART_S + 0.1;
        filter.misses = REACQUIRE;
        let e = filter.step(&c, &observe(&c, &seen(&c, eye_at(&c), 8.0, 4.0, t))).unwrap();
        assert_ne!(e.update, Update::Start);
    }

    #[test]
    fn a_partly_hidden_rim_never_re_takes_even_just_after_a_saccade() {
        // Whole-rim frames had already disagreed ELL_REACQ_MOVING times (fewer than ELL_REACQ: no re-take yet); then
        // the eye jumps to (-12, -6), where the rim is partly hidden and the ellipse disagrees: the saccade lowers the
        // count needed, but a partly hidden ellipse still never makes the filter take its angles
        let c = camera();
        let mut filter = EyeFilter::default();
        let mut t = settled_at(&mut filter, &c, 8.0, 4.0);
        filter.ell_off = ELL_REACQ_MOVING;
        let retakes = filter.retakes;
        for k in 0..10 {
            filter.step(&c, &observe(&c, &cut(&c, -12.0, -6.0, t, 0.9)));
            if k == 0 {
                assert!(filter.saccade);
            }
            t += 1.0 / 90.0;
        }
        assert_eq!(filter.retakes, retakes);
    }

    #[test]
    fn just_after_a_saccade_a_disagreeing_ellipse_is_believed_sooner() {
        let c = camera();
        // Still: ELL_REACQ frames; within SACCADE_REACQ_S of a saccade frame (the jump to (-12, -6)): ELL_REACQ_MOVING
        for (jump, need) in [(false, ELL_REACQ), (true, ELL_REACQ_MOVING)] {
            let mut filter = EyeFilter::default();
            let mut t = settled_at(&mut filter, &c, 8.0, 4.0);
            let (yaw, pitch) = if jump { (-12.0, -6.0) } else { (8.0, 4.0) };
            let retakes = filter.retakes;
            let mut taken = None;
            for k in 0..30u32 {
                filter.step(&c, &observe(&c, &cut(&c, yaw, pitch, t, 1.0)));
                if k == 0 {
                    assert_eq!(filter.saccade, jump);
                }
                if taken.is_none() && filter.retakes > retakes {
                    taken = Some(k + 1);
                }
                t += 1.0 / 90.0;
            }
            assert_eq!(taken, Some(need), "jump {jump}");
        }
    }
}
