//! The eyeball source's maps (gaze_source "eyeball", eyeball.rs): each eye's affine map from its filter's angles to
//! the direction it looks, and its centre for this wear, the same kind of map as the pupil source's (pupil.rs EyeMap).
//! frameeyeosc fits them itself from the panel's eye fit, so the panel needs nothing new: while gaze_source is
//! "eyeball", each gaze capture also keeps the median of each eye's filter angles over the samples it averaged
//! (capture.rs), and
//!   - a "center" capture (the first dot of a whole fit, or a re-center by hand or when the headset is put on) sets
//!     each eye's centre with the map it has, and starts a new set of dots;
//!   - once the set has all five dots of the whole fit (center, up, down, left, right; the latest try of each), the
//!     map is fitted to where each eye looks at each dot (as the panel's eye fit places them: TARGET_DISTANCE_M away,
//!     the eyes IPD_M apart) and the centre is set from the centre dot with it.
//! Each eye's median needs MIN_SAMPLES samples; a map that misses its dots by more than MAX_RMS_DEG, or a centre further
//! than MAX_CENTER_DEG, is not taken (the old one stays). The maps are kept in a file of their own (`eyeball_map` next to
//! the eyelid calibration), never in config.json, which is the panel's: they last across restarts; the replay reads
//! the file and never writes it.

use crate::pupil::EyeMap;
use std::path::{Path, PathBuf};

/// The eye fit's dots (degrees: yaw + right, pitch + up), as the panel's eye fit shows them (gaze_fit.cpp kTargets).
pub const DOTS: [(&str, f64, f64); 5] =
    [("center", 0.0, 0.0), ("up", 0.0, 15.0), ("down", 0.0, -15.0), ("left", -20.0, 0.0), ("right", 20.0, 0.0)];
/// How far away the dots are (m), as the panel draws them (gaze_fit.h kTargetDistanceM)...
pub const TARGET_DISTANCE_M: f64 = 0.9;
/// ...and the eyes' distance taken for them (m; a typical adult's, as frameeyeosc doesn't know the wearer's: 5 mm off
/// moves each eye's direction at the dots by about 0.16 degrees, which the centre takes up).
pub const IPD_M: f64 = 0.063;
/// Each eye's median at a dot needs at least this many samples (as the panel's pupil map, gaze_fit.h kMinPupilFresh).
pub const MIN_SAMPLES: u32 = 30;
/// A map is taken only while it fits its dots within this (degrees, root mean square)...
pub const MAX_RMS_DEG: f64 = 4.0;
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
    let name = ["left", "right"][eye];
    let u: Vec<f64> = medians.iter().map(|m| f64::from(m[0])).collect();
    let v: Vec<f64> = medians.iter().map(|m| f64::from(m[1])).collect();
    if !u.iter().chain(&v).all(|x| x.is_finite()) {
        return Err(format!("the {name} eye's angles are not all numbers"));
    }
    let wanted: Vec<[f64; 2]> = (0..5).map(|i| eye_direction(DOTS[i].1, DOTS[i].2, eye)).collect();
    let (mu, mv) = (u.iter().sum::<f64>() / 5.0, v.iter().sum::<f64>() / 5.0);
    let suu = u.iter().map(|x| (x - mu).powi(2)).sum::<f64>() / 5.0;
    let svv = v.iter().map(|x| (x - mv).powi(2)).sum::<f64>() / 5.0;
    let suv = u.iter().zip(&v).map(|(x, y)| (x - mu) * (y - mv)).sum::<f64>() / 5.0;
    let half = (suu + svv) / 2.0;
    let smaller = (half - (half * half - (suu * svv - suv * suv)).max(0.0).sqrt()).max(0.0).sqrt();
    if smaller < MIN_SPREAD_DEG {
        return Err(format!("the {name} eye's angles hardly moved between the dots ({smaller:.1} deg, needs {MIN_SPREAD_DEG:.0})"));
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
        let c = solve3(a, b).ok_or_else(|| format!("the {name} eye's angles don't give a map"))?;
        *out = [c[0] - c[1] * mu - c[2] * mv, c[1], c[2]];
    }
    let map = EyeMap {
        yaw: coefficients[0].map(|c| c as f32),
        pitch: coefficients[1].map(|c| c as f32),
        center: [0.0; 2],
    };
    let rms = ((0..5)
        .map(|i| {
            let [y, p] = map.apply(medians[i]);
            (f64::from(y) - wanted[i][0]).powi(2) + (f64::from(p) - wanted[i][1]).powi(2)
        })
        .sum::<f64>()
        / 5.0)
        .sqrt();
    if !rms.is_finite() || rms > MAX_RMS_DEG {
        return Err(format!("the {name} eye's map misses its dots by {rms:.1} deg (at most {MAX_RMS_DEG:.0})"));
    }
    Ok((map, rms))
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
    }
    text
}

/// Both maps from the file's text; None unless both eyes' yaw and pitch lines are there and are numbers (a missing
/// centre is 0).
pub fn parse_maps(text: &str) -> Option<[EyeMap; 2]> {
    let value = |key: &str| -> Option<Vec<f32>> {
        let line = text.lines().find_map(|line| line.trim().strip_prefix(key)?.strip_prefix('='))?;
        line.split_whitespace().map(|v| v.parse::<f32>().ok().filter(|v| v.is_finite())).collect()
    };
    let eye = |name: &str| -> Option<EyeMap> {
        let yaw = value(&format!("yaw_{name}")).filter(|v| v.len() == 3)?;
        let pitch = value(&format!("pitch_{name}")).filter(|v| v.len() == 3)?;
        let center = value(&format!("center_{name}")).filter(|v| v.len() == 2).unwrap_or(vec![0.0, 0.0]);
        Some(EyeMap { yaw: [yaw[0], yaw[1], yaw[2]], pitch: [pitch[0], pitch[1], pitch[2]], center: [center[0], center[1]] })
    };
    Some([eye("left")?, eye("right")?])
}

/// The maps, the file they are kept in, and the dots of the fit in progress.
#[derive(Clone, Debug, Default)]
pub struct MapStore {
    /// Where they are saved (None: never, as in a replay).
    path: Option<PathBuf>,
    pub maps: Option<[EyeMap; 2]>,
    /// The dots of the set in progress (DOTS order): each eye's median.
    dots: [Option<[[f32; 2]; 2]>; 5],
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
        (Self { path, maps, dots: [None; 5] }, line)
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

    /// A gaze capture is done: its target and each eye's median filter angles with how many samples they are of (None
    /// without any). Returns the lines to log (none for a capture that changes nothing).
    pub fn capture_done(&mut self, target: &str, medians: [Option<([f32; 2], u32)>; 2]) -> Vec<String> {
        let Some(dot) = DOTS.iter().position(|(name, ..)| *name == target) else {
            return Vec::new();
        };
        let mut lines = Vec::new();
        // A centre dot starts a new set
        if dot == 0 {
            self.dots = [None; 5];
        }
        let usable = medians.map(|m| m.filter(|(_, n)| *n >= MIN_SAMPLES).map(|(angles, _)| angles));
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
            } else {
                format!("this fit makes no eyeball map ({})", if self.maps.is_some() { "the old one stays" } else { "there is none yet" })
            };
            lines.push(format!(
                "Eyeball map: {eyes} at the {target} dot: {} and {} samples of a settled filter (L, R; needs \
                 {MIN_SAMPLES}); {outcome}. Fit again once the journal's centre line says both eyes have settled",
                counts[0], counts[1]
            ));
            self.dots[dot] = None;
            return lines;
        };
        self.dots[dot] = Some([left, right]);
        let mut changed = None;
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
                        changed = Some(new);
                    }
                    Err(why) => lines.push(format!("Eyeball map: centre kept: {why}")),
                }
            }
        } else if let [Some(c), Some(u), Some(d), Some(l), Some(r)] = self.dots {
            // The whole set: a new map, and the centre from its centre dot
            let set = [c, u, d, l, r];
            let fitted = (0..2).map(|eye| fit_eye(&set.map(|dot| dot[eye]), eye)).collect::<Result<Vec<_>, _>>();
            match fitted {
                Ok(fits) => {
                    let maps = [fits[0].0, fits[1].0];
                    match centers(&maps, c) {
                        Ok(centre) => {
                            let mut new = maps;
                            new[0].center = centre[0];
                            new[1].center = centre[1];
                            lines.push(format!(
                                "Eyeball map fitted: misses its dots by L {:.2}, R {:.2} deg; centre L {:+.2} {:+.2}, R {:+.2} {:+.2}",
                                fits[0].1, fits[1].1, centre[0][0], centre[0][1], centre[1][0], centre[1][1]
                            ));
                            changed = Some(new);
                        }
                        Err(why) => lines.push(format!("Eyeball map not made: {why}")),
                    }
                }
                Err(why) => lines.push(format!(
                    "Eyeball map {}: {why}",
                    if self.maps.is_some() { "kept as it was" } else { "not made" }
                )),
            }
            self.dots = [None; 5];
        } else if dot == DOTS.len() - 1 {
            // The whole fit's last dot, with some of its dots missing: said, so a fit that made no map is not taken for
            // one that did
            let missing: Vec<&str> = (0..DOTS.len()).filter(|&i| self.dots[i].is_none()).map(|i| DOTS[i].0).collect();
            lines.push(format!(
                "Eyeball map {}: no settled eyes at the {} dot{} of this fit",
                if self.maps.is_some() { "kept as it was" } else { "not made" },
                missing.join(", "),
                if missing.len() == 1 { "" } else { "s" }
            ));
        }
        if let Some(maps) = changed {
            self.maps = Some(maps);
            lines.extend(self.save(&maps));
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

    #[test]
    fn a_whole_fit_makes_the_maps_and_a_re_center_moves_only_the_centre() {
        let dir = std::env::temp_dir().join(format!("frameeyeosc-eyeball-map-{}", std::process::id()));
        let path = dir.join(FILE_NAME);
        let _ = std::fs::remove_dir_all(&dir);
        let (mut store, line) = MapStore::load(Some(path.clone()));
        assert!(store.maps.is_none() && line.contains("none yet"), "{line}");
        // The five dots in the panel's order, then the eyes shut (not one of ours: nothing)
        let mut lines = Vec::new();
        for dot in 0..5 {
            lines.extend(store.capture_done(DOTS[dot].0, medians(reading, dot, 120)));
        }
        assert!(store.capture_done("closed", [None; 2]).is_empty());
        let maps = store.maps.expect("fitted");
        assert!(lines.iter().any(|l| l.starts_with("Eyeball map fitted: misses its dots by L 0.00")), "{lines:?}");
        assert!(lines.last().unwrap().starts_with("Eyeball map saved to"));
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
        // Too few samples of a settled filter at a dot: said plainly, nothing changed; a set without one of its dots
        // fits nothing, and its last dot says so
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
        for dot in [2, 3] {
            assert!(store.capture_done(DOTS[dot].0, medians(reading, dot, 120)).is_empty());
        }
        let lines = store.capture_done("right", medians(reading, 4, 120));
        assert_eq!(lines, vec!["Eyeball map kept as it was: no settled eyes at the up dot of this fit".to_owned()]);
        assert_eq!(store.maps, Some(recentred));
        // A centre dot without settled eyes keeps the centre
        let lines = store.capture_done("center", [None; 2]);
        assert!(lines[0].starts_with("Eyeball map: neither eye counted at the center dot"), "{lines:?}");
        assert!(lines[0].contains("0 and 0 samples") && lines[0].contains("the centre is kept as it was"), "{lines:?}");
        assert_eq!(store.maps, Some(recentred));
        // A read-only store (the replay) never writes
        std::fs::remove_file(&path).unwrap();
        let (store, _) = MapStore::load(Some(path.clone()));
        let mut store = store.read_only();
        for dot in 0..5 {
            store.capture_done(DOTS[dot].0, medians(reading, dot, 120));
        }
        assert!(store.maps.is_some() && !path.exists());
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn a_map_that_misses_its_dots_or_eyes_that_did_not_move_are_refused() {
        let mut store = MapStore::default();
        // The eyes did not move between the dots
        let still = |_eye: usize, _yaw: f64, _pitch: f64| [1.0f32, 2.0];
        for dot in 0..5 {
            store.capture_done(DOTS[dot].0, medians(still, dot, 120));
        }
        assert!(store.maps.is_none());
        // A wild reading at one dot: misses by far more than MAX_RMS_DEG
        let wild = |eye: usize, yaw: f64, pitch: f64| {
            let r = reading(eye, yaw, pitch);
            if yaw > 10.0 { [r[0] - 25.0, r[1] + 20.0] } else { r }
        };
        let mut lines = Vec::new();
        for dot in 0..5 {
            lines.extend(store.capture_done(DOTS[dot].0, medians(wild, dot, 120)));
        }
        assert!(store.maps.is_none() && lines.last().unwrap().contains("misses its dots"), "{lines:?}");
        // A centre too far is not taken
        let maps = [fit_eye(&[0, 1, 2, 3, 4].map(|d| reading(0, DOTS[d].1, DOTS[d].2)), 0).unwrap().0; 2];
        assert!(centers(&maps, [[60.0, 0.0], [3.0, -2.0]]).unwrap_err().contains("left eye's centre"));
    }

    #[test]
    fn the_file_is_read_loosely() {
        let maps = parse_maps("yaw_left=1 2 3\npitch_left=4 5 6\nyaw_right=7 8 9\npitch_right=1 0 1\ncenter_right=0.5 -1\n").unwrap();
        assert_eq!((maps[0].center, maps[1].center, maps[1].yaw), ([0.0, 0.0], [0.5, -1.0], [7.0, 8.0, 9.0]));
        assert!(parse_maps("yaw_left=1 2 3\npitch_left=4 5 6\n").is_none());
        assert!(parse_maps("yaw_left=1 2\npitch_left=4 5 6\nyaw_right=7 8 9\npitch_right=1 0 1\n").is_none());
        assert!(parse_maps("yaw_left=1 x 3\npitch_left=4 5 6\nyaw_right=7 8 9\npitch_right=1 0 1\n").is_none());
        assert_eq!(parse_maps(&format_maps(&maps)), Some(maps));
    }
}
