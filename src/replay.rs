//! Recording the eye server's samples to CSV (--record), with the eye camera's values read with each, and running a
//! recording back through the same processing (--replay) to compare settings by a few numbers.

use crate::config::{LidFit, Settings};
use crate::eyecam_live::{Eye, FrameGaps, Live};
use crate::{
    CAL_SETTLE, EyeData, LidCalibration, LidFrom, MAX_GAP, NOMINAL_DT, Sample, Saturation, Smoother, TIMEOUT,
    gaze_angles, step,
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
        writeln!(out, "{},{},{CAMERA_ANATOMICAL}", columns().join(","), camera_columns().join(","))?;
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
    let mut gaps = RecordedGaps::default();
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
                        if swapped {
                            live.eyes.swap(0, 1);
                            live.fresh.swap(0, 1);
                            ages.swap(0, 1);
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
fn replay(samples: &[EyeData], cameras: &[Option<Live>], settings: &Settings, mut calibration: LidCalibration) -> Vec<Sample> {
    let mut smoother = Smoother::new(settings);
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
    }
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
/// overrode the eye server's (the eye seen open while the eye server read it closing, seen closed, or widened below the
/// eye server's relaxed open), and how often each eye went out visibly widened (LID_WIDE) with them (`with`) and
/// without them (`without`, camera_lids off).
fn camera_lines(cameras: &[Option<Live>], with: &[Sample], without: &[Sample]) -> String {
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
    let wide = |sent: &[Sample]| {
        let [left, right] = [0, 1].map(|eye| share(sent.iter().filter(|sample| sample.lids[eye] >= LID_WIDE).count()));
        format!("{left:.1}% / {right:.1}%")
    };
    format!(
        "\nEye camera (eyecam-rec): values with {:.1}% of the samples; fresh (L / R) {:.1}% / {:.1}%; \
         with a baseline for the wear {baseline:.1}%\n  \
         eyelid and squint from it (L / R)          {:.1}% / {:.1}%\n  \
         eyelid from it over the eye server's (L / R): seen open {}, seen closed {}, widened {}\n  \
         sent widened, VRCFT {LID_WIDE} or more (L / R)    {} with it, {} without it\n",
        share(read.len()),
        fresh[0],
        fresh[1],
        used[0],
        used[1],
        from(LidFrom::CameraOpen),
        from(LidFrom::CameraClosed),
        from(LidFrom::CameraWidened),
        wide(with),
        wide(without),
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
        writeln!(out, "{}", fields.join(","))?;
    }
    out.flush()
}

/// Replay a recording with the new stages off and with `settings`, and print the comparison.
pub fn run(input: &Path, output: Option<&Path>, settings: &Settings, calibration: &LidCalibration) -> Result<(), Box<dyn Error>> {
    let text = fs::read_to_string(input).map_err(|error| format!("{}: {error}", input.display()))?;
    let (all, camera) = parse(&text).map_err(|error| format!("{}: {error}", input.display()))?;
    let rows: Vec<Row> = all.iter().copied().filter(|row| row.data.is_finite()).collect();
    let samples: Vec<EyeData> = rows.iter().map(|row| row.data).collect();
    let cameras: Vec<Option<Live>> = rows.iter().map(|row| row.camera).collect();
    let before = replay(&samples, &cameras, &without_new_stages(settings), calibration.clone());
    let after = replay(&samples, &cameras, settings, calibration.clone());
    if let Some(output) = output {
        write_processed(output, &samples, &after, camera).map_err(|error| format!("{}: {error}", output.display()))?;
    }
    let camera_text = camera.then(|| {
        let off = Settings {
            camera_lids: false,
            ..settings.clone()
        };
        camera_lines(&cameras, &after, &replay(&samples, &cameras, &off, calibration.clone()))
    });
    let (before, after) = (metrics(&samples, &before), metrics(&samples, &after));
    print!("{}", report(input, &samples, all.len() - samples.len(), settings, &before, &after));
    print!("{}", camera_text.unwrap_or_default());
    let header: Vec<&str> = text.lines().next().unwrap_or("").split(',').map(str::trim).collect();
    if camera_eyes_swapped(&header) {
        println!(
            "  (no {CAMERA_ANATOMICAL} column: recorded with the eyecam of 0.7.2-0.7.5, which called the right eye L, \
             so cam_l_* and cam_r_* were read the other way round)"
        );
    }
    Ok(())
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
        assert_eq!(read, expected);
        // The camera's columns come last, after the eye server's as before: empty without a live file; then the
        // mark that they name the eyes anatomically ("1" with them)
        let lines: Vec<&str> = text.lines().collect();
        assert!(lines[0].starts_with(&(columns().join(",") + ",cam_l_fresh,cam_l_valid,cam_l_closed,cam_l_lid,")));
        assert!(lines[0].ends_with(",cam_r_confidence,cam_r_age_ms,cam_calib_state,cam_live,cam_eyes_anatomical"));
        assert!(lines[1].ends_with(&",".repeat(23)), "{}", lines[1]);
        let cells: Vec<&str> = lines[2].split(',').skip(columns().len()).collect();
        assert_eq!(
            cells,
            [
                "1", "1", "0", "0.93", "0", "0.25", "4", "0.5", "0", "1012.3",
                "0", "1", "1", "0.93", "0.72", "0.25", "3.75", "0.4", "0.8", "12.3",
                "5", "1", "1",
            ]
        );
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
        assert!((420..450).contains(&widened), "{widened}");
        let lines = camera_lines(&cameras, &sent, &without);
        assert!(lines.contains("values with 33.3% of the samples; fresh (L / R) 33.3% / 33.3%"), "{lines}");
        assert!(lines.contains("eyelid and squint from it (L / R)          33.3% / 33.3%"), "{lines}");
        // Here the eye server reads the eyes as open as can be, so the camera's eyelid never overrides it
        let overrides =
            "eyelid from it over the eye server's (L / R): seen open 0.0% / 0.0%, seen closed 0.0% / 0.0%, widened 0.0% / 0.0%";
        assert!(lines.contains(overrides), "{lines}");
        let share = format!("{:.1}%", widened as f64 * 100.0 / 1350.0);
        assert!(lines.contains(&format!("{share} / {share} with it, 0.0% / 0.0% without it")), "{lines}");
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
