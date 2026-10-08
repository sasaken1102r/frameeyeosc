//! Reading a recorded session (eye_L.raw / eye_R.raw, frames.csv, valve.csv, cues.csv, meta.txt) and running the
//! feature pipeline over it offline.
//!
//! Eyes are anatomical (0 = left). Sessions recorded since 2026-10-07 say so in meta.txt (`eye_files=anatomical`):
//! eye_L.raw holds the left eye. Older ones named the lower-address camera L, and it is the right eye (see `ring`),
//! so eye_L.raw holds the right eye; except that builds up to a417dab sometimes named the cameras the other way
//! (slot_camera=1,1,1,1,0,0,0,0), and fix_swap.py exchanges the files (repaired_swap=1). All of that is read so
//! that each file reaches the eye it holds; `swap` flips the choice again. The left eye's camera (the higher-address
//! slot group) stores its picture upside down: it is flipped vertically to be upright, as everywhere in the analysis
//! (vision::UPSIDE_DOWN_EYE). The flip goes with the camera, so with the file that holds it (`upside_down_file`):
//! --swap, when recording or here, renames the files' eyes but never moves a flip to the other camera.

use crate::feat::{self, Extractor, Features, Pupil};
use crate::vision::{self, H, W};
use std::collections::HashMap;
use std::fs::{self, File};
use std::io::{BufWriter, Write};
use std::os::unix::fs::FileExt;
use std::path::{Path, PathBuf};

pub const FRAME: usize = W * H;

/// meta.txt's key and value saying eye_L.raw / eye_R.raw (and frames.csv's L / R) are the anatomical left and right
/// eye. Sessions without it named the lower-address camera (the right eye) L.
pub const META_EYE_FILES: &str = "eye_files";
pub const EYE_FILES_ANATOMICAL: &str = "anatomical";

/// meta.txt's `upside_down_eye`: the name (L or R) of the camera that stores its picture upside down, as a session
/// recorded with or without --swap names it (--swap renames it), or "none" with one camera streaming (which eye it
/// is is not known; its frames are not flipped).
pub fn upside_down_name(both_eyes: bool, swap: bool) -> &'static str {
    if !both_eyes {
        return "none";
    }
    ["L", "R"][vision::UPSIDE_DOWN_EYE ^ swap as usize]
}

/// Which file (0 = eye_L.raw, 1 = eye_R.raw) holds the camera that stores its picture upside down (the higher-address
/// slot group, the left eye's camera), None if none does (one camera streaming). meta.txt's `upside_down_eye` says
/// so since it is written as L, R or none; before that it follows from how the session named its cameras: the
/// higher-address group was L since 2026-10-07 and R before (L when named by picture), and --swap when recording
/// renamed it. fix_swap.py exchanging the files moves it either way.
pub fn upside_down_file(meta: &HashMap<String, String>) -> Option<usize> {
    let repaired = meta.get("repaired_swap").is_some_and(|v| v == "1");
    match meta.get("upside_down_eye").map(String::as_str) {
        Some("L") => return Some(repaired as usize),
        Some("R") => return Some(1 ^ repaired as usize),
        Some("none") => return None,
        // Not written yet, or as the first builds of 2026-10-07 wrote it ("L: stored raw; ...", even under --swap)
        _ => {}
    }
    if meta.get("both_eyes").is_some_and(|v| v == "false") {
        return None;
    }
    let anatomical = meta.get(META_EYE_FILES).is_some_and(|v| v == EYE_FILES_ANATOMICAL);
    let picture_order = !anatomical && meta.get("slot_camera").is_some_and(|s| s.starts_with('1'));
    let swap = meta.get("swap").is_some_and(|v| v == "true" || v == "1");
    // The slots' own name for the higher-address group, then what --swap and fix_swap.py did to it
    let higher = if anatomical { vision::UPSIDE_DOWN_EYE } else if picture_order { 0 } else { 1 };
    Some(higher ^ swap as usize ^ repaired as usize)
}

/// Whether eye_L.raw of a session with this meta.txt holds the right eye (and eye_R.raw the left), before `--swap`.
pub fn files_swapped(meta: &HashMap<String, String>) -> bool {
    let anatomical = meta.get(META_EYE_FILES).is_some_and(|v| v == EYE_FILES_ANATOMICAL);
    // Builds up to a417dab could name the cameras by picture, the other way round (L = the higher addresses).
    let picture_order = !anatomical && meta.get("slot_camera").is_some_and(|s| s.starts_with('1'));
    let repaired = meta.get("repaired_swap").is_some_and(|v| v == "1");
    // Old names: L = the lower addresses = the right eye, unless named by picture; fix_swap.py exchanged the files.
    (!anatomical && !picture_order) != repaired
}

/// A recorded session.
pub struct Session {
    pub dir: PathBuf,
    /// Whether eye_L.raw holds the right eye (and eye_R.raw the left).
    pub swapped: bool,
    files: [File; 2],
    /// Per anatomical eye, whether its file holds the camera that stores its picture upside down (upside_down_file).
    upside_down: [bool; 2],
    /// Camera timestamps (t_cam) by eye_index, per anatomical eye.
    pub t_cam: [Vec<f64>; 2],
    pub valve: Valve,
    /// (label, start t_raw, seconds) per cue, in order ("end" last).
    pub cues: Vec<(String, f64, f64)>,
    /// After `resample`: per eye, which stored frame each frame is (None: all of them, in order).
    map: [Option<Vec<usize>>; 2],
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
        let swapped = files_swapped(&meta) != swap;
        let file = |e: &str| File::open(dir.join(format!("eye_{e}.raw"))).map_err(|err| format!("eye_{e}.raw: {err}"));
        let (fl, fr) = (file("L")?, file("R")?);
        let files = if swapped { [fr, fl] } else { [fl, fr] };
        // The flip stays with the file (its camera), whichever eye it is read as
        let flipped = upside_down_file(&meta);
        let upside_down = [0, 1].map(|eye| flipped == Some(eye ^ swapped as usize));

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
        Ok(Self { dir: dir.to_path_buf(), swapped, files, upside_down, t_cam, valve, cues, map: [None, None] })
    }

    /// Number of frames of an anatomical eye (0 = L, 1 = R).
    pub fn frames(&self, eye: usize) -> usize {
        match &self.map[eye] {
            Some(m) => m.len(),
            None => self.files[eye].metadata().map_or(0, |m| m.len() as usize / FRAME),
        }
    }

    /// As if the cameras had run at `fps`: per eye, the stored frame nearest to each tick of an `fps` grid (from the
    /// session's first frame), stamped with the tick's time. Every 6th frame of a 90 fps session at 15, every 5th at
    /// 18; at 72 the frames' own spacing would alternate 11 and 22 ms, the ticks' does not. A tick with no frame
    /// within half a tick (frames lost) is left out. Nothing is copied: frames are read where they are stored.
    pub fn resample(&mut self, fps: f64) {
        let t0 = self.t_cam.iter().filter_map(|t| t.iter().copied().find(|v| v.is_finite())).fold(f64::INFINITY, f64::min);
        for e in 0..2 {
            // The stored frames by time (two frames can be stored in the other order).
            let stored = self.frames(e).min(self.t_cam[e].len());
            let mut order: Vec<(f64, usize)> =
                (0..stored).filter(|&k| self.t_cam[e][k].is_finite()).map(|k| (self.t_cam[e][k], k)).collect();
            order.sort_by(|a, b| a.0.total_cmp(&b.0));
            let t: Vec<f64> = order.iter().map(|o| o.0).collect();
            let (mut map, mut times) = (Vec::new(), Vec::new());
            let (mut j, mut last) = (0, None);
            for tick in 0.. {
                let g = t0 + tick as f64 / fps;
                if t.last().is_none_or(|&end| g > end + 0.5 / fps) {
                    break;
                }
                while j + 1 < t.len() && (t[j + 1] - g).abs() <= (t[j] - g).abs() {
                    j += 1;
                }
                if (t[j] - g).abs() <= 0.5 / fps && last.is_none_or(|m| j > m) {
                    last = Some(j);
                    map.push(order[j].1);
                    times.push(g);
                }
            }
            self.map[e] = Some(map);
            self.t_cam[e] = times;
        }
    }

    /// Whether an anatomical eye's file holds the camera that stores its picture upside down (read flips it).
    pub fn upside_down(&self, eye: usize) -> bool {
        self.upside_down[eye]
    }

    /// Frame `k` of an anatomical eye, upright (the upside-down camera's flipped vertically: the left eye's).
    pub fn read(&self, eye: usize, k: usize, out: &mut [u8]) -> std::io::Result<()> {
        let k = self.map[eye].as_ref().map_or(k, |m| m[k]);
        self.files[eye].read_exact_at(out, (k * FRAME) as u64)?;
        if self.upside_down[eye] {
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

/// Which calibration `replay_calib` runs.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CalibRun {
    Wear,
    User,
}

/// A calibration session (a `calib_*` kept with `dev`, or any session whose cues.csv has the steps) replayed the
/// way live collects and fits it: the frames labelled by step (the first STEP_DELAY s of each left out), the same
/// fit and report. `calib` gives what live would have: the history and the user calibration, and for `User` this
/// wear's levels (`wear`). Returns what calib_result.json would hold (kind, ok, failed_eye, message, values, params).
pub fn replay_calib(s: &Session, kind: CalibRun, calib: &crate::live::CalibFile, tune: crate::live::Tune) -> Result<String, String> {
    use crate::live::{self, EyeEngine, Label, Params, Sample};
    let hist = calib.history_params();
    let mut engines = [EyeEngine::default(), EyeEngine::default()];
    let mut params = [Params { tune, ..Params::default() }; 2];
    for (e, p) in params.iter_mut().enumerate() {
        p.user = calib.user.map(|u| u[e]);
        p.hist = hist.map(|h| h[e]);
        p.pupil_measured = calib.pupil_measured;
    }
    let end = s.cues.iter().filter(|c| c.0 != "end").map(|c| c.1 + c.2).fold(f64::NEG_INFINITY, f64::max);
    let mut samples: [Vec<Sample>; 2] = [Vec::new(), Vec::new()];
    let mut img = vec![0u8; FRAME];
    let n = (0..2).map(|e| s.frames(e)).max().unwrap_or(0);
    for k in 0..n {
        for e in 0..2 {
            let Some(&t) = s.t_cam[e].get(k).filter(|_| k < s.frames(e)) else { continue };
            if t > end {
                continue;
            }
            s.read(e, k, &mut img).map_err(|err| err.to_string())?;
            let pitch = s.valve.nearest(t).map_or(f64::NAN, |j| s.valve.pitch[e][j]);
            let o = engines[e].process(&img, pitch, t, &params[e]);
            if let (Some(l), _) = s.label_at(t, live::STEP_DELAY) {
                samples[e].push(Sample::from_out(Label::parse(l), &o, t));
            }
        }
    }
    let (kind_name, result, values, lines, params_json, failed_eye) = match kind {
        CalibRun::Wear => {
            let fallback = [0, 1].map(|e| hist.map_or(live::DEFAULT_WIDEN_STEP, |h| h[e].step));
            let out = live::fit_wear_settled(&samples, fallback, calib.measured_wear(), hist);
            let (values, lines) = live::wear_report(&samples);
            let failed = match &out {
                Ok(o) => o.failed_eye().to_string(),
                Err(_) => "LR".to_string(),
            };
            let params = out.as_ref().ok().map(|o| live::wear_params_json(&o.wear));
            ("wear", out.map(|o| o.message), values, lines, params, failed)
        }
        CalibRun::User => {
            let wear = calib.wear.ok_or("the calibration file has no wear levels (needed for a user calibration)")?;
            let out = live::fit_user(&samples, &wear, calib.user, true);
            let (values, lines) = live::user_report(&samples, &wear);
            let params = out.as_ref().ok().map(|(u, _)| live::user_params_json(u));
            let result = out.map(|(_, w)| {
                if w.is_empty() { "校正できた（ユーザー）".to_string() } else { format!("校正できた（ユーザー）。注意: {}", w.join("、")) }
            });
            ("user", result, values, lines, params, String::new())
        }
    };
    for l in &lines {
        eprintln!("{l}");
    }
    let (ok, message) = match &result {
        Ok(m) => (true, m.as_str()),
        Err(m) => (false, m.as_str()),
    };
    eprintln!("calibration ({kind_name}): {}", if ok { format!("ok: {message}") } else { format!("failed: {message}") });
    Ok(format!(
        "{{\n  \"kind\": \"{kind_name}\",\n  {},\n  \"ok\": {ok},\n  \"failed_eye\": {},\n  \"message\": {},\n  \"values\": {values},\n  \"params\": {}\n}}\n",
        crate::live::JSON_EYES_ANATOMICAL,
        crate::json::string(&failed_eye),
        crate::json::string(message),
        params_json.unwrap_or_else(|| "null".into())
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A session folder with `n` frames per eye at `fps`, less the `lost` ones (only frames.csv, the eye files and
    /// meta.txt's eye_files matter here), as recorded now. Each frame holds its own number in its first pixel
    /// (upright).
    fn fake_session(name: &str, n: usize, fps: f64, lost: &[usize]) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("eyecam-replay-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let mut csv = String::from("index,eye,eye_index,slot,t_cam,t_raw,t_copy,valve_seq\n");
        let mut files = [Vec::new(), Vec::new()];
        let mut stored = [0usize; 2];
        for k in 0..n {
            if lost.contains(&k) {
                continue;
            }
            for (e, name) in ["L", "R"].iter().enumerate() {
                csv += &format!("0,{name},{},0,{:.9},0,0,0\n", stored[e], 100.0 + k as f64 / fps + e as f64 * 1e-4);
                let mut f = vec![0u8; FRAME];
                // The left eye is stored upside down.
                f[if vision::stored_upside_down(e) { (H - 1) * W } else { 0 }] = (k % 251) as u8;
                files[e].extend_from_slice(&f);
                stored[e] += 1;
            }
        }
        fs::write(dir.join("frames.csv"), csv).unwrap();
        fs::write(dir.join("eye_L.raw"), &files[0]).unwrap();
        fs::write(dir.join("eye_R.raw"), &files[1]).unwrap();
        fs::write(dir.join("meta.txt"), format!("slot_camera=1,1,1,1,0,0,0,0\n{META_EYE_FILES}={EYE_FILES_ANATOMICAL}\n")).unwrap();
        dir
    }

    /// A session as builds before 2026-10-07 wrote it (no eye_files in meta.txt). The right eye's camera (the lower
    /// addresses) stores its frames upright, with 100 + k in the first pixel; the left eye's stores them upside
    /// down, with 200 + k in the first pixel once upright, its camera times 0.5 ms earlier. `left_in_l`: eye_L.raw
    /// holds the left eye's camera (named by picture by builds up to a417dab, or exchanged by fix_swap.py), else the
    /// right eye's (the usual old naming: L = the lower addresses).
    fn old_session(name: &str, meta: &str, left_in_l: bool) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("eyecam-replay-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let (l, r) = if left_in_l { ("R", "L") } else { ("L", "R") };
        let mut csv = String::from("index,eye,eye_index,slot,t_cam,t_raw,t_copy,valve_seq\n");
        let (mut right, mut left) = (Vec::new(), Vec::new());
        for k in 0..5 {
            let mut f = vec![0u8; FRAME];
            f[0] = 100 + k as u8;
            right.extend_from_slice(&f);
            let mut f = vec![0u8; FRAME];
            f[(H - 1) * W] = 200 + k as u8;
            left.extend_from_slice(&f);
            csv += &format!("0,{l},{k},0,{:.9},0,0,0\n0,{r},{k},4,{:.9},0,0,0\n", 10.0005 + k as f64 / 90.0, 10.0 + k as f64 / 90.0);
        }
        fs::write(dir.join("frames.csv"), csv).unwrap();
        let (file_l, file_r) = if left_in_l { (left, right) } else { (right, left) };
        fs::write(dir.join("eye_L.raw"), file_l).unwrap();
        fs::write(dir.join("eye_R.raw"), file_r).unwrap();
        fs::write(dir.join("meta.txt"), meta).unwrap();
        dir
    }

    #[test]
    fn old_sessions_are_read_swapped_so_each_camera_keeps_its_flip() {
        let dir = old_session("old", "slot_camera=0,0,0,0,1,1,1,1\nlabels=camera0=L camera1=R\n", false);
        let s = Session::open(&dir, false).unwrap();
        assert!(s.swapped);
        // The left eye (eye_R.raw, upside down) comes out upright; the right eye (eye_L.raw) as stored.
        assert_eq!(first_pixels(&s, 0, 5), [200, 201, 202, 203, 204]);
        assert_eq!(first_pixels(&s, 1, 5), [100, 101, 102, 103, 104]);
        assert!((s.t_cam[0][1] - (10.0 + 1.0 / 90.0)).abs() < 1e-9, "{:?}", s.t_cam);
        assert!((s.t_cam[1][1] - (10.0005 + 1.0 / 90.0)).abs() < 1e-9, "{:?}", s.t_cam);
        // --swap turns it back: each file then goes to the other eye, still flipped as its camera is (eye_L.raw, the
        // right eye's camera, upright; eye_R.raw, the left eye's, flipped).
        let s = Session::open(&dir, true).unwrap();
        assert!(!s.swapped);
        assert_eq!((s.upside_down(0), s.upside_down(1)), (false, true));
        let mut img = vec![0u8; FRAME];
        s.read(0, 2, &mut img).unwrap();
        assert_eq!((img[0], img[(H - 1) * W]), (102, 0));
        assert_eq!(first_pixels(&s, 1, 2), [200, 201]);
        fs::remove_dir_all(&dir).unwrap();
        // Named by picture by builds up to a417dab (L = the higher addresses, the left eye): read as stored ...
        let dir = old_session("picture", "slot_camera=1,1,1,1,0,0,0,0\n", true);
        let s = Session::open(&dir, false).unwrap();
        assert!(!s.swapped);
        assert_eq!((first_pixels(&s, 0, 2), first_pixels(&s, 1, 2)), (vec![200, 201], vec![100, 101]));
        fs::remove_dir_all(&dir).unwrap();
        // ... and such a session after fix_swap.py exchanged its files (repaired_swap=1) as an ordinary old one.
        let dir = old_session("repaired", "slot_camera=1,1,1,1,0,0,0,0\n# fix_swap.py\nrepaired_swap=1\n", false);
        let s = Session::open(&dir, false).unwrap();
        assert!(s.swapped);
        assert_eq!((first_pixels(&s, 0, 2), first_pixels(&s, 1, 2)), (vec![200, 201], vec![100, 101]));
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn new_sessions_are_read_as_named() {
        let dir = fake_session("new", 3, 90.0, &[]);
        let s = Session::open(&dir, false).unwrap();
        assert!(!s.swapped);
        assert_eq!((first_pixels(&s, 0, 3), first_pixels(&s, 1, 3)), (vec![0, 1, 2], vec![0, 1, 2]));
        // Only the left eye is flipped: its stored frame has the number in the last row.
        let mut raw = vec![0u8; FRAME];
        s.files[0].read_exact_at(&mut raw, FRAME as u64).unwrap();
        assert_eq!((raw[0], raw[(H - 1) * W]), (0, 1));
        s.files[1].read_exact_at(&mut raw, FRAME as u64).unwrap();
        assert_eq!((raw[0], raw[(H - 1) * W]), (1, 0));
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn the_flip_stays_with_the_camera_under_swap() {
        // Recorded with --swap (since 2026-10-07): the left eye's camera (upside down) was named R, so eye_R.raw holds
        // it; read as recorded, the eye named R is the one flipped, and the right eye's camera (eye_L.raw) is not.
        let swapped = "slot_camera=1,1,1,1,0,0,0,0\nswap=true\neye_files=anatomical\n";
        let dir = old_session("swap", &format!("{swapped}upside_down_eye=R\n"), false);
        let s = Session::open(&dir, false).unwrap();
        assert!(!s.swapped);
        assert_eq!((s.upside_down(0), s.upside_down(1)), (false, true));
        assert_eq!((first_pixels(&s, 0, 2), first_pixels(&s, 1, 2)), (vec![100, 101], vec![200, 201]));
        fs::remove_dir_all(&dir).unwrap();
        // The same from the first builds of that day (upside_down_eye=L written even under --swap): from swap=true
        let dir = old_session("swap-early", &format!("{swapped}upside_down_eye=L: stored raw\n"), false);
        let s = Session::open(&dir, false).unwrap();
        assert_eq!((first_pixels(&s, 0, 2), first_pixels(&s, 1, 2)), (vec![100, 101], vec![200, 201]));
        fs::remove_dir_all(&dir).unwrap();
        // What meta.txt gets: the upside-down camera's name, renamed by --swap; none with one camera
        assert_eq!((upside_down_name(true, false), upside_down_name(true, true), upside_down_name(false, true)), ("L", "R", "none"));
        let file = |text: &str| {
            let meta: HashMap<String, String> =
                text.split(';').filter_map(|l| l.split_once('=').map(|(k, v)| (k.to_string(), v.to_string()))).collect();
            upside_down_file(&meta)
        };
        assert_eq!(file("eye_files=anatomical;swap=false;upside_down_eye=L"), Some(0));
        assert_eq!(file("eye_files=anatomical;swap=true;upside_down_eye=R"), Some(1));
        assert_eq!(file("eye_files=anatomical;swap=false"), Some(0));
        // Old sessions: their R was the left eye's camera; --swap made it L; fix_swap.py exchanged the files
        assert_eq!(file("slot_camera=0,0,0,0,1,1,1,1;swap=false"), Some(1));
        assert_eq!(file("slot_camera=0,0,0,0,1,1,1,1;swap=true"), Some(0));
        assert_eq!(file("slot_camera=1,1,1,1,0,0,0,0;swap=false"), Some(0));
        assert_eq!(file("slot_camera=1,1,1,1,0,0,0,0;repaired_swap=1"), Some(1));
        assert_eq!(file("eye_files=anatomical;upside_down_eye=L;repaired_swap=1"), Some(1));
        // One camera streaming: nothing flipped, as recorded or from both_eyes=false
        assert_eq!(file("eye_files=anatomical;upside_down_eye=none"), None);
        assert_eq!(file("eye_files=anatomical;both_eyes=false;upside_down_eye=L: stored raw"), None);
        let dir = old_session("one", "slot_camera=0,0,0,0\nboth_eyes=false\neye_files=anatomical\nupside_down_eye=none\n", false);
        let s = Session::open(&dir, false).unwrap();
        assert_eq!((s.upside_down(0), s.upside_down(1)), (false, false));
        assert_eq!(first_pixels(&s, 0, 2), [100, 101]);
        fs::remove_dir_all(&dir).unwrap();
    }

    fn first_pixels(s: &Session, eye: usize, n: usize) -> Vec<u8> {
        let mut img = vec![0u8; FRAME];
        (0..n)
            .map(|k| {
                s.read(eye, k, &mut img).unwrap();
                img[0]
            })
            .collect()
    }

    #[test]
    fn resample_keeps_the_frames_nearest_to_a_slower_grid() {
        let dir = fake_session("resample", 180, 90.0, &[61]);
        let mut s = Session::open(&dir, false).unwrap();
        assert_eq!(s.frames(0), 179);
        s.resample(15.0);
        // Every 6th frame (the lost 61 was not needed), both eyes, stamped on the 15 fps grid; the last tick (2 s)
        // takes the last frame, 11 ms before it.
        assert_eq!(s.frames(0), 31);
        assert_eq!(first_pixels(&s, 0, 31)[30], 179);
        assert_eq!(first_pixels(&s, 0, 30), (0..30).map(|k| (k * 6) as u8).collect::<Vec<_>>());
        assert_eq!(first_pixels(&s, 1, 3), [0, 6, 12]);
        assert!(s.t_cam[0].windows(2).all(|w| (w[1] - w[0] - 1.0 / 15.0).abs() < 1e-9));
        // Two frames stored in the other order (it happens): taken by their times.
        let mut s = Session::open(&dir, false).unwrap();
        s.t_cam[0].swap(30, 31);
        s.resample(15.0);
        assert_eq!(first_pixels(&s, 0, 7), [0, 6, 12, 18, 24, 31, 36]);
        // At 72 the ticks are 1.25 frames apart: the frame nearest to each (one in five left out).
        let mut s = Session::open(&dir, false).unwrap();
        s.resample(72.0);
        let picked = first_pixels(&s, 0, 40);
        assert!(picked.windows(2).all(|w| (1..=2).contains(&(w[1] - w[0]))), "{picked:?}");
        assert!(picked.iter().enumerate().all(|(i, &k)| (k as f64 - i as f64 * 1.25).abs() <= 0.5 + 1e-9), "{picked:?}");
        assert!((s.frames(0) as f64 - 180.0 * 72.0 / 90.0).abs() <= 2.0, "{}", s.frames(0));
        fs::remove_dir_all(&dir).unwrap();
    }
}
