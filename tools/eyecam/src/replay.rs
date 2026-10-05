//! Reading a recorded session (eye_L.raw / eye_R.raw, frames.csv, valve.csv, cues.csv, meta.txt) and running the
//! feature pipeline over it offline.
//!
//! Eyes are anatomical: sessions recorded by builds up to a417dab may hold the right camera in eye_L.raw
//! (meta.txt slot_camera=1,1,1,1,0,0,0,0 and no repaired_swap=1); those are read swapped automatically, and
//! `swap` flips the choice again. The right eye is flipped vertically to be upright, as everywhere in the analysis.

use crate::feat::{self, Extractor, Features, Pupil};
use crate::vision::{self, H, W};
use std::collections::HashMap;
use std::fs::{self, File};
use std::io::{BufWriter, Write};
use std::os::unix::fs::FileExt;
use std::path::{Path, PathBuf};

pub const FRAME: usize = W * H;

/// A recorded session.
pub struct Session {
    pub dir: PathBuf,
    /// Whether eye_L.raw holds the right eye (and eye_R.raw the left).
    pub swapped: bool,
    files: [File; 2],
    /// Camera timestamps (t_cam) by eye_index, per anatomical eye.
    pub t_cam: [Vec<f64>; 2],
    pub valve: Valve,
    /// (label, start t_raw, seconds) per cue, in order ("end" last).
    pub cues: Vec<(String, f64, f64)>,
}

/// Valve samples: time, per-eye gaze pitch (degrees) and openness.
#[derive(Default)]
pub struct Valve {
    pub t: Vec<f64>,
    pub pitch: [Vec<f64>; 2],
    pub open: [Vec<f64>; 2],
}

impl Valve {
    /// The sample nearest to `t` (index), like the analysis' searchsorted-based lookup.
    pub fn nearest(&self, t: f64) -> Option<usize> {
        if self.t.len() < 2 {
            return None;
        }
        let j = self.t.partition_point(|&v| v < t).clamp(1, self.t.len() - 1);
        Some(if (self.t[j - 1] - t).abs() < (self.t[j] - t).abs() { j - 1 } else { j })
    }
}

fn read_csv(path: &Path) -> Result<(Vec<String>, Vec<Vec<String>>), String> {
    let text = fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let mut lines = text.lines();
    let head: Vec<String> = lines.next().unwrap_or("").split(',').map(str::to_string).collect();
    Ok((head, lines.filter(|l| !l.is_empty()).map(|l| l.split(',').map(str::to_string).collect()).collect()))
}

fn col(head: &[String], name: &str) -> Result<usize, String> {
    head.iter().position(|h| h == name).ok_or_else(|| format!("no column {name}"))
}

fn num(s: &str) -> f64 {
    s.parse().unwrap_or(f64::NAN)
}

/// key=value lines of meta.txt (later ones win).
pub fn read_meta(dir: &Path) -> HashMap<String, String> {
    fs::read_to_string(dir.join("meta.txt"))
        .unwrap_or_default()
        .lines()
        .filter(|l| !l.starts_with('#'))
        .filter_map(|l| l.split_once('=').map(|(k, v)| (k.to_string(), v.to_string())))
        .collect()
}

impl Session {
    pub fn open(dir: &Path, swap: bool) -> Result<Self, String> {
        let meta = read_meta(dir);
        if meta.get("frame_width").is_some_and(|w| w != "400") {
            return Err("only 400-pixel-wide sessions are supported".into());
        }
        let wrong_order = meta.get("slot_camera").is_some_and(|s| s.starts_with('1'));
        let repaired = meta.get("repaired_swap").is_some_and(|v| v == "1");
        let swapped = (wrong_order && !repaired) != swap;
        let file = |e: &str| File::open(dir.join(format!("eye_{e}.raw"))).map_err(|err| format!("eye_{e}.raw: {err}"));
        let (fl, fr) = (file("L")?, file("R")?);
        let files = if swapped { [fr, fl] } else { [fl, fr] };

        let (h, rows) = read_csv(&dir.join("frames.csv"))?;
        let (ie, ii, it) = (col(&h, "eye")?, col(&h, "eye_index")?, col(&h, "t_cam")?);
        let mut t_cam: [Vec<f64>; 2] = [Vec::new(), Vec::new()];
        for r in &rows {
            let file_eye = (r[ie] == "R") as usize;
            let eye = file_eye ^ swapped as usize;
            let k: usize = r[ii].parse().map_err(|_| "bad eye_index")?;
            let v = &mut t_cam[eye];
            if v.len() <= k {
                v.resize(k + 1, f64::NAN);
            }
            v[k] = num(&r[it]);
        }

        let mut valve = Valve::default();
        if let Ok((h, rows)) = read_csv(&dir.join("valve.csv")) {
            let ts = col(&h, "sample_time")?;
            let gy = [col(&h, "gaze_l_y")?, col(&h, "gaze_r_y")?];
            let op = [col(&h, "open_l")?, col(&h, "open_r")?];
            for r in &rows {
                valve.t.push(num(&r[ts]));
                for e in 0..2 {
                    valve.pitch[e].push(num(&r[gy[e]]).clamp(-1.0, 1.0).asin().to_degrees());
                    valve.open[e].push(num(&r[op[e]]));
                }
            }
        }
        let cues = match read_csv(&dir.join("cues.csv")) {
            Ok((h, rows)) => {
                let (il, it, is) = (col(&h, "label")?, col(&h, "t_raw")?, col(&h, "seconds")?);
                rows.iter().map(|r| (r[il].clone(), num(&r[it]), num(&r[is]))).collect()
            }
            Err(_) => Vec::new(),
        };
        Ok(Self { dir: dir.to_path_buf(), swapped, files, t_cam, valve, cues })
    }

    /// Number of frames of an anatomical eye (0 = L, 1 = R).
    pub fn frames(&self, eye: usize) -> usize {
        self.files[eye].metadata().map_or(0, |m| m.len() as usize / FRAME)
    }

    /// Frame `k` of an anatomical eye, upright (right eye flipped vertically).
    pub fn read(&self, eye: usize, k: usize, out: &mut [u8]) -> std::io::Result<()> {
        self.files[eye].read_exact_at(out, (k * FRAME) as u64)?;
        if eye == 1 {
            for y in 0..H / 2 {
                let (a, b) = out.split_at_mut((H - 1 - y) * W);
                a[y * W..(y + 1) * W].swap_with_slice(&mut b[..W]);
            }
        }
        Ok(())
    }

    /// Step labels by time, trimmed by `delay` s at the start of each step (io2.label_times): (label, step index).
    pub fn label_at(&self, t: f64, delay: f64) -> (Option<&str>, i32) {
        for (i, (lab, t0, sec)) in self.cues.iter().enumerate() {
            if lab == "end" {
                break;
            }
            if t >= t0 + delay && t < t0 + sec {
                return (Some(lab.as_str()), i as i32);
            }
        }
        (None, -1)
    }
}

/// The CSV columns `compat` writes (a subset of the prototype's feats_s*.csv).
pub const COMPAT_HEADER: &str =
    "eye,eye_index,t_cam,ok_pupil,pupil_cx,pupil_cy,pupil_a,pupil_b,iris_R,iris_n,upper_skin_y,upper_y,lower_y,box_mean";

fn fmt(v: f64) -> String {
    if v.is_finite() { format!("{v:.4}") } else { String::new() }
}

pub fn compat_row(eye: usize, k: usize, t: f64, f: &Features) -> String {
    format!(
        "{},{k},{t:.6},{},{},{},{},{},{},{},{},{},{},{}",
        ["L", "R"][eye],
        f.ok_pupil as u8,
        fmt(f.pupil_cx),
        fmt(f.pupil_cy),
        fmt(f.pupil_a),
        fmt(f.pupil_b),
        fmt(f.iris_r),
        f.iris_n,
        fmt(f.upper_skin_y),
        fmt(f.upper_y),
        fmt(f.lower_y),
        fmt(f.box_mean)
    )
}

/// The prototype's run2.py over one eye: lens edge from the mean of every 200th frame, iris radius for the search
/// windows from every 25th frame, then all frames in chunks of 400 (tracking restarts at each chunk, as there).
/// Returns (xmax, R_search, per-frame features).
pub fn run_compat(s: &Session, eye: usize, limit: Option<usize>) -> std::io::Result<(f64, f64, Vec<Features>)> {
    let n = s.frames(eye);
    let mut img = vec![0u8; FRAME];
    let mut cm = vec![0.0; W];
    let mut count = 0.0;
    for k in (0..n).step_by(200) {
        s.read(eye, k, &mut img)?;
        for (a, b) in cm.iter_mut().zip(feat::column_means(&img)) {
            *a += b;
        }
        count += 1.0;
    }
    cm.iter_mut().for_each(|v| *v /= count);
    let xmax = feat::occluder_x(&cm);
    let mut ex = Extractor::default();
    let mut rs: Vec<f64> = Vec::new();
    for k in (0..n).step_by(25) {
        s.read(eye, k, &mut img)?;
        let (f, _) = ex.extract(&img, xmax, None, None, None);
        if f.ok_pupil && f.iris_r.is_finite() && f.iris_n >= 4 {
            rs.push(f.iris_r);
        }
    }
    let r_search = vision::median(&mut rs);
    let n = limit.map_or(n, |l| l.min(n));
    let mut out = Vec::with_capacity(n);
    let (mut prev, mut prev_p): (Option<(f64, f64)>, Option<Pupil>) = (None, None);
    for k in 0..n {
        if k % 400 == 0 {
            prev = None;
            prev_p = None;
        }
        s.read(eye, k, &mut img)?;
        let (f, p) = ex.extract(&img, xmax, prev, Some(r_search).filter(|r| r.is_finite()), prev_p.as_ref());
        if let Some(p) = p {
            prev = Some((p.cx, p.cy));
            prev_p = Some(p);
        }
        out.push(f);
    }
    Ok((xmax, r_search, out))
}

/// `eyecam-rec --replay DIR --compat --out FILE`: run2-identical features for both eyes.
pub fn write_compat(s: &Session, out: &Path, limit: Option<usize>) -> Result<(), String> {
    let mut w = BufWriter::new(File::create(out).map_err(|e| format!("{}: {e}", out.display()))?);
    writeln!(w, "{COMPAT_HEADER}").map_err(|e| e.to_string())?;
    for eye in 0..2 {
        let t0 = std::time::Instant::now();
        let (xmax, r, feats) = run_compat(s, eye, limit).map_err(|e| e.to_string())?;
        let dt = t0.elapsed().as_secs_f64();
        eprintln!(
            "eye {}: xmax {xmax}, R_search {r:.3}, {} frames, {:.2} ms/frame (including the two estimation passes)",
            ["L", "R"][eye],
            feats.len(),
            dt * 1e3 / feats.len().max(1) as f64
        );
        for (k, f) in feats.iter().enumerate() {
            let t = s.t_cam[eye].get(k).copied().unwrap_or(f64::NAN);
            writeln!(w, "{}", compat_row(eye, k, t, f)).map_err(|e| e.to_string())?;
        }
    }
    w.flush().map_err(|e| e.to_string())
}

/// Options for `write_live`.
#[derive(Default)]
pub struct LiveOpts {
    /// User calibration (and the reference wear levels it was saved with).
    pub calib: Option<crate::live::CalibFile>,
    /// Fit this wear's levels from the first N cue steps once they are over (the evaluation's 15 s block).
    pub block_steps: Option<usize>,
    /// Fit a user calibration from this whole session and write it here.
    pub fit_user: Option<PathBuf>,
    /// Start from the calibration file's wear levels (as live does right after a `calib wear`).
    pub use_wear: bool,
    /// Learn the auto baselines from a first pass over the session (until both eyes are ready), then run it again
    /// from the start with them: as if the HMD had been worn for the warm-up before the session began.
    pub prewarm: bool,
    pub tune: crate::live::Tune,
    pub limit: Option<usize>,
}

pub const LIVE_HEADER: &str = "eye,eye_index,t_cam,label,step,ok_pupil,skin_up,aperture,lower,pupil_ratio,box_f,r,pitch,v_open,\
closed,eye_wide,eye_squint,eye_lid,pupil_mm,pupil_dilation,confidence,valid,wear_cal,b_n,b_w,pupil_vis,upper_frac,lower_frac,ready";

/// The live engine over a session, both eyes in step, causally. Returns ms per processed frame.
pub fn write_live(s: &Session, out: &Path, opts: &LiveOpts) -> Result<f64, String> {
    use crate::live::{self, EyeEngine, Label, Params, Sample};
    let mut w = BufWriter::new(File::create(out).map_err(|e| format!("{}: {e}", out.display()))?);
    writeln!(w, "{LIVE_HEADER}").map_err(|e| e.to_string())?;
    let mut engines = [EyeEngine::default(), EyeEngine::default()];
    let mut params = [Params { tune: opts.tune, ..Params::default() }; 2];
    if let Some(c) = &opts.calib {
        let hist = c.history_params();
        for (e, p) in params.iter_mut().enumerate() {
            p.user = c.user.map(|u| u[e]);
            p.hist = hist.map(|h| h[e]);
            p.pupil_measured = c.pupil_measured;
        }
    }
    let n = (0..2).map(|e| s.frames(e)).max().unwrap_or(0);
    let n = opts.limit.map_or(n, |l| l.min(n));
    let mut img = vec![0u8; FRAME];
    let pitch_of = |e: usize, t: f64| s.valve.nearest(t).map_or(f64::NAN, |j| s.valve.pitch[e][j]);
    if opts.prewarm {
        let mut warm = [EyeEngine::default(), EyeEngine::default()];
        'pass: for k in 0..n {
            for e in 0..2 {
                if k < s.frames(e) {
                    s.read(e, k, &mut img).map_err(|err| err.to_string())?;
                    let t = s.t_cam[e].get(k).copied().unwrap_or(f64::NAN);
                    warm[e].process(&img, pitch_of(e, t), t, &params[e]);
                }
            }
            if warm.iter().all(EyeEngine::baseline_ready) {
                eprintln!("prewarm: baselines ready after {} frames", k + 1);
                break 'pass;
            }
        }
        for e in 0..2 {
            match warm[e].baseline_px() {
                Some((b, a)) => {
                    eprintln!("prewarm {}: skin line {b:.2} px, aperture {a:.2} px", ["L", "R"][e]);
                    engines[e].seed_px(b, a);
                }
                None => eprintln!("prewarm {}: not enough usable frames", ["L", "R"][e]),
            }
        }
    }
    if opts.use_wear
        && let Some(w) = opts.calib.as_ref().and_then(|c| c.wear)
    {
        for e in 0..2 {
            params[e].wear = Some(w[e]);
            engines[e].seed(&w[e]);
        }
    }
    let block_end = opts.block_steps.and_then(|n| s.cues.get(n.checked_sub(1)?)).map(|(_, t0, sec)| t0 + sec);
    let mut block: [Vec<Sample>; 2] = [Vec::new(), Vec::new()];
    let mut all: [Vec<Sample>; 2] = [Vec::new(), Vec::new()];
    let mut busy = std::time::Duration::ZERO;
    let mut processed = 0usize;
    let mut last_t = [f64::NEG_INFINITY; 2];
    let f = |v: f64| if v.is_finite() { format!("{v:.4}") } else { String::new() };
    for k in 0..n {
        for e in 0..2 {
            if k >= s.frames(e) {
                continue;
            }
            s.read(e, k, &mut img).map_err(|err| err.to_string())?;
            let t = s.t_cam[e].get(k).copied().unwrap_or(f64::NAN);
            let j = s.valve.nearest(t);
            let pitch = j.map_or(f64::NAN, |j| s.valve.pitch[e][j]);
            let v_open = j.map_or(f64::NAN, |j| s.valve.open[e][j]);
            let t0 = std::time::Instant::now();
            let o = engines[e].process(&img, pitch, t, &params[e]);
            busy += t0.elapsed();
            processed += 1;
            last_t[e] = t;
            let (label, step) = s.label_at(t, live::STEP_DELAY);
            if let Some(l) = label {
                let smp = Sample::from_out(Label::parse(l), &o, t);
                if opts.block_steps.is_some_and(|nb| (step as usize) < nb) {
                    block[e].push(smp);
                }
                if opts.fit_user.is_some() {
                    all[e].push(smp);
                }
            }
            let row = [
                ["L", "R"][e].to_string(),
                k.to_string(),
                format!("{t:.6}"),
                label.unwrap_or("").to_string(),
                step.to_string(),
                (o.f.ok_pupil as u8).to_string(),
                f(o.skin_up),
                f(o.aperture),
                f(o.lower),
                f(o.pupil_ratio),
                f(o.box_f),
                f(o.r),
                f(o.pitch),
                f(v_open),
                (o.closed as u8).to_string(),
                f(o.eye_wide),
                f(o.eye_squint),
                f(o.eye_lid),
                f(o.pupil_mm),
                f(o.pupil_dilation),
                f(o.confidence),
                (o.valid as u8).to_string(),
                (params[e].wear.is_some() as u8).to_string(),
                f(o.b_n),
                f(o.b_w),
                f(o.f.pupil_vis),
                f(o.f.upper_frac),
                f(o.f.lower_frac),
                (engines[e].baseline_ready() as u8).to_string(),
            ];
            writeln!(w, "{}", row.join(",")).map_err(|e| e.to_string())?;
        }
        if let Some(tb) = block_end
            && params[0].wear.is_none()
            && last_t.iter().all(|&t| t >= tb)
            && !block[0].is_empty()
        {
            match live::fit_wear(&block, [live::DEFAULT_WIDEN_STEP; 2]) {
                Ok((wp, _)) => {
                    eprintln!("block calibration: L {:?}\n                   R {:?}", wp[0], wp[1]);
                    for e in 0..2 {
                        params[e].wear = Some(wp[e]);
                        engines[e].seed(&wp[e]);
                    }
                }
                Err(e) => eprintln!("block calibration failed: {e}"),
            }
            block = [Vec::new(), Vec::new()];
        }
    }
    w.flush().map_err(|e| e.to_string())?;
    if let Some(path) = &opts.fit_user {
        let (wear, _) = live::fit_wear(&all, [live::DEFAULT_WIDEN_STEP; 2])?;
        let old = opts.calib.as_ref().and_then(|c| c.user);
        let (mut user, warnings) = live::fit_user(&all, &wear, old, false)?;
        for w in &warnings {
            eprintln!("calibration check (not enforced offline): {w}");
        }
        let pupil = live::fit_pupil(&all, Some([wear[0].r_px, wear[1].r_px]));
        if let Some(pr) = pupil {
            for e in 0..2 {
                (user[e].pd_min, user[e].pd_max) = pr[e];
            }
        }
        let file = live::CalibFile {
            user: Some(user),
            user_time: s.dir.display().to_string(),
            user_warnings: warnings.clone(),
            pupil_measured: pupil.is_some(),
            wear: Some(wear),
            wear_time: s.dir.display().to_string(),
            wear_widen_measured: None,
            wear_failed_eye: String::new(),
            history: vec![(s.dir.display().to_string(), [live::WearRecord::of(&wear[0]), live::WearRecord::of(&wear[1])])],
            setup_done: true,
        };
        fs::write(path, file.to_json()).map_err(|e| format!("{}: {e}", path.display()))?;
        eprintln!("user calibration written to {}", path.display());
    }
    Ok(busy.as_secs_f64() * 1e3 / processed.max(1) as f64)
}
