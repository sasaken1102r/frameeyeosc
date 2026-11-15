//! The eyeball source's maps (gaze_source "eyeball", eyeball.rs): each eye's affine map from its filter's angles to
//! the direction it looks, and its centre for this wear, the same kind of map as the pupil source's (pupil.rs EyeMap).
//! frameeyeosc fits them itself from the panel's eye fit, so the panel needs nothing new: while gaze_source is
//! "eyeball", each gaze capture also keeps the median of each eye's filter angles over the samples it averaged
//! (capture.rs), and
//!   - a "center" capture (the first dot of a whole fit, or a re-center by hand or when the headset is put on) sets
//!     each eye's centre with the map it has, and starts a new set of dots;
//!   - the whole fit's dots follow (center, up, down, left, right, and with this source also the four corners; the
//!     latest try of each), and at its eyes-shut step ("closed") each eye's map is fitted to where it looked at each dot
//!     (as the panel's eye fit places them: TARGET_DISTANCE_M away, the eyes IPD_M apart), and the centre is set from the
//!     centre dot with it. With the corners, each eye's map is quadratic (Q9: yaw and pitch each from 1, a, b, a², ab,
//!     b² of the filter's angles a, b), fitted robustly (a dot far off the others' fit is left out, at most
//!     Q9_MAX_DROPPED); without enough of them, affine on the five dots, as before.
//! Each eye's median needs MIN_SAMPLES samples; a map that misses the dots it was fitted on by more than MAX_RMS_DEG,
//! or a centre further than MAX_CENTER_DEG, is not taken (the old one stays). The maps are kept in a file of their own (`eyeball_map` next to
//! the eyelid calibration), never in config.json, which is the panel's: they last across restarts; the replay reads
//! the file and never writes it.

use crate::pupil::EyeMap;
use std::path::{Path, PathBuf};

/// The eye fit's dots (degrees: yaw + right, pitch + up), as the panel's eye fit shows them (gaze_fit.cpp kTargets):
/// the five of every whole fit, then the four corners the panel adds with this source (the gaze error check's corners).
pub const DOTS: [(&str, f64, f64); 9] = [
    ("center", 0.0, 0.0),
    ("up", 0.0, 15.0),
    ("down", 0.0, -15.0),
    ("left", -20.0, 0.0),
    ("right", 20.0, 0.0),
    ("up_left", -20.0, 15.0),
    ("up_right", 20.0, 15.0),
    ("down_right", 20.0, -15.0),
    ("down_left", -20.0, -15.0),
];
/// The five dots every whole fit has (DOTS' first): the affine map's.
pub const FIVE: usize = 5;
/// The capture that ends a whole fit (the eyes-shut step, capture.rs CLOSED_TARGET): the maps are fitted then.
pub const END_TARGET: &str = crate::capture::CLOSED_TARGET;
/// A Q9 map needs at least this many dots (6 numbers per axis, and one more to say how well it fits)...
pub const Q9_MIN_DOTS: usize = 7;
/// ...and leaves out at most this many: the dot without which the others fit best, when their fit misses it by more
/// than Q9_DROP_DEG and by Q9_DROP_RATIO times their own misses (rms; measured with it left out, so it can't pull the
/// fit towards itself), is taken for a bad one (an eye frozen at a corner) and the fit is made again without it.
/// (25 deg, 2026-11-16, on the recorded runs' medians: a good dot left out is missed by 5-21 deg, as its fit then
/// reaches past the others; the eye frozen at a corner of the 14:51 run by 49 (its Q9 misses 12.8 -> 1.3 deg without it)
/// and nothing else of the re-seat rounds scored was left out, so their next-round errors stay those of plain Q9.)
pub const Q9_MAX_DROPPED: usize = 2;
pub const Q9_DROP_DEG: f64 = 25.0;
pub const Q9_DROP_RATIO: f64 = 3.0;
/// The filter's angles are scaled by this (degrees) in the Q9 fit, so its six columns are of a size.
const Q9_SCALE: f64 = 20.0;
/// How far away the dots are (m), as the panel draws them (gaze_fit.h kTargetDistanceM)...
pub const TARGET_DISTANCE_M: f64 = 0.9;
/// ...and the eyes' distance taken for them (m; a typical adult's, as frameeyeosc doesn't know the wearer's: 5 mm off
/// moves each eye's direction at the dots by about 0.16 degrees, which the centre takes up).
pub const IPD_M: f64 = 0.063;
/// Each eye's median at a dot needs at least this many samples (as the panel's pupil map, gaze_fit.h kMinPupilFresh).
pub const MIN_SAMPLES: u32 = 30;
/// A map is taken only while it fits its dots within this (degrees, root mean square)... (3 since 2026-11-15: on the
/// headset a fit while the right eye's filter was still settling missed its dots by 3.4 degrees and the gaze check's
/// corners by 10-22; the fits before missed by 2.0-2.3 for that eye and 1.0-1.5 for the other. A rule on the centre's
/// own movement between the dots was tried and would have refused nearly every fit: that eye's centre moves 1-5 mm
/// between the dots, in depth too, in every fit.)
pub const MAX_RMS_DEG: f64 = 3.0;
/// ...and the eye's angles spread at least this much between the dots (degrees, the smaller spread of the two axes).
pub const MIN_SPREAD_DEG: f64 = 2.0;
/// A centre further than this (degrees, yaw or pitch) is not taken.
pub const MAX_CENTER_DEG: f64 = 20.0;
/// The file's name next to the eyelid calibration.
pub const FILE_NAME: &str = "eyeball_map";

/// Where an eye looks to see a dot (yaw, pitch degrees from straight ahead) TARGET_DISTANCE_M away: its direction from
/// that eye (the left one IPD_M / 2 to the left), as the panel's eye fit works it out (gaze_fit.cpp eyeDirection).
pub fn eye_direction(yaw_deg: f64, pitch_deg: f64, eye: usize) -> [f64; 2] {
    let (yaw, pitch) = (yaw_deg.to_radians(), pitch_deg.to_radians());
    let half = if eye == 0 { IPD_M / 2.0 } else { -IPD_M / 2.0 };
    let side = TARGET_DISTANCE_M * yaw.sin() * pitch.cos() + half;
    let up = TARGET_DISTANCE_M * pitch.sin();
    let ahead = TARGET_DISTANCE_M * yaw.cos() * pitch.cos();
    [side.atan2(ahead).to_degrees(), up.atan2(ahead).to_degrees()]
}

/// Solve a 3x3 system (partial pivoting); None when it is (nearly) singular.
fn solve3(mut a: [[f64; 3]; 3], mut b: [f64; 3]) -> Option<[f64; 3]> {
    for col in 0..3 {
        let pivot = (col..3).max_by(|&p, &q| a[p][col].abs().total_cmp(&a[q][col].abs()))?;
        if a[pivot][col].abs() < 1e-12 {
            return None;
        }
        a.swap(col, pivot);
        b.swap(col, pivot);
        for row in col + 1..3 {
            let f = a[row][col] / a[col][col];
            for k in col..3 {
                a[row][k] -= f * a[col][k];
            }
            b[row] -= f * b[col];
        }
    }
    for row in (0..3).rev() {
        for k in row + 1..3 {
            b[row] -= a[row][k] * b[k];
        }
        b[row] /= a[row][row];
    }
    Some(b)
}

/// One eye's map from its five dots' medians (DOTS order; the filter's yaw / pitch, degrees) to where it looked, and
/// how far it misses them (rms, degrees); Err says why there is none.
pub fn fit_eye(medians: &[[f32; 2]; 5], eye: usize) -> Result<(EyeMap, f64), String> {
    fit_eye_misses(medians, eye).map_err(|(why, _)| why)
}

/// The smaller spread of a set of angles between its dots (degrees: the root of the covariance's smaller eigenvalue).
fn smaller_spread(angles: &[[f64; 2]]) -> f64 {
    let n = angles.len() as f64;
    let (mu, mv) = (angles.iter().map(|a| a[0]).sum::<f64>() / n, angles.iter().map(|a| a[1]).sum::<f64>() / n);
    let suu = angles.iter().map(|a| (a[0] - mu).powi(2)).sum::<f64>() / n;
    let svv = angles.iter().map(|a| (a[1] - mv).powi(2)).sum::<f64>() / n;
    let suv = angles.iter().map(|a| (a[0] - mu) * (a[1] - mv)).sum::<f64>() / n;
    let half = (suu + svv) / 2.0;
    (half - (half * half - (suu * svv - suv * suv)).max(0.0).sqrt()).max(0.0).sqrt()
}

/// The root mean square of how far a map puts each dot's angles from where the eye looked (degrees).
fn rms_of(map: &EyeMap, dots: &[([f32; 2], [f64; 2])]) -> f64 {
    let sum: f64 = dots
        .iter()
        .map(|(angles, wanted)| {
            let [y, p] = map.apply(*angles);
            (f64::from(y) - wanted[0]).powi(2) + (f64::from(p) - wanted[1]).powi(2)
        })
        .sum();
    (sum / dots.len() as f64).sqrt()
}

/// fit_eye, with how far the map misses its dots on a refusal too (None when there was no map to measure).
fn fit_eye_misses(medians: &[[f32; 2]; 5], eye: usize) -> Result<(EyeMap, f64), (String, Option<f64>)> {
    let name = ["left", "right"][eye];
    let u: Vec<f64> = medians.iter().map(|m| f64::from(m[0])).collect();
    let v: Vec<f64> = medians.iter().map(|m| f64::from(m[1])).collect();
    if !u.iter().chain(&v).all(|x| x.is_finite()) {
        return Err((format!("the {name} eye's angles are not all numbers"), None));
    }
    let wanted: Vec<[f64; 2]> = (0..5).map(|i| eye_direction(DOTS[i].1, DOTS[i].2, eye)).collect();
    let (mu, mv) = (u.iter().sum::<f64>() / 5.0, v.iter().sum::<f64>() / 5.0);
    let smaller = smaller_spread(&(0..5).map(|i| [u[i], v[i]]).collect::<Vec<_>>());
    if smaller < MIN_SPREAD_DEG {
        return Err((
            format!("the {name} eye's angles hardly moved between the dots ({smaller:.1} deg, needs {MIN_SPREAD_DEG:.0})"),
            None,
        ));
    }
    // Least squares about the mean, for a well-conditioned system: angle = c0 + c1 (u - mu) + c2 (v - mv)
    let mut coefficients = [[0.0; 3]; 2];
    for (axis, out) in coefficients.iter_mut().enumerate() {
        let mut a = [[0.0; 3]; 3];
        let mut b = [0.0; 3];
        for i in 0..5 {
            let row = [1.0, u[i] - mu, v[i] - mv];
            for j in 0..3 {
                for k in 0..3 {
                    a[j][k] += row[j] * row[k];
                }
                b[j] += row[j] * wanted[i][axis];
            }
        }
        let c = solve3(a, b).ok_or_else(|| (format!("the {name} eye's angles don't give a map"), None))?;
        *out = [c[0] - c[1] * mu - c[2] * mv, c[1], c[2]];
    }
    let map = EyeMap {
        yaw: coefficients[0].map(|c| c as f32),
        pitch: coefficients[1].map(|c| c as f32),
        center: [0.0; 2],
        quad: crate::pupil::NO_QUAD,
    };
    let dots: Vec<([f32; 2], [f64; 2])> = (0..5).map(|i| (medians[i], wanted[i])).collect();
    let rms = rms_of(&map, &dots);
    if !rms.is_finite() || rms > MAX_RMS_DEG {
        return Err((format!("the {name} eye's map misses its dots by {rms:.1} deg (at most {MAX_RMS_DEG:.0})"), Some(rms)));
    }
    Ok((map, rms))
}

/// Solve an n x n system (partial pivoting); None when it is (nearly) singular.
fn solve_n(mut a: Vec<Vec<f64>>, mut b: Vec<f64>) -> Option<Vec<f64>> {
    let n = b.len();
    for col in 0..n {
        let pivot = (col..n).max_by(|&p, &q| a[p][col].abs().total_cmp(&a[q][col].abs()))?;
        if a[pivot][col].abs() < 1e-10 {
            return None;
        }
        a.swap(col, pivot);
        b.swap(col, pivot);
        for row in col + 1..n {
            let f = a[row][col] / a[col][col];
            for k in col..n {
                a[row][k] -= f * a[col][k];
            }
            b[row] -= f * b[col];
        }
    }
    for row in (0..n).rev() {
        for k in row + 1..n {
            b[row] -= a[row][k] * b[k];
        }
        b[row] /= a[row][row];
    }
    Some(b)
}

/// The Q9 columns of a dot's angles (scaled by Q9_SCALE).
fn q9_row(angles: [f32; 2]) -> [f64; 6] {
    let (u, v) = (f64::from(angles[0]) / Q9_SCALE, f64::from(angles[1]) / Q9_SCALE);
    [1.0, u, v, u * u, u * v, v * v]
}

/// The least-squares Q9 map through these dots (angles, where the eye looked); None when they don't give one.
fn q9_solve(dots: &[([f32; 2], [f64; 2])]) -> Option<EyeMap> {
    let mut c = [[0.0; 6]; 2];
    for (axis, out) in c.iter_mut().enumerate() {
        let mut a = vec![vec![0.0; 6]; 6];
        let mut b = vec![0.0; 6];
        for (angles, wanted) in dots {
            let row = q9_row(*angles);
            for j in 0..6 {
                for k in 0..6 {
                    a[j][k] += row[j] * row[k];
                }
                b[j] += row[j] * wanted[axis];
            }
        }
        let solved = solve_n(a, b)?;
        out.copy_from_slice(&solved);
    }
    // Back to the filter's own degrees: linear terms / scale, quadratic ones / scale²
    let (s, s2) = (Q9_SCALE, Q9_SCALE * Q9_SCALE);
    let map = EyeMap {
        yaw: [c[0][0] as f32, (c[0][1] / s) as f32, (c[0][2] / s) as f32],
        pitch: [c[1][0] as f32, (c[1][1] / s) as f32, (c[1][2] / s) as f32],
        center: [0.0; 2],
        quad: [
            [(c[0][3] / s2) as f32, (c[0][4] / s2) as f32, (c[0][5] / s2) as f32],
            [(c[1][3] / s2) as f32, (c[1][4] / s2) as f32, (c[1][5] / s2) as f32],
        ],
    };
    map.yaw.iter().chain(&map.pitch).chain(map.quad.iter().flatten()).all(|c| c.is_finite()).then_some(map)
}

/// A Q9 map: the map, how far it misses the dots it was fitted on (rms, degrees), and the dots left out (DOTS indices).
#[derive(Clone, Debug, PartialEq)]
pub struct Q9Fit {
    pub map: EyeMap,
    pub rms: f64,
    pub dropped: Vec<usize>,
}

/// One eye's Q9 map from the dots it has (DOTS order; None for a dot without a settled median), robustly: while more than
/// Q9_MIN_DOTS are left, the dot without which the others fit best is left out when the others' fit misses it by more
/// than Q9_DROP_DEG and by Q9_DROP_RATIO times their own misses (at most Q9_MAX_DROPPED). Err (with the misses when
/// there was a map) when fewer than Q9_MIN_DOTS are there, the angles hardly moved, or the map misses its dots by more than MAX_RMS_DEG;
/// the first means: use the affine map instead.
pub fn fit_eye_q9(medians: &[Option<[f32; 2]>; 9], eye: usize) -> Result<Q9Fit, (String, Option<f64>)> {
    q9_fit(medians, eye, true)
}

/// fit_eye_q9; with `checked` false, the map however far it misses (or hardly the eyes moved), for comparing the forms
/// offline (the harness test below).
fn q9_fit(medians: &[Option<[f32; 2]>; 9], eye: usize, checked: bool) -> Result<Q9Fit, (String, Option<f64>)> {
    let name = ["left", "right"][eye];
    let mut kept: Vec<usize> = (0..DOTS.len())
        .filter(|&i| medians[i].is_some_and(|m| m.iter().all(|x| x.is_finite())))
        .collect();
    if kept.len() < Q9_MIN_DOTS {
        return Err((format!("the {name} eye has {} of the 9 dots (a quadratic map needs {Q9_MIN_DOTS})", kept.len()), None));
    }
    let dot = |i: usize| (medians[i].unwrap_or_default(), eye_direction(DOTS[i].1, DOTS[i].2, eye));
    let miss = |map: &EyeMap, i: usize| {
        let (angles, wanted) = dot(i);
        let [y, p] = map.apply(angles);
        (f64::from(y) - wanted[0]).hypot(f64::from(p) - wanted[1])
    };
    let mut dropped = Vec::new();
    while dropped.len() < Q9_MAX_DROPPED && kept.len() > Q9_MIN_DOTS {
        // The dot whose leaving out lets the others fit best, how far the others' fit then misses it, and how well they
        // fit themselves
        let mut best: Option<(usize, f64, f64)> = None;
        for (at, &i) in kept.iter().enumerate() {
            let others: Vec<_> = kept.iter().filter(|&&k| k != i).map(|&k| dot(k)).collect();
            let Some(map) = q9_solve(&others) else { continue };
            let rms = rms_of(&map, &others);
            if best.is_none_or(|(_, _, b)| rms < b) {
                best = Some((at, miss(&map, i), rms));
            }
        }
        let Some((at, far, rms)) = best else { break };
        if !(far > Q9_DROP_DEG && far > Q9_DROP_RATIO * rms) {
            break;
        }
        dropped.push(kept.remove(at));
    }
    let dots: Vec<_> = kept.iter().map(|&i| dot(i)).collect();
    let smaller = smaller_spread(&dots.iter().map(|(a, _)| [f64::from(a[0]), f64::from(a[1])]).collect::<Vec<_>>());
    if checked && smaller < MIN_SPREAD_DEG {
        return Err((
            format!("the {name} eye's angles hardly moved between the dots ({smaller:.1} deg, needs {MIN_SPREAD_DEG:.0})"),
            None,
        ));
    }
    let map = q9_solve(&dots).ok_or_else(|| (format!("the {name} eye's angles don't give a quadratic map"), None))?;
    let rms = rms_of(&map, &dots);
    if checked && (!rms.is_finite() || rms > MAX_RMS_DEG) {
        return Err((format!("the {name} eye's map misses its dots by {rms:.1} deg (at most {MAX_RMS_DEG:.0})"), Some(rms)));
    }
    Ok(Q9Fit { map, rms, dropped })
}

/// One eye's map for a whole fit: Q9 with enough dots, else affine on the five (None when one of those is missing).
/// Ok: the map, its misses and how it was made ("q9", with the dots left out, or "affine"); Err: why, with the misses.
pub fn fit_eye_any(medians: &[Option<[f32; 2]>; 9], eye: usize) -> Result<(EyeMap, f64, String), (String, Option<f64>)> {
    // Not enough dots for Q9 (no corners: the Valve source's fit, or a panel from before them): the five dots' affine map
    if medians.iter().filter(|m| m.is_some()).count() < Q9_MIN_DOTS {
        let five: Option<Vec<[f32; 2]>> = medians[..FIVE].iter().copied().collect();
        let Some(five) = five else {
            return Err((format!("the {} eye has not all of the five dots", ["left", "right"][eye]), None));
        };
        let five: [[f32; 2]; 5] = [five[0], five[1], five[2], five[3], five[4]];
        return fit_eye_misses(&five, eye).map(|(map, rms)| (map, rms, "affine".to_owned()));
    }
    let fit = fit_eye_q9(medians, eye)?;
    let form = if fit.dropped.is_empty() {
        "q9".to_owned()
    } else {
        format!("q9 without {}", fit.dropped.iter().map(|&i| DOTS[i].0).collect::<Vec<_>>().join(", "))
    };
    Ok((fit.map, fit.rms, form))
}

/// Each eye's centre: where it should look at the centre dot less where its map (without a centre) puts its median
/// there; Err when either is further than MAX_CENTER_DEG.
pub fn centers(maps: &[EyeMap; 2], medians: [[f32; 2]; 2]) -> Result<[[f32; 2]; 2], String> {
    let mut out = [[0.0; 2]; 2];
    for eye in 0..2 {
        let map = EyeMap { center: [0.0; 2], ..maps[eye] };
        let [y, p] = map.apply(medians[eye]);
        let wanted = eye_direction(0.0, 0.0, eye);
        let center = [(wanted[0] - f64::from(y)) as f32, (wanted[1] - f64::from(p)) as f32];
        if !center.iter().all(|c| c.is_finite() && f64::from(c.abs()) <= MAX_CENTER_DEG) {
            return Err(format!(
                "the {} eye's centre {:+.1} {:+.1} deg is further than {MAX_CENTER_DEG:.0} deg",
                ["left", "right"][eye],
                center[0],
                center[1]
            ));
        }
        out[eye] = center;
    }
    Ok(out)
}

/// The file's text for both maps.
fn format_maps(maps: &[EyeMap; 2]) -> String {
    let mut text = String::from("# frameeyeosc's maps for gaze_source \"eyeball\" (fitted from the eye fit; degrees)\n");
    for (eye, name) in ["left", "right"].iter().enumerate() {
        let m = &maps[eye];
        text += &format!("yaw_{name}={} {} {}\n", m.yaw[0], m.yaw[1], m.yaw[2]);
        text += &format!("pitch_{name}={} {} {}\n", m.pitch[0], m.pitch[1], m.pitch[2]);
        text += &format!("center_{name}={} {}\n", m.center[0], m.center[1]);
        // The form: "q9" with its quadratic terms, "affine" without (a file from before has neither line: affine)
        if m.quad == crate::pupil::NO_QUAD {
            text += &format!("form_{name}=affine\n");
        } else {
            text += &format!("form_{name}=q9\n");
            text += &format!("quad_yaw_{name}={} {} {}\n", m.quad[0][0], m.quad[0][1], m.quad[0][2]);
            text += &format!("quad_pitch_{name}={} {} {}\n", m.quad[1][0], m.quad[1][1], m.quad[1][2]);
        }
    }
    text
}

/// Both maps from the file's text; None unless both eyes' yaw and pitch lines are there and are numbers (a missing
/// centre is 0), and a "q9" eye has its quadratic lines (an eye without a form line is affine, as in a file from
/// before the corners).
pub fn parse_maps(text: &str) -> Option<[EyeMap; 2]> {
    let value = |key: &str| -> Option<Vec<f32>> {
        let line = text.lines().find_map(|line| line.trim().strip_prefix(key)?.strip_prefix('='))?;
        line.split_whitespace().map(|v| v.parse::<f32>().ok().filter(|v| v.is_finite())).collect()
    };
    let eye = |name: &str| -> Option<EyeMap> {
        let yaw = value(&format!("yaw_{name}")).filter(|v| v.len() == 3)?;
        let pitch = value(&format!("pitch_{name}")).filter(|v| v.len() == 3)?;
        let center = value(&format!("center_{name}")).filter(|v| v.len() == 2).unwrap_or(vec![0.0, 0.0]);
        let form = text.lines().find_map(|line| line.trim().strip_prefix(&format!("form_{name}="))).map(str::trim);
        let quad = match form {
            None | Some("affine") => crate::pupil::NO_QUAD,
            Some("q9") => {
                let y = value(&format!("quad_yaw_{name}")).filter(|v| v.len() == 3)?;
                let p = value(&format!("quad_pitch_{name}")).filter(|v| v.len() == 3)?;
                [[y[0], y[1], y[2]], [p[0], p[1], p[2]]]
            }
            Some(_) => return None,
        };
        Some(EyeMap {
            yaw: [yaw[0], yaw[1], yaw[2]],
            pitch: [pitch[0], pitch[1], pitch[2]],
            center: [center[0], center[1]],
            quad,
        })
    };
    Some([eye("left")?, eye("right")?])
}

/// What a whole fit did to the maps (MapStore::outcome; status.json "eyeball_map"), so the panel can say when a fit kept
/// the old map.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct FitOutcome {
    /// A new map was taken.
    pub fitted: bool,
    /// Not taken: whether an older map stays (false: there is none).
    pub kept: bool,
    /// Why not: "misses" (an eye's map missed its dots by more than MAX_RMS_DEG), "not_settled" (an eye's filter had
    /// not settled at some dot) or "other" (the eyes hardly moved, no map, a centre too far); None when fitted.
    pub reason: Option<&'static str>,
    /// The eye that reason is about: [left, right] (both true: both eyes; neither: not one eye's).
    pub eyes: [bool; 2],
    /// How far each eye's map missed its dots (degrees, rms), where there was one to measure.
    pub misses_deg: [Option<f64>; 2],
}

/// The maps, the file they are kept in, and the dots of the fit in progress.
#[derive(Clone, Debug, Default)]
pub struct MapStore {
    /// Where they are saved (None: never, as in a replay).
    path: Option<PathBuf>,
    pub maps: Option<[EyeMap; 2]>,
    /// The dots of the set in progress (DOTS order): each eye's median (None: the eye didn't count there).
    dots: [[Option<[f32; 2]>; 2]; 9],
    /// A set is in progress (a centre dot started it; the eyes-shut step ends it).
    started: bool,
    /// What the last whole fit did (set at its eyes-shut step; taken by the caller).
    pub outcome: Option<FitOutcome>,
}

impl MapStore {
    /// The maps from `path` if it has them; the line to log.
    pub fn load(path: Option<PathBuf>) -> (Self, String) {
        let read = path.as_deref().map(|path| (path, std::fs::read_to_string(path)));
        let (maps, line) = match read {
            None => (None, "Eyeball map: no file".to_owned()),
            Some((path, Ok(text))) => match parse_maps(&text) {
                Some(maps) => (Some(maps), format!("Eyeball map read from {}", path.display())),
                None => (None, format!("Eyeball map: {} has none; the next whole eye fit makes one", path.display())),
            },
            Some((path, Err(_))) => (None, format!("Eyeball map: none yet ({}); the next whole eye fit makes one", path.display())),
        };
        (Self { path, maps, dots: [[None; 2]; 9], started: false, outcome: None }, line)
    }

    /// Never write the file (a replay).
    pub fn read_only(mut self) -> Self {
        self.path = None;
        self
    }

    fn save(&self, maps: &[EyeMap; 2]) -> Option<String> {
        let path = self.path.as_deref()?;
        Some(match write_atomically(path, &format_maps(maps)) {
            Ok(()) => format!("Eyeball map saved to {}", path.display()),
            Err(error) => format!("Could not save {}: {error}", path.display()),
        })
    }

    /// A whole fit that took no map: the old one kept, or none made.
    fn not_taken(&self, reason: &'static str, eyes: [bool; 2], misses_deg: [Option<f64>; 2]) -> FitOutcome {
        FitOutcome { fitted: false, kept: self.maps.is_some(), reason: Some(reason), eyes, misses_deg }
    }

    /// A gaze capture is done: its target and each eye's median filter angles with how many samples they are of (None
    /// without any). Returns the lines to log (none for a capture that changes nothing).
    pub fn capture_done(&mut self, target: &str, medians: [Option<([f32; 2], u32)>; 2]) -> Vec<String> {
        if target == END_TARGET {
            return self.end_of_fit();
        }
        let Some(dot) = DOTS.iter().position(|(name, ..)| *name == target) else {
            return Vec::new();
        };
        let mut lines = Vec::new();
        // A centre dot starts a new set
        if dot == 0 {
            self.dots = [[None; 2]; 9];
            self.started = true;
        }
        let usable = medians.map(|m| m.filter(|(_, n)| *n >= MIN_SAMPLES).map(|(angles, _)| angles));
        self.dots[dot] = usable;
        let [Some(left), Some(right)] = usable else {
            // Said plainly: which eye did not count, and what that means for the centre or the whole fit
            let counts = medians.map(|m| m.map_or(0, |(_, n)| n));
            let eyes = match usable {
                [None, None] => "neither eye counted",
                [None, Some(_)] => "the left eye not counted",
                _ => "the right eye not counted",
            };
            let outcome = if dot == 0 && self.maps.is_some() {
                "the centre is kept as it was".to_owned()
            } else if dot == 0 {
                "a whole fit started here makes no eyeball map".to_owned()
            } else if dot >= FIVE {
                "a corner left out (a quadratic map needs 7 of the 9 dots, else the five dots' affine map)".to_owned()
            } else {
                format!("this fit makes no eyeball map ({})", if self.maps.is_some() { "the old one stays" } else { "there is none yet" })
            };
            lines.push(format!(
                "Eyeball map: {eyes} at the {target} dot: {} and {} samples of a settled filter (L, R; needs \
                 {MIN_SAMPLES}); {outcome}. Fit again once the journal's centre line says both eyes have settled",
                counts[0], counts[1]
            ));
            return lines;
        };
        if dot == 0 {
            // A re-center: the centre with the map there is
            if let Some(maps) = self.maps {
                match centers(&maps, [left, right]) {
                    Ok(c) => {
                        let mut new = maps;
                        new[0].center = c[0];
                        new[1].center = c[1];
                        lines.push(format!(
                            "Eyeball map: centre L {:+.2} {:+.2}, R {:+.2} {:+.2} deg",
                            c[0][0], c[0][1], c[1][0], c[1][1]
                        ));
                        self.maps = Some(new);
                        lines.extend(self.save(&new));
                    }
                    Err(why) => lines.push(format!("Eyeball map: centre kept: {why}")),
                }
            }
        }
        lines
    }

    /// The whole fit's eyes-shut step: the maps from its dots (Q9 with the corners, else affine on the five), the centre
    /// from its centre dot, and what came of it (`outcome`).
    fn end_of_fit(&mut self) -> Vec<String> {
        if !std::mem::take(&mut self.started) {
            return Vec::new();
        }
        let dots = std::mem::replace(&mut self.dots, [[None; 2]; 9]);
        let state = if self.maps.is_some() { "kept as it was" } else { "not made" };
        // One of the five missing for an eye: no map (said with the dots and the eyes)
        let unsettled = [0, 1].map(|eye| dots[..FIVE].iter().any(|dot| dot[eye].is_none()));
        if unsettled.contains(&true) {
            let missing: Vec<&str> =
                (0..FIVE).filter(|&i| dots[i].iter().any(Option::is_none)).map(|i| DOTS[i].0).collect();
            self.outcome = Some(self.not_taken("not_settled", unsettled, [None; 2]));
            return vec![format!(
                "Eyeball map {state}: no settled eyes at the {} dot{} of this fit",
                missing.join(", "),
                if missing.len() == 1 { "" } else { "s" }
            )];
        }
        let tries = [0, 1].map(|eye| fit_eye_any(&dots.map(|dot| dot[eye]), eye));
        let misses = tries.each_ref().map(|t| match t {
            Ok((_, rms, _)) => Some(*rms),
            Err((_, rms)) => *rms,
        });
        if tries.iter().any(Result::is_err) {
            // Every eye that failed, and why (the outcome names the eyes whose maps missed their dots when that was
            // why; else the eyes that failed otherwise)
            let whys: Vec<&str> = tries.iter().filter_map(|t| t.as_ref().err().map(|(why, _)| why.as_str())).collect();
            let failed = tries.each_ref().map(Result::is_err);
            let missed = tries.each_ref().map(|t| matches!(t, Err((_, Some(_)))));
            self.outcome = Some(if missed.contains(&true) {
                self.not_taken("misses", missed, misses)
            } else {
                self.not_taken("other", failed, misses)
            });
            return vec![format!("Eyeball map {state}: {}", whys.join("; "))];
        }
        let fits = tries.map(|t| t.unwrap_or_else(|_| unreachable!()));
        let maps = [fits[0].0, fits[1].0];
        let centre_dot = [dots[0][0].unwrap_or_default(), dots[0][1].unwrap_or_default()];
        let mut lines = Vec::new();
        match centers(&maps, centre_dot) {
            Ok(centre) => {
                let mut new = maps;
                new[0].center = centre[0];
                new[1].center = centre[1];
                lines.push(format!(
                    "Eyeball map fitted (L {}, R {}): misses its dots by L {:.2}, R {:.2} deg; centre L {:+.2} {:+.2}, \
                     R {:+.2} {:+.2}",
                    fits[0].2, fits[1].2, fits[0].1, fits[1].1, centre[0][0], centre[0][1], centre[1][0], centre[1][1]
                ));
                self.outcome = Some(FitOutcome { fitted: true, kept: false, reason: None, eyes: [false; 2], misses_deg: misses });
                self.maps = Some(new);
                lines.extend(self.save(&new));
            }
            Err(why) => {
                lines.push(format!("Eyeball map {state}: {why}"));
                self.outcome = Some(self.not_taken("other", [false; 2], misses));
            }
        }
        lines
    }
}

/// Write a file through a temporary one next to it, renamed over it.
fn write_atomically(path: &Path, text: &str) -> std::io::Result<()> {
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    let name = path.file_name().and_then(|n| n.to_str()).unwrap_or("eyeball_map");
    let tmp = path.with_file_name(format!(".{name}.new"));
    std::fs::write(&tmp, text)?;
    std::fs::rename(&tmp, path)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A made-up eye: its filter reads 0.8 of each angle plus an offset, slightly cross-coupled.
    fn reading(eye: usize, yaw: f64, pitch: f64) -> [f32; 2] {
        let [y, p] = eye_direction(yaw, pitch, eye);
        [(0.8 * y + 0.05 * p + 3.0) as f32, (0.8 * p - 0.03 * y - 2.0) as f32]
    }

    fn medians(eye_reading: impl Fn(usize, f64, f64) -> [f32; 2], dot: usize, n: u32) -> [Option<([f32; 2], u32)>; 2] {
        let (_, yaw, pitch) = DOTS[dot];
        [0, 1].map(|eye| Some((eye_reading(eye, yaw, pitch), n)))
    }

    #[test]
    fn the_dots_are_where_the_panel_puts_them() {
        // The left eye sees the centre dot a little to its right, the right eye to its left (2.0 deg at 0.9 m)
        let [l, r] = [eye_direction(0.0, 0.0, 0), eye_direction(0.0, 0.0, 1)];
        assert!((l[0] - 2.004).abs() < 0.01 && (r[0] + 2.004).abs() < 0.01 && l[1] == 0.0, "{l:?} {r:?}");
        let up = eye_direction(0.0, 15.0, 0);
        assert!((up[1] - 15.0).abs() < 1e-9, "{up:?}");
    }

    /// A whole fit's dots (DOTS 0..n) with this reading, then the eyes-shut step; the lines.
    fn whole_fit(store: &mut MapStore, eye_reading: impl Fn(usize, f64, f64) -> [f32; 2], n: usize) -> Vec<String> {
        let mut lines = Vec::new();
        for dot in 0..n {
            lines.extend(store.capture_done(DOTS[dot].0, medians(&eye_reading, dot, 120)));
        }
        lines.extend(store.capture_done(END_TARGET, [None; 2]));
        lines
    }

    #[test]
    fn a_whole_fit_makes_the_maps_and_a_re_center_moves_only_the_centre() {
        let dir = std::env::temp_dir().join(format!("frameeyeosc-eyeball-map-{}", std::process::id()));
        let path = dir.join(FILE_NAME);
        let _ = std::fs::remove_dir_all(&dir);
        let (mut store, line) = MapStore::load(Some(path.clone()));
        assert!(store.maps.is_none() && line.contains("none yet"), "{line}");
        // The five dots in the panel's order (the Valve source's fit, no corners): nothing until the eyes-shut step,
        // then the affine map
        for dot in 0..5 {
            assert!(store.capture_done(DOTS[dot].0, medians(reading, dot, 120)).is_empty());
        }
        assert!(store.maps.is_none() && store.outcome.is_none());
        let lines = store.capture_done(END_TARGET, [None; 2]);
        let maps = store.maps.expect("fitted");
        assert!(lines[0].starts_with("Eyeball map fitted (L affine, R affine): misses its dots by L 0.00"), "{lines:?}");
        assert_eq!(maps.map(|m| m.quad), [crate::pupil::NO_QUAD; 2]);
        let outcome = store.outcome.take().expect("an outcome");
        assert!(outcome.fitted && outcome.reason.is_none() && outcome.misses_deg.iter().all(|m| m.is_some_and(|m| m < 0.01)));
        assert!(lines.last().unwrap().starts_with("Eyeball map saved to"));
        // A second eyes-shut try of the same fit: nothing more
        assert!(store.capture_done(END_TARGET, [None; 2]).is_empty() && store.outcome.is_none());
        // The map puts each eye's readings where it looked, the centre included
        for eye in 0..2 {
            for (_, yaw, pitch) in DOTS {
                let got = maps[eye].apply(reading(eye, yaw, pitch));
                let want = eye_direction(yaw, pitch, eye);
                assert!((f64::from(got[0]) - want[0]).abs() < 0.01 && (f64::from(got[1]) - want[1]).abs() < 0.01);
            }
        }
        // Read back from the file
        let (again, line) = MapStore::load(Some(path.clone()));
        assert_eq!(again.maps, Some(maps));
        assert!(line.starts_with("Eyeball map read from"));
        // A re-center after the headset sat differently (every reading 2 deg further right): only the centre moves
        let shifted = |eye: usize, yaw: f64, pitch: f64| {
            let [y, p] = reading(eye, yaw, pitch);
            [y + 2.0, p]
        };
        let lines = store.capture_done("center", medians(shifted, 0, 60));
        assert!(lines[0].starts_with("Eyeball map: centre L"), "{lines:?}");
        let recentred = store.maps.unwrap();
        for eye in 0..2 {
            assert_eq!((recentred[eye].yaw, recentred[eye].pitch), (maps[eye].yaw, maps[eye].pitch));
            let got = recentred[eye].apply(shifted(eye, 0.0, 0.0));
            let want = eye_direction(0.0, 0.0, eye);
            assert!((f64::from(got[0]) - want[0]).abs() < 0.01, "{got:?} {want:?}");
        }
        // Too few samples of a settled filter at a dot: said plainly, nothing changed; a set without one of its five
        // dots fits nothing, and its eyes-shut step says so
        store.capture_done("center", medians(shifted, 0, 120));
        let recentred = store.maps.unwrap();
        let mut one = medians(reading, 1, 120);
        one[1] = Some((one[1].unwrap().0, 10));
        let lines = store.capture_done("up", one);
        assert_eq!(
            lines,
            vec![
                "Eyeball map: the right eye not counted at the up dot: 120 and 10 samples of a settled filter (L, R; needs \
                 30); this fit makes no eyeball map (the old one stays). Fit again once the journal's centre line says \
                 both eyes have settled"
                    .to_owned()
            ]
        );
        assert_eq!(store.maps, Some(recentred));
        for dot in [2, 3, 4] {
            assert!(store.capture_done(DOTS[dot].0, medians(reading, dot, 120)).is_empty());
        }
        let lines = store.capture_done(END_TARGET, [None; 2]);
        assert_eq!(lines, vec!["Eyeball map kept as it was: no settled eyes at the up dot of this fit".to_owned()]);
        assert_eq!(store.maps, Some(recentred));
        // ...and the outcome says so: kept, the right eye not settled
        let kept = FitOutcome { fitted: false, kept: true, reason: Some("not_settled"), eyes: [false, true], misses_deg: [None; 2] };
        assert_eq!(store.outcome.take(), Some(kept));
        // A centre dot without settled eyes keeps the centre
        let lines = store.capture_done("center", [None; 2]);
        assert!(lines[0].starts_with("Eyeball map: neither eye counted at the center dot"), "{lines:?}");
        assert!(lines[0].contains("0 and 0 samples") && lines[0].contains("the centre is kept as it was"), "{lines:?}");
        assert_eq!(store.maps, Some(recentred));
        // An eyes-shut step without a fit before it (no centre dot since the last): nothing
        let mut fresh = MapStore::default();
        assert!(fresh.capture_done(END_TARGET, [None; 2]).is_empty() && fresh.outcome.is_none());
        // A read-only store (the replay) never writes
        std::fs::remove_file(&path).unwrap();
        let (store, _) = MapStore::load(Some(path.clone()));
        let mut store = store.read_only();
        whole_fit(&mut store, reading, 9);
        assert!(store.maps.is_some() && !path.exists());
        std::fs::remove_dir_all(&dir).unwrap();
    }

    /// A made-up eye whose angles bend: pitch with yaw² and yaw with yaw·pitch (as the right eye's on the headset), which
    /// an affine map can't follow.
    fn bent(eye: usize, yaw: f64, pitch: f64) -> [f32; 2] {
        let [y, p] = eye_direction(yaw, pitch, eye);
        [(0.8 * y + 0.004 * y * p + 3.0) as f32, (0.8 * p - 0.012 * y * y - 2.0) as f32]
    }

    #[test]
    fn nine_dots_make_a_quadratic_map_that_follows_bent_angles() {
        let mut store = MapStore::default();
        let lines = whole_fit(&mut store, bent, 9);
        assert!(lines[0].starts_with("Eyeball map fitted (L q9, R q9): misses its dots by L 0.3"), "{lines:?}");
        let maps = store.maps.unwrap();
        assert!(maps.iter().all(|m| m.quad != crate::pupil::NO_QUAD));
        // Between the dots too (the gaze check's inner ring), each eye within 0.6 deg (the made-up bend isn't exactly
        // quadratic the other way round)
        for eye in 0..2 {
            for (yaw, pitch) in [(10.0, 7.5), (-10.0, -7.5), (10.0, 0.0), (0.0, 0.0)] {
                let got = maps[eye].apply(bent(eye, yaw, pitch));
                let want = eye_direction(yaw, pitch, eye);
                assert!((f64::from(got[0]) - want[0]).hypot(f64::from(got[1]) - want[1]) < 0.6, "{eye} {yaw} {pitch}: {got:?}");
            }
        }
        // The same angles on five dots: the affine map misses (the bend is about 4.8 deg at the side dots)
        let mut five = MapStore::default();
        let lines = whole_fit(&mut five, bent, 5);
        let affine = five.maps.map(|m| m[0]).filter(|_| lines[0].contains("affine"));
        let worse = affine.map_or(true, |map| {
            let got = map.apply(bent(0, 20.0, 15.0));
            let want = eye_direction(20.0, 15.0, 0);
            (f64::from(got[0]) - want[0]).hypot(f64::from(got[1]) - want[1]) > 1.0
        });
        assert!(worse, "{lines:?}");
        // The file keeps the form and its terms, and reads back the same
        let text = format_maps(&maps);
        assert!(text.contains("form_left=q9") && text.contains("quad_pitch_right="), "{text}");
        assert_eq!(parse_maps(&text), Some(maps));
    }

    #[test]
    fn one_bad_corner_is_left_out_and_too_few_dots_fall_back_to_affine() {
        // The left eye frozen at the up-left corner (its angles still those of the centre dot)
        let frozen = |eye: usize, yaw: f64, pitch: f64| {
            if eye == 0 && yaw < -10.0 && pitch > 10.0 { bent(eye, 0.0, 0.0) } else { bent(eye, yaw, pitch) }
        };
        let medians9 = |eye: usize, f: &dyn Fn(usize, f64, f64) -> [f32; 2]| DOTS.map(|(_, y, p)| Some(f(eye, y, p)));
        let fit = fit_eye_q9(&medians9(0, &frozen), 0).unwrap();
        assert_eq!(fit.dropped, vec![5], "{fit:?}");
        assert!(fit.rms < 0.5, "{fit:?}");
        // The right eye has nothing to leave out
        assert!(fit_eye_q9(&medians9(1, &frozen), 1).unwrap().dropped.is_empty());
        // Without leaving it out the fit would be far off (what the drop is for)
        let all: Vec<_> = (0..9).map(|i| (frozen(0, DOTS[i].1, DOTS[i].2), eye_direction(DOTS[i].1, DOTS[i].2, 0))).collect();
        assert!(rms_of(&q9_solve(&all).unwrap(), &all) > 2.0);
        // A whole fit with it: the left eye's map made without that corner, said in the journal
        let mut store = MapStore::default();
        let lines = whole_fit(&mut store, frozen, 9);
        assert!(lines[0].starts_with("Eyeball map fitted (L q9 without up_left, R q9)"), "{lines:?}");
        // Two corners missing for an eye (not settled there): 7 dots, still Q9; three: the five dots' affine map
        let mut seven = medians9(1, &bent);
        seven[6] = None;
        seven[7] = None;
        assert!(fit_eye_any(&seven, 1).unwrap().2 == "q9");
        seven[8] = None;
        let (_, _, form) = fit_eye_any(&seven, 1).unwrap_or_else(|e| panic!("{e:?}"));
        assert_eq!(form, "affine");
        // A whole fit whose corners didn't count (one eye unsettled at three corners): affine for that eye
        let mut store = MapStore::default();
        for dot in 0..9 {
            let mut m = medians(bent, dot, 120);
            if dot >= 6 {
                m[1] = Some((m[1].unwrap().0, 5));
            }
            store.capture_done(DOTS[dot].0, m);
        }
        let lines = store.capture_done(END_TARGET, [None; 2]);
        assert!(lines[0].starts_with("Eyeball map fitted (L q9, R affine)") || lines[0].contains("R affine"), "{lines:?}");
    }

    #[test]
    fn a_map_that_misses_its_dots_or_eyes_that_did_not_move_are_refused() {
        let mut store = MapStore::default();
        // The eyes did not move between the dots
        let still = |_eye: usize, _yaw: f64, _pitch: f64| [1.0f32, 2.0];
        whole_fit(&mut store, still, 5);
        assert!(store.maps.is_none());
        whole_fit(&mut store, still, 9);
        assert!(store.maps.is_none());
        // One dot 6.8 off in the right eye's reading (as an eye still settling): the map misses by about 3.3 degrees,
        // which the old limit of 4 let through and MAX_RMS_DEG refuses
        let medians5 = |shift: f32| {
            [0, 1, 2, 3, 4].map(|d| {
                let r = reading(1, DOTS[d].1, DOTS[d].2);
                if d == 0 { [r[0] + shift, r[1]] } else { r }
            })
        };
        let err = fit_eye(&medians5(6.8), 1).unwrap_err();
        assert!(err.contains("misses its dots by 3.3 deg (at most 3)"), "{err}");
        assert!(fit_eye(&medians5(4.0), 1).is_ok());
        // A wild reading at one dot: misses by far more than MAX_RMS_DEG
        let wild = |eye: usize, yaw: f64, pitch: f64| {
            let r = reading(eye, yaw, pitch);
            if yaw > 10.0 { [r[0] - 25.0, r[1] + 20.0] } else { r }
        };
        let lines = whole_fit(&mut store, wild, 5);
        assert!(store.maps.is_none() && lines.last().unwrap().contains("misses its dots"), "{lines:?}");
        // Its outcome: not made (no old map), both eyes' maps missed their dots, by how much; the journal names both
        let outcome = store.outcome.take().expect("an outcome");
        assert!(!outcome.fitted && !outcome.kept && outcome.reason == Some("misses") && outcome.eyes == [true, true]);
        assert!(outcome.misses_deg.iter().all(|m| m.is_some_and(|m| m > MAX_RMS_DEG)), "{outcome:?}");
        assert!(lines[0].contains("the left eye's map misses") && lines[0].contains("the right eye's map misses"), "{lines:?}");
        // One eye only (the right one 6.8° off at the centre dot), with an old map: kept, the right eye, its misses; the
        // journal names only that eye (what the outcome says)
        store.maps = Some([fit_eye(&[0, 1, 2, 3, 4].map(|d| reading(0, DOTS[d].1, DOTS[d].2)), 0).unwrap().0; 2]);
        let off = |eye: usize, yaw: f64, pitch: f64| {
            let r = reading(eye, yaw, pitch);
            if eye == 1 && yaw == 0.0 && pitch == 0.0 { [r[0] + 6.8, r[1]] } else { r }
        };
        let lines = whole_fit(&mut store, off, 5);
        let outcome = store.outcome.take().expect("an outcome");
        assert!(!outcome.fitted && outcome.kept && outcome.reason == Some("misses") && outcome.eyes == [false, true]);
        assert!(outcome.misses_deg[0].is_some_and(|m| m < 0.01) && outcome.misses_deg[1].is_some_and(|m| m > 3.0));
        let last = lines.last().unwrap();
        assert!(last.contains("the right eye's map misses") && !last.contains("left"), "{lines:?}");
        // A mixed failure: the left eye's angles didn't move, the right eye's map missed: both said, the outcome is the
        // misses of the right eye
        let mixed = |eye: usize, yaw: f64, pitch: f64| if eye == 0 { [1.0, 2.0] } else { off(eye, yaw, pitch) };
        let lines = whole_fit(&mut store, mixed, 5);
        let outcome = store.outcome.take().expect("an outcome");
        assert!(outcome.reason == Some("misses") && outcome.eyes == [false, true], "{outcome:?}");
        let last = lines.last().unwrap();
        assert!(last.contains("the left eye's angles hardly moved") && last.contains("the right eye's map misses"), "{lines:?}");
        // The last of the five without a settled eye: not settled, that eye
        for dot in 0..4 {
            store.capture_done(DOTS[dot].0, medians(reading, dot, 120));
        }
        let mut right = medians(reading, 4, 120);
        right[0] = Some((right[0].unwrap().0, 5));
        store.capture_done("right", right);
        store.capture_done(END_TARGET, [None; 2]);
        assert_eq!(store.outcome.map(|o| (o.reason, o.eyes, o.kept)), Some((Some("not_settled"), [true, false], true)));
        // A centre too far is not taken
        let maps = [fit_eye(&[0, 1, 2, 3, 4].map(|d| reading(0, DOTS[d].1, DOTS[d].2)), 0).unwrap().0; 2];
        assert!(centers(&maps, [[60.0, 0.0], [3.0, -2.0]]).unwrap_err().contains("left eye's centre"));
    }

    #[test]
    fn an_affine_file_from_before_the_corners_still_reads() {
        let text = "yaw_left=1 2 3\npitch_left=4 5 6\ncenter_left=0.5 -0.5\nyaw_right=1 2 3\npitch_right=4 5 6\n";
        let maps = parse_maps(text).unwrap();
        assert_eq!(maps[0].quad, crate::pupil::NO_QUAD);
        assert_eq!(maps[0].center, [0.5, -0.5]);
        // A q9 eye without its quadratic lines, or a form this doesn't know: not read
        assert!(parse_maps(&format!("{text}form_left=q9\n")).is_none());
        assert!(parse_maps(&format!("{text}form_left=cubic\n")).is_none());
        // What this writes for an affine map says so, and reads back
        let written = format_maps(&maps);
        assert!(written.contains("form_left=affine") && !written.contains("quad_"));
        assert_eq!(parse_maps(&written), Some(maps));
    }


    /// The map forms on recorded medians (the eyeball-drift harness's, exported by fitcases_*.py to $FIT_CASES): the
    /// affine map on the five dots, Q9 on the nine without leaving any out, and Q9 as the code fits it (robust), each
    /// scored as the harness does. Run by hand: FIT_CASES=cases.json cargo test --release form_harness -- --ignored
    /// --nocapture
    #[test]
    #[ignore]
    fn form_harness() {
        let Ok(path) = std::env::var("FIT_CASES") else { return };
        let cases: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap();
        // A case's dots: [yaw, pitch, a, b] (the target and the filter's median angles), by target
        let index = |yaw: f64, pitch: f64| DOTS.iter().position(|(_, y, p)| *y == yaw && *p == pitch);
        let medians9 = |dots: &serde_json::Value| {
            let mut m = [None; 9];
            for d in dots.as_array().unwrap() {
                let d: Vec<f64> = d.as_array().unwrap().iter().map(|x| x.as_f64().unwrap_or(f64::NAN)).collect();
                if let Some(i) = index(d[0], d[1]).filter(|_| d[2].is_finite() && d[3].is_finite()) {
                    m[i] = Some([d[2] as f32, d[3] as f32]);
                }
            }
            m
        };
        let error = |map: &EyeMap, d: &[f64], eye: usize| {
            let [y, p] = map.apply([d[2] as f32, d[3] as f32]);
            let w = eye_direction(d[0], d[1], eye);
            (f64::from(y) - w[0]).hypot(f64::from(p) - w[1])
        };
        type Form = fn(&[Option<[f32; 2]>; 9], usize) -> Option<(EyeMap, f64, usize)>;
        let affine: Form = |m, eye| {
            let five: Option<Vec<[f32; 2]>> = m[..FIVE].iter().copied().collect();
            let five = five?;
            let five = [five[0], five[1], five[2], five[3], five[4]];
            // (the misses whatever they are: the fit made without its checks, as fit_eye_misses does before them)
            match fit_eye_misses(&five, eye) {
                Ok((map, rms)) => Some((map, rms, 0)),
                Err((_, Some(rms))) => {
                    let dots: Vec<_> = (0..5).map(|i| (five[i], eye_direction(DOTS[i].1, DOTS[i].2, eye))).collect();
                    let map = q9_affine(&dots)?;
                    Some((map, rms, 0))
                }
                Err(_) => None,
            }
        };
        let plain: Form = |m, eye| {
            let dots: Vec<_> = (0..9).filter_map(|i| m[i].map(|a| (a, eye_direction(DOTS[i].1, DOTS[i].2, eye)))).collect();
            (dots.len() == 9).then_some(())?;
            let map = q9_solve(&dots)?;
            Some((map, rms_of(&map, &dots), 0))
        };
        let robust: Form = |m, eye| {
            (m.iter().all(Option::is_some)).then_some(())?;
            q9_fit(m, eye, false).ok().map(|fit| (fit.map, fit.rms, fit.dropped.len()))
        };
        let forms: [(&str, Form); 3] = [("A", affine), ("Q9", plain), ("Q9code", robust)];
        for run in cases["live"].as_array().unwrap() {
            for (name, form) in forms {
                let mut misses = [0.0; 2];
                let mut inner = [Vec::new(), Vec::new()];
                let mut both = Vec::new();
                let mut dropped = 0;
                let mut maps = Vec::new();
                for eye in 0..2 {
                    let dots = &run["eyes"][eye];
                    let (map, rms, d) = form(&medians9(dots), eye).unwrap();
                    misses[eye] = rms;
                    dropped += d;
                    maps.push(map);
                }
                let all0 = run["eyes"][0].as_array().unwrap();
                let all1 = run["eyes"][1].as_array().unwrap();
                for (d0, d1) in all0.iter().zip(all1) {
                    let d0: Vec<f64> = d0.as_array().unwrap().iter().map(|x| x.as_f64().unwrap_or(f64::NAN)).collect();
                    let d1: Vec<f64> = d1.as_array().unwrap().iter().map(|x| x.as_f64().unwrap_or(f64::NAN)).collect();
                    if index(d0[0], d0[1]).is_some() || !d0[2].is_finite() || !d1[2].is_finite() {
                        continue;
                    }
                    inner[0].push(error(&maps[0], &d0, 0));
                    inner[1].push(error(&maps[1], &d1, 1));
                    let a = maps[0].apply([d0[2] as f32, d0[3] as f32]);
                    let b = maps[1].apply([d1[2] as f32, d1[3] as f32]);
                    let mean = [f64::from(a[0] + b[0]) / 2.0, f64::from(a[1] + b[1]) / 2.0];
                    both.push((mean[0] - d0[0]).hypot(mean[1] - d0[1]));
                }
                let avg = |v: &[f64]| v.iter().sum::<f64>() / v.len() as f64;
                println!(
                    "{} {name:6} misses L {:.2} R {:.2} | inner both {:.2} (L {:.2} R {:.2}) dropped {dropped}",
                    run["run"].as_str().unwrap(), misses[0], misses[1], avg(&both), avg(&inner[0]), avg(&inner[1])
                );
            }
        }
        for run in cases["reseat"].as_array().unwrap() {
            let rounds = run["rounds"].as_array().unwrap();
            for (name, form) in forms {
                let mut misses = [Vec::new(), Vec::new()];
                let mut next = [Vec::new(), Vec::new()];
                let mut dropped = 0;
                for r in 0..rounds.len().saturating_sub(1) {
                    for eye in 0..2 {
                        let now = medians9(&rounds[r][eye]);
                        let then = medians9(&rounds[r + 1][eye]);
                        if !then.iter().all(Option::is_some) {
                            continue;
                        }
                        let Some((map, rms, d)) = form(&now, eye) else { continue };
                        dropped += d;
                        misses[eye].push(rms);
                        let errs: Vec<f64> = (0..9)
                            .map(|i| {
                                let a = then[i].unwrap();
                                error(&map, &[DOTS[i].1, DOTS[i].2, f64::from(a[0]), f64::from(a[1])], eye)
                            })
                            .collect();
                        next[eye].push(errs.iter().sum::<f64>() / 9.0);
                    }
                }
                let median = |v: &mut Vec<f64>| {
                    v.sort_by(f64::total_cmp);
                    v[v.len() / 2]
                };
                let over = misses[1].iter().filter(|&&m| m > 3.0).count();
                let n = misses[1].len();
                let avg = |v: &[f64]| v.iter().sum::<f64>() / v.len() as f64;
                let (nl, nr) = (avg(&next[0]), avg(&next[1]));
                println!(
                    "{} {name:6} misses p50 L {:.2} R {:.2} (R > 3: {over}/{n}) | next round 9 dots L {nl:.2} R {nr:.2} dropped {dropped}",
                    run["run"].as_str().unwrap(), median(&mut misses[0]), median(&mut misses[1])
                );
            }
        }
    }

    /// The affine map through five dots without its checks (for the harness test: its misses whatever they are).
    fn q9_affine(dots: &[([f32; 2], [f64; 2])]) -> Option<EyeMap> {
        let mut c = [[0.0; 3]; 2];
        for (axis, out) in c.iter_mut().enumerate() {
            let mut a = vec![vec![0.0; 3]; 3];
            let mut b = vec![0.0; 3];
            for (angles, wanted) in dots {
                let row = [1.0, f64::from(angles[0]), f64::from(angles[1])];
                for j in 0..3 {
                    for k in 0..3 {
                        a[j][k] += row[j] * row[k];
                    }
                    b[j] += row[j] * wanted[axis];
                }
            }
            out.copy_from_slice(&solve_n(a, b)?);
        }
        Some(EyeMap { yaw: c[0].map(|x| x as f32), pitch: c[1].map(|x| x as f32), center: [0.0; 2], quad: crate::pupil::NO_QUAD })
    }

}
