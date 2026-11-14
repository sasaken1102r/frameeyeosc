//! The glint log: while a recording runs (`start <protocol>`, the rec_* folders; `eyecam-rec --replay … --glintlog
//! FILE` for a recording on disk), one CSV row per eye per frame with what the live engine and the glint tracker
//! (glint.rs) worked out for it: the pupil ellipse, the iris radius, the lids, every glint found near the pupil (up to
//! glint::MAX_GLINTS, brightest first) and the template, offset and tracking state. It is the raw material for
//! studying the pupil gaze against the glints (each glint's own place, a slip shared by both eyes, a gaze map refitted
//! across re-seats), which the live file's single offset can't give. Numbers only, no pictures; it reads what the
//! engine computes and changes none of it.
//!
//! Columns (HEADER; empty where there is no value):
//!   t_cam          the frame's camera time (CLOCK_MONOTONIC_RAW s: frames.csv's t_cam, and the eye server's
//!                  sample_time, so frameeyeosc's samples line up with it)
//!   t_mono_ns      the same frame on CLOCK_MONOTONIC (ns), as the live file's capture time for the eye (what
//!                  frameeyeosc reads with its samples); empty in a replay
//!   wall_unix      the wall clock when the row was made (s; empty in a replay)
//!   eye            L or R (anatomical)          frame   the eye's frame count (live: the live file's frame_count;
//!                                                       replay: the frame's index in the recording + 1)
//!   valid          the frame's values were published as valid     ok_pupil   a pupil was found
//!   pupil_cx, pupil_cy, pupil_a, pupil_b, pupil_angle_deg   the pupil ellipse in the upright picture (px; a >= b are
//!                  semi-axes, the angle is the long axis's from +x towards +y), pupil_vis its visible ray share
//!   pupil_ratio, pupil_mm, pupil_dilation   the pupil's size as published
//!   iris_r, iris_n the iris radius fitted on this frame (px) and its inliers; r_used the radius in use (the engine's
//!                  running value). The iris has no centre of its own here: it is fitted around the pupil's
//!   closed, lid, wide, squint, confidence   as published; upper_skin_y, upper_y, lower_y the lid lines at the pupil
//!                  column (px); skin_up, aperture the normalised lid features (5-frame medians)
//!   valve_open, valve_pitch   Valve's openness and gaze pitch (degrees) for this eye at this frame
//!   wear_t0        the camera time of this wear's first frame (a new wear: the headset taken off and put back on)
//!   template_id, template_t, template_mono_ns   the glint template in use (0 before the first; its camera time, and
//!                  the same on CLOCK_MONOTONIC as the live file has it; empty in a replay)
//!   tracked        the offset was worked out from this frame; relocked: by matching the whole pattern anywhere after
//!                  a loss; matches, since_good as glint::Slip; offset_x, offset_y the glints' offset from the template
//!                  (px, held while not tracked)
//!   glints         glints found; then g0_x, g0_y, g0_area, g0_peak .. g9_*: each one (px, px, px, 0..255)
//! The file is written through a buffer by a thread of its own (LiveLog: the live worker only hands it each frame's row)
//! and closed when the recording ends or eyecam-rec exits; past CAP_BYTES it stops writing.

use crate::glint::{Glint, MAX_GLINTS, Slip};
use crate::live::Out;
use std::fmt::Write as _;
use std::fs::{self, File};
use std::io::{BufWriter, Write};
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, mpsc};
use std::thread::JoinHandle;

/// The log's name in the recording's folder.
pub const FILE_NAME: &str = "glints.csv";
/// A file is not written past this many bytes (about four hours of both eyes at 90 frames a second).
pub const CAP_BYTES: u64 = 1 << 30;

/// The columns before the glints'.
const HEAD: &str = "t_cam,t_mono_ns,wall_unix,eye,frame,valid,ok_pupil,\
pupil_cx,pupil_cy,pupil_a,pupil_b,pupil_angle_deg,pupil_vis,pupil_ratio,pupil_mm,pupil_dilation,\
iris_r,iris_n,r_used,closed,lid,wide,squint,confidence,upper_skin_y,upper_y,lower_y,skin_up,aperture,\
valve_open,valve_pitch,wear_t0,\
template_id,template_t,template_mono_ns,tracked,relocked,matches,since_good,offset_x,offset_y,glints";

/// The header line: HEAD, then g<i>_x, g<i>_y, g<i>_area, g<i>_peak for each of the MAX_GLINTS glints.
pub fn header() -> String {
    let mut h = String::from(HEAD);
    for i in 0..MAX_GLINTS {
        let _ = write!(h, ",g{i}_x,g{i}_y,g{i}_area,g{i}_peak");
    }
    h
}

/// One eye's frame, as the log takes it.
#[derive(Clone, Copy, Debug)]
pub struct Row {
    pub t_cam: f64,
    /// None in a replay.
    pub t_mono_ns: Option<u64>,
    /// NaN in a replay.
    pub wall_unix: f64,
    pub eye: usize,
    pub frame: u64,
    pub valid: bool,
    pub ok_pupil: bool,
    pub pupil_cx: f64,
    pub pupil_cy: f64,
    pub pupil_a: f64,
    pub pupil_b: f64,
    pub pupil_angle_deg: f64,
    pub pupil_vis: f64,
    pub pupil_ratio: f64,
    pub pupil_mm: f64,
    pub pupil_dilation: f64,
    pub iris_r: f64,
    pub iris_n: u32,
    pub r_used: f64,
    pub closed: bool,
    pub lid: f64,
    pub wide: f64,
    pub squint: f64,
    pub confidence: f64,
    pub upper_skin_y: f64,
    pub upper_y: f64,
    pub lower_y: f64,
    pub skin_up: f64,
    pub aperture: f64,
    pub valve_open: f64,
    pub valve_pitch: f64,
    pub wear_t0: f64,
    pub slip: Slip,
    /// The template's time on CLOCK_MONOTONIC (ns; None in a replay or before the first template).
    pub template_mono_ns: Option<u64>,
    pub glints: [Glint; MAX_GLINTS],
    pub n_glints: usize,
}

impl Row {
    /// The frame `o` the engine put out for `eye` at camera time `t_cam`, with the glint tracker's `slip` and the
    /// `glints` it was given (only the first MAX_GLINTS are kept; there are never more). `valve_open`: Valve's
    /// openness of that eye (NaN without it); `wear_t0`: the engine's wear's first frame time.
    #[allow(clippy::too_many_arguments)]
    pub fn of(
        eye: usize,
        frame: u64,
        t_cam: f64,
        t_mono_ns: Option<u64>,
        wall_unix: f64,
        o: &Out,
        valve_open: f64,
        wear_t0: f64,
        slip: &Slip,
        template_mono_ns: Option<u64>,
        glints: &[Glint],
    ) -> Self {
        let mut kept = [Glint { x: f32::NAN, y: f32::NAN, area: 0, peak: 0 }; MAX_GLINTS];
        let n = glints.len().min(MAX_GLINTS);
        kept[..n].copy_from_slice(&glints[..n]);
        let f = &o.f;
        Self {
            t_cam,
            t_mono_ns,
            wall_unix,
            eye,
            frame,
            valid: o.valid,
            ok_pupil: f.ok_pupil,
            pupil_cx: f.pupil_cx,
            pupil_cy: f.pupil_cy,
            pupil_a: f.pupil_a,
            pupil_b: f.pupil_b,
            pupil_angle_deg: f.pupil_angle_deg,
            pupil_vis: f.pupil_vis,
            pupil_ratio: o.pupil_ratio,
            pupil_mm: o.pupil_mm,
            pupil_dilation: o.pupil_dilation,
            iris_r: f.iris_r,
            iris_n: f.iris_n,
            r_used: o.r,
            closed: o.closed,
            lid: o.eye_lid,
            wide: o.eye_wide,
            squint: o.eye_squint,
            confidence: o.confidence,
            upper_skin_y: f.upper_skin_y,
            upper_y: f.upper_y,
            lower_y: f.lower_y,
            skin_up: o.skin_up,
            aperture: o.aperture,
            valve_open,
            valve_pitch: o.pitch,
            wear_t0,
            slip: *slip,
            template_mono_ns: template_mono_ns.filter(|_| slip.template_id > 0),
            glints: kept,
            n_glints: n,
        }
    }

    /// The row's line (without the newline) into `out`, which is cleared first.
    pub fn write_line(&self, out: &mut String) {
        out.clear();
        // A number with `places` decimals, or nothing when it isn't one
        let num = |out: &mut String, v: f64, places: usize| {
            out.push(',');
            if v.is_finite() {
                let _ = write!(out, "{v:.places$}");
            }
        };
        let int = |out: &mut String, v: u64| {
            let _ = write!(out, ",{v}");
        };
        let flag = |out: &mut String, v: bool| out.push_str(if v { ",1" } else { ",0" });
        let s = &self.slip;
        if self.t_cam.is_finite() {
            let _ = write!(out, "{:.9}", self.t_cam);
        }
        out.push(',');
        if let Some(ns) = self.t_mono_ns {
            let _ = write!(out, "{ns}");
        }
        num(out, self.wall_unix, 3);
        out.push_str([",L", ",R"][self.eye.min(1)]);
        int(out, self.frame);
        flag(out, self.valid);
        flag(out, self.ok_pupil);
        num(out, self.pupil_cx, 2);
        num(out, self.pupil_cy, 2);
        num(out, self.pupil_a, 2);
        num(out, self.pupil_b, 2);
        num(out, self.pupil_angle_deg, 1);
        num(out, self.pupil_vis, 3);
        num(out, self.pupil_ratio, 4);
        num(out, self.pupil_mm, 3);
        num(out, self.pupil_dilation, 3);
        num(out, self.iris_r, 2);
        int(out, self.iris_n as u64);
        num(out, self.r_used, 2);
        flag(out, self.closed);
        num(out, self.lid, 3);
        num(out, self.wide, 3);
        num(out, self.squint, 3);
        num(out, self.confidence, 3);
        num(out, self.upper_skin_y, 2);
        num(out, self.upper_y, 2);
        num(out, self.lower_y, 2);
        num(out, self.skin_up, 4);
        num(out, self.aperture, 4);
        num(out, self.valve_open, 3);
        num(out, self.valve_pitch, 2);
        num(out, self.wear_t0, 3);
        int(out, s.template_id as u64);
        num(out, if s.template_id > 0 { s.template_t } else { f64::NAN }, 9);
        out.push(',');
        if let Some(ns) = self.template_mono_ns {
            let _ = write!(out, "{ns}");
        }
        flag(out, s.tracked);
        flag(out, s.relocked);
        int(out, s.matches as u64);
        int(out, s.since_good as u64);
        num(out, s.offset[0] as f64, 3);
        num(out, s.offset[1] as f64, 3);
        int(out, self.n_glints as u64);
        for (i, g) in self.glints.iter().enumerate() {
            if i < self.n_glints {
                num(out, g.x as f64, 2);
                num(out, g.y as f64, 2);
                int(out, g.area as u64);
                int(out, g.peak as u64);
            } else {
                out.push_str(",,,,");
            }
        }
    }
}

/// How the log went: the file, its rows and bytes, whether it reached CAP_BYTES, and why writing stopped if a write
/// failed ("" otherwise).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct GlintStatus {
    pub on: bool,
    pub path: String,
    pub rows: u64,
    pub bytes: u64,
    pub full: bool,
    pub error: String,
}

/// One glint log file.
pub struct GlintLog<W: Write = BufWriter<File>> {
    path: PathBuf,
    out: Option<W>,
    cap: u64,
    bytes: u64,
    rows: u64,
    full: bool,
    error: String,
    line: String,
}

impl GlintLog {
    /// A new FILE_NAME in the recording's folder `dir` (which is there), mode 0600, its header written.
    pub fn create(dir: &Path) -> Result<Self, String> {
        Self::create_at(&dir.join(FILE_NAME))
    }

    /// A new file at `path` (not one that is there already), mode 0600, its header written.
    pub fn create_at(path: &Path) -> Result<Self, String> {
        let at = |e: std::io::Error| format!("{}: {e}", path.display());
        let f = File::options().write(true).create_new(true).mode(0o600).open(path).map_err(at)?;
        // (set outright too, whatever the umask)
        f.set_permissions(fs::Permissions::from_mode(0o600)).map_err(at)?;
        Ok(Self::new(path.to_path_buf(), BufWriter::with_capacity(1 << 18, f), CAP_BYTES))
    }
}

impl<W: Write> GlintLog<W> {
    /// A log writing to `out` (the file at `path`), at most `cap` bytes; the header goes first.
    pub fn new(path: PathBuf, out: W, cap: u64) -> Self {
        let mut log =
            Self { path, out: Some(out), cap, bytes: 0, rows: 0, full: false, error: String::new(), line: String::with_capacity(512) };
        let head = header();
        log.write_line(&head);
        log
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    /// One row.
    pub fn push(&mut self, row: &Row) {
        let mut line = std::mem::take(&mut self.line);
        row.write_line(&mut line);
        if self.write_line(&line) {
            self.rows += 1;
        }
        self.line = line;
    }

    /// Write one line: false once the file is closed (full, failed or finished).
    fn write_line(&mut self, line: &str) -> bool {
        let Some(out) = self.out.as_mut() else { return false };
        let len = line.len() as u64 + 1;
        if self.bytes + len > self.cap {
            self.full = true;
            eprintln!("glintlog: {} reached {} MB, writing no more to it", self.path.display(), self.cap >> 20);
            self.finish();
            return false;
        }
        match out.write_all(line.as_bytes()).and_then(|()| out.write_all(b"\n")) {
            Ok(()) => {
                self.bytes += len;
                true
            }
            Err(e) => {
                self.error = e.to_string();
                eprintln!("glintlog: {}: {e}", self.path.display());
                self.out = None;
                false
            }
        }
    }

    /// Flush and close the file (nothing more is written after it).
    pub fn finish(&mut self) {
        if let Some(mut out) = self.out.take()
            && let Err(e) = out.flush()
        {
            self.error = e.to_string();
            eprintln!("glintlog: {}: {e}", self.path.display());
        }
    }

    pub fn status(&self, on: bool) -> GlintStatus {
        GlintStatus {
            on,
            path: self.path.display().to_string(),
            rows: self.rows,
            bytes: self.bytes,
            full: self.full,
            error: self.error.clone(),
        }
    }
}

/// What the live worker hands a LiveLog's thread.
enum Msg {
    Row(Box<Row>),
    /// Answered once everything sent before it is written (tests).
    #[cfg(test)]
    Sync(mpsc::Sender<()>),
}

/// A glint log written by a thread of its own: the live worker only hands it each frame's row over a channel (under
/// Shared::glintlog's lock), and the thread formats and writes it, so neither the text nor the disk holds the worker
/// up. Dropping it (the sender) ends the thread, which closes the file.
pub struct LiveLog {
    path: PathBuf,
    tx: mpsc::Sender<Msg>,
    status: Arc<Mutex<GlintStatus>>,
    thread: JoinHandle<()>,
}

impl GlintLog {
    /// This log written by a thread of its own (see LiveLog).
    pub fn start(mut self) -> Result<LiveLog, String> {
        let path = self.path.clone();
        let status = Arc::new(Mutex::new(self.status(true)));
        let shared = status.clone();
        let (tx, rx) = mpsc::channel::<Msg>();
        let thread = std::thread::Builder::new()
            .name("eyecam-glintlog".into())
            .spawn(move || {
                let mut since = 0u32;
                for msg in rx {
                    match msg {
                        Msg::Row(row) => self.push(&row),
                        #[cfg(test)]
                        Msg::Sync(done) => {
                            *shared.lock().unwrap() = self.status(true);
                            let _ = done.send(());
                            continue;
                        }
                    }
                    // (the status kept up about once a second of rows)
                    since += 1;
                    if since >= 180 {
                        since = 0;
                        *shared.lock().unwrap() = self.status(true);
                    }
                }
                self.finish();
                eprintln!("glintlog: closed {} ({} rows, {} bytes)", self.path.display(), self.rows, self.bytes);
                *shared.lock().unwrap() = self.status(false);
            })
            .map_err(|e| format!("{}: {e}", path.display()))?;
        Ok(LiveLog { path, tx, status, thread })
    }
}

/// A live log being closed by its thread: `wait` for it once Shared::glintlog's lock is let go (this waits on the
/// disk).
#[must_use = "wait for the file to be closed, after letting go of the lock"]
pub struct Closing(Option<JoinHandle<()>>);

impl Closing {
    pub fn wait(self) {
        if let Some(thread) = self.0 {
            let _ = thread.join();
        }
    }
}

/// The glint log of `eyecam-rec --serve`: turned on when a recording starts and off when it ends, fed by the live
/// worker. Off, a frame costs the worker only the look at whether it is on.
#[derive(Default)]
pub struct Control {
    log: Option<LiveLog>,
    /// The status of the file being written, or of the last one once it was turned off.
    status: Arc<Mutex<GlintStatus>>,
}

impl Control {
    pub fn is_on(&self) -> bool {
        self.log.is_some()
    }

    /// Start writing `log` (closing the one being written, if any): the closing of the one before.
    pub fn on(&mut self, log: LiveLog) -> Closing {
        let before = self.off();
        eprintln!("glintlog: writing {}", log.path.display());
        self.status = log.status.clone();
        self.log = Some(log);
        before
    }

    /// Close the file being written (nothing when off): its closing.
    pub fn off(&mut self) -> Closing {
        Closing(self.log.take().map(|log| {
            drop(log.tx);
            log.thread
        }))
    }

    pub fn push(&mut self, row: Row) {
        if let Some(log) = &self.log {
            let _ = log.tx.send(Msg::Row(Box::new(row)));
        }
    }

    pub fn status(&self) -> GlintStatus {
        GlintStatus { on: self.is_on(), ..self.status.lock().unwrap().clone() }
    }

    /// Wait until the rows pushed so far are written (tests; with the worker running, let go of the lock first).
    #[cfg(test)]
    pub(crate) fn sync(&self) {
        if let Some(log) = &self.log {
            let (done, wait) = mpsc::channel();
            if log.tx.send(Msg::Sync(done)).is_ok() {
                let _ = wait.recv();
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn slip() -> Slip {
        Slip {
            glints: 3,
            tracked: true,
            relocked: false,
            matches: 3,
            since_good: 0,
            offset: [1.25, -0.5],
            template_id: 2,
            template_t: 1234.5,
        }
    }

    /// A frame with a pupil and three glints (as the engine and the tracker give them).
    fn row() -> Row {
        let mut o = Out::default();
        o.valid = true;
        o.f.ok_pupil = true;
        o.f.pupil_cx = 240.125;
        o.f.pupil_cy = 190.5;
        o.f.pupil_a = 21.0;
        o.f.pupil_b = 19.75;
        o.f.pupil_angle_deg = 12.34;
        o.f.pupil_vis = 0.95;
        o.f.iris_r = 55.5;
        o.f.iris_n = 17;
        o.f.upper_skin_y = 140.0;
        o.f.upper_y = 146.0;
        o.f.lower_y = 250.0;
        o.pupil_ratio = 0.3812;
        o.pupil_mm = 4.2;
        o.pupil_dilation = 0.45;
        o.r = 56.0;
        o.eye_lid = 0.75;
        o.confidence = 0.9;
        o.skin_up = 1.1;
        o.aperture = 1.9;
        o.pitch = -12.0;
        let glints = [
            Glint { x: 230.0, y: 185.5, area: 9, peak: 250 },
            Glint { x: 250.25, y: 186.0, area: 7, peak: 240 },
            Glint { x: 241.0, y: 200.0, area: 5, peak: 200 },
        ];
        Row::of(1, 4567, 1234.567890123, Some(1_234_000_000_123), 1.7e9 + 0.25, &o, 0.92, 1200.0, &slip(), Some(1_233_000_000_000), &glints)
    }

    #[test]
    fn the_header_has_every_glint_and_a_row_its_columns() {
        let h = header();
        let cols: Vec<&str> = h.split(',').collect();
        assert_eq!(cols.len(), 42 + 4 * MAX_GLINTS);
        assert_eq!(&cols[..5], ["t_cam", "t_mono_ns", "wall_unix", "eye", "frame"]);
        assert_eq!(&cols[cols.len() - 4..], ["g9_x", "g9_y", "g9_area", "g9_peak"]);
        let mut line = String::new();
        row().write_line(&mut line);
        assert_eq!(line.split(',').count(), cols.len(), "{line}");
        let get = |name: &str| line.split(',').nth(cols.iter().position(|c| *c == name).unwrap()).unwrap().to_string();
        assert_eq!((get("t_cam"), get("t_mono_ns"), get("wall_unix"), get("eye"), get("frame")), ("1234.567890123".into(), "1234000000123".into(), "1700000000.250".into(), "R".into(), "4567".into()));
        assert_eq!((get("pupil_cx"), get("pupil_a"), get("pupil_angle_deg"), get("pupil_ratio")), ("240.12".into(), "21.00".into(), "12.3".into(), "0.3812".into()));
        assert_eq!((get("iris_r"), get("iris_n"), get("r_used"), get("valve_open"), get("valve_pitch")), ("55.50".into(), "17".into(), "56.00".into(), "0.920".into(), "-12.00".into()));
        assert_eq!((get("template_id"), get("template_t"), get("template_mono_ns")), ("2".into(), "1234.500000000".into(), "1233000000000".into()));
        assert_eq!((get("tracked"), get("relocked"), get("offset_x"), get("offset_y"), get("glints")), ("1".into(), "0".into(), "1.250".into(), "-0.500".into(), "3".into()));
        assert_eq!((get("g1_x"), get("g1_y"), get("g1_area"), get("g1_peak")), ("250.25".into(), "186.00".into(), "7".into(), "240".into()));
        // Past the glints found: empty
        assert_eq!((get("g3_x"), get("g9_peak")), (String::new(), String::new()));
        // The whole line, as written (the seven glints not found: four empty cells each)
        let expected = "1234.567890123,1234000000123,1700000000.250,R,4567,1,1,240.12,190.50,21.00,19.75,12.3,0.950,0.3812,\
4.200,0.450,55.50,17,56.00,0,0.750,0.000,0.000,0.900,140.00,146.00,250.00,1.1000,1.9000,0.920,-12.00,1200.000,\
2,1234.500000000,1233000000000,1,0,3,0,1.250,-0.500,3,230.00,185.50,9,250,250.25,186.00,7,240,241.00,200.00,5,200"
            .to_string()
            + &",".repeat(4 * 7);
        assert_eq!(line, expected);
    }

    #[test]
    fn no_pupil_no_template_and_a_replay_leave_cells_empty() {
        let o = Out::default();
        let none = Slip::default();
        let r = Row::of(0, 1, 50.0, None, f64::NAN, &o, f64::NAN, f64::NAN, &none, Some(99), &[]);
        let mut line = String::new();
        r.write_line(&mut line);
        let cols = header();
        let cols: Vec<&str> = cols.split(',').collect();
        assert_eq!(line.split(',').count(), cols.len(), "{line}");
        // (Out::default's numbers are 0, not NaN; the features' are NaN)
        assert!(line.starts_with("50.000000000,,,L,1,0,0,,,,,,,0.0000,0.000,0.000,,0,0.00,0,"), "{line}");
        // No template: no template time (the one given is dropped), not tracked, offset 0, no glints
        let get = |name: &str| line.split(',').nth(cols.iter().position(|c| *c == name).unwrap()).unwrap().to_string();
        assert_eq!((get("template_id"), get("template_t"), get("template_mono_ns")), ("0".into(), String::new(), String::new()));
        assert_eq!((get("tracked"), get("glints"), get("g0_x")), ("0".into(), "0".into(), String::new()));
        assert_eq!((get("valve_open"), get("wear_t0")), (String::new(), String::new()));
    }

    /// A writer the test keeps a handle on.
    #[derive(Clone, Default)]
    struct Sink(Arc<Mutex<Vec<u8>>>);

    impl Write for Sink {
        fn write(&mut self, b: &[u8]) -> std::io::Result<usize> {
            self.0.lock().unwrap().extend_from_slice(b);
            Ok(b.len())
        }

        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }

    #[test]
    fn rows_follow_the_header_and_stop_at_the_cap() {
        let sink = Sink::default();
        let head = header().len() as u64 + 1;
        let mut line = String::new();
        row().write_line(&mut line);
        let one = line.len() as u64 + 1;
        let mut log = GlintLog::new(PathBuf::from("/x/glints.csv"), sink.clone(), head + 3 * one + 10);
        for _ in 0..5 {
            log.push(&row());
        }
        let st = log.status(true);
        assert!(st.full && st.rows == 3 && st.bytes == head + 3 * one, "{st:?}");
        let text = String::from_utf8(sink.0.lock().unwrap().clone()).unwrap();
        let lines: Vec<&str> = text.lines().collect();
        assert_eq!(lines.len(), 4);
        assert_eq!(lines[0], header());
        assert!(lines[1..].iter().all(|l| *l == line));
    }

    /// Through the thread: each row pushed is written, the file is 0600 and closed when turned off; on again makes no
    /// second file over the first (create_new).
    #[test]
    fn a_live_log_writes_its_rows_and_closes_when_turned_off() {
        let dir = std::env::temp_dir().join(format!("eyecam-glintlog-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let mut c = Control::default();
        assert!(!c.is_on());
        c.push(row());
        c.on(GlintLog::create(&dir).unwrap().start().unwrap()).wait();
        assert!(c.is_on());
        for _ in 0..200 {
            c.push(row());
        }
        c.sync();
        assert_eq!(c.status().rows, 200);
        c.off().wait();
        let st = c.status();
        let path = dir.join(FILE_NAME);
        assert!(!st.on && st.rows == 200 && st.path == path.display().to_string(), "{st:?}");
        let text = fs::read_to_string(&path).unwrap();
        assert_eq!(text.lines().count(), 201);
        assert_eq!(st.bytes, text.len() as u64);
        assert_eq!(fs::metadata(&path).unwrap().permissions().mode() & 0o777, 0o600);
        // Off: pushes go nowhere; a second log in the same folder is refused, the first left as it is
        c.push(row());
        assert!(GlintLog::create(&dir).is_err());
        assert_eq!(fs::read_to_string(&path).unwrap(), text);
        fs::remove_dir_all(&dir).unwrap();
    }
}
