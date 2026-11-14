//! Recording the eye server's samples to CSV (--record), with the eye camera's values read with each, and running a
//! recording back through the same processing (--replay) to compare settings by a few numbers.

use crate::config::{LidFit, Settings};
use crate::eyecam_live::{CamShape, CamSlip, Eye, FrameGaps, Live};
use crate::{
    CAL_SETTLE, EyeData, EyeballSetup, LidCalibration, LidFrom, MAX_GAP, NOMINAL_DT, Sample, Saturation, Smoother,
    TIMEOUT, eyeball, gaze_angles, step,
};
use std::error::Error;
use std::fs::{self, File};
use std::io::{self, BufWriter, Write};
use std::path::Path;

// A sample counts as part of a blink while both eyes' Frame openness is below this.
const BLINK_BELOW: f32 = 0.5;
// ...and as open while both are at least this.
const OPEN_ABOVE: f32 = 0.6;
// Blinks this few samples apart are one blink.
const BLINK_MERGE: usize = 2;
// How long after a blink its eyelids may still be closing, given the filters' lag.
const BLINK_TAIL: f64 = 0.15;
// Sent eyelids (VRCFT) at or below this look closed on an avatar...
const LID_SHUT: f32 = 0.05;
// ...and at or above this visibly widened (40% of the way from relaxed open to full).
const LID_WIDE: f32 = 0.85;
// Gaze jumps are looked for this close to a sample where either eye is below BLINK_BELOW.
const BLINK_NEAR: f64 = 0.1;
// Flicker is only measured this far from any sample where either eye is below BLINK_BELOW.
const OPEN_CLEAR: f64 = 0.2;
// Gaze jitter is the spread within windows this long...
const JITTER_WINDOW: f64 = 0.3;
// ...where the tracker's own combined gaze spreads less than this many degrees, so the eyes are still
// and any movement in the output is jitter rather than the eyes looking somewhere else.
const FIXATION_SPREAD: f64 = 1.0;
// Gaze values of 1.0 are 45°.
const GAZE_DEGREES: f32 = 45.0;
// "Closing while looking down": the tracker's gaze is this many degrees down or more, the eye's reading
// is at least DOWN_OPEN_SHARE of its straight-ahead open reading, it is away from blinks, and the sent
// eyelid (VRCFT, relaxed = 0.75) is below DOWN_CLOSING: visibly a third closed.
const DOWN_DEGREES: f32 = 15.0;
const DOWN_OPEN_SHARE: f32 = 0.6;
const DOWN_CLOSING: f32 = 0.5;
// The sideways gaze is judged while the tracker's gaze is at least this far down, where the Frame's x has
// jumped (2026-09-28 recordings).
const FAR_DOWN_DEGREES: f32 = 32.0;
// Estimating a lid fit from a recording: where the live fit's 15° targets landed in the tracker's own
// gaze (+15.8° and -17.8° on 2026-09-28), straight ahead within 3°, and each needs this many samples
// with both eyes clearly open.
const FIT_ESTIMATE_UP: (f32, f32) = (12.0, 19.0);
const FIT_ESTIMATE_DOWN: (f32, f32) = (-21.0, -15.0);
const FIT_ESTIMATE_AHEAD: f32 = 3.0;
const FIT_ESTIMATE_OPEN: f32 = 0.5;
const FIT_ESTIMATE_SAMPLES: usize = 45;
// Eyes-shut stretches: both readings below this for at least this long; their first half second is skipped.
const FIT_ESTIMATE_SHUT: f32 = 0.45;
const FIT_ESTIMATE_SHUT_SECONDS: f64 = 1.0;

/// Column names of a recording, in the order `values` lists them.
fn columns() -> Vec<String> {
    let mut names = vec!["sample_time".to_owned()];
    let per_eye = |names: &mut Vec<String>, name: &str| {
        for eye in ["left", "right"] {
            names.extend(["x", "y", "z"].map(|axis| format!("{name}_{eye}_{axis}")));
        }
    };
    per_eye(&mut names, "gaze");
    per_eye(&mut names, "gaze_cov");
    names.extend(["x", "y", "z"].map(|axis| format!("fixation_{axis}")));
    per_eye(&mut names, "pre_gaze");
    per_eye(&mut names, "pre_cov");
    names.extend(["openness_left".to_owned(), "openness_right".to_owned()]);
    names.extend((0..8).map(|i| format!("extra_{i}")));
    names
}

/// Every value of a sample, in column order.
fn values(data: &EyeData) -> Vec<f64> {
    let mut values = vec![data.sample_time];
    let floats = [
        data.gaze.as_flattened(),
        data.gaze_covariance.as_flattened(),
        &data.fixation_point,
        data.pre_fusion_gaze.as_flattened(),
        data.pre_fusion_covariance.as_flattened(),
        &data.openness,
        &data.extra,
    ];
    values.extend(floats.into_iter().flatten().map(|value| f64::from(*value)));
    values
}

// Each eye's eye-camera columns (cam_l_*, cam_r_*): whether its values were fresh when read with the sample (and so
// could be used), eyecam-rec's values, and how long before that read the camera frame was taken.
const CAMERA_EYE_FIELDS: [&str; 10] =
    ["fresh", "valid", "closed", "lid", "wide", "squint", "pupil_mm", "pupil_dilation", "confidence", "age_ms"];

// A recording keeps how old each camera frame was when read (age_ms), not when it was taken, so the replay works the
// frame intervals out from the sample's time less that age. The same frame read with two samples comes out a little
// apart that way (at most 2.9 ms over the 2026-10-04 and 10-07 recordings, with frames 11.1 ms apart), so a time
// within this of the last frame's is that frame.
const SAME_FRAME_NS: u64 = 4_000_000;

// The last column of recordings made since eyecam names the eyes anatomically (after the eye camera's), "1" on rows with its values: cam_l_* is the
// left eye. The eyecam of 0.7.2-0.7.5 called the right eye's camera L, so in recordings without this column cam_l_* is
// the right eye and cam_r_* the left; they are read the other way round (camera_eyes_swapped). Only its name in the
// header counts.
const CAMERA_ANATOMICAL: &str = "cam_eyes_anatomical";

/// Names of the eye camera's columns, after the eye server's: each eye's (CAMERA_EYE_FIELDS), then the calibration
/// state and whether eyecam-rec was processing. Recordings from before there were any have none.
fn camera_columns() -> Vec<String> {
    let mut names: Vec<String> =
        ["l", "r"].iter().flat_map(|eye| CAMERA_EYE_FIELDS.map(|field| format!("cam_{eye}_{field}"))).collect();
    names.extend(["cam_calib_state".to_owned(), "cam_live".to_owned()]);
    names
}

/// The eye camera's cells of one row: its values as read at `now_ns` (CLOCK_MONOTONIC), or all empty without a live
/// file to read.
fn camera_fields(camera: Option<(&Live, u64)>) -> Vec<String> {
    let Some((live, now_ns)) = camera else {
        return vec![String::new(); camera_columns().len()];
    };
    let flag = |value: bool| u8::from(value).to_string();
    let mut fields = Vec::new();
    for (eye, fresh) in live.eyes.iter().zip(live.fresh) {
        fields.extend([flag(fresh), flag(eye.valid), flag(eye.closed)]);
        let floats = [eye.lid, eye.wide, eye.squint, eye.pupil_mm, eye.pupil_dilation, eye.confidence];
        fields.extend(floats.map(|value| value.to_string()));
        let age_ns = i128::from(now_ns) - i128::from(eye.time_ns);
        fields.push(format!("{:.1}", age_ns as f64 / 1e6));
    }
    fields.extend([live.calib_state.to_string(), flag(live.live)]);
    fields
}

// Each eye's glint columns (cam_l_glint_*, cam_r_glint_*), after CAMERA_ANATOMICAL: eyecam-rec's glint part as read
// with the sample (eyecam_live.rs CamSlip; what pupil_slip.rs and the gaze guard follow), empty while there is none.
// The frame's time and the template's are CLOCK_MONOTONIC ns as eyecam-rec wrote them (only compared with each other).
// Recordings from before have none, and replay without glints as before.
const GLINT_FIELDS: [&str; 11] = [
    "frame_ns",
    "pupil_x",
    "pupil_y",
    "tracked",
    "template",
    "template_ns",
    "x",
    "y",
    "matches",
    "since_good",
    "count",
];

/// Names of the glint columns, after CAMERA_ANATOMICAL.
fn glint_columns() -> Vec<String> {
    ["l", "r"].iter().flat_map(|eye| GLINT_FIELDS.map(|field| format!("cam_{eye}_glint_{field}"))).collect()
}

/// The glint cells of one row (in glint_columns order): each eye's glint part as read, empty without one.
fn glint_fields(camera: Option<&Live>) -> Vec<String> {
    let mut fields = Vec::new();
    for eye in 0..2 {
        match camera.and_then(|live| live.slip[eye]) {
            Some(slip) => {
                let pupil = |axis: usize| slip.pupil.map_or_else(String::new, |pupil| pupil[axis].to_string());
                let (template, template_ns) =
                    slip.template.map_or((String::new(), String::new()), |(id, at)| (id.to_string(), at.to_string()));
                fields.extend([
                    slip.time_ns.to_string(),
                    pupil(0),
                    pupil(1),
                    u8::from(slip.tracked).to_string(),
                    template,
                    template_ns,
                    slip.offset[0].to_string(),
                    slip.offset[1].to_string(),
                    slip.matches.to_string(),
                    slip.since_good.to_string(),
                    slip.glints.to_string(),
                ]);
            }
            None => fields.extend(std::iter::repeat_n(String::new(), GLINT_FIELDS.len())),
        }
    }
    fields
}

// Each eye's pupil shape columns (cam_l_shape_*, cam_r_shape_*), after the glint columns: eyecam-rec's shape part as
// read with the sample (eyecam_live.rs CamShape; what gaze_source "eyeball" follows, with the glint part's pupil
// centre and frame time), empty while there is none. Recordings from before have none, and replay without it.
const SHAPE_FIELDS: [&str; 6] = ["a", "b", "angle", "vis", "aperture", "flipped"];

/// Names of the shape columns, after the glint columns.
fn shape_columns() -> Vec<String> {
    ["l", "r"].iter().flat_map(|eye| SHAPE_FIELDS.map(|field| format!("cam_{eye}_shape_{field}"))).collect()
}

/// The shape cells of one row (in shape_columns order): each eye's shape part as read, empty without one.
fn shape_fields(camera: Option<&Live>) -> Vec<String> {
    let mut fields = Vec::new();
    for eye in 0..2 {
        match camera.and_then(|live| live.shape[eye]) {
            Some(shape) => {
                let float = |value: f32| if value.is_finite() { value.to_string() } else { String::new() };
                fields.extend([
                    float(shape.a),
                    float(shape.b),
                    float(shape.angle_deg),
                    float(shape.vis),
                    float(shape.aperture),
                    u8::from(shape.flipped).to_string(),
                ]);
            }
            None => fields.extend(std::iter::repeat_n(String::new(), SHAPE_FIELDS.len())),
        }
    }
    fields
}

/// One eye's shape part from its cells (in SHAPE_FIELDS order): None when `flipped` is empty (no shape part read);
/// an empty number is NaN, as eyecam-rec writes it without a pupil.
fn shape_from(cells: &[&str]) -> Option<CamShape> {
    let float = |i: usize| if cells[i].is_empty() { Some(f32::NAN) } else { cells[i].parse::<f32>().ok() };
    Some(CamShape {
        a: float(0)?,
        b: float(1)?,
        angle_deg: float(2)?,
        vis: float(3)?,
        aperture: float(4)?,
        flipped: cells[5].parse::<u8>().ok()? != 0,
    })
}

/// One eye's glint part from its cells (in GLINT_FIELDS order): None when they are empty or not numbers.
fn glint_from(cells: &[&str]) -> Option<CamSlip> {
    let int = |i: usize| cells[i].parse::<u64>().ok();
    let float = |i: usize| cells[i].parse::<f32>().ok();
    let pupil = match (float(1), float(2)) {
        (Some(x), Some(y)) => Some([x, y]),
        _ => None,
    };
    let template = match (int(4), int(5)) {
        (Some(id), Some(at)) => Some((u32::try_from(id).ok()?, at)),
        _ => None,
    };
    Some(CamSlip {
        time_ns: int(0)?,
        pupil,
        tracked: int(3)? != 0,
        template,
        offset: [float(6)?, float(7)?],
        matches: u32::try_from(int(8)?).ok()?,
        since_good: u32::try_from(int(9)?).ok()?,
        glints: u32::try_from(int(10)?).ok()?,
    })
}

/// The eye camera's values from a row's cells (in camera_columns order), as they were read for that sample, and each
/// eye's age_ms: whether each eye's were fresh is as recorded, not judged again. None when the cells are empty; Err
/// when they are not numbers.
fn camera_from(cells: &[&str]) -> Result<Option<(Live, [f64; 2])>, ()> {
    if cells.iter().all(|cell| cell.is_empty()) {
        return Ok(None);
    }
    let values = cells.iter().map(|cell| cell.parse::<f64>()).collect::<Result<Vec<_>, _>>().map_err(|_| ())?;
    let per_eye = CAMERA_EYE_FIELDS.len();
    let eye = |eye: usize| {
        let value = |field: usize| values[eye * per_eye + field];
        let eye = Eye {
            time_ns: 0,
            valid: value(1) != 0.0,
            closed: value(2) != 0.0,
            lid: value(3) as f32,
            wide: value(4) as f32,
            squint: value(5) as f32,
            pupil_mm: value(6) as f32,
            pupil_dilation: value(7) as f32,
            confidence: value(8) as f32,
        };
        (value(0) != 0.0, eye)
    };
    let [(fresh_left, left), (fresh_right, right)] = [eye(0), eye(1)];
    let live = Live {
        calib_state: values[2 * per_eye] as u32,
        recalib_suggested: false,
        live: values[2 * per_eye + 1] != 0.0,
        eyes: [left, right],
        fresh: [fresh_left, fresh_right],
        frame_ns: [0; 2],
        // (the glint and shape parts come from their own columns, see parse)
        slip: [None; 2],
        shape: [None; 2],
    };
    Ok(Some((live, [values[per_eye - 1], values[2 * per_eye - 1]])))
}

/// Each eye camera's frame interval as it would have been measured live (see FrameGaps), worked out from a recording's
/// sample times and the ages of the camera frames read with them (see SAME_FRAME_NS).
#[derive(Default)]
struct RecordedGaps {
    last_ns: [Option<u64>; 2],
    gaps: [FrameGaps; 2],
}

impl RecordedGaps {
    /// Note the frames read with the sample at `sample_time` (seconds) at `age_ms` old, and set `live`'s frame intervals.
    fn note(&mut self, live: &mut Live, sample_time: f64, age_ms: [f64; 2]) {
        for eye in 0..2 {
            let taken = sample_time * 1e9 - age_ms[eye] * 1e6;
            if taken.is_finite() && taken > 0.0 {
                let taken = taken as u64;
                let frame = match self.last_ns[eye] {
                    Some(last) if last.abs_diff(taken) < SAME_FRAME_NS => last,
                    _ => taken,
                };
                self.last_ns[eye] = Some(frame);
                self.gaps[eye].note(frame);
            }
            live.frame_ns[eye] = self.gaps[eye].frame_ns();
        }
    }
}

/// Diagnostics only (FEO_CAM_FPS, FEO_CAM_DELAY_MS): the recorded camera stream as a slower camera would have given it,
/// each eye's newest frame on a 1/fps grid taken at least `delay` before the sample, with its age and freshness to
/// match.
struct Resampler {
    interval: f64,
    delay: f64,
    // Per eye: recorded frames not yet due (taken, values, fresh), and the one being served.
    pending: [std::collections::VecDeque<(f64, Eye, bool)>; 2],
    current: [Option<(f64, Eye, bool)>; 2],
    last_taken: [Option<f64>; 2],
}

impl Resampler {
    fn from_env() -> Option<Self> {
        let read = |name: &str| std::env::var(name).ok().and_then(|value| value.parse::<f64>().ok());
        let fps = read("FEO_CAM_FPS").filter(|fps| *fps > 0.0);
        let delay = read("FEO_CAM_DELAY_MS").unwrap_or(0.0) / 1000.0;
        (fps.is_some() || delay > 0.0).then(|| Self {
            interval: fps.map_or(0.0, |fps| 1.0 / fps),
            delay,
            pending: Default::default(),
            current: [None; 2],
            last_taken: [None; 2],
        })
    }

    fn apply(&mut self, live: &mut Live, ages: &mut [f64; 2], sample_time: f64) {
        for eye in 0..2 {
            let taken = sample_time - ages[eye] / 1000.0;
            if taken.is_finite() && self.last_taken[eye].is_none_or(|last| (taken - last).abs() * 1e9 >= SAME_FRAME_NS as f64) {
                self.last_taken[eye] = Some(taken);
                self.pending[eye].push_back((taken, live.eyes[eye], live.fresh[eye]));
            }
            while let Some(&(due, values, fresh)) = self.pending[eye].front() {
                if due > sample_time - self.delay {
                    break;
                }
                self.pending[eye].pop_front();
                if self.current[eye].is_none_or(|(served, ..)| due - served >= self.interval - 0.002) {
                    self.current[eye] = Some((due, values, fresh));
                }
            }
            match self.current[eye] {
                Some((served, values, fresh)) => {
                    // The delay stands for processing lag (the values are late), not an older capture time
                    let age = sample_time - served - self.delay;
                    live.eyes[eye] = values;
                    live.fresh[eye] = fresh && live.live && age <= (2.0 * self.interval).max(0.1);
                    ages[eye] = age * 1000.0;
                }
                None => live.fresh[eye] = false,
            }
        }
    }
}

/// The sample `values` came from.
fn from_values(values: &[f64]) -> EyeData {
    let mut floats = values[1..].iter().map(|value| *value as f32);
    let mut next = || floats.next().unwrap_or(f32::NAN);
    let mut vector = || [next(), next(), next()];
    let gaze = [vector(), vector()];
    let gaze_covariance = [vector(), vector()];
    let fixation_point = vector();
    let pre_fusion_gaze = [vector(), vector()];
    let pre_fusion_covariance = [vector(), vector()];
    let openness = [next(), next()];
    let extra = std::array::from_fn(|_| next());
    EyeData {
        sample_time: values[0],
        gaze,
        gaze_covariance,
        fixation_point,
        pre_fusion_gaze,
        pre_fusion_covariance,
        openness,
        extra,
    }
}

/// Writes samples to a CSV file, one line each, with the eye camera's columns last.
pub struct Recorder {
    out: BufWriter<File>,
    pub count: u64,
    // Lines with the eye camera's values.
    pub camera_count: u64,
}

impl Recorder {
    pub fn create(path: &Path) -> io::Result<Self> {
        let mut out = BufWriter::new(File::create(path)?);
        writeln!(
            out,
            "{},{},{CAMERA_ANATOMICAL},{},{}",
            columns().join(","),
            camera_columns().join(","),
            glint_columns().join(","),
            shape_columns().join(",")
        )?;
        Ok(Self {
            out,
            count: 0,
            camera_count: 0,
        })
    }

    /// One sample, with the eye camera's values as read for it at `now_ns` (CLOCK_MONOTONIC), if there were any.
    pub fn write(&mut self, data: &EyeData, camera: Option<(&Live, u64)>) -> io::Result<()> {
        // Floats print in their shortest form that reads back exactly.
        let mut fields: Vec<String> = values(data)
            .iter()
            .enumerate()
            .map(|(i, value)| if i == 0 { value.to_string() } else { (*value as f32).to_string() })
            .collect();
        fields.extend(camera_fields(camera));
        fields.push(if camera.is_some() { "1" } else { "" }.to_owned());
        fields.extend(glint_fields(camera.map(|(live, _)| live)));
        fields.extend(shape_fields(camera.map(|(live, _)| live)));
        writeln!(self.out, "{}", fields.join(","))?;
        self.count += 1;
        self.camera_count += u64::from(camera.is_some());
        Ok(())
    }

    pub fn flush(&mut self) -> io::Result<()> {
        self.out.flush()
    }
}

/// One recorded sample, and the eye camera's values read with it (None while there was no live file, and in
/// recordings from before there were any).
#[derive(Clone, Copy, Debug, PartialEq)]
struct Row {
    data: EyeData,
    camera: Option<Live>,
}

/// Whether a recording's eye camera columns name the eyes the other way round: it has them, but not
/// CAMERA_ANATOMICAL (recorded with the eyecam of 0.7.2-0.7.5, whose L was the right eye).
fn camera_eyes_swapped(header: &[&str]) -> bool {
    let has = |name: &str| header.contains(&name);
    camera_columns().iter().all(|name| has(name)) && !has(CAMERA_ANATOMICAL)
}

/// Read a recording, and whether it has the eye camera's columns. Columns are found by name, so their order does not
/// matter and extra ones are ignored. The camera's values of a recording without CAMERA_ANATOMICAL are read with
/// cam_l_* and cam_r_* exchanged, so each reaches the eye it was of.
fn parse(text: &str) -> Result<(Vec<Row>, bool), String> {
    let mut lines = text.lines().enumerate().filter(|(_, line)| !line.trim().is_empty());
    let (_, header) = lines.next().ok_or("the file is empty")?;
    let header: Vec<&str> = header.split(',').map(str::trim).collect();
    let swapped = camera_eyes_swapped(&header);
    let position = |name: &String| header.iter().position(|column| column == name);
    let positions = columns()
        .iter()
        .map(|name| position(name).ok_or_else(|| format!("column {name} is missing")))
        .collect::<Result<Vec<_>, _>>()?;
    let camera_positions = camera_columns().iter().map(position).collect::<Option<Vec<_>>>();
    let glint_positions = glint_columns().iter().map(position).collect::<Option<Vec<_>>>();
    let shape_positions = shape_columns().iter().map(position).collect::<Option<Vec<_>>>();
    let mut gaps = RecordedGaps::default();
    let mut resampler = Resampler::from_env();
    let rows = lines
        .map(|(index, line)| {
            let not_numbers = || format!("line {}: not a row of numbers", index + 1);
            let fields: Vec<&str> = line.split(',').map(str::trim).collect();
            let values = positions
                .iter()
                .map(|position| fields.get(*position)?.parse::<f64>().ok())
                .collect::<Option<Vec<_>>>()
                .ok_or_else(not_numbers)?;
            let data = from_values(&values);
            let camera = match &camera_positions {
                Some(positions) => {
                    let cells: Vec<&str> =
                        positions.iter().map(|position| fields.get(*position).copied().unwrap_or("")).collect();
                    camera_from(&cells).map_err(|_| not_numbers())?.map(|(mut live, mut ages)| {
                        if let Some(positions) = &glint_positions {
                            let cells: Vec<&str> =
                                positions.iter().map(|position| fields.get(*position).copied().unwrap_or("")).collect();
                            let per_eye = GLINT_FIELDS.len();
                            live.slip = [0, 1].map(|eye| glint_from(&cells[eye * per_eye..(eye + 1) * per_eye]));
                        }
                        if let Some(positions) = &shape_positions {
                            let cells: Vec<&str> =
                                positions.iter().map(|position| fields.get(*position).copied().unwrap_or("")).collect();
                            let per_eye = SHAPE_FIELDS.len();
                            live.shape = [0, 1].map(|eye| shape_from(&cells[eye * per_eye..(eye + 1) * per_eye]));
                        }
                        if swapped {
                            live.shape.swap(0, 1);
                            live.slip.swap(0, 1);
                            live.eyes.swap(0, 1);
                            live.fresh.swap(0, 1);
                            ages.swap(0, 1);
                        }
                        if let Some(resampler) = &mut resampler {
                            resampler.apply(&mut live, &mut ages, data.sample_time);
                        }
                        gaps.note(&mut live, data.sample_time, ages);
                        live
                    })
                }
                None => None,
            };
            Ok(Row { data, camera })
        })
        .collect::<Result<Vec<_>, String>>()?;
    Ok((rows, camera_positions.is_some()))
}

/// Everything sent for each sample, as the live loop would have sent it, with the eye camera's values recorded with
/// it. The calibration settles and restarts on sample time the way the live loop does on the clock.
fn replay(samples: &[EyeData], cameras: &[Option<Live>], settings: &Settings, calibration: LidCalibration) -> Vec<Sample> {
    replay_with(samples, cameras, settings, calibration, None)
}

/// The same, with gaze_source "eyeball"'s calibration and maps taken from `eyeball` as a live run takes them up.
fn replay_with(
    samples: &[EyeData],
    cameras: &[Option<Live>],
    settings: &Settings,
    mut calibration: LidCalibration,
    eyeball: Option<&EyeballSetup>,
) -> Vec<Sample> {
    let mut smoother = Smoother::new(settings);
    if let Some(eyeball) = eyeball {
        eyeball.clone().prepare(settings, &mut smoother);
    }
    let mut since = None;
    let mut last: Option<f64> = None;
    samples
        .iter()
        .zip(cameras)
        .map(|(data, camera)| {
            let time = data.sample_time;
            if last.is_some_and(|last| time - last >= TIMEOUT.as_secs_f64()) {
                since = None;
            }
            last = Some(time);
            let settled = time - *since.get_or_insert(time) >= CAL_SETTLE.as_secs_f64();
            step(settings, &mut smoother, &mut calibration, data, settled, camera.as_ref())
        })
        .collect()
}

/// Diagnostics only (FEO_DOUBT_DIAG): how each eye's LidDoubt went over a recording, as report lines. Per eye: when
/// its eyelid switched to the camera's (state.camera), after how many eye-server closings since the doubt last
/// cleared and how long it stayed; the CameraOpen stretches, and how many began while the eye server read the eye
/// closing; how often the two did not agree while both read it open; and after each eye-server closing, how long the
/// camera's "closed" kept the eyelid at 0 (CameraClosed) and the sent eyelid stayed shut, with the camera and without.
fn doubt_diag(samples: &[EyeData], cameras: &[Option<Live>], settings: &Settings, calibration: &LidCalibration) -> String {
    let mut smoother = Smoother::new(settings);
    let mut cal = calibration.clone();
    let mut since = None;
    let mut last: Option<f64> = None;
    let mut sent = Vec::with_capacity(samples.len());
    let mut states = Vec::with_capacity(samples.len());
    for (data, camera) in samples.iter().zip(cameras) {
        let time = data.sample_time;
        if last.is_some_and(|last| time - last >= TIMEOUT.as_secs_f64()) {
            since = None;
        }
        last = Some(time);
        let settled = time - *since.get_or_insert(time) >= CAL_SETTLE.as_secs_f64();
        sent.push(step(settings, &mut smoother, &mut cal, data, settled, camera.as_ref()));
        states.push(smoother.lid_doubt);
    }
    let off = replay(samples, cameras, &Settings { camera_lids: false, ..settings.clone() }, calibration.clone());
    let times: Vec<f64> = samples.iter().map(|data| data.sample_time).collect();
    let start = times.first().copied().unwrap_or(0.0);
    let hours = (times.last().copied().unwrap_or(0.0) - start) / 3600.0;
    let mut text = format!("\nLidDoubt diagnostics ({hours:.3} h)\n");
    let stats = |values: &mut Vec<f64>| {
        let count = values.len();
        let positive = values.iter().filter(|value| **value > 0.0).count();
        if count == 0 {
            return "none".to_owned();
        }
        let total: f64 = values.iter().sum();
        format!(
            "n {count}, >0 {positive}, median {:.0} ms, p90 {:.0} ms, max {:.0} ms, total {:.1} s",
            percentile(values, 50.0) * 1000.0,
            percentile(values, 90.0) * 1000.0,
            percentile(values, 100.0) * 1000.0,
            total
        )
    };
    let n = samples.len();
    for eye in 0..2 {
        let mut flips: Vec<String> = Vec::new();
        let mut camera_seconds = 0.0;
        let mut closings_since_clear = 0;
        // Doubt brought into the current eye-server closing from earlier ones, and the closings that added to it
        let mut carried = 0.0;
        let mut adding = 0;
        let mut run_added = false;
        // Eye-server closings as (first sample, first sample after it)
        let mut runs: Vec<(usize, usize)> = Vec::new();
        let mut increments = Vec::new();
        let mut run_from = 0;
        let mut carried_flips = 0;
        let mut adding_at_flip = Vec::new();
        let mut clear_time = start;
        let mut flip_start: Option<f64> = None;
        let mut on_durations = Vec::new();
        let mut open_runs = 0;
        let mut open_runs_closing = 0;
        let mut closings = 0;
        let mut both_open = 0;
        let mut not_agree = 0;
        let mut max_doubt = 0.0_f64;
        let mut tails_closed = Vec::new();
        let mut tails_shut = Vec::new();
        let mut tails_shut_off = Vec::new();
        for i in 0..n {
            let state = states[i][eye];
            let previous = (i > 0).then(|| states[i - 1][eye]);
            let (_, closing, agree) = state.diag;
            let has_camera = sent[i].camera.squint[eye].is_some();
            let dt = if i > 0 { (times[i] - times[i - 1]).clamp(0.0, MAX_GAP) } else { 0.0 };
            max_doubt = max_doubt.max(state.doubted);
            if previous.is_some_and(|previous| previous.doubted > 0.0) && state.doubted == 0.0 {
                closings_since_clear = 0;
                adding = 0;
                clear_time = times[i];
            }
            let was_closing = previous.is_some_and(|previous| previous.diag.1);
            if closing && !was_closing {
                closings += 1;
                closings_since_clear += 1;
                carried = previous.map_or(0.0, |previous| previous.doubted);
                run_added = false;
                run_from = i;
            }
            if previous.is_some_and(|previous| state.doubted > previous.doubted) && !run_added {
                adding += 1;
                run_added = true;
            }
            if state.camera {
                camera_seconds += dt;
            }
            if state.camera && !previous.is_some_and(|previous| previous.camera) {
                flip_start = Some(times[i]);
                // Mostly brought in from earlier closings: the doubt added up over blinks, not one long closing
                if closing && carried >= 0.5 * state.doubted {
                    carried_flips += 1;
                    adding_at_flip.push(adding as f64);
                }
                flips.push(format!(
                    "{:.1} s (doubt {:.0} ms, {:.0} ms of it from earlier closings; {} closings added to it, {} closings since it last cleared {:.1} s ago)",
                    times[i] - start,
                    state.doubted * 1000.0,
                    carried * 1000.0,
                    adding,
                    closings_since_clear,
                    times[i] - clear_time
                ));
            }
            if !state.camera {
                if let Some(from) = flip_start.take() {
                    on_durations.push(times[i] - from);
                }
            }
            let camera_open = sent[i].lid_from[eye] == LidFrom::CameraOpen;
            if camera_open && (i == 0 || sent[i - 1].lid_from[eye] != LidFrom::CameraOpen) {
                open_runs += 1;
                if closing {
                    open_runs_closing += 1;
                }
            }
            if has_camera && !closing && sent[i].lid_from[eye] != LidFrom::CameraClosed {
                both_open += 1;
                if !agree {
                    not_agree += 1;
                }
            }
            // The eye server reopened: how long the camera's "closed" kept it at 0, and the sent eyelid stayed shut
            if was_closing && !closing {
                runs.push((run_from, i));
                let added = previous.map_or(0.0, |previous| previous.doubted) - carried;
                if added > 0.0 {
                    increments.push(added);
                }
                let run = |shut: &dyn Fn(usize) -> bool| {
                    let mut j = i;
                    while j < n && shut(j) && (j == i || times[j] - times[j - 1] < MAX_GAP) {
                        j += 1;
                    }
                    if j > i { times[j.min(n - 1)] - times[i] } else { 0.0 }
                };
                tails_closed.push(run(&|j| sent[j].lid_from[eye] == LidFrom::CameraClosed));
                tails_shut.push(run(&|j| sent[j].lids[eye] <= LID_SHUT));
                tails_shut_off.push(run(&|j| off[j].lids[eye] <= LID_SHUT));
            }
        }
        if let Some(from) = flip_start {
            on_durations.push(times[n - 1] - from);
        }
        // Blinks (closings up to half a second): whether the sent eyelid got shut (by BLINK_TAIL after), with the camera
        // and without; how late it got shut; and whether the eye was on the camera's eyelid then
        let mut blinks = 0;
        let mut missed = [0; 2];
        let mut lost = 0;
        let mut lost_on_camera = 0;
        let mut on_camera_blinks = 0;
        let mut late = [Vec::new(), Vec::new()];
        for &(from, to) in &runs {
            if times[to] - times[from] > 0.5 {
                continue;
            }
            blinks += 1;
            let shut_at = |lids: &dyn Fn(usize) -> f32| {
                (from..n).take_while(|j| times[*j] <= times[to] + BLINK_TAIL).find(|j| lids(*j) <= LID_SHUT)
            };
            let shut = [shut_at(&|j| sent[j].lids[eye]), shut_at(&|j| off[j].lids[eye])];
            let on_camera = (from..to).any(|j| states[j][eye].camera);
            on_camera_blinks += usize::from(on_camera);
            for k in 0..2 {
                match shut[k] {
                    Some(j) => late[k].push(times[j] - times[from]),
                    None => missed[k] += 1,
                }
            }
            if shut[0].is_none() && shut[1].is_some() {
                lost += 1;
                lost_on_camera += usize::from(on_camera);
            }
        }
        let [late_on, late_off] = &mut late;
        let side = ["L", "R"][eye];
        text.push_str(&format!(
            "  {side}: switches to the camera's eyelid {} ({:.1}/h); on it {:.1} s ({:.1}%); max doubt {:.0} ms\n     stretches on it: {}\n",
            flips.len(),
            flips.len() as f64 / hours.max(1e-9),
            camera_seconds,
            camera_seconds * 100.0 / (hours * 3600.0).max(1e-9),
            max_doubt * 1000.0,
            stats(&mut on_durations),
        ));
        text.push_str(&format!(
            "     of them mostly carried over from earlier closings: {carried_flips} (closings adding to it: median {:.0})\n",
            median(&mut adding_at_flip)
        ));
        for flip in &flips {
            text.push_str(&format!("     at {flip}\n"));
        }
        text.push_str(&format!(
            "     CameraOpen stretches {open_runs}, {open_runs_closing} begun while the eye server read it closing; eye-server closings {closings}\n"
        ));
        text.push_str(&format!(
            "     both reading it open, not agreeing: {:.1}% of {both_open} samples\n",
            not_agree as f64 * 100.0 / both_open.max(1) as f64
        ));
        text.push_str(&format!(
            "     blinks {blinks} ({on_camera_blinks} on the camera's eyelid): not sent shut {} with camera, {} without; lost by the camera {lost} ({lost_on_camera} of them on its eyelid)\n",
            missed[0], missed[1]
        ));
        text.push_str(&format!("     doubt added per closing that added any: {}\n", stats(&mut increments)));
        text.push_str(&format!("     blink shut after its start (with camera): {}\n", stats(late_on)));
        text.push_str(&format!("     blink shut after its start (camera off):  {}\n", stats(late_off)));
        text.push_str(&format!("     after reopening, camera closed held at 0: {}\n", stats(&mut tails_closed)));
        text.push_str(&format!("     after reopening, sent shut (with camera):  {}\n", stats(&mut tails_shut)));
        text.push_str(&format!("     after reopening, sent shut (camera off):   {}\n", stats(&mut tails_shut_off)));
    }
    text
}

/// The settings with this version's new stages turned off, to compare against.
fn without_new_stages(settings: &Settings) -> Settings {
    Settings {
        gaze_quality_limit: 0.0,
        blink_hold_ms: 0.0,
        despike: false,
        blink_sync_below: 0.0,
        gaze_down_hold_x_deg: 0.0,
        ..settings.clone()
    }
}

/// How the output behaved over a recording.
#[derive(Debug)]
struct Metrics {
    // Blinks where both eyes' Frame openness went below BLINK_BELOW together.
    blinks: usize,
    // ...of which both sent eyelids reached LID_SHUT at the same time.
    blinks_shut: usize,
    // Median over all blinks of how long both sent eyelids stayed shut together, in ms.
    shut_ms: f64,
    // Median spread of the sent combined gaze within JITTER_WINDOW windows with the eyes open and still,
    // in degrees.
    jitter: f64,
    // 90th percentile of the sent combined gaze's change per sample around blinks, in degrees.
    blink_jump: f64,
    // Mean change of the sent eyelids per sample while the eyes are clearly open, in VRCFT units.
    flicker: f64,
    // Share of samples where each eye's gaze failed the quality check.
    unreliable: [f64; 2],
    // Stretches (per eye) where an eye looking down and clearly open, away from blinks, was sent as closing.
    down_closes: usize,
    // Median sent eyelid (VRCFT, both eyes) with the eyes open and away from blinks, looking down and ahead.
    lid_down: f64,
    lid_ahead: f64,
    // Median |sent combined x| in degrees while looking FAR_DOWN_DEGREES down or more.
    far_down_x: f64,
    // In the same fixation windows as `jitter`: the median spread of each eye's sent gaze, and the
    // median left minus right sent x (vergence; positive when the eyes turn toward each other), in degrees.
    eye_jitter: f64,
    vergence: f64,
    // Stretches where intentional cross-eye was let through: seconds from the start, and how long.
    cross_eye: Vec<(f64, f64)>,
}

fn median(values: &mut [f64]) -> f64 {
    percentile(values, 50.0)
}

fn percentile(values: &mut [f64], percent: f64) -> f64 {
    if values.is_empty() {
        return f64::NAN;
    }
    values.sort_by(f64::total_cmp);
    values[((values.len() - 1) as f64 * percent / 100.0).round() as usize]
}

/// For each sample, whether one of the `marked` samples is at most `within` seconds away.
fn near(times: &[f64], marked: &[bool], within: f64) -> Vec<bool> {
    let mut out = vec![false; times.len()];
    let mut last = f64::NEG_INFINITY;
    for i in 0..times.len() {
        if marked[i] {
            last = times[i];
        }
        out[i] = times[i] - last <= within;
    }
    let mut next = f64::INFINITY;
    for i in (0..times.len()).rev() {
        if marked[i] {
            next = times[i];
        }
        out[i] |= next - times[i] <= within;
    }
    out
}

fn metrics(samples: &[EyeData], sent: &[Sample]) -> Metrics {
    let times: Vec<f64> = samples.iter().map(|data| data.sample_time).collect();
    let n = samples.len();
    // Whether sample i follows sample i - 1 without a gap.
    let joined = |i: usize| i > 0 && times[i] - times[i - 1] < MAX_GAP;
    let both_blinking: Vec<bool> = samples
        .iter()
        .map(|data| data.openness.iter().all(|openness| *openness < BLINK_BELOW))
        .collect();
    let either_blinking: Vec<bool> = samples
        .iter()
        .map(|data| data.openness.iter().any(|openness| *openness < BLINK_BELOW))
        .collect();
    let both_shut: Vec<bool> = sent
        .iter()
        .map(|sample| sample.lids.iter().all(|lid| *lid <= LID_SHUT))
        .collect();

    // Blinks as runs of both_blinking samples, allowing short breaks.
    let mut blinks: Vec<(usize, usize)> = Vec::new();
    for i in (0..n).filter(|i| both_blinking[*i]) {
        match blinks.last_mut() {
            Some((_, end)) if i - *end <= BLINK_MERGE + 1 && (*end + 1..=i).all(joined) => *end = i,
            _ => blinks.push((i, i)),
        }
    }
    let mut shut_ms: Vec<f64> = blinks
        .iter()
        .map(|&(start, end)| {
            // The longest stretch of both eyelids sent shut, from the blink's start until its tail is over.
            let mut longest = 0.0_f64;
            let mut run_start = None;
            let mut i = start;
            while i < n && times[i] <= times[end] + BLINK_TAIL && (i == start || joined(i)) {
                match (both_shut[i], run_start) {
                    (true, None) => run_start = Some(times[i]),
                    (false, Some(from)) => {
                        longest = longest.max(times[i] - from);
                        run_start = None;
                    }
                    _ => {}
                }
                i += 1;
            }
            if let Some(from) = run_start {
                longest = longest.max(times[i - 1] - from + f64::from(NOMINAL_DT));
            }
            longest * 1000.0
        })
        .collect();
    let blinks_shut = shut_ms.iter().filter(|ms| **ms > 0.0).count();

    let degrees = |sample: &Sample| [sample.gaze[4], sample.gaze[5]].map(|value| f64::from(value * GAZE_DEGREES));
    let raw_degrees =
        |sample: &Sample| [sample.raw_gaze[4], sample.raw_gaze[5]].map(|value| f64::from(value * GAZE_DEGREES));
    let spread = |window: &[usize], gaze: &dyn Fn(&Sample) -> [f64; 2]| {
        let points: Vec<[f64; 2]> = window.iter().map(|i| gaze(&sent[*i])).collect();
        let count = points.len() as f64;
        let variance: f64 = (0..2)
            .map(|axis| {
                let mean = points.iter().map(|point| point[axis]).sum::<f64>() / count;
                points.iter().map(|point| (point[axis] - mean).powi(2)).sum::<f64>() / count
            })
            .sum();
        variance.sqrt()
    };
    let eye_degrees = |eye: usize| {
        move |sample: &Sample| [sample.gaze[eye * 2], sample.gaze[eye * 2 + 1]].map(|value| f64::from(value * GAZE_DEGREES))
    };
    let mut spreads = Vec::new();
    let mut eye_spreads = Vec::new();
    let mut vergences = Vec::new();
    let mut window: Vec<usize> = Vec::new();
    let mut window_start = 0.0;
    for i in 0..n {
        let open = samples[i].openness.iter().all(|openness| *openness >= OPEN_ABOVE);
        if !open || !joined(i) {
            window.clear();
        }
        if !open {
            continue;
        }
        if window.is_empty() {
            window_start = times[i];
        }
        window.push(i);
        if times[i] - window_start >= JITTER_WINDOW {
            if spread(&window, &raw_degrees) < FIXATION_SPREAD {
                spreads.push(spread(&window, &degrees));
                eye_spreads.push(spread(&window, &eye_degrees(0)));
                eye_spreads.push(spread(&window, &eye_degrees(1)));
                vergences.extend(window.iter().map(|i| f64::from((sent[*i].gaze[0] - sent[*i].gaze[2]) * GAZE_DEGREES)));
            }
            window.clear();
        }
    }

    let around_blinks = near(&times, &either_blinking, BLINK_NEAR);
    let mut jumps: Vec<f64> = (1..n)
        .filter(|i| around_blinks[*i] && joined(*i))
        .map(|i| {
            let [x0, y0] = degrees(&sent[i - 1]);
            let [x1, y1] = degrees(&sent[i]);
            (x1 - x0).hypot(y1 - y0)
        })
        .collect();

    let blink_nearby = near(&times, &either_blinking, OPEN_CLEAR);
    let calm = |i: usize| {
        !blink_nearby[i] && samples[i].openness.iter().all(|openness| *openness >= OPEN_ABOVE)
    };
    let changes: Vec<f64> = (1..n)
        .filter(|i| joined(*i) && calm(*i) && calm(*i - 1))
        .flat_map(|i| (0..2).map(move |eye| f64::from((sent[i].lids[eye] - sent[i - 1].lids[eye]).abs())))
        .collect();

    let unreliable = [0, 1].map(|eye| {
        sent.iter().filter(|sample| !sample.reliable[eye]).count() as f64 / n.max(1) as f64
    });

    let reference = open_reference(samples);
    let down_limit = -DOWN_DEGREES / GAZE_DEGREES;
    let ahead_limit = FIT_ESTIMATE_AHEAD / GAZE_DEGREES;
    let settled_open = |i: usize| {
        !blink_nearby[i] && (0..2).all(|eye| samples[i].openness[eye] >= DOWN_OPEN_SHARE * reference[eye])
    };
    let lid_median = |keep: &dyn Fn(f32) -> bool| {
        let mut lids: Vec<f64> = (0..n)
            .filter(|i| settled_open(*i) && keep(gaze_angles(samples[*i].fixation_point)[1]))
            .map(|i| f64::from(sent[i].lids[0] + sent[i].lids[1]) / 2.0)
            .collect();
        median(&mut lids)
    };
    let far_down = -FAR_DOWN_DEGREES / GAZE_DEGREES;
    let mut far_down_x: Vec<f64> = (0..n)
        .filter(|i| gaze_angles(samples[*i].fixation_point)[1] <= far_down)
        .map(|i| f64::from((sent[i].gaze[4] * GAZE_DEGREES).abs()))
        .collect();
    let far_down_x = median(&mut far_down_x);
    let lid_down = lid_median(&|vertical| vertical <= down_limit);
    let lid_ahead = lid_median(&|vertical| vertical.abs() <= ahead_limit);
    let mut down_closes = 0;
    for (eye, reference) in reference.into_iter().enumerate() {
        let mut last: Option<usize> = None;
        for i in 0..n {
            let looking_down = gaze_angles(samples[i].fixation_point)[1] <= down_limit;
            let open = samples[i].openness[eye] >= DOWN_OPEN_SHARE * reference;
            // Away from blinks, so a blink's tail (held shut, then reopening) does not count
            if looking_down && open && !blink_nearby[i] && sent[i].lids[eye] < DOWN_CLOSING {
                if last.is_none_or(|last| i - last > BLINK_MERGE + 1) {
                    down_closes += 1;
                }
                last = Some(i);
            }
        }
    }
    Metrics {
        blinks: blinks.len(),
        blinks_shut,
        shut_ms: median(&mut shut_ms),
        jitter: median(&mut spreads),
        blink_jump: percentile(&mut jumps, 90.0),
        flicker: changes.iter().sum::<f64>() / changes.len().max(1) as f64,
        unreliable,
        down_closes,
        lid_down,
        lid_ahead,
        far_down_x,
        eye_jitter: median(&mut eye_spreads),
        vergence: median(&mut vergences),
        cross_eye: cross_eye_stretches(&times, sent),
    }
}

/// Where intentional cross-eye was let through: (seconds from the start, how long) for each stretch.
fn cross_eye_stretches(times: &[f64], sent: &[Sample]) -> Vec<(f64, f64)> {
    let mut stretches = Vec::new();
    let mut start: Option<usize> = None;
    for i in 0..=sent.len() {
        let on = i < sent.len() && sent[i].cross_eye;
        match (on, start) {
            (true, None) => start = Some(i),
            (false, Some(from)) => {
                stretches.push((times[from] - times[0], times[i - 1] - times[from] + f64::from(NOMINAL_DT)));
                start = None;
            }
            _ => {}
        }
    }
    stretches
}

/// The median of each eye's openness over the samples that `keep` picks; None with too few.
fn median_openness(samples: &[EyeData], keep: impl Fn(usize) -> bool) -> Option<[f32; 2]> {
    let picked: Vec<usize> = (0..samples.len()).filter(|i| keep(*i)).collect();
    if picked.len() < FIT_ESTIMATE_SAMPLES {
        return None;
    }
    Some([0, 1].map(|eye| {
        let mut values: Vec<f64> = picked.iter().map(|i| f64::from(samples[*i].openness[eye])).collect();
        median(&mut values) as f32
    }))
}

/// Each eye's straight-ahead open reading, to judge "still open" by (1.0 when it can't be told).
fn open_reference(samples: &[EyeData]) -> [f32; 2] {
    let ahead = FIT_ESTIMATE_AHEAD / GAZE_DEGREES;
    median_openness(samples, |i| {
        gaze_angles(samples[i].fixation_point)[1].abs() <= ahead
            && samples[i].openness.iter().all(|openness| *openness > FIT_ESTIMATE_OPEN)
    })
    .unwrap_or([1.0; 2])
}

/// A lid fit guessed from a recording that has looking up, straight ahead and down, and the eyes held
/// shut for a while: the same readings the panel's eye fit measures. None if something is missing.
fn estimate_lid_fit(samples: &[EyeData]) -> Option<[LidFit; 2]> {
    let vertical: Vec<f32> = samples.iter().map(|data| gaze_angles(data.fixation_point)[1]).collect();
    let open = |i: usize| samples[i].openness.iter().all(|openness| *openness > FIT_ESTIMATE_OPEN);
    let within = |i: usize, low: f32, high: f32| (low / GAZE_DEGREES..=high / GAZE_DEGREES).contains(&vertical[i]);
    let up = median_openness(samples, |i| open(i) && within(i, FIT_ESTIMATE_UP.0, FIT_ESTIMATE_UP.1))?;
    let ahead = median_openness(samples, |i| open(i) && within(i, -FIT_ESTIMATE_AHEAD, FIT_ESTIMATE_AHEAD))?;
    let down = median_openness(samples, |i| open(i) && within(i, FIT_ESTIMATE_DOWN.0, FIT_ESTIMATE_DOWN.1))?;
    // Eyes-shut stretches long enough to be on purpose, not blinks
    let shut = |data: &EyeData| data.openness.iter().all(|openness| *openness < FIT_ESTIMATE_SHUT);
    let mut in_shut = vec![false; samples.len()];
    let mut start = None;
    for i in 0..=samples.len() {
        match (samples.get(i).is_some_and(shut), start) {
            (true, None) => start = Some(i),
            (false, Some(from)) => {
                let times = |j: usize| samples[j].sample_time;
                if times(i - 1) - times(from) >= FIT_ESTIMATE_SHUT_SECONDS {
                    (from..i).filter(|j| times(*j) - times(from) >= 0.5).for_each(|j| in_shut[j] = true);
                }
                start = None;
            }
            _ => {}
        }
    }
    let closed = median_openness(samples, |i| in_shut[i])?;
    Some([0, 1].map(|eye| LidFit {
        closed: closed[eye],
        up: up[eye],
        open: ahead[eye],
        down: down[eye],
    }))
}

/// Whether and from when the live loop would have reported the openness as saturated (status.json's
/// openness_saturated), as a report line.
fn saturation_line(samples: &[EyeData]) -> String {
    let mut saturation = Saturation::default();
    let mut first = None;
    let mut on = 0;
    for data in samples {
        saturation.add(data);
        if saturation.on {
            on += 1;
            first.get_or_insert(data.sample_time);
        }
    }
    let start = samples.first().map_or(0.0, |data| data.sample_time);
    match first {
        Some(time) => format!(
            "Openness saturated (relaxed open eyes read 1.000, so widening can't come through): from {:.1} s in, \
             {:.1}% of the samples\n",
            time - start,
            on as f64 * 100.0 / samples.len() as f64
        ),
        None => "Openness not saturated (widening can come through)\n".to_owned(),
    }
}

/// p50 / p90 / p99 of each eye's larger x/y variance in one of the covariance fields.
fn covariance_line(samples: &[EyeData], label: &str, field: fn(&EyeData) -> [[f32; 3]; 2], limit: f32) -> String {
    let eyes = [0, 1].map(|eye| {
        let mut values: Vec<f64> = samples
            .iter()
            .map(|data| {
                let [x, y, _] = field(data)[eye];
                f64::from(x.max(y))
            })
            .collect();
        let above = values.iter().filter(|value| **value > f64::from(limit)).count() as f64
            / values.len().max(1) as f64;
        format!(
            "{:.4} / {:.4} / {:.4} ({:.1}% above)",
            percentile(&mut values, 50.0),
            percentile(&mut values, 90.0),
            percentile(&mut values, 99.0),
            above * 100.0
        )
    });
    format!("  {label:<22} L {}\n  {:<22} R {}", eyes[0], "", eyes[1])
}

fn report(input: &Path, samples: &[EyeData], skipped: usize, settings: &Settings, before: &Metrics, after: &Metrics) -> String {
    let span = match (samples.first(), samples.last()) {
        (Some(first), Some(last)) => last.sample_time - first.sample_time,
        _ => 0.0,
    };
    let mut text = format!(
        "{}: {} samples over {span:.1} s{}\n\n",
        input.display(),
        samples.len(),
        if skipped > 0 { format!(" ({skipped} unreadable samples skipped)") } else { String::new() }
    );
    let row = |text: &mut String, label: &str, before: String, after: String| {
        text.push_str(&format!("{label:<44}{before:>16}{after:>16}\n"));
    };
    row(&mut text, "", "new stages off".into(), "these settings".into());
    let blinks = format!("both-eye blinks fully closed (of {})", after.blinks);
    row(&mut text, &blinks, before.blinks_shut.to_string(), after.blinks_shut.to_string());
    row(&mut text, "  median time fully closed (ms)", format!("{:.0}", before.shut_ms), format!("{:.0}", after.shut_ms));
    row(&mut text, "gaze jitter while fixating (deg)", format!("{:.3}", before.jitter), format!("{:.3}", after.jitter));
    row(&mut text, "gaze jump around blinks, p90 (deg)", format!("{:.3}", before.blink_jump), format!("{:.3}", after.blink_jump));
    row(&mut text, "eyelid flicker while open (VRCFT/sample)", format!("{:.4}", before.flicker), format!("{:.4}", after.flicker));
    let [left, right] = after.unreliable.map(|share| share * 100.0);
    row(&mut text, "gaze left out as unreliable (L / R)", "-".into(), format!("{left:.1}% / {right:.1}%"));
    let lids = |m: &Metrics| format!("{:.2} / {:.2}", m.lid_down, m.lid_ahead);
    row(&mut text, "eyelid looking down / ahead (median)", lids(before), lids(after));
    row(&mut text, "closing while looking down (stretches)", before.down_closes.to_string(), after.down_closes.to_string());
    row(&mut text, "each eye's jitter while fixating (deg)", format!("{:.3}", before.eye_jitter), format!("{:.3}", after.eye_jitter));
    row(&mut text, "left - right while fixating (median deg)", format!("{:+.2}", before.vergence), format!("{:+.2}", after.vergence));
    let crossed = |m: &Metrics| format!("{} ({:.1} s)", m.cross_eye.len(), m.cross_eye.iter().map(|(_, long)| long).sum::<f64>() + 0.0);
    row(&mut text, "cross-eye let through (times, total)", crossed(before), crossed(after));
    if !after.cross_eye.is_empty() {
        let shown: Vec<String> =
            after.cross_eye.iter().take(8).map(|(at, long)| format!("{at:.1} s for {long:.2} s")).collect();
        text.push_str(&format!("  cross-eye at {}\n", shown.join(", ")));
    }
    let far = |m: &Metrics| format!("{:.1}", m.far_down_x);
    row(&mut text, "sideways gaze looking 32°+ down (median deg)", far(before), far(after));
    match estimate_lid_fit(samples) {
        Some(fits) => {
            text.push_str("\nLid fit read from this recording (as config.json keys):\n ");
            for (fit, side) in fits.iter().zip(["left", "right"]) {
                let values = [("closed", fit.closed), ("up", fit.up), ("open", fit.open), ("down", fit.down)];
                for (name, value) in values {
                    text.push_str(&format!(" \"lid_fit_{name}_{side}\": {value:.3},"));
                }
            }
            text.pop();
            text.push('\n');
        }
        None => text.push_str("\nNo lid fit can be read from this recording (it needs up, ahead, down and eyes shut).\n"),
    }
    text.push('\n');
    text.push_str(&saturation_line(samples));
    text.push_str(&format!(
        "\nCovariance, larger of x/y per eye: p50 / p90 / p99 (share above gaze_quality_limit {})\n",
        settings.gaze_quality_limit
    ));
    let limit = settings.gaze_quality_limit;
    text.push_str(&covariance_line(samples, "after fusion", |data| data.gaze_covariance, limit));
    text.push('\n');
    text.push_str(&covariance_line(samples, "before fusion (used)", |data| data.pre_fusion_covariance, limit));
    text.push_str("\n\nextra_0..7 medians:");
    for i in 0..8 {
        let mut values: Vec<f64> = samples.iter().map(|data| f64::from(data.extra[i])).collect();
        text.push_str(&format!(" {:.4}", median(&mut values)));
    }
    text.push('\n');
    text
}

/// How the eye camera's values were used over a recording that has them, as report lines: how often there were
/// values, fresh and with a baseline, how often each eye's eyelid and squint came from them, how often its eyelid
/// overrode the eye server's (the eye seen open while the eye server read it closing, seen closed, widened below the
/// eye server's relaxed open, narrowed on the camera while the eye server's reading jumped to 1.000, or raised to the
/// camera's where the eye server read the open eye lower), for how long its "closed" was taken as open because the eye
/// server read the eye open and was sure of it (Sample::closed_overruled; time by the samples' own, `samples`), how
/// often each eye went out visibly widened (LID_WIDE) with them (`with`) and without them (`without`, camera_lids off),
/// and how much of the recording a gaze capture could use (Sample::capture_usable), with them and without, and how
/// often an eye counted open for it by its pupil alone, its eyelid not the camera's (Sample::pupil_open).
fn camera_lines(cameras: &[Option<Live>], samples: &[EyeData], with: &[Sample], without: &[Sample]) -> String {
    let n = cameras.len().max(1) as f64;
    let share = |count: usize| count as f64 * 100.0 / n;
    let read: Vec<&Live> = cameras.iter().flatten().collect();
    let fresh = [0, 1].map(|eye| share(read.iter().filter(|live| live.fresh[eye]).count()));
    let baseline = share(read.iter().filter(|live| live.baseline()).count());
    let used = [0, 1].map(|eye| share(with.iter().filter(|sample| sample.camera.squint[eye].is_some()).count()));
    let from = |from: LidFrom| {
        let [left, right] = [0, 1].map(|eye| share(with.iter().filter(|sample| sample.lid_from[eye] == from).count()));
        format!("{left:.1}% / {right:.1}%")
    };
    // Each sample counts for the time since the one before it (a sample after a gap, or the first, for NOMINAL_DT)
    let overruled = [0, 1].map(|eye| {
        let mut seconds = 0.0;
        for (i, sample) in with.iter().enumerate() {
            if sample.closed_overruled[eye] {
                let since = i.checked_sub(1).map(|before| samples[i].sample_time - samples[before].sample_time);
                seconds += since.filter(|since| (0.0..MAX_GAP).contains(since)).unwrap_or(f64::from(NOMINAL_DT));
            }
        }
        seconds
    });
    let wide = |sent: &[Sample]| {
        let [left, right] = [0, 1].map(|eye| share(sent.iter().filter(|sample| sample.lids[eye] >= LID_WIDE).count()));
        format!("{left:.1}% / {right:.1}%")
    };
    // The camera's "closed" held back by the close gate: the share of samples, and how many times it started
    let held = {
        let [left, right] = [0, 1].map(|eye| {
            let samples = with.iter().filter(|sample| sample.close_held[eye]).count();
            let starts = with.windows(2).filter(|pair| pair[1].close_held[eye] && !pair[0].close_held[eye]).count();
            format!("{:.1}% ({starts}x)", share(samples))
        });
        format!("{left} / {right}")
    };
    let usable = |sent: &[Sample]| share(sent.iter().filter(|sample| sample.capture_usable()).count());
    let pupil_only = [0, 1].map(|eye| {
        share(with.iter().filter(|sample| sample.pupil_open[eye] && sample.lid_from[eye] != LidFrom::CameraOpen).count())
    });
    format!(
        "\nEye camera (eyecam-rec): values with {:.1}% of the samples; fresh (L / R) {:.1}% / {:.1}%; \
         with a baseline for the wear {baseline:.1}%\n  \
         eyelid and squint from it (L / R)          {:.1}% / {:.1}%\n  \
         eyelid from it over the eye server's (L / R): seen open {}, seen closed {}, widened {}, at 1.000 {}, \
         raised to it {}\n  \
         its closed taken as open, the eye server sure (L / R)  {:.1} s / {:.1} s\n  \
         its \"closed\" held back, not a blink or a wink yet (L / R): {held}\n  \
         sent widened, VRCFT {LID_WIDE} or more (L / R)    {} with it, {} without it\n  \
         usable for a gaze capture                  {:.1}% with it, {:.1}% without it; \
         open by its pupil alone (L / R) {:.1}% / {:.1}%\n",
        share(read.len()),
        fresh[0],
        fresh[1],
        used[0],
        used[1],
        from(LidFrom::CameraOpen),
        from(LidFrom::CameraClosed),
        from(LidFrom::CameraWidened),
        from(LidFrom::CameraAtCeiling),
        from(LidFrom::CameraRaised),
        overruled[0],
        overruled[1],
        wide(with),
        wide(without),
        usable(with),
        usable(without),
        pupil_only[0],
        pupil_only[1],
    )
}

/// Every processed sample: Frame openness, the gaze and VRCFT eyelids as sent, and the quality check. A recording
/// with the eye camera's columns also gets, per eye, whether its eyelid could come from the camera, and the squint,
/// pupil dilation and pupil diameter (mm) as sent (empty where they were not).
fn write_processed(path: &Path, samples: &[EyeData], sent: &[Sample], camera: bool) -> io::Result<()> {
    let mut out = BufWriter::new(File::create(path)?);
    write!(
        out,
        "sample_time,openness_left,openness_right,gaze_left_x,gaze_left_y,gaze_right_x,gaze_right_y,\
         gaze_x,gaze_y,lid_left,lid_right,reliable_left,reliable_right"
    )?;
    if camera {
        write!(
            out,
            ",camera_left,camera_right,squint_left,squint_right,pupil_dilation_left,pupil_dilation_right,\
             pupil_mm_left,pupil_mm_right"
        )?;
    }
    // Where the gaze came from (gaze_source "pupil" with a map), only then
    let in_use = sent.iter().any(|sample| sample.gaze_in_use.is_some());
    if in_use {
        write!(out, ",gaze_in_use")?;
    }
    // The gaze guard (gaze_guard), only with it: side by side, before smoothing and the gaze scale (as the panel's
    // checks score them; -1..1), Valve's own combined gaze, the guarded one and the witness's (the pupils', empty
    // while not both are there); then each eye's offset (degrees) and whether the witness was trusted
    let guarded = sent.iter().any(|sample| sample.guard.is_some());
    if guarded {
        write!(
            out,
            ",guard_valve_x,guard_valve_y,guard_gaze_x,guard_gaze_y,guard_pupil_x,guard_pupil_y,\
             guard_offset_left_x_deg,guard_offset_left_y_deg,guard_offset_right_x_deg,guard_offset_right_y_deg,\
             guard_trusted"
        )?;
    }
    writeln!(out)?;
    let optional = |value: Option<f32>| value.map_or_else(String::new, |value| value.to_string());
    for (data, sample) in samples.iter().zip(sent) {
        let mut fields = vec![data.sample_time.to_string()];
        fields.extend(data.openness.iter().chain(&sample.gaze).chain(&sample.lids).map(f32::to_string));
        fields.extend(sample.reliable.map(|reliable| u8::from(reliable).to_string()));
        if camera {
            let values = sample.camera;
            fields.extend(values.squint.map(|squint| u8::from(squint.is_some()).to_string()));
            let optionals = [values.squint, values.pupil_dilation, values.pupil_diameter];
            fields.extend(optionals.iter().flatten().map(|value| optional(*value)));
        }
        if in_use {
            fields.push(sample.gaze_in_use.map_or("", |in_use| in_use.name()).to_owned());
        }
        if guarded {
            match sample.guard {
                Some(guard) => {
                    fields.extend(guard.valve_measure.iter().chain(&sample.measure_gaze).map(f32::to_string));
                    let witness = guard.witness.map(|witness| witness.map(|degrees| degrees / GAZE_DEGREES));
                    fields.extend([0, 1].map(|axis| optional(witness.map(|witness| witness[axis]))));
                    fields.extend(guard.offset.as_flattened().iter().map(f32::to_string));
                    fields.push(u8::from(guard.trusted).to_string());
                }
                None => fields.extend(std::iter::repeat_n(String::new(), 11)),
            }
        }
        writeln!(out, "{}", fields.join(","))?;
    }
    out.flush()
}

/// The gaze guard over a replay (gaze_guard), as a report line: how often the witness was trusted, the largest offset
/// and the one at the end (degrees, each eye). Empty without it.
fn guard_line(sent: &[Sample]) -> String {
    let guards: Vec<_> = sent.iter().filter_map(|sample| sample.guard).collect();
    let Some(last) = guards.last() else {
        return String::new();
    };
    let trusted = 100.0 * guards.iter().filter(|guard| guard.trusted).count() as f64 / guards.len() as f64;
    let largest = guards.iter().flat_map(|guard| guard.offset).map(|offset| offset[0].hypot(offset[1])).fold(0.0, f32::max);
    let [left, right] = last.offset;
    format!(
        "\nGaze guard: witness trusted {trusted:.1}% of {} samples, largest offset {largest:.1}°, at the end L ({:+.1}, \
         {:+.1})° R ({:+.1}, {:+.1})°\n",
        guards.len(),
        left[0],
        left[1],
        right[0],
        right[1]
    )
}

/// Replay a recording with the new stages off and with `settings`, and print the comparison. An eyecam-rec replay
/// output (its live CSV) instead goes through the eyeball filter alone (eyeball.rs replay_eyecam), with the factory
/// eye-camera calibration `eyeball` names.
pub fn run(
    input: &Path,
    output: Option<&Path>,
    settings: &Settings,
    calibration: &LidCalibration,
    eyeball: EyeballSetup,
) -> Result<(), Box<dyn Error>> {
    let text = fs::read_to_string(input).map_err(|error| format!("{}: {error}", input.display()))?;
    if eyeball::is_eyecam_replay(text.lines().next().unwrap_or("")) {
        let cameras = eyeball::load_calibration(&eyeball.calibration_path)?;
        let report = eyeball::replay_eyecam(&text, &cameras, output).map_err(|error| format!("{}: {error}", input.display()))?;
        println!("{}: eyecam-rec's replay output, through the eyeball filter", input.display());
        print!("{report}");
        return Ok(());
    }
    let (all, camera) = parse(&text).map_err(|error| format!("{}: {error}", input.display()))?;
    let rows: Vec<Row> = all.iter().copied().filter(|row| row.data.is_finite()).collect();
    let samples: Vec<EyeData> = rows.iter().map(|row| row.data).collect();
    let cameras: Vec<Option<Live>> = rows.iter().map(|row| row.camera).collect();
    // gaze_source "eyeball": what it takes up, said once (nothing otherwise)
    for line in eyeball.clone().prepare(settings, &mut Smoother::new(settings)) {
        println!("  {line}");
    }
    let before = replay_with(&samples, &cameras, &without_new_stages(settings), calibration.clone(), Some(&eyeball));
    let after = replay_with(&samples, &cameras, settings, calibration.clone(), Some(&eyeball));
    if let Some(output) = output {
        write_processed(output, &samples, &after, camera).map_err(|error| format!("{}: {error}", output.display()))?;
    }
    let camera_text = camera.then(|| {
        let off = Settings {
            camera_lids: false,
            ..settings.clone()
        };
        camera_lines(&cameras, &samples, &after, &replay_with(&samples, &cameras, &off, calibration.clone(), Some(&eyeball)))
    });
    let guard_text = guard_line(&after);
    let (before, after) = (metrics(&samples, &before), metrics(&samples, &after));
    print!("{}", report(input, &samples, all.len() - samples.len(), settings, &before, &after));
    print!("{}", camera_text.unwrap_or_default());
    print!("{guard_text}");
    if camera && std::env::var_os("FEO_DOUBT_DIAG").is_some() {
        print!("{}", doubt_diag(&samples, &cameras, settings, calibration));
    }
    let header: Vec<&str> = text.lines().next().unwrap_or("").split(',').map(str::trim).collect();
    if camera_eyes_swapped(&header) {
        println!(
            "  (no {CAMERA_ANATOMICAL} column: recorded with the eyecam of 0.7.2-0.7.5, which called the right eye L, \
             so cam_l_* and cam_r_* were read the other way round)"
        );
    }
    Ok(())
}

/// A recording's samples that can be run, each with the eye camera's values read with it (for main.rs's wire_dump).
#[cfg(test)]
pub fn load(path: &Path) -> Result<Vec<(EyeData, Option<Live>)>, String> {
    let text = fs::read_to_string(path).map_err(|error| format!("{}: {error}", path.display()))?;
    let (rows, _) = parse(&text)?;
    Ok(rows.into_iter().filter(|row| row.data.is_finite()).map(|row| (row.data, row.camera)).collect())
}

#[cfg(test)]
mod tests {
    use super::*;

    // Six seconds made up by tests/fixtures/make_synthetic.py: steady gaze with a little noise, four
    // blinks (the last seen closed by one eye only), a one-sample dropout, a stretch where the left eye's
    // covariance is high and its gaze wild, and a wink.
    const SYNTHETIC: &str = include_str!("../tests/fixtures/synthetic.csv");

    fn synthetic() -> Vec<EyeData> {
        parse(SYNTHETIC).unwrap().0.into_iter().map(|row| row.data).collect()
    }

    /// `replay` without the eye camera's values.
    fn replay_alone(samples: &[EyeData], settings: &Settings, calibration: LidCalibration) -> Vec<Sample> {
        replay(samples, &vec![None; samples.len()], settings, calibration)
    }

    fn calibration() -> LidCalibration {
        LidCalibration::load(None, Settings::default().lid_open)
    }

    // A made-up pupil map for both eyes (synthetic numbers): 200 degrees per unit around the pair (0.2, 0.1).
    fn with_pupil_map(settings: &Settings) -> Settings {
        Settings {
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
            ..settings.clone()
        }
    }

    #[test]
    fn gaze_source_valve_or_no_map_sends_every_sample_exactly_as_before() {
        use crate::config::GazeSource;
        let samples = synthetic();
        let base = all_stages();
        let before = replay_alone(&samples, &base, calibration());
        let unmapped = Settings { gaze_source: GazeSource::Pupil, ..base.clone() };
        let mapped_but_valve = with_pupil_map(&base);
        for settings in [unmapped, mapped_but_valve] {
            let after = replay_alone(&samples, &settings, calibration());
            assert_eq!(after.len(), before.len());
            for (a, b) in before.iter().zip(&after) {
                assert_eq!(a.gaze.map(f32::to_bits), b.gaze.map(f32::to_bits));
                assert_eq!(a.lids.map(f32::to_bits), b.lids.map(f32::to_bits));
                assert!(a.gaze_in_use.is_none() && b.gaze_in_use.is_none());
            }
        }
        // With the map and "pupil" the gaze goes the pupils' way (here the eye server's shifted, the made-up pairs
        // being still), and says where it came from
        let pupil = replay_alone(&samples, &Settings { gaze_source: GazeSource::Pupil, ..with_pupil_map(&base) }, calibration());
        assert!(pupil.iter().all(|sample| sample.gaze_in_use.is_some()));
    }

    #[test]
    fn a_long_wink_goes_on_with_the_open_eyes_pupil_and_not_the_eye_servers_eye() {
        use crate::config::GazeSource;
        use crate::pupil::InUse;
        // Made up: 5 s looking 3 deg right (the eye server says straight ahead), the pupils moving a hair; the right eye
        // winked shut from 1 s to 4 s; meanwhile the eye server reads each eye 10 deg apart from the combined gaze, and the
        // shut eye's pair still moves a little, mapping to 5 deg left
        let template = synthetic()[0];
        let samples: Vec<EyeData> = (0..450)
            .map(|i| {
                let mut data = template;
                data.sample_time = 2000.0 + f64::from(i) / 90.0;
                data.fixation_point = [0.0, 0.0, -0.7];
                data.gaze = [[0.0, 0.0, -1.0]; 2];
                data.pre_fusion_gaze = data.gaze;
                data.gaze_covariance = [[0.0005; 3]; 2];
                data.pre_fusion_covariance = data.gaze_covariance;
                let shut = (90..360).contains(&i);
                data.openness = [0.95, if shut { 0.05 } else { 0.95 }];
                if shut {
                    data.gaze[0] = [0.1736, 0.0, -0.9848];
                    data.gaze[1] = [-0.1736, 0.0, -0.9848];
                }
                let jitter = (i % 2) as f32 * 1e-5;
                let (u, v) = ((3.0 + 40.0) / 200.0 + jitter, 20.0 / 200.0);
                // The shut eye's pair: in range and moving, but not where the open eye looks
                let (ur, vr) = if shut { ((-5.0 + 40.0) / 200.0 + jitter, 20.0 / 200.0) } else { (u, v) };
                data.extra[..4].copy_from_slice(&[u, v, ur, vr]);
                data
            })
            .collect();
        let settings = Settings { gaze_source: GazeSource::Pupil, ..with_pupil_map(&all_stages()) };
        let sent = replay_alone(&samples, &settings, calibration());
        // Once the wink outlasts the one-eye hold (the gaze held before, as always), the left eye's pupil alone, and the
        // gaze sent where it looks: 3 deg right, not the eye server's 0, either eye's own +-10, or the mean with the shut
        // eye's pair (-1)
        let start = 90 + ((crate::ONE_EYE_HOLD_MAX_S + 0.3) * 90.0) as usize;
        let wink = &sent[start..355];
        assert!(wink.iter().all(|sample| sample.gaze_in_use == Some(InUse::Left)), "{:?}", wink[0].gaze_in_use);
        assert!(wink.iter().all(|sample| (sample.gaze[4] * 45.0 - 3.0).abs() < 0.3), "{}", wink[0].gaze[4] * 45.0);
        assert!(wink.iter().all(|sample| !sample.gaze_held));
        // Both open again: both pupils
        assert!(sent[420..].iter().all(|sample| sample.gaze_in_use == Some(InUse::Pupil)));
    }

    #[test]
    fn with_smoothing_off_the_pupil_gaze_still_runs() {
        use crate::config::GazeSource;
        use crate::pupil::InUse;
        // As below, with smoothing off (raw): the pupils give the gaze (not the eye server's), unsmoothed, the same
        // sources as with it on
        let mut samples = synthetic();
        for (i, data) in samples.iter_mut().enumerate() {
            let [x, y] = gaze_angles(data.fixation_point).map(|value| value * 45.0);
            let jitter = (i % 2) as f32 * 1e-5;
            let (u, v) = ((x + 5.0 + 40.0) / 200.0 + jitter, (20.0 - (y + 4.0)) / 200.0);
            data.extra[..4].copy_from_slice(&[u, v, u, v]);
        }
        let base = all_stages();
        let smooth = replay_alone(&samples, &Settings { gaze_source: GazeSource::Pupil, ..with_pupil_map(&base) }, calibration());
        let raw_settings = Settings { gaze_source: GazeSource::Pupil, raw: true, ..with_pupil_map(&base) };
        let raw = replay_alone(&samples, &raw_settings, calibration());
        let valve = replay_alone(&samples, &Settings { raw: true, ..base.clone() }, calibration());
        let used = |sent: &[Sample]| sent.iter().filter(|sample| sample.gaze_in_use == Some(InUse::Pupil)).count();
        assert!(used(&raw) > 200 && used(&raw).abs_diff(used(&smooth)) <= used(&smooth) / 20, "{} {}", used(&raw), used(&smooth));
        // About 5 and 4 deg off the eye server's gaze where both pupils give it
        let moved: Vec<[f32; 2]> = valve
            .iter()
            .zip(&raw)
            .skip(90)
            .filter(|(_, p)| p.gaze_in_use == Some(InUse::Pupil) && !p.gaze_held && !p.gaze_blending)
            .map(|(v, p)| [(p.gaze[4] - v.gaze[4]) * 45.0, (p.gaze[5] - v.gaze[5]) * 45.0])
            .collect();
        assert!(moved.len() > 200, "{}", moved.len());
        let mut dx: Vec<f32> = moved.iter().map(|m| m[0]).collect();
        let mut dy: Vec<f32> = moved.iter().map(|m| m[1]).collect();
        dx.sort_by(f32::total_cmp);
        dy.sort_by(f32::total_cmp);
        let (mx, my) = (dx[dx.len() / 2], dy[dy.len() / 2]);
        assert!((mx - 5.0).abs() < 0.3 && (my - 4.0).abs() < 0.3, "{mx} {my}");
    }

    #[test]
    fn the_pupil_gaze_follows_the_pupils_through_the_whole_pipeline() {
        use crate::config::GazeSource;
        // The synthetic recording with each eye's pupil pair moving as if looking 5 deg right and 4 deg up of where the
        // eye server says, plus a hair of noise: the combined gaze sent goes there
        let mut samples = synthetic();
        for (i, data) in samples.iter_mut().enumerate() {
            let [x, y] = gaze_angles(data.fixation_point).map(|value| value * 45.0);
            let jitter = (i % 2) as f32 * 1e-5;
            let (u, v) = ((x + 5.0 + 40.0) / 200.0 + jitter, (20.0 - (y + 4.0)) / 200.0);
            data.extra[..4].copy_from_slice(&[u, v, u, v]);
        }
        let base = all_stages();
        let valve = replay_alone(&samples, &base, calibration());
        let settings = Settings { gaze_source: GazeSource::Pupil, ..with_pupil_map(&base) };
        let pupil = replay_alone(&samples, &settings, calibration());
        // Away from the start, the blinks and the wild stretch: about 5 and 4 deg off the eye server's gaze
        let moved: Vec<[f32; 2]> = valve
            .iter()
            .zip(&pupil)
            .skip(90)
            .filter(|(_, p)| p.gaze_in_use == Some(crate::pupil::InUse::Pupil) && !p.gaze_held)
            .map(|(v, p)| [(p.gaze[4] - v.gaze[4]) * 45.0, (p.gaze[5] - v.gaze[5]) * 45.0])
            .collect();
        assert!(moved.len() > 200, "{}", moved.len());
        let mut dx: Vec<f32> = moved.iter().map(|m| m[0]).collect();
        let mut dy: Vec<f32> = moved.iter().map(|m| m[1]).collect();
        dx.sort_by(f32::total_cmp);
        dy.sort_by(f32::total_cmp);
        let (mx, my) = (dx[dx.len() / 2], dy[dy.len() / 2]);
        assert!((mx - 5.0).abs() < 0.3 && (my - 4.0).abs() < 0.3, "{mx} {my}");
    }

    /// The defaults with the optional quality check turned on too, so every stage is exercised.
    fn all_stages() -> Settings {
        Settings {
            gaze_quality_limit: 0.03,
            ..Settings::default()
        }
    }

    fn run_with(settings: &Settings) -> Metrics {
        let samples = synthetic();
        metrics(&samples, &replay_alone(&samples, settings, calibration()))
    }

    #[test]
    fn recordings_read_back_exactly() {
        let data = EyeData {
            sample_time: 19892.729813125,
            gaze: [[-0.014556, -0.276537, -0.960893], [-0.097535, -0.275248, -0.956413]],
            gaze_covariance: [[0.038169, 0.036536, 0.033531], [0.064817, 0.036536, 0.063212]],
            fixation_point: [-0.042491, -0.208806, -0.725547],
            pre_fusion_gaze: [[0.1, 0.2, -0.9], [0.3, 0.4, -0.8]],
            pre_fusion_covariance: [[1e-5, 2e-5, 3e-5], [0.5, 0.25, 0.125]],
            openness: [0.821048, 0.781017],
            extra: [0.077017, 0.019944, 0.105099, 0.034945, 0.000022, 0.000021, 0.000044, 0.000040],
        };
        let path = std::env::temp_dir().join(format!("frameeyeosc-record-{}.csv", std::process::id()));
        let mut recorder = Recorder::create(&path).unwrap();
        recorder.write(&data, None).unwrap();
        let mut camera = crate::tests::camera(0.93, 0.25);
        camera.eyes[1] = Eye {
            time_ns: 1_000_000_000,
            closed: true,
            wide: 0.72,
            pupil_mm: 3.75,
            pupil_dilation: 0.4,
            confidence: 0.8,
            ..camera.eyes[1]
        };
        camera.fresh = [true, false];
        camera.calib_state = 5;
        // The left eye's glint part (the right one has none)
        camera.slip[0] = Some(CamSlip {
            time_ns: 1_011_111_111,
            pupil: Some([201.5, 148.25]),
            tracked: true,
            template: Some((3, 987_654_321)),
            offset: [-2.5, 0.125],
            matches: 4,
            since_good: 0,
            glints: 6,
        });
        // ...and its shape part (a pupil, its picture turned upright, no lids found)
        camera.shape[0] = Some(CamShape {
            a: 21.5,
            b: 19.25,
            angle_deg: 133.5,
            vis: 0.96,
            aperture: f32::NAN,
            flipped: true,
        });
        recorder.write(&EyeData { sample_time: 19892.74, ..data }, Some((&camera, 1_012_345_678))).unwrap();
        recorder.flush().unwrap();
        assert_eq!((recorder.count, recorder.camera_count), (2, 1));
        drop(recorder);
        let text = fs::read_to_string(&path).unwrap();
        fs::remove_file(path).unwrap();
        let (read, has_camera) = parse(&text).unwrap();
        assert!(has_camera);
        let expected = [
            Row { data, camera: None },
            Row {
                data: EyeData { sample_time: 19892.74, ..data },
                // As read, with the capture times left out (only their age is recorded)
                camera: Some(Live {
                    eyes: camera.eyes.map(|eye| Eye { time_ns: 0, ..eye }),
                    ..camera
                }),
            },
        ];
        // (NaN is not equal to itself: the aperture compared on its own)
        let shape_of = |row: &Row| row.camera.unwrap().shape;
        assert!(shape_of(&read[1])[0].unwrap().aperture.is_nan());
        let without_nan = |mut rows: Vec<Row>| {
            for row in &mut rows {
                if let Some(Some(shape)) = row.camera.as_mut().map(|live| &mut live.shape[0]) {
                    shape.aperture = 0.0;
                }
            }
            rows
        };
        assert_eq!(without_nan(read.clone()), without_nan(expected.to_vec()));
        // The camera's columns come after the eye server's as before: empty without a live file; then the mark that
        // they name the eyes anatomically ("1" with them); then each eye's glint part, empty without one; then each
        // eye's shape part, likewise
        let lines: Vec<&str> = text.lines().collect();
        assert!(lines[0].starts_with(&(columns().join(",") + ",cam_l_fresh,cam_l_valid,cam_l_closed,cam_l_lid,")));
        assert!(lines[0].contains(",cam_r_confidence,cam_r_age_ms,cam_calib_state,cam_live,cam_eyes_anatomical,cam_l_glint_frame_ns,"));
        assert!(lines[0].contains(",cam_r_glint_matches,cam_r_glint_since_good,cam_r_glint_count,cam_l_shape_a,"));
        assert!(lines[0].ends_with(",cam_r_shape_vis,cam_r_shape_aperture,cam_r_shape_flipped"));
        assert!(lines[1].ends_with(&",".repeat(23 + 22 + 12)), "{}", lines[1]);
        let cells: Vec<&str> = lines[2].split(',').skip(columns().len()).collect();
        let mut want = vec![
            "1", "1", "0", "0.93", "0", "0.25", "4", "0.5", "0", "1012.3",
            "0", "1", "1", "0.93", "0.72", "0.25", "3.75", "0.4", "0.8", "12.3",
            "5", "1", "1",
            "1011111111", "201.5", "148.25", "1", "3", "987654321", "-2.5", "0.125", "4", "0", "6",
        ];
        want.extend([""; 11]);
        want.extend(["21.5", "19.25", "133.5", "0.96", "", "1"]);
        want.extend([""; 6]);
        assert_eq!(cells, want);
        // A recording from before the glint columns reads as it did: no glint part
        let header_end = lines[0].find(",cam_l_glint_frame_ns").unwrap();
        let old_rows: Vec<String> = lines[1..].iter().map(|line| {
            let keep = columns().len() + camera_columns().len() + 1;
            line.split(',').take(keep).collect::<Vec<_>>().join(",")
        }).collect();
        let old = format!("{}\n{}\n", &lines[0][..header_end], old_rows.join("\n"));
        let (read_old, _) = parse(&old).unwrap();
        assert_eq!(read_old[1].camera.unwrap().slip, [None; 2]);
        assert_eq!(read_old[1].camera.unwrap().shape, [None; 2]);
        // ...and one with the glint columns but from before the shape columns: its glints, no shape part
        let header_end = lines[0].find(",cam_l_shape_a").unwrap();
        let glint_rows: Vec<String> = lines[1..].iter().map(|line| {
            let keep = columns().len() + camera_columns().len() + 1 + glint_columns().len();
            line.split(',').take(keep).collect::<Vec<_>>().join(",")
        }).collect();
        let glint_only = format!("{}\n{}\n", &lines[0][..header_end], glint_rows.join("\n"));
        let (read_glint, _) = parse(&glint_only).unwrap();
        assert_eq!((read_glint[1].camera.unwrap().slip, read_glint[1].camera.unwrap().shape), (camera.slip, [None; 2]));
    }

    #[test]
    fn camera_columns_without_the_mark_are_read_the_other_way_round() {
        // As 0.7.2-0.7.5 recorded them (no cam_eyes_anatomical): their cam_l_* is the right eye, so it is read as R
        let header = columns().join(",") + "," + &camera_columns().join(",");
        let left = ["1", "1", "0", "0.9", "0.6", "0.1", "4.5", "0.55", "0.95", "3.2"].join(",");
        let right = ["0", "1", "1", "0.2", "0", "0.3", "3.5", "0.25", "0.75", "8.1"].join(",");
        let sample = ["2"; 38].join(",");
        let old = format!("{header}\n{sample},{left},{right},4,1\n");
        let (rows, has_camera) = parse(&old).unwrap();
        let live = rows[0].camera.unwrap();
        assert!(has_camera && camera_eyes_swapped(&header.split(',').collect::<Vec<_>>()));
        assert_eq!((live.eyes[0].lid, live.eyes[1].lid, live.fresh), (0.2, 0.9, [false, true]));
        assert_eq!((live.eyes[0].pupil_mm, live.eyes[1].pupil_mm, live.eyes[0].closed), (3.5, 4.5, true));
        // With the mark: as named
        let new = format!("{header},cam_eyes_anatomical\n{sample},{left},{right},4,1,1\n");
        let live = parse(&new).unwrap().0[0].camera.unwrap();
        assert_eq!((live.eyes[0].lid, live.eyes[1].lid, live.fresh), (0.9, 0.2, [true, false]));
        // What the Recorder writes now reads back as written
        let path = std::env::temp_dir().join(format!("frameeyeosc-record-marked-{}.csv", std::process::id()));
        let mut recorder = Recorder::create(&path).unwrap();
        recorder.write(&EyeData::default(), Some((&live, 0))).unwrap();
        recorder.flush().unwrap();
        drop(recorder);
        let text = fs::read_to_string(&path).unwrap();
        fs::remove_file(path).unwrap();
        let back = parse(&text).unwrap().0[0].camera.unwrap();
        assert_eq!((back.eyes[0].lid, back.eyes[1].lid, back.fresh), (0.9, 0.2, [true, false]));
        // No camera columns at all: nothing to exchange
        assert!(!camera_eyes_swapped(&columns().iter().map(String::as_str).collect::<Vec<_>>()));
    }

    #[test]
    fn columns_are_found_by_name() {
        let text = "note,".to_owned() + &columns().join(",") + "\n" + "9," + &["1"; 38].join(",");
        let (read, has_camera) = parse(&text).unwrap();
        assert_eq!((read[0].data.sample_time, read[0].data.extra[7]), (1.0, 1.0));
        assert!(!has_camera && read[0].camera.is_none());
        assert!(parse("sample_time\n1\n").unwrap_err().contains("gaze_left_x"));
        let broken = columns().join(",") + "\n1,2,x";
        assert!(parse(&broken).unwrap_err().starts_with("line 2"));
    }

    #[test]
    fn old_recordings_have_no_camera_and_new_ones_may_lack_it_for_a_while() {
        // From before the camera's columns: read as before
        let (old, has_camera) = parse(SYNTHETIC).unwrap();
        assert!(!has_camera && old.len() == 540 && old.iter().all(|row| row.camera.is_none()));
        // With them: rows without a live file have empty cells
        let header = columns().join(",") + "," + &camera_columns().join(",");
        let eye = ["1", "1", "0", "0.9", "0.6", "0.1", "4.5", "0.55", "0.95", "3.2"].join(",");
        let sample = ["2"; 38].join(",");
        let text = format!("{header}\n{sample}{}\n{sample},{eye},{eye},4,1\n", ",".repeat(22));
        let (rows, has_camera) = parse(&text).unwrap();
        assert!(has_camera);
        assert_eq!(rows[0].camera, None);
        let live = rows[1].camera.unwrap();
        assert_eq!((live.calib_state, live.live, live.fresh), (4, true, [true; 2]));
        assert_eq!((live.eyes[1].lid, live.eyes[1].wide, live.eyes[1].pupil_mm), (0.9, 0.6, 4.5));
        assert_eq!(live.lids_usable(), [true; 2]);
        // Not numbers there: refused like any other cell
        let broken = format!("{header}\n{sample},{eye},{},4,1\n", eye.replace("0.9", "x"));
        assert!(parse(&broken).unwrap_err().starts_with("line 2"));
    }

    /// A recording of `seconds` at 90 Hz looking straight ahead with both eyes reading 1.000 (relaxed open on SteamOS
    /// 0.4.3), and the eye camera's values from `camera(sample index)`, written by the Recorder and read back.
    fn saturated_recording(seconds: usize, camera: impl Fn(usize) -> Option<Live>) -> (Vec<EyeData>, Vec<Option<Live>>) {
        let path = std::env::temp_dir().join(format!("frameeyeosc-camera-replay-{}.csv", std::process::id()));
        let mut recorder = Recorder::create(&path).unwrap();
        for i in 0..seconds * 90 {
            let data = EyeData {
                sample_time: 500.0 + i as f64 / 90.0,
                gaze: [[0.0, 0.0, -1.0]; 2],
                fixation_point: [0.0, 0.0, -1.0],
                pre_fusion_gaze: [[0.0, 0.0, -1.0]; 2],
                openness: [1.0; 2],
                ..EyeData::default()
            };
            recorder.write(&data, camera(i).as_ref().map(|live| (live, 0))).unwrap();
        }
        recorder.flush().unwrap();
        drop(recorder);
        let text = fs::read_to_string(&path).unwrap();
        fs::remove_file(path).unwrap();
        let rows = parse(&text).unwrap().0;
        (rows.iter().map(|row| row.data).collect(), rows.iter().map(|row| row.camera).collect())
    }

    #[test]
    fn a_replay_mixes_in_the_recorded_camera_widening_past_the_saturation_cap() {
        let settings = Settings {
            lid_calibration: false,
            ..Settings::default()
        };
        // The camera sees the eyes widened from 10 s in, after the openness has been told to be saturated (6.7 s)
        let wide = crate::tests::camera(0.95, 0.2);
        let (samples, cameras) = saturated_recording(15, |i| (i >= 900).then_some(wide));
        let sent = replay(&samples, &cameras, &settings, calibration());
        assert!(sent[700..900].iter().all(|sample| sample.lids.iter().all(|lid| *lid <= 0.75)));
        let last = sent.last().unwrap();
        assert!(last.lids.iter().all(|lid| (lid - 0.95).abs() < 0.005), "{:?}", last.lids);
        assert_eq!(last.camera.squint, [Some(0.2); 2]);
        // Its freshness is as recorded, whatever the replay's clock: stale values are not used
        let stale = Live {
            fresh: [false; 2],
            ..wide
        };
        let (samples, cameras) = saturated_recording(15, |i| (i >= 900).then_some(stale));
        let sent = replay(&samples, &cameras, &settings, calibration());
        assert!(sent[700..].iter().all(|sample| sample.lids.iter().all(|lid| *lid <= 0.75)));
        // The report says how much of the widening came from the camera (here all: marks 3 and 4 at 1.000 keep the
        // eye server's readings from widening even before the cap)
        let marks = Settings {
            lid_widen_start: 1.0,
            lid_wide: 1.0,
            ..settings.clone()
        };
        let (samples, cameras) = saturated_recording(15, |i| (i >= 900).then_some(wide));
        let sent = replay(&samples, &cameras, &marks, calibration());
        let off = Settings {
            camera_lids: false,
            ..marks.clone()
        };
        let without = replay(&samples, &cameras, &off, calibration());
        let widened = sent.iter().filter(|sample| sample.lids[0] >= LID_WIDE).count();
        // (the camera's eyelid taken once it has kept coming for CAMERA_GAP_HOLD, 18 samples)
        assert!((400..432).contains(&widened), "{widened}");
        let lines = camera_lines(&cameras, &samples, &sent, &without);
        assert!(lines.contains("values with 33.3% of the samples; fresh (L / R) 33.3% / 33.3%"), "{lines}");
        assert!(lines.contains("eyelid and squint from it (L / R)          33.3% / 33.3%"), "{lines}");
        // Here the eye server reads the eyes as open as can be and the camera sees them widened, so the camera's
        // eyelid never overrides the usual mix
        let overrides = "eyelid from it over the eye server's (L / R): seen open 0.0% / 0.0%, seen closed 0.0% / 0.0%, \
                         widened 0.0% / 0.0%, at 1.000 0.0% / 0.0%";
        assert!(lines.contains(overrides), "{lines}");
        assert!(lines.contains("its closed taken as open, the eye server sure (L / R)  0.0 s / 0.0 s"), "{lines}");
        let share = format!("{:.1}%", widened as f64 * 100.0 / 1350.0);
        assert!(lines.contains(&format!("{share} / {share} with it, 0.0% / 0.0% without it")), "{lines}");
        // How much a gaze capture could use; none of it open by the pupil alone (the camera's confidence is 0 here)
        let usable = |sent: &[Sample]| sent.iter().filter(|sample| sample.capture_usable()).count() as f64 * 100.0 / 1350.0;
        let line = format!(
            "usable for a gaze capture                  {:.1}% with it, {:.1}% without it; \
             open by its pupil alone (L / R) 0.0% / 0.0%",
            usable(&sent),
            usable(&without)
        );
        assert!(lines.contains(&line), "{lines}");
        let path = std::env::temp_dir().join(format!("frameeyeosc-camera-processed-{}.csv", std::process::id()));
        write_processed(&path, &samples, &sent, true).unwrap();
        let text = fs::read_to_string(&path).unwrap();
        write_processed(&path, &samples, &sent, false).unwrap();
        let plain = fs::read_to_string(&path).unwrap();
        fs::remove_file(path).unwrap();
        let header = text.lines().next().unwrap();
        assert!(header.ends_with(",reliable_right,camera_left,camera_right,squint_left,squint_right,\
            pupil_dilation_left,pupil_dilation_right,pupil_mm_left,pupil_mm_right"), "{header}");
        let first = text.lines().nth(1).unwrap();
        assert!(first.ends_with(&format!(",1,1,0,0{}", ",".repeat(6))), "{first}");
        assert!(text.lines().last().unwrap().ends_with(",1,1,1,1,0.2,0.2,0.5,0.5,4,4"), "{}", text.lines().last().unwrap());
        // Without the camera's columns, as before
        assert!(plain.lines().next().unwrap().ends_with(",reliable_left,reliable_right"));
    }

    #[test]
    fn camera_frame_intervals_come_from_the_recorded_ages() {
        let mut gaps = RecordedGaps::default();
        let mut live = crate::tests::camera(0.75, 0.0);
        // 90 samples a second, the left camera delivering 15 frames a second, each read a few times at a growing
        // age, with up to 0.8 ms of jitter in when it was read; the right eye's ages are not numbers
        let frame = 1.0 / 15.0;
        for i in 0..90 {
            let time = 100.0 + i as f64 / 90.0;
            let age_ms = (time % frame) * 1000.0 + if i % 2 == 0 { 0.8 } else { 0.0 };
            gaps.note(&mut live, time, [age_ms, f64::NAN]);
        }
        assert!((65_500_000..68_000_000).contains(&live.frame_ns[0]), "{:?}", live.frame_ns);
        assert_eq!(live.frame_ns[1], 0);
    }

    #[test]
    fn synthetic_recording_has_the_expected_events() {
        let before = run_with(&without_new_stages(&Settings::default()));
        assert_eq!(before.blinks, 4, "{before:?}");
        assert!((before.unreliable[0], before.unreliable[1]) == (0.0, 0.0));
        let after = run_with(&all_stages());
        // Both eyes fail the quality check during the four blinks (5 samples each), and the left eye also
        // during its bad stretch (27 samples), out of 540.
        let failed = after.unreliable.map(|share| (share * 540.0).round());
        assert_eq!(failed, [47.0, 20.0], "{after:?}");
    }

    #[test]
    fn new_stages_close_every_blink_and_steady_the_gaze() {
        let before = run_with(&without_new_stages(&Settings::default()));
        let after = run_with(&all_stages());
        assert!(before.blinks_shut < before.blinks, "{before:?}");
        assert_eq!(after.blinks_shut, after.blinks, "{after:?}");
        // Held for --blink-hold-ms (80) from the last closed sample.
        assert!(after.shut_ms >= 80.0, "{after:?}");
        assert!(after.blink_jump < before.blink_jump, "{before:?} {after:?}");
        // The filter and the deadzone keep both all but still while fixating (under 0.02° against noise of about 0.2°,
        // a 0.004 spread against the deadzone's 0.005), the despike more so; the left eye's wild stretch is not a
        // fixation.
        assert!(after.jitter <= before.jitter && before.jitter < 0.02, "{before:?} {after:?}");
        assert!(after.flicker <= before.flicker, "{before:?} {after:?}");
    }

    #[test]
    fn a_lid_fit_is_read_from_looking_around_and_closing_the_eyes() {
        // 2 s each of looking 15° up, straight ahead and 15° down, then 3 s with the eyes shut.
        let tilt = (15.0_f32).to_radians().tan();
        let parts: [(f32, [f32; 2], usize); 4] =
            [(tilt, [0.95, 0.85], 180), (0.0, [0.9, 0.8], 180), (-(18.0_f32).to_radians().tan(), [0.7, 0.62], 180), (0.0, [0.15, 0.26], 270)];
        let mut samples = Vec::new();
        for (y, openness, count) in parts {
            for _ in 0..count {
                let direction = [0.0, y, -1.0];
                samples.push(EyeData {
                    sample_time: samples.len() as f64 / 90.0,
                    gaze: [direction; 2],
                    fixation_point: direction,
                    openness,
                    ..EyeData::default()
                });
            }
        }
        let [left, right] = estimate_lid_fit(&samples).unwrap();
        assert_eq!((left.up, left.open, left.down, left.closed), (0.95, 0.9, 0.7, 0.15));
        assert_eq!((right.up, right.open, right.down, right.closed), (0.85, 0.8, 0.62, 0.26));
        assert!(estimate_lid_fit(&samples[..540]).is_none());
        assert_eq!(open_reference(&samples), [0.9, 0.8]);
    }

    #[test]
    fn saturation_is_reported_from_when_it_shows() {
        // The synthetic recording: six seconds, open well below 1.000
        assert_eq!(saturation_line(&synthetic()), "Openness not saturated (widening can come through)\n");
        let samples: Vec<EyeData> = (0..900)
            .map(|i| EyeData {
                sample_time: 50.0 + f64::from(i) / 90.0,
                openness: [1.0, 0.97],
                ..EyeData::default()
            })
            .collect();
        let line = saturation_line(&samples);
        assert!(line.starts_with("Openness saturated ("), "{line}");
        assert!(line.ends_with(": from 6.7 s in, 33.4% of the samples\n"), "{line}");
    }

    #[test]
    fn percentiles_pick_the_nearest_rank() {
        let mut values = [5.0, 1.0, 4.0, 2.0, 3.0];
        assert_eq!(median(&mut values), 3.0);
        assert_eq!(percentile(&mut values, 90.0), 5.0);
        assert!(median(&mut []).is_nan());
        let times = [0.0, 0.05, 0.1, 0.3, 0.5];
        assert_eq!(near(&times, &[false, false, true, false, false], 0.06), [false, true, true, false, false]);
    }
}
