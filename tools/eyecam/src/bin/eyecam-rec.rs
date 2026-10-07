//! eyecam-rec: records the Steam Frame eye camera frames, while Valve's eye tracker keeps running, together with
//! the eye tracker's own samples. Runs as the user; the one root step is eyecam-grab, which the user runs with sudo
//! and which only passes the shared buffers' descriptors over a socket. The buffers are mapped read-only and the
//! eye server's shared memory is only read.
//!
//! Two modes: one recording and exit (the default), or `--serve`, which keeps the buffers and records whenever a
//! `start` arrives on the control socket, publishing its state to status.json for the VR panel.

use eyecam::autograb;
use eyecam::cues::{self, Player, Step};
use eyecam::mem::Arena;
use eyecam::ring::{self, Clock, FRAME_BYTES, HEADER_BYTES, HEIGHT, Ring, STRIDE, WIDTH};
use eyecam::shm::{self, Sample, Shm};
use eyecam::livesvc::{self, CollectKind, Msg};
use eyecam::settings::{self, Settings};
use eyecam::status::{self, CalibKind, Command, Status};
use eyecam::{now_raw, proto};
use std::error::Error;
use std::fs::{self, File, OpenOptions};
use std::io::{BufRead, BufReader, BufWriter, ErrorKind, Read, Write};
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::os::unix::fs::{FileTypeExt, MetadataExt, PermissionsExt};
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, mpsc};
use std::thread;
use std::time::Duration;

const POLL: Duration = Duration::from_micros(2500);
// Without a slot changing for this long the frames have stopped (headset off, tracker paused): search again.
const LOST_AFTER: f64 = 2.0;
// Headset worn this long without a single change anywhere in the buffers: they are no longer the live ones.
const STALE_AFTER: f64 = 120.0;
// The proximity reading has to stay above --prox-min this long before the headset counts as worn.
const WORN_FOR: f64 = 1.0;
// How often a search looks at the buffers: every second while the headset counts as worn (or the sensor can't be
// read), less often while the proximity sensor says it is off. It looks either way, since the sensor reads low on
// some faces: with the cameras off nothing changes in the buffers, and a look costs about 2.5 ms.
const SCAN_EVERY: f64 = 1.0;
const SCAN_EVERY_NOT_WORN: f64 = 2.0;
// Assumed frame rate per eye when estimating disk use.
const EST_FPS: f64 = 90.0;
const MIN_FREE: u64 = 1 << 30;

/// What waiting_fds says while the buffers can't be fetched automatically: the camera tool (eyecam-grab with
/// cap_sys_ptrace) is put in by the panel's first-time setup. Once it is in, the automatic grab's messages replace this.
const INSTALL_TOOL: &str = "パネルの「目のカメラ」タブで、目のカメラの道具を入れてね";

static STOP: AtomicBool = AtomicBool::new(false);

extern "C" fn on_signal(_: libc::c_int) {
    STOP.store(true, Ordering::SeqCst);
}

type Result<T> = std::result::Result<T, Box<dyn Error>>;

struct Args {
    seconds: Option<f64>,
    out: PathBuf,
    swap: bool,
    cues: Option<PathBuf>,
    serve: bool,
    fake: bool,
    /// --fake-search: the fake never finds the eye video, for this status.json `search` reason.
    fake_search: Option<&'static str>,
    /// --fake-calib: the fake `calib wear` goes through without this eye ("L" / "R"), or fails ("LR").
    fake_calib: Option<&'static str>,
    /// --fake-camfps: the fake cameras' frame rate (status.json cam_fps, and the fake calibration's counts).
    fake_camfps: f64,
    run_dir: PathBuf,
    replay: Option<PathBuf>,
    compat: bool,
    limit: Option<usize>,
    out_given: bool,
    user_calib: Option<PathBuf>,
    calib_block: Option<usize>,
    fit_user: Option<PathBuf>,
    use_wear: bool,
    prewarm: bool,
    /// --resample: replay as if the cameras had run at this frame rate.
    resample: Option<f64>,
    /// --calib: replay the session as this calibration and write its calib_result.json to --out.
    calib_replay: Option<eyecam::replay::CalibRun>,
    tune: eyecam::live::Tune,
    wait_grab: f64,
    wait_lock: f64,
    prox_min: f64,
    allow_one_eye: bool,
    full_width: bool,
    beep: bool,
    volume: f32,
}

const USAGE: &str = "usage: eyecam-rec [options]
  --serve            stay running: keep the buffers and record on `start` from the control socket
                     (/run/user/1000/eyecam/ctl.sock), publishing /run/user/1000/eyecam/status.json
  --fake             like --serve, but with no buffers, camera or files: it plays the states and the protocol
                     timeline so a panel can be built and tested against it (implies --serve)
  --fake-search R    like --fake, but the eye video is never found while searching (idle with live on, start,
                     calib), for status.json search reason R: not_worn, no_video or one_eye
  --fake-calib E     like --fake, but `calib wear` goes through without eye E (L or R), or fails (LR)
  --fake-camfps N    like --fake, but the cameras deliver N frames a second (default 90): status.json cam_fps, and
                     the fake calibration's frame counts and message
  --run-dir DIR      where status.json and ctl.sock go (default /run/user/1000/eyecam)
  --replay DIR       run the eye-feature pipeline over a recorded session, writing a CSV to --out
  --compat           with --replay: reproduce the Python prototype's run2.py features (for agreement checks)
  --limit N          with --replay: only the first N frames of each eye
  --user-calib FILE  with --replay: a calib.json to use (user shape and reference wear levels)
  --calib-block N    with --replay: fit this wear's levels from the first N cue steps once they are over
  --fit-user FILE    with --replay: fit a user calibration from the whole session and write it to FILE
  --use-wear         with --replay --user-calib: start from that file's wear levels (as right after calib wear)
  --prewarm          with --replay: learn the auto baselines in a first pass, then replay from the start with them
  --resample FPS     with --replay: as if the cameras had run at FPS (the frames nearest to an FPS grid), e.g. 15
  --calib KIND       with --replay: replay the session as a calibration (wear or user, its steps from cues.csv) and
                     write what calib_result.json would hold to --out; --user-calib gives the history, the user
                     calibration and (for user) the wear levels
  --no-track         with --replay: do not follow the baselines after warm-up (evaluation)
  --no-gaze-drop     with --replay: no EyeWide hold after a downward glance (evaluation)
  --wide-curve S,W   with --replay: EyeWide = (rise - S * step) / (W * step) (evaluation)
  --widen-sensitivity X  with --replay: the curve for widen sensitivity X in 0..1 (default 0.5, as live)
  --seconds N        how long to record once frames are found (default 120, or the cue protocol's length)
  --out DIR          where session directories go (default ~/eyecam)
  --cues FILE        play a beep cue protocol (lines `seconds label`, see protocol_widen.txt) and log cues.csv
  --swap             swap which camera is labelled L and R
  --wait-grab N      seconds to wait for `sudo eyecam-grab` (default 600; --serve waits forever)
  --wait-lock N      seconds to wait for eye frames each time they are searched for (default 300)
  --prox-min V       proximity reading above which the headset counts as worn (default 20)
  --allow-one-eye    record even if only one camera is streaming
  --full-width       keep the 512-byte rows (padding included) instead of 400-pixel rows
  --no-beep          log cues without playing them
  --volume V         beep volume 0..1 (default 0.25)";

fn parse_args() -> Result<Args> {
    let home = home_dir();
    let mut args = Args {
        seconds: None,
        out: home.join("eyecam"),
        swap: false,
        cues: None,
        serve: false,
        fake: false,
        fake_search: None,
        fake_calib: None,
        fake_camfps: 90.0,
        run_dir: PathBuf::from(status::DEFAULT_DIR),
        replay: None,
        compat: false,
        limit: None,
        out_given: false,
        user_calib: None,
        calib_block: None,
        fit_user: None,
        use_wear: false,
        prewarm: false,
        resample: None,
        calib_replay: None,
        tune: eyecam::live::Tune::default(),
        wait_grab: 600.0,
        wait_lock: 300.0,
        prox_min: 20.0,
        allow_one_eye: false,
        full_width: false,
        beep: true,
        volume: 0.25,
    };
    let mut it = std::env::args().skip(1);
    while let Some(arg) = it.next() {
        let mut value = |name: &str| it.next().ok_or_else(|| format!("{name} needs a value"));
        let number = |s: String, name: &str| -> Result<f64> {
            s.parse::<f64>().ok().filter(|v| v.is_finite() && *v >= 0.0).ok_or_else(|| format!("bad {name}").into())
        };
        match arg.as_str() {
            "--seconds" => args.seconds = Some(number(value("--seconds")?, "--seconds")?),
            "--out" => {
                args.out = PathBuf::from(value("--out")?);
                args.out_given = true;
            }
            "--replay" => args.replay = Some(PathBuf::from(value("--replay")?)),
            "--compat" => args.compat = true,
            "--user-calib" => args.user_calib = Some(PathBuf::from(value("--user-calib")?)),
            "--calib-block" => args.calib_block = Some(number(value("--calib-block")?, "--calib-block")? as usize),
            "--fit-user" => args.fit_user = Some(PathBuf::from(value("--fit-user")?)),
            "--use-wear" => args.use_wear = true,
            "--prewarm" => args.prewarm = true,
            "--resample" => {
                let v = number(value("--resample")?, "--resample")?;
                if v < 1.0 {
                    return Err("--resample needs a frame rate of at least 1".into());
                }
                args.resample = Some(v);
            }
            "--calib" => {
                args.calib_replay = Some(match value("--calib")?.as_str() {
                    "wear" => eyecam::replay::CalibRun::Wear,
                    "user" => eyecam::replay::CalibRun::User,
                    _ => return Err("--calib must be wear or user".into()),
                })
            }
            "--no-track" => args.tune.track = false,
            "--no-gaze-drop" => args.tune.gaze_drop = false,
            "--widen-sensitivity" => {
                let v = number(value("--widen-sensitivity")?, "--widen-sensitivity")?;
                (args.tune.wide_start, args.tune.wide_width) = eyecam::live::wide_curve(v);
            }
            "--wide-curve" => {
                let v = value("--wide-curve")?;
                let (a, b) = v.split_once(',').ok_or("--wide-curve needs START,WIDTH")?;
                args.tune.wide_start = a.trim().parse().map_err(|_| "--wide-curve needs numbers")?;
                args.tune.wide_width = b.trim().parse().map_err(|_| "--wide-curve needs numbers")?;
            }
            "--limit" => args.limit = Some(number(value("--limit")?, "--limit")? as usize),
            "--cues" => args.cues = Some(PathBuf::from(value("--cues")?)),
            "--serve" => args.serve = true,
            "--fake" => {
                args.serve = true;
                args.fake = true;
            }
            "--fake-search" => {
                let v = value("--fake-search")?;
                let reason = [status::SEARCH_NOT_WORN, status::SEARCH_NO_VIDEO, status::SEARCH_ONE_EYE]
                    .into_iter()
                    .find(|r| *r == v)
                    .ok_or("--fake-search must be not_worn, no_video or one_eye")?;
                args.serve = true;
                args.fake = true;
                args.fake_search = Some(reason);
            }
            "--fake-calib" => {
                let v = value("--fake-calib")?;
                args.fake_calib = Some(["L", "R", "LR"].into_iter().find(|e| *e == v).ok_or("--fake-calib must be L, R or LR")?);
                args.serve = true;
                args.fake = true;
            }
            "--fake-camfps" => {
                let v = number(value("--fake-camfps")?, "--fake-camfps")?;
                if !(1.0..=500.0).contains(&v) {
                    return Err("--fake-camfps needs 1 to 500 frames a second".into());
                }
                args.fake_camfps = v;
                args.serve = true;
                args.fake = true;
            }
            "--run-dir" => args.run_dir = PathBuf::from(value("--run-dir")?),
            "--swap" => args.swap = true,
            "--wait-grab" => args.wait_grab = number(value("--wait-grab")?, "--wait-grab")?,
            "--wait-lock" => args.wait_lock = number(value("--wait-lock")?, "--wait-lock")?,
            "--prox-min" => args.prox_min = number(value("--prox-min")?, "--prox-min")?,
            "--allow-one-eye" => args.allow_one_eye = true,
            "--full-width" => args.full_width = true,
            "--no-beep" => args.beep = false,
            "--volume" => args.volume = number(value("--volume")?, "--volume")?.min(1.0) as f32,
            "-h" | "--help" => {
                println!("{USAGE}");
                std::process::exit(0);
            }
            _ => return Err(format!("unknown argument {arg}\n{USAGE}").into()),
        }
    }
    if args.serve && args.cues.is_some() {
        return Err("--cues is for one recording; with --serve the protocol comes with `start <name>`".into());
    }
    Ok(args)
}

fn home_dir() -> PathBuf {
    std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("."))
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("eyecam-rec: {error}");
            ExitCode::FAILURE
        }
    }
}

fn run() -> Result<()> {
    let args = parse_args()?;
    if let Some(dir) = &args.replay {
        if !args.out_given {
            return Err("--replay needs --out FILE.csv".into());
        }
        let mut session = eyecam::replay::Session::open(dir, args.swap)?;
        eprintln!("replay {}{}", dir.display(), if session.swapped { " (eye files swapped)" } else { "" });
        if let Some(fps) = args.resample {
            session.resample(fps);
            eprintln!("resampled to {fps} fps: {} L + {} R frames", session.frames(0), session.frames(1));
        }
        if args.compat {
            return Ok(eyecam::replay::write_compat(&session, &args.out, args.limit)?);
        }
        let calib = match &args.user_calib {
            Some(p) => Some(eyecam::live::CalibFile::parse(&fs::read_to_string(p).map_err(|e| format!("{}: {e}", p.display()))?)?),
            None => None,
        };
        if let Some(kind) = args.calib_replay {
            let json = eyecam::replay::replay_calib(&session, kind, &calib.unwrap_or_default(), args.tune)?;
            fs::write(&args.out, json).map_err(|e| format!("{}: {e}", args.out.display()))?;
            return Ok(());
        }
        let opts = eyecam::replay::LiveOpts {
            calib,
            block_steps: args.calib_block,
            fit_user: args.fit_user.clone(),
            use_wear: args.use_wear,
            prewarm: args.prewarm,
            tune: args.tune,
            limit: args.limit,
        };
        let ms = eyecam::replay::write_live(&session, &args.out, &opts)?;
        eprintln!("live replay: {ms:.3} ms per frame (one eye) on this core");
        return Ok(());
    }
    if unsafe { libc::getuid() } != proto::USER_UID {
        return Err(format!("run this as the user (uid {}), not with sudo", proto::USER_UID).into());
    }
    unsafe {
        libc::signal(libc::SIGINT, on_signal as *const () as libc::sighandler_t);
        libc::signal(libc::SIGTERM, on_signal as *const () as libc::sighandler_t);
        libc::setpriority(libc::PRIO_PROCESS, 0, 10);
    }
    fs::create_dir_all(&args.out).map_err(|e| format!("{}: {e}", args.out.display()))?;
    if args.serve {
        return serve(&args);
    }

    let steps = match &args.cues {
        Some(path) => Some(cues::parse(&fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?)?),
        None => None,
    };
    let seconds = recording_seconds(&args, &steps);
    let row_bytes = if args.full_width { STRIDE } else { WIDTH };
    let need = (seconds * 2.0 * EST_FPS * (row_bytes * HEIGHT + 300) as f64) as u64;
    let free = free_bytes(&args.out);
    let out = args.out.display();
    eprintln!("recording {seconds:.0} s needs about {} MB; {} MB free in {out}", need >> 20, free >> 20);
    if free < need + MIN_FREE {
        eprintln!("WARNING: that may not fit; recording stops when less than 1 GB is left");
    }

    let mut buffers = Buffers::receive(args.wait_grab, None)?;
    let mut prox = Proximity::find();
    let protocol = args.cues.as_ref().map_or(String::new(), |p| p.display().to_string());
    let ended = record(&mut buffers, &mut None, &args, steps, &protocol, &mut prox, None, None, Mode::Record)?;
    match (ended.dir, ended.abort) {
        (None, Some(abort)) => Err(abort.message().into()),
        _ => Ok(()),
    }
}

/// How long a recording runs: --seconds, else the protocol's length plus a moment for the end beep, else 120 s.
fn recording_seconds(args: &Args, steps: &Option<Vec<Step>>) -> f64 {
    let protocol_len: f64 = steps.iter().flatten().map(|s| s.seconds).sum();
    args.seconds.unwrap_or(if steps.is_some() { protocol_len + 1.5 } else { 120.0 })
}

/// Why searching or recording stopped early.
#[derive(Clone, Copy, Debug, PartialEq)]
enum Abort {
    Interrupted,
    Stopped,
    TrackerExited,
    Stale,
    NotFound,
    DiskFull,
    /// The recording's time ran out while frames were being searched for again (not an error).
    TimeUp,
    /// An idle search gave way to a control request.
    Busy,
}

impl Abort {
    fn message(self) -> &'static str {
        match self {
            Abort::Interrupted => "中断した（Ctrl-C / SIGTERM）",
            Abort::Stopped => "stop で止めた",
            Abort::TrackerExited => "アイトラッカーが終了した",
            Abort::Stale => "バッファが更新されなくなった",
            Abort::NotFound => "目の映像が見つからなかった（ヘッドセットをかぶってから start してね）",
            Abort::DiskFull => "ディスクの空きが 1 GB 未満",
            Abort::TimeUp => "録画時間が終わった",
            Abort::Busy => "",
        }
    }

    /// The buffers are no longer the live ones: new descriptors are needed.
    fn needs_grab(self) -> bool {
        matches!(self, Abort::TrackerExited | Abort::Stale)
    }
}

// ------------------------------------------------------------------------------------------------- serve mode

/// A control request, answered with one line through `reply`.
struct Request {
    command: Command,
    reply: mpsc::Sender<String>,
}

fn answer(request: Request, reply: &str) {
    let _ = request.reply.send(reply.to_string());
}

/// Accept a `stop`: show `idle` first, so status.json says so by the time the client has its `ok`.
fn accept_stop(ctl: Option<&Daemon>, request: Request, message: &str) {
    if let Some(d) = ctl {
        d.set(|s| {
            s.state = "idle";
            s.message = message.into();
            s.locked = false;
            s.clear_recording();
        });
    }
    answer(request, "ok");
}

/// The idle messages while the eyes are in view (replaced by each other as the live state changes).
const LIVE_IDLE_MESSAGES: [&str; 4] = [
    "待機中",
    "目の値を出しているよ",
    "見開きの基準を覚えているところ（目を開けて、ふつうに前を見ていてね）",
    "見開きの幅がまだわからないので仮の値。一度だけ calib wear をしてね",
];

fn live_idle_message(calib_saved: bool, ready: bool) -> &'static str {
    match (calib_saved, ready) {
        (false, _) => LIVE_IDLE_MESSAGES[3],
        (true, false) => LIVE_IDLE_MESSAGES[2],
        (true, true) => LIVE_IDLE_MESSAGES[1],
    }
}

/// status.json, written whole (temp file + rename). Writes are serialized, so a snapshot taken earlier can never
/// land after a newer one.
struct StatusFile {
    path: PathBuf,
    write_lock: Mutex<()>,
    live: Option<Arc<livesvc::Shared>>,
    /// settings.json, for `dev` (edited by hand, so it is followed while running).
    settings: Mutex<settings::Watch>,
}

impl StatusFile {
    fn write(&self, status: &Mutex<Status>) {
        let _guard = self.write_lock.lock().unwrap();
        let json = {
            let mut s = status.lock().unwrap();
            s.grab_outdated = eyecam::autograb::bundled_grab().is_some_and(|b| eyecam::autograb::grab_outdated(&b));
            s.dev = self.settings.lock().unwrap().get().dev;
            if let Some(l) = &self.live {
                s.calib_state = l.calib_state.load(Ordering::Relaxed);
                s.recalib_suggested = l.recalib_suggested.load(Ordering::Relaxed);
                s.live = l.live_on.load(Ordering::Relaxed);
                s.live_ms = l.us_per_frame.load(Ordering::Relaxed) as f64 / 1000.0;
                let ready = l.baseline_ready.load(Ordering::Relaxed);
                s.baseline = if ready { "ready" } else { "warming" };
                s.warmup_remaining_s = if ready { 0.0 } else { l.warmup_ms.load(Ordering::Relaxed) as f64 / 1000.0 };
                s.calib_saved = l.calib_saved.load(Ordering::Relaxed);
                s.widen_sensitivity = l.widen_sensitivity();
                s.setup_done = l.setup_done.load(Ordering::Relaxed);
                s.last_calib_widen = match l.last_calib_widen.load(Ordering::Relaxed) {
                    1 => "measured",
                    2 => "default",
                    _ => "",
                };
                s.calib_failed_eye = match l.calib_failed_eye.load(Ordering::Relaxed) & 3 {
                    1 => "L",
                    2 => "R",
                    3 => "LR",
                    _ => "",
                };
                s.pupil = [0, 1].map(|e| if s.live { l.pupil_share(e) } else { f64::NAN });
                let now = now_raw();
                s.cam_fps = [0, 1].map(|e| if s.locked { l.cam_fps(e, now) } else { f64::NAN });
                s.last_calib = l.last_calib.lock().unwrap().clone();
                // While idle with the eyes in view, say what the live values are waiting for.
                if s.state == "idle" && s.live && s.locked && LIVE_IDLE_MESSAGES.contains(&s.message.as_str()) {
                    s.message = live_idle_message(s.calib_saved, ready).into();
                }
            }
            s.to_json(unix_now(), std::process::id())
        };
        let tmp = self.path.with_extension("json.tmp");
        if fs::write(&tmp, json + "\n").is_ok() {
            let _ = fs::rename(&tmp, &self.path);
        }
    }
}

/// The status file writer and the control socket, each on its own thread; the main thread does the work.
struct Daemon {
    status: Arc<Mutex<Status>>,
    requests: mpsc::Receiver<Request>,
    shutdown: Arc<AtomicBool>,
    threads: Vec<thread::JoinHandle<()>>,
    file: Arc<StatusFile>,
}

impl Daemon {
    /// Publish into `dir` (created 0700 if missing; an existing one must be ours and not a symlink).
    fn start(dir: &Path, live: Option<Arc<livesvc::Shared>>) -> Result<Self> {
        match fs::symlink_metadata(dir) {
            Ok(meta) if meta.is_dir() && meta.uid() == proto::USER_UID => {}
            Ok(_) => return Err(format!("{} exists and is not our directory", dir.display()).into()),
            Err(e) if e.kind() == ErrorKind::NotFound => fs::create_dir(dir)?,
            Err(e) => return Err(format!("{}: {e}", dir.display()).into()),
        }
        fs::set_permissions(dir, fs::Permissions::from_mode(0o700))?;
        let file = Arc::new(StatusFile {
            path: dir.join(status::STATUS_FILE),
            write_lock: Mutex::new(()),
            live,
            settings: Mutex::new(settings::Watch::new(settings::path())),
        });
        let listener = bind_private(&dir.join(status::CTL_FILE), "another eyecam-rec --serve is already running")?;
        listener.0.set_nonblocking(true)?;
        let status = Arc::new(Mutex::new(Status::default()));
        let shutdown = Arc::new(AtomicBool::new(false));
        let (tx, requests) = mpsc::channel();
        file.write(&status);
        let ctl = {
            let (status, shutdown, file) = (status.clone(), shutdown.clone(), file.clone());
            thread::spawn(move || ctl_loop(listener, status, file, tx, shutdown))
        };
        let writer = {
            let (status, shutdown, file) = (status.clone(), shutdown.clone(), file.clone());
            thread::spawn(move || status_loop(status, file, shutdown))
        };
        Ok(Self { status, requests, shutdown, threads: vec![ctl, writer], file })
    }

    fn set(&self, f: impl FnOnce(&mut Status)) {
        let mut s = self.status.lock().unwrap();
        let before = (s.state == "error").then(|| s.message.clone());
        f(&mut s);
        s.note_error(before.as_deref(), unix_now());
    }

    fn request(&self) -> Option<Request> {
        self.requests.try_recv().ok()
    }

    /// Stop the threads, remove the control socket, and leave a final status.json with state `stopped`.
    fn finish(mut self) {
        self.shutdown.store(true, Ordering::SeqCst);
        for t in self.threads.drain(..) {
            let _ = t.join();
        }
        self.set(|s| {
            s.state = "stopped";
            s.message = "eyecam-rec は止まっている".into();
            s.locked = false;
            s.clear_recording();
        });
        if let Some(l) = &self.file.live {
            l.live_on.store(false, Ordering::Relaxed);
        }
        self.file.write(&self.status);
    }
}

/// Write status.json about 10 times a second, whatever the main thread is doing.
fn status_loop(status: Arc<Mutex<Status>>, file: Arc<StatusFile>, shutdown: Arc<AtomicBool>) {
    while !shutdown.load(Ordering::SeqCst) {
        file.write(&status);
        thread::sleep(Duration::from_millis(100));
    }
}

/// Wall-clock seconds, for status.json's `updated_unix`.
fn unix_now() -> f64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0.0, |d| d.as_secs_f64())
}

/// Serve the control socket: one command line per connection, one reply line.
fn ctl_loop(
    listener: SocketFile,
    status: Arc<Mutex<Status>>,
    file: Arc<StatusFile>,
    tx: mpsc::Sender<Request>,
    shutdown: Arc<AtomicBool>,
) {
    while !shutdown.load(Ordering::SeqCst) {
        match listener.0.accept() {
            Ok((stream, _)) => {
                let reply = ctl_reply(&stream, &status, &file, &tx);
                if let Some(reply) = reply {
                    // The main thread updated the state before replying; publish it before the reply goes out,
                    // so a client reading status.json right after `ok` already sees the new state.
                    file.write(&status);
                    let _ = (&stream).write_all(format!("{reply}\n").as_bytes());
                }
            }
            Err(_) => thread::sleep(Duration::from_millis(20)),
        }
    }
}

/// `set widen_sensitivity`: only the output mapping and the saved setting change, so it is done here, in every state,
/// without waiting for the main thread. The fake daemon (no live engine) shows it but saves nothing.
fn set_widen_sensitivity(v: f64, status: &Mutex<Status>, file: &StatusFile) -> String {
    if let Some(l) = &file.live {
        // Keep the rest of the file (`dev`) as it is.
        let path = settings::path();
        if let Err(e) = (Settings { widen_sensitivity: v, ..Settings::load(&path) }).save(&path) {
            return format!("err 保存できなかった: {e}");
        }
        l.set_widen_sensitivity(v);
    }
    status.lock().unwrap().widen_sensitivity = v;
    "ok".into()
}

/// The reply to one connection (None: not the user, so it gets nothing).
fn ctl_reply(stream: &UnixStream, status: &Mutex<Status>, file: &StatusFile, tx: &mpsc::Sender<Request>) -> Option<String> {
    if peer_cred(stream).ok()?.uid != proto::USER_UID {
        return None;
    }
    stream.set_nonblocking(false).ok()?;
    stream.set_read_timeout(Some(Duration::from_secs(2))).ok()?;
    stream.set_write_timeout(Some(Duration::from_secs(2))).ok()?;
    let mut line = String::new();
    if BufReader::new(stream.take(256)).read_line(&mut line).is_err() {
        return Some("err 読めなかった".into());
    }
    Some(match status::parse_command(&line) {
        Err(e) => format!("err {e}"),
        Ok(Command::Status) => {
            status.lock().unwrap().to_json(unix_now(), std::process::id())
        }
        Ok(Command::SetWidenSensitivity(v)) => set_widen_sensitivity(v, status, file),
        Ok(command) => {
            let (reply, rx) = mpsc::channel();
            if tx.send(Request { command, reply }).is_err() {
                return Some("err 終了中".into());
            }
            rx.recv_timeout(Duration::from_secs(10)).unwrap_or_else(|_| "err 応答がない".into())
        }
    })
}

/// `--serve`: wait for buffers, then idle and record on request; start over when the buffers go stale.
fn serve(args: &Args) -> Result<()> {
    let shared = Arc::new(livesvc::Shared::default());
    if !args.fake {
        shared.set_widen_sensitivity(Settings::load(&settings::path()).widen_sensitivity);
    }
    let daemon = Daemon::start(&args.run_dir, (!args.fake).then(|| shared.clone()))?;
    daemon.set(|s| s.prox_min = args.prox_min);
    eprintln!(
        "serving{}: status {}, control {}",
        if args.fake { " (fake)" } else { "" },
        args.run_dir.join(status::STATUS_FILE).display(),
        args.run_dir.join(status::CTL_FILE).display()
    );
    if args.fake {
        fake(args, &daemon);
        daemon.finish();
        return Ok(());
    }
    let mut live = match Live::start(&args.run_dir, Some(&args.out), shared) {
        Ok(l) => l,
        Err(e) => {
            daemon.finish();
            return Err(e);
        }
    };
    eprintln!("live output: {}", args.run_dir.join("live").display());
    let mut prox = Proximity::find();
    let mut why: Option<&str> = None;
    while !STOP.load(Ordering::SeqCst) {
        let message = why.map_or(INSTALL_TOOL.to_string(), |w| format!("{w}。{INSTALL_TOOL}"));
        daemon.set(|s| {
            s.state = "waiting_fds";
            s.message = message;
            s.has_buffers = false;
            s.locked = false;
            s.search = "";
            s.clear_recording();
        });
        let mut buffers = match Buffers::receive(f64::INFINITY, Some(&daemon)) {
            Ok(b) => b,
            Err(e) => {
                if STOP.load(Ordering::SeqCst) {
                    break;
                }
                eprintln!("eyecam-rec: {e}");
                daemon.set(|s| {
                    s.state = "error";
                    s.message = format!("バッファを受け取れなかった: {e}");
                });
                // Wait a little before trying again, still answering control requests.
                for _ in 0..20 {
                    while let Some(req) = daemon.request() {
                        let reply = match req.command {
                            Command::Stop => "err 録画していない",
                            _ => "err まだバッファを受け取ってない",
                        };
                        answer(req, reply);
                    }
                    thread::sleep(Duration::from_millis(100));
                }
                continue;
            }
        };
        live.send(Msg::Buffers(true));
        let outcome = idle(&mut buffers, args, &mut prox, &daemon, &mut live);
        live.send(Msg::Buffers(false));
        match outcome {
            Some(abort) => {
                eprintln!("{}", abort.message());
                live.send(Msg::Stale);
                why = Some(abort.message());
            }
            None => break,
        }
    }
    live.stop();
    daemon.finish();
    Ok(())
}

/// The live engine thread (see eyecam::livesvc) and what the polling loops feed it.
struct Live {
    tx: mpsc::SyncSender<Msg>,
    handle: Option<thread::JoinHandle<()>>,
    shared: Arc<livesvc::Shared>,
    on: bool,
    valve: Option<Shm>,
    valve_retry: f64,
    pitch: [f32; 2],
    pitch_at: f64,
    dropped: u64,
    /// Each camera's frame rate (status.json cam_fps).
    rate: [livesvc::CamRate; 2],
}

impl Live {
    fn start(run_dir: &Path, data_dir: Option<&Path>, shared: Arc<livesvc::Shared>) -> Result<Self> {
        let (tx, handle) = livesvc::spawn(run_dir, livesvc::calib_path(), data_dir, shared.clone())?;
        Ok(Self {
            tx,
            handle: Some(handle),
            shared,
            on: true,
            valve: None,
            valve_retry: 0.0,
            pitch: [f32::NAN; 2],
            pitch_at: f64::NEG_INFINITY,
            dropped: 0,
            rate: Default::default(),
        })
    }

    /// Keep the latest Valve gaze pitch per eye (the eye server's shared memory, read-only).
    fn poll_valve(&mut self, now: f64) {
        if self.valve.is_none() && now >= self.valve_retry {
            self.valve = Shm::open().ok();
            self.valve_retry = now + 5.0;
        }
        if let Some(s) = self.valve.as_mut().and_then(Shm::poll) {
            for (p, g) in self.pitch.iter_mut().zip(&s.gaze) {
                *p = g[1].clamp(-1.0, 1.0).asin().to_degrees();
            }
            self.pitch_at = now;
        }
    }

    /// Hand a new frame (the slot header, then the 512-byte-pitch frame) of camera `eye` (0 = L, 1 = R) to the
    /// engine. Frames are dropped, not queued, if it falls behind.
    fn frame(&mut self, eye: usize, buf: &[u8], now: f64) {
        if !self.on {
            return;
        }
        let (header, frame) = buf.split_at(HEADER_BYTES);
        let t_cam_ns = u64::from_le_bytes(header[..8].try_into().unwrap());
        self.shared.note_camera_frame(eye, &mut self.rate[eye], t_cam_ns as f64 * 1e-9, now);
        let mut data = Vec::with_capacity(WIDTH * HEIGHT);
        for r in 0..HEIGHT {
            data.extend_from_slice(&frame[r * STRIDE..r * STRIDE + WIDTH]);
        }
        let pitch = if now - self.pitch_at < 0.5 { self.pitch[eye] } else { f32::NAN };
        if self.tx.try_send(Msg::Frame { eye, t_cam_ns, pitch, data }).is_err() {
            self.dropped += 1;
        }
    }

    fn send(&self, m: Msg) {
        let _ = self.tx.send(m);
    }

    fn set_on(&mut self, on: bool) {
        self.on = on;
        self.send(Msg::SetLive(on));
    }

    fn wear_calibrated(&self) -> bool {
        self.shared.calib_state.load(Ordering::Relaxed) & 1 != 0
    }

    /// Fit what was collected: the message to show, or why to redo it. The values behind it go into `dir`.
    fn finish(&self, dir: Option<&Path>) -> std::result::Result<String, String> {
        let (tx, rx) = mpsc::channel();
        self.send(Msg::Finish(tx, dir.map(Path::to_path_buf)));
        rx.recv_timeout(Duration::from_secs(10)).unwrap_or_else(|_| Err("校正の計算が終わらなかった".into()))
    }

    fn stop(&mut self) {
        self.send(Msg::Shutdown);
        if let Some(h) = self.handle.take() {
            let _ = h.join();
        }
    }
}

/// Polls a locked ring and hands new frames to the live engine while idle.
struct Pump {
    ring: Ring,
    slots: SlotStates,
    buf: Vec<u8>,
    last_change: f64,
}

impl Pump {
    fn new(arena: &Arena, ring: Ring) -> Self {
        let slots = SlotStates::new(arena, &ring);
        Self { ring, slots, buf: vec![0u8; HEADER_BYTES + FRAME_BYTES], last_change: now_raw() }
    }

    /// One pass over the slots. False once frames have stopped for LOST_AFTER.
    fn poll(&mut self, arena: &Arena, now: f64, swap: bool, live: &mut Live) -> bool {
        for k in 0..self.ring.off.len() {
            match self.slots.poll(arena, &self.ring, k, now, &mut self.buf) {
                SlotEvent::None => {}
                SlotEvent::Changed => self.last_change = now,
                SlotEvent::Frame(_) => live.frame(self.ring.eye[k] as usize ^ swap as usize, &self.buf, now),
            }
        }
        now - self.last_change <= LOST_AFTER
    }
}

/// Wait for `start` / `calib` while holding the buffers, keeping the ring locked and feeding the live engine while
/// live processing is on. None on shutdown, or why the buffers had to be given up.
fn idle(b: &mut Buffers, args: &Args, prox: &mut Proximity, d: &Daemon, live: &mut Live) -> Option<Abort> {
    let mut cached: Option<Ring> = None;
    let mut pump: Option<Pump> = None;
    let mut deferred: Vec<Request> = Vec::new();
    let mut next_check = 0.0;
    let mut next_search = 0.0;
    let waiting = || if live.on { status::search_message("") } else { "待機中" };
    let mut state: (&'static str, String) = ("idle", waiting().into());
    // The buffers are held from here on: say so right away (the first search can take a while).
    d.set(|s| {
        s.state = state.0;
        s.message = state.1.clone();
        s.has_buffers = true;
        s.locked = false;
        s.search = "";
        s.clear_recording();
    });
    loop {
        if STOP.load(Ordering::SeqCst) {
            return None;
        }
        let mut requests: Vec<Request> = std::mem::take(&mut deferred);
        while let Some(req) = d.request() {
            requests.push(req);
        }
        for req in requests {
            let (mode, name, steps) = match &req.command {
                Command::Live(on) => {
                    live.set_on(*on);
                    if !*on {
                        if let Some(p) = pump.take() {
                            cached = Some(p.ring);
                        }
                        live.send(Msg::Stale);
                    }
                    answer(req, "ok");
                    continue;
                }
                Command::Start(name) => match load_protocol(name) {
                    Ok(steps) => (Mode::Record, name.clone(), steps),
                    Err(e) => {
                        answer(req, &format!("err {e}"));
                        continue;
                    }
                },
                Command::Calib(CalibKind::User) if !live.wear_calibrated() => {
                    answer(req, "err 先に calib wear をしてね");
                    continue;
                }
                Command::Calib(kind) => {
                    let collect = if *kind == CalibKind::Wear { CollectKind::Wear } else { CollectKind::User };
                    let name = if *kind == CalibKind::Wear { "calib wear" } else { "calib user" };
                    (Mode::Calib(collect), name.to_string(), calib_steps(*kind))
                }
                Command::Stop | Command::Status => {
                    answer(req, "err 録画していない");
                    continue;
                }
                // Answered by the control thread; never sent here.
                Command::SetWidenSensitivity(_) => {
                    answer(req, "ok");
                    continue;
                }
            };
            let recording = mode == Mode::Record;
            let total = if recording { recording_seconds(args, &Some(steps.clone())) } else { steps.iter().map(|s| s.seconds).sum::<f64>() + 1.5 };
            d.set(|s| {
                s.state = if recording { "searching" } else { "calibrating" };
                s.message = status::search_message("").into();
                s.search = "";
                s.clear_recording();
                s.step_count = steps.len();
                s.total_s = total;
                s.protocol = name.clone();
                s.session_dir.clear();
            });
            answer(req, "ok");
            eprintln!("start: {name}");
            if let Some(p) = pump.take() {
                cached = Some(p.ring);
            }
            let feed = if live.on { Some(&mut *live) } else { None };
            let ended = record(b, &mut cached, args, Some(steps), &name, prox, Some(d), feed, mode).unwrap_or_else(|e| {
                eprintln!("eyecam-rec: {e}");
                Ended { dir: None, abort: None, failure: Some(format!("失敗した: {e}")), aborted: true, calib: None }
            });
            match ended.abort {
                Some(a) if a.needs_grab() => return Some(a),
                Some(Abort::Interrupted) => return None,
                _ => {}
            }
            let saved = ended.dir.as_ref().and_then(|p| p.file_name()).map(|n| n.to_string_lossy().to_string());
            state = match (ended.failure, ended.calib, ended.abort, saved) {
                (Some(failure), _, _, _) => ("error", failure),
                (None, Some(Ok(msg)), _, _) => ("idle", msg),
                (None, Some(Err(why)), _, _) => ("error", why),
                (None, None, Some(a @ (Abort::NotFound | Abort::DiskFull)), saved) => {
                    ("error", saved.map_or(a.message().to_string(), |n| format!("{}（途中まで保存: {n}）", a.message())))
                }
                (None, None, _, _) if !recording => ("idle", "校正を中止した".into()),
                (None, None, _, Some(n)) if ended.aborted => ("idle", format!("保存した（途中で止めた）: {n}")),
                (None, None, _, Some(n)) => ("idle", format!("保存した: {n}")),
                (None, None, _, None) => ("idle", "中止した".into()),
            };
            if live.on
                && let Some(r) = cached.take()
            {
                pump = Some(Pump::new(&b.arenas[r.arena], r));
            }
            next_check = 0.0;
        }

        let now = now_raw();
        live.poll_valve(now);
        if live.on {
            if let Some(p) = pump.as_mut() {
                if !p.poll(&b.arenas[p.ring.arena], now, args.swap, live) {
                    eprintln!("eye frames stopped (idle); searching again");
                    pump = None;
                    live.send(Msg::Stale);
                    next_search = now + 1.0;
                    if state.0 == "idle" {
                        state.1 = status::search_message("").into();
                    }
                    next_check = 0.0;
                }
            } else if now >= next_search {
                if let Some(r) = cached.take().filter(|r| ring_is_live(&b.arenas[r.arena], r)) {
                    pump = Some(Pump::new(&b.arenas[r.arena], r));
                } else {
                    match lock_ring(b, args, prox, Some(d), Ctx::Idle, &mut || true, &mut deferred) {
                        Ok(r) => {
                            pump = Some(Pump::new(&b.arenas[r.arena], r));
                            if state.0 == "idle" && state.1 == status::search_message("") {
                                state.1 = "待機中".into();
                            }
                            next_check = 0.0;
                        }
                        Err(Abort::Interrupted) => return None,
                        Err(a) if a.needs_grab() => return Some(a),
                        Err(Abort::Busy) => {}
                        Err(_) => next_search = now_raw() + 2.0,
                    }
                }
            }
        }

        let now = now_raw();
        if now >= next_check {
            next_check = now + 1.0;
            let reading = prox.read();
            if let Err(a) = b.watch(reading.map(|v| v > args.prox_min)) {
                return Some(a);
            }
            let locked = pump.as_ref().is_some_and(|p| now - p.last_change < 1.0);
            let searching = live.on && pump.is_none();
            d.set(|s| {
                s.state = state.0;
                // Between searches, the waiting message goes with why the last one found nothing.
                let waiting = state.0 == "idle" && state.1 == status::search_message("");
                s.message = if waiting && searching { status::search_message(s.search).into() } else { state.1.clone() };
                s.locked = locked;
                s.prox = reading.unwrap_or(-1.0);
                if !searching {
                    s.search = "";
                }
                s.clear_recording();
            });
        }
        thread::sleep(if pump.is_some() { POLL } else { Duration::from_millis(50) });
    }
}

/// `--fake`: the serve state machine with nothing behind it, for building the panel. waiting_fds for 5 s, then
/// idle; `start` searches for 2 s, then "records" along the protocol's timeline (90 fps, no files, no beeps);
/// `calib wear` / `calib user` run their protocols the same way and then count as calibrated; `stop`, `live` and
/// the replies work as in the real thing. With `--fake-search R` the eye video is never found: idle with live on
/// and every search stay unlocked with `search` R (and its message) until `stop`.
fn fake(args: &Args, d: &Daemon) {
    enum Phase {
        Waiting,
        Idle(String),
        Searching(f64, Vec<Step>, String, Option<CalibKind>),
        Running(f64, Vec<Step>, String, Option<CalibKind>),
    }
    let t0 = now_raw();
    let session = args.out.join("rec_fake").display().to_string();
    let total = |steps: &[Step], calib: Option<CalibKind>| {
        if calib.is_some() { steps.iter().map(|s| s.seconds).sum::<f64>() + 1.5 } else { recording_seconds(args, &Some(steps.to_vec())) }
    };
    let mut calib_state = 0u32;
    let mut live_on = true;
    let mut failed_calib = false;
    let render = |phase: &Phase, now: f64, calib_state: u32, live_on: bool, failed_calib: bool| {
        d.set(|s| {
            let left = (eyecam::live::WARMUP_S - (now - t0 - 5.0)).clamp(0.0, eyecam::live::WARMUP_S);
            let ready = live_on && left == 0.0 || calib_state & 1 != 0;
            // As live: the auto baseline only counts in calib_state once the setup (a calib wear) is done.
            let setup = calib_state & 1 != 0;
            s.calib_state = calib_state | ((ready && setup) as u32) << 2;
            s.baseline = if ready { "ready" } else { "warming" };
            s.warmup_remaining_s = if ready { 0.0 } else { left };
            s.calib_saved = setup;
            s.setup_done = setup;
            s.last_calib_widen = if setup { "measured" } else { "" };
            s.has_buffers = !matches!(phase, Phase::Waiting);
            s.live = live_on && !matches!(phase, Phase::Waiting);
            s.live_ms = if s.live { 1.2 } else { 0.0 };
            // --fake-search: never locked while searching (idle with live on, or after start / calib)
            let searching = matches!(phase, Phase::Searching(..)) || (matches!(phase, Phase::Idle(_)) && live_on);
            let unlocked = args.fake_search.filter(|_| searching);
            let seen = s.live && unlocked.is_none() && !matches!(phase, Phase::Waiting | Phase::Searching(..));
            s.pupil = if seen { [0.97, 0.95] } else { [f64::NAN; 2] };
            s.cam_fps = if seen { [args.fake_camfps; 2] } else { [f64::NAN; 2] };
            s.search = "";
            // What the last look saw: the ring (8 slots), or where it stopped for --fake-search
            if !matches!(phase, Phase::Waiting) && (seen || unlocked.is_some()) {
                s.search_detail = Some(fake_search_detail(unlocked));
            }
            match phase {
                Phase::Waiting => {
                    s.state = "waiting_fds";
                    s.message = format!("（fake）{INSTALL_TOOL}");
                    s.has_buffers = false;
                    s.locked = false;
                    s.prox = -1.0;
                    s.clear_recording();
                }
                Phase::Idle(message) => {
                    s.state = if failed_calib { "error" } else { "idle" };
                    s.message = message.clone();
                    s.locked = live_on;
                    s.prox = 30.0;
                    s.clear_recording();
                    if let Some(reason) = unlocked {
                        s.message = format!("（fake）{}", status::search_message(reason));
                        s.locked = false;
                        s.search = reason;
                        s.prox = if reason == status::SEARCH_NOT_WORN { 9.5 } else { 30.0 };
                    }
                }
                Phase::Searching(_, steps, name, calib) | Phase::Running(_, steps, name, calib) => {
                    s.step_count = steps.len();
                    s.total_s = total(steps, *calib);
                    s.protocol = name.clone();
                    s.prox = 30.0;
                    if let Phase::Running(t, ..) = phase {
                        let elapsed = now - t;
                        let mut at = 0.0;
                        let mut current = (steps.len() as i64, "end".to_string(), 0.0);
                        for (i, step) in steps.iter().enumerate() {
                            if elapsed < at + step.seconds {
                                current = (i as i64, step.label.clone(), at + step.seconds - elapsed);
                                break;
                            }
                            at += step.seconds;
                        }
                        s.state = if calib.is_some() { "calibrating" } else { "recording" };
                        s.message = if calib.is_some() { "（fake）校正中" } else { "（fake）録画中" }.into();
                        s.locked = true;
                        s.fps = [90.0, 90.0];
                        (s.step_index, s.step_label, s.step_remaining_s) = current;
                        s.elapsed_s = elapsed;
                        s.session_dir = if calib.is_some() { String::new() } else { session.clone() };
                    } else {
                        s.state = if calib.is_some() { "calibrating" } else { "searching" };
                        s.message = format!("（fake）{}", status::search_message(unlocked.unwrap_or("")));
                        s.locked = false;
                        s.session_dir.clear();
                        if let Some(reason) = unlocked {
                            s.search = reason;
                            s.prox = if reason == status::SEARCH_NOT_WORN { 9.5 } else { 30.0 };
                        }
                    }
                }
            }
        })
    };
    let mut phase = Phase::Waiting;
    while !STOP.load(Ordering::SeqCst) {
        let now = now_raw();
        while let Some(Request { command, reply }) = d.request() {
            if matches!(command, Command::Start(_) | Command::Calib(_)) {
                failed_calib = false;
            }
            let text;
            (phase, text) = match (phase, command) {
                (p, Command::Live(on)) => {
                    live_on = on;
                    (p, "ok".to_string())
                }
                (Phase::Waiting, Command::Start(_) | Command::Calib(_)) => (Phase::Waiting, "err まだバッファを受け取ってない".into()),
                (Phase::Waiting, _) => (Phase::Waiting, "err 録画していない".into()),
                (Phase::Idle(message), Command::Start(name)) => match load_protocol(&name) {
                    Ok(steps) => (Phase::Searching(now, steps, name, None), "ok".into()),
                    Err(e) => (Phase::Idle(message), format!("err {e}")),
                },
                (Phase::Idle(message), Command::Calib(CalibKind::User)) if calib_state & 1 == 0 => {
                    (Phase::Idle(message), "err 先に calib wear をしてね".into())
                }
                (Phase::Idle(_), Command::Calib(kind)) => {
                    let name = if kind == CalibKind::Wear { "calib wear" } else { "calib user" };
                    (Phase::Searching(now, calib_steps(kind), name.into(), Some(kind)), "ok".into())
                }
                (Phase::Idle(message), _) => (Phase::Idle(message), "err 録画していない".into()),
                (Phase::Searching(_, _, _, calib), Command::Stop) => {
                    let msg = if calib.is_some() { "（fake）校正を中止した" } else { "（fake）中止した" };
                    (Phase::Idle(msg.into()), "ok".into())
                }
                (Phase::Running(_, _, _, Some(_)), Command::Stop) => (Phase::Idle("（fake）校正を中止した".into()), "ok".into()),
                (Phase::Running(_, _, _, None), Command::Stop) => {
                    d.set(|s| s.last_session_aborted = true);
                    (Phase::Idle("（fake）保存した（途中で止めた）: rec_fake".into()), "ok".into())
                }
                (p @ (Phase::Searching(_, _, _, Some(_)) | Phase::Running(_, _, _, Some(_))), _) => (p, "err 校正中".into()),
                (p, _) => (p, "err 録画中".into()),
            };
            // The state changes before the reply goes out, so status.json already shows it.
            render(&phase, now, calib_state, live_on, failed_calib);
            let _ = reply.send(text);
        }
        phase = match phase {
            Phase::Waiting if now - t0 >= 5.0 => Phase::Idle("（fake）待機中".into()),
            Phase::Searching(t, steps, name, calib) if now - t >= 2.0 && args.fake_search.is_none() => {
                Phase::Running(now, steps, name, calib)
            }
            Phase::Running(t, steps, _, calib) if now - t >= total(&steps, calib) => match calib {
                Some(CalibKind::Wear) => {
                    let result = fake_last_calib(args.fake_calib, args.fake_camfps);
                    let message = format!("（fake）{}", result.message);
                    let failed = result.failed_eye.clone();
                    d.set(|s| {
                        s.calib_failed_eye = ["", "L", "R", "LR"].into_iter().find(|e| *e == failed).unwrap_or("");
                        s.last_calib = Some(result);
                    });
                    // (failed: `error` until the next command, as the real one)
                    failed_calib = failed == "LR";
                    if !failed_calib {
                        calib_state |= 1;
                    }
                    Phase::Idle(message)
                }
                Some(CalibKind::User) => {
                    calib_state |= 2;
                    Phase::Idle("（fake）校正できた（ユーザー）".into())
                }
                None => {
                    d.set(|s| s.last_session_aborted = false);
                    Phase::Idle("（fake）保存した: rec_fake".into())
                }
            },
            p => p,
        };
        render(&phase, now, calib_state, live_on, failed_calib);
        thread::sleep(Duration::from_millis(50));
    }
}

/// The fake's last look: the whole ring with both eyes, or where it stops for a --fake-search reason.
fn fake_search_detail(reason: Option<&str>) -> status::SearchDetail {
    let (look, changed_blocks) = match reason {
        Some(status::SEARCH_ONE_EYE) => {
            (ring::Look { candidates: 4, refresh_hz: 90.0, slots: 4, both_eyes: false, stopped_at: ring::STOP_ONE_EYE }, 64)
        }
        Some(status::SEARCH_NO_VIDEO) => (ring::Look { stopped_at: ring::STOP_NO_CANDIDATES, ..ring::Look::default() }, 3),
        Some(_) => (ring::Look { stopped_at: ring::STOP_NO_CANDIDATES, ..ring::Look::default() }, 0),
        None => (ring::Look { candidates: 8, refresh_hz: 90.0, slots: 8, both_eyes: true, stopped_at: "" }, 128),
    };
    status::SearchDetail { look, changed_blocks, unix: unix_now() }
}

/// The fake `calib wear`'s result: both eyes, without one (--fake-calib L / R), or failed (LR), with made-up numbers.
fn fake_last_calib(failed: Option<&str>, fps: f64) -> status::LastCalib {
    use eyecam::live::{MSG_PUPIL, NEED_NORMAL, rate_note};
    let failed = failed.unwrap_or("");
    let lost = |e: usize| failed.contains(["L", "R"][e]);
    // The 5.4 s of normal steps at this rate (486 frames at 90 fps), and the counts scaled from those at 90.
    let normal = (5.4 * fps).round();
    let need = NEED_NORMAL.frames(normal as usize, fps);
    let scaled = |n: f64| (n * fps / 90.0).round().max(1.0);
    let lost_r = if failed == "LR" { 30.0 } else { 12.0 };
    let frames = [if lost(0) { scaled(12.0) } else { scaled(470.0) }, if lost(1) { scaled(lost_r) } else { scaled(482.0) }];
    let message = match failed {
        "L" | "R" => {
            let eye = if failed == "L" { "左" } else { "右" };
            format!("校正できた（{eye}目は瞳がうまく見えなかったので、前の値を使うよ）[{}/{normal}、{need} 必要]", scaled(12.0))
        }
        "LR" => {
            let both = format!("両目{MSG_PUPIL}[左 {}/{normal}・右 {}/{normal}、{need} 必要]", frames[0], frames[1]);
            rate_note(fps).map_or(both.clone(), |note| format!("{both}。{note}"))
        }
        _ => "校正できた（かぶり）".to_string(),
    };
    status::LastCalib {
        time: local_stamp().split_once('_').map(|(day, time)| format!("{day} {}", time.replace('-', ":"))).unwrap_or_default(),
        ok: failed != "LR",
        failed_eye: failed.into(),
        message,
        pupil_frames: frames,
        normal_frames: [normal; 2],
        pupil_x: [if lost(0) { f64::NAN } else { 238.0 }, if lost(1) { f64::NAN } else { 252.0 }],
        pupil_y: [if lost(0) { f64::NAN } else { 201.0 }, if lost(1) { f64::NAN } else { 194.0 }],
        window: [[186.0, 346.0], [180.0, 340.0]],
        fps: [fps; 2],
    }
}

/// The steps of ~/eyecam-src/protocol_<name>.txt (`name` is already checked to be a plain word).
/// The steps of protocol_<name>.txt next to the eyecam-rec binary (an installed copy), else in ~/eyecam-src.
fn load_protocol(name: &str) -> std::result::Result<Vec<Step>, String> {
    let file = format!("protocol_{name}.txt");
    let beside = std::env::current_exe().ok().and_then(|e| e.parent().map(|d| d.join(&file)));
    let path = beside.filter(|p| p.is_file()).unwrap_or_else(|| home_dir().join("eyecam-src").join(&file));
    let text = fs::read_to_string(&path).map_err(|_| format!("プロトコルが見つからない: protocol_{name}.txt"))?;
    cues::parse(&text).map_err(|e| format!("protocol_{name}.txt の書き方がおかしい: {e}"))
}

// ------------------------------------------------------------------------------------------- receiving buffers

/// A listening socket whose file is removed again when dropped.
struct SocketFile(UnixListener, PathBuf);

impl Drop for SocketFile {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.1);
    }
}

/// Listen on `path` with mode 0600, replacing a stale socket of ours but never a live one or anything else.
fn bind_private(path: &Path, busy: &str) -> Result<SocketFile> {
    if let Ok(meta) = fs::symlink_metadata(path) {
        if !meta.file_type().is_socket() || meta.uid() != proto::USER_UID {
            return Err(format!("{} exists and is not our socket; not touching it", path.display()).into());
        }
        if UnixStream::connect(path).is_ok() {
            return Err(format!("{busy} ({})", path.display()).into());
        }
        fs::remove_file(path)?;
    }
    let old_mask = unsafe { libc::umask(0o177) };
    let bound = UnixListener::bind(path);
    unsafe { libc::umask(old_mask) };
    let socket = SocketFile(bound.map_err(|e| format!("bind {}: {e}", path.display()))?, path.to_path_buf());
    fs::set_permissions(path, fs::Permissions::from_mode(0o600))?;
    Ok(socket)
}

/// Create the socket, ask the user to run eyecam-grab, and take the descriptors it sends. In serve mode, control
/// requests are refused meanwhile (there is nothing to record from yet).
fn receive_buffers(wait: f64, ctl: Option<&Daemon>) -> Result<(proto::Header, Vec<OwnedFd>)> {
    let socket = bind_private(Path::new(proto::SOCKET_PATH), "another eyecam-rec is already waiting")?;
    socket.0.set_nonblocking(true)?;

    let grab = std::env::current_exe()
        .ok()
        .and_then(|exe| exe.parent().map(|d| d.join("eyecam-grab")))
        .and_then(|p| p.canonicalize().ok())
        .map_or_else(|| "eyecam-grab".to_string(), |p| p.display().to_string());
    eprintln!("\nwaiting for the eye camera buffers. In another terminal, run:\n\n    sudo {grab}\n");

    // Serve mode: start the installed eyecam-grab (cap_sys_ptrace) ourselves when it is there and safe. While it is
    // not, look again every few seconds: the panel's first-time setup installs it while this process is waiting.
    let mut auto = ctl.map(|_| autograb::find());
    let mut next_check = now_raw() + 5.0;
    let set_auto = |label: String, message: Option<String>| {
        if let Some(d) = ctl {
            d.set(|s| {
                s.auto_grab = label;
                if let Some(m) = message {
                    s.message = m;
                }
            });
        }
    };
    match &auto {
        Some(autograb::State::Ready(p)) => {
            eprintln!("automatic grab: {} (cap_sys_ptrace)", p.display());
            set_auto("waiting_tracker".into(), Some("自動でバッファを取りに行くよ（アイトラッキングが始まるのを待ってる）".into()));
        }
        Some(other) => {
            eprintln!("automatic grab not available: {}", other.label());
            set_auto(other.label(), None);
        }
        None => {}
    }
    let mut child: Option<(std::process::Child, f64)> = None;
    let mut next_try = 0.0;
    let mut by_child = false;

    let deadline = now_raw() + wait;
    let stream = loop {
        match socket.0.accept() {
            // Only eyecam-grab may hand over buffers: as root (sudo), or the capability copy this process just
            // started (identified by its pid). Anything else, such as another eyecam-rec checking whether this one
            // is running, is dropped and the wait goes on.
            Ok((stream, _)) => match peer_cred(&stream) {
                Ok(cred) if cred.uid == 0 => break stream,
                Ok(cred) if child.as_ref().is_some_and(|(c, _)| c.id() as i32 == cred.pid) && cred.uid == proto::USER_UID => {
                    by_child = true;
                    break stream;
                }
                Ok(cred) => eprintln!("ignored a connection from uid {} pid {} (expected eyecam-grab)", cred.uid, cred.pid),
                Err(e) => eprintln!("ignored a connection: {e}"),
            },
            Err(e) if e.kind() == ErrorKind::WouldBlock || e.kind() == ErrorKind::Interrupted => {}
            Err(e) => return Err(format!("accept: {e}").into()),
        }
        if STOP.load(Ordering::SeqCst) {
            return Err("interrupted while waiting for eyecam-grab".into());
        }
        if now_raw() > deadline {
            return Err(format!("eyecam-grab did not connect within {wait:.0} s").into());
        }
        while let Some(req) = ctl.and_then(Daemon::request) {
            let reply = match req.command {
                Command::Start(_) | Command::Calib(_) => "err まだバッファを受け取ってない",
                Command::Live(_) => "err まだバッファを受け取ってない",
                _ => "err 録画していない",
            };
            answer(req, reply);
        }
        if auto.as_ref().is_some_and(|a| !matches!(a, autograb::State::Ready(_))) && now_raw() >= next_check {
            next_check = now_raw() + 5.0;
            let found = autograb::find();
            if auto.as_ref() != Some(&found) {
                match &found {
                    autograb::State::Ready(p) => {
                        eprintln!("automatic grab: {} (cap_sys_ptrace), installed while waiting", p.display());
                        set_auto(
                            "waiting_tracker".into(),
                            Some("自動でバッファを取りに行くよ（アイトラッキングが始まるのを待ってる）".into()),
                        );
                        next_try = 0.0;
                    }
                    other => set_auto(other.label(), None),
                }
                auto = Some(found);
            }
        }
        if let Some(autograb::State::Ready(path)) = &auto {
            let now = now_raw();
            if let Some((c, started)) = child.as_mut() {
                match c.try_wait() {
                    Ok(Some(status)) => {
                        let mut err = String::new();
                        if let Some(mut e) = c.stderr.take() {
                            let _ = e.read_to_string(&mut err);
                        }
                        let last = err.lines().rev().find(|l| !l.trim().is_empty()).unwrap_or("").to_string();
                        eprintln!("automatic grab exited ({status}): {last}");
                        let why = last.trim_start_matches("eyecam-grab: ").to_string();
                        set_auto(
                            format!("failed: {why}"),
                            Some(format!("自動でバッファを取れなかった: {why}")),
                        );
                        child = None;
                        next_try = now + 5.0;
                    }
                    Ok(None) if now - *started > 10.0 => {
                        let _ = c.kill();
                        let _ = c.wait();
                        set_auto("failed: timeout".into(), None);
                        child = None;
                        next_try = now + 5.0;
                    }
                    _ => {}
                }
            } else if now >= next_try {
                next_try = now + 5.0;
                if autograb::eyetracking_running() {
                    match std::process::Command::new(path)
                        .env_clear()
                        .stdin(std::process::Stdio::null())
                        .stdout(std::process::Stdio::null())
                        .stderr(std::process::Stdio::piped())
                        .spawn()
                    {
                        Ok(c) => {
                            child = Some((c, now));
                            set_auto("trying".into(), Some("自動でバッファを取りに行ってる…".into()));
                        }
                        Err(e) => set_auto(format!("failed: {e}"), None),
                    }
                } else {
                    set_auto("waiting_tracker".into(), Some("自動でバッファを取りに行くよ（アイトラッキングが始まるのを待ってる）".into()));
                }
            }
        }
        thread::sleep(Duration::from_millis(100));
    };
    if let Some((mut c, _)) = child {
        let _ = c.wait();
    }
    if by_child {
        eprintln!("buffers received from the automatic grab");
        set_auto("ok".into(), None);
    }
    stream.set_nonblocking(false)?;
    stream.set_read_timeout(Some(Duration::from_secs(5)))?;
    let (bytes, fds) = recv_fds(&stream)?;
    drop(socket);
    let header = proto::parse_header(&bytes)?;
    if fds.len() != header.count {
        return Err(format!("header says {} buffers, got {} descriptors", header.count, fds.len()).into());
    }
    for (fd, &size) in fds.iter().zip(&header.sizes) {
        let info = fs::read_to_string(format!("/proc/self/fdinfo/{}", fd.as_raw_fd()))?;
        let field = |key: &str| {
            info.lines()
                .find_map(|l| l.split_once(':').filter(|(k, _)| k.trim() == key).map(|(_, v)| v.trim().to_string()))
        };
        if field("exp_name").as_deref() != Some("udmabuf") || field("size") != Some(size.to_string()) {
            return Err(format!("descriptor {} is not a {size}-byte udmabuf", fd.as_raw_fd()).into());
        }
    }
    Ok((header, fds))
}

/// The received buffers, mapped read-only, and what is known about whether they are still live.
struct Buffers {
    header: proto::Header,
    arenas: Vec<Arena>,
    _maps: Vec<Mapping>,
    tracker: Tracker,
    last_activity: f64,
    worn_since: Option<f64>,
    valve: ValveWatch,
    /// Valve's eye tracker delivered a new tracked sample at the last `watch` that asked it (only asked while the
    /// proximity sensor doesn't say worn).
    valve_tracking: bool,
}

impl Buffers {
    fn receive(wait: f64, ctl: Option<&Daemon>) -> Result<Self> {
        let (header, fds) = receive_buffers(wait, ctl)?;
        let mut maps = Vec::new();
        for (fd, &size) in fds.iter().zip(&header.sizes) {
            maps.push(Mapping::new(fd, size as usize)?);
        }
        drop(fds);
        eprintln!(
            "mapped {} buffer(s) read-only: {:?} bytes; eye tracker pid {}",
            maps.len(),
            header.sizes,
            header.pid
        );
        let arenas = maps.iter().map(|m| unsafe { Arena::new(m.ptr, m.len) }).collect();
        let tracker = Tracker::open(header.pid);
        let valve = ValveWatch::default();
        Ok(Self {
            header,
            arenas,
            _maps: maps,
            tracker,
            last_activity: now_raw(),
            worn_since: None,
            valve,
            valve_tracking: false,
        })
    }

    /// Note which blocks changed (whether or not the headset is worn: the search looks either way), and fail if the
    /// tracker exited or the buffers went stale: worn for STALE_AFTER without any change. Worn means the proximity
    /// reading `prox_worn` says so, or Valve's eye tracker delivered a new tracked sample since the last call (its
    /// cameras are running, so live buffers would be changing). Neither on a desk, so stale never starts a re-grab
    /// there; an unknown proximity without Valve samples never counts as stale. Called about once a second.
    fn watch(&mut self, prox_worn: Option<bool>) -> std::result::Result<(), Abort> {
        if self.tracker.exited() {
            return Err(Abort::TrackerExited);
        }
        let now = now_raw();
        let mut any = false;
        for arena in &mut self.arenas {
            any |= arena.note_block_changes();
        }
        if any {
            self.last_activity = now;
        }
        self.valve_tracking = prox_worn != Some(true) && self.valve.tracking(now);
        if prox_worn == Some(true) || self.valve_tracking {
            self.worn_since.get_or_insert(now);
        } else {
            self.worn_since = None;
        }
        match self.worn_since {
            Some(since) if now - since > STALE_AFTER && now - self.last_activity > STALE_AFTER => Err(Abort::Stale),
            _ => Ok(()),
        }
    }
}

fn peer_cred(stream: &UnixStream) -> Result<libc::ucred> {
    let mut cred = libc::ucred { pid: 0, uid: u32::MAX, gid: u32::MAX };
    let mut len = size_of::<libc::ucred>() as libc::socklen_t;
    let ret = unsafe {
        libc::getsockopt(stream.as_raw_fd(), libc::SOL_SOCKET, libc::SO_PEERCRED, (&raw mut cred).cast(), &mut len)
    };
    if ret != 0 {
        return Err(format!("SO_PEERCRED: {}", std::io::Error::last_os_error()).into());
    }
    Ok(cred)
}

/// One message: the header bytes and the SCM_RIGHTS descriptors attached to it.
fn recv_fds(stream: &UnixStream) -> Result<(Vec<u8>, Vec<OwnedFd>)> {
    let mut data = vec![0u8; proto::HEADER_LEN + 1];
    let space = unsafe { libc::CMSG_SPACE((proto::MAX_BUFFERS * size_of::<RawFd>()) as u32) } as usize;
    let mut control = vec![0u8; space];
    let mut iov = libc::iovec { iov_base: data.as_mut_ptr().cast(), iov_len: data.len() };
    let mut msg: libc::msghdr = unsafe { std::mem::zeroed() };
    msg.msg_iov = &mut iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.as_mut_ptr().cast();
    msg.msg_controllen = control.len() as _;
    let n = unsafe { libc::recvmsg(stream.as_raw_fd(), &mut msg, libc::MSG_CMSG_CLOEXEC) };
    if n < 0 {
        return Err(format!("recvmsg: {}", std::io::Error::last_os_error()).into());
    }
    let mut fds = Vec::new();
    unsafe {
        let mut cmsg = libc::CMSG_FIRSTHDR(&msg);
        while !cmsg.is_null() {
            if (*cmsg).cmsg_level == libc::SOL_SOCKET && (*cmsg).cmsg_type == libc::SCM_RIGHTS {
                let count = ((*cmsg).cmsg_len as usize - libc::CMSG_LEN(0) as usize) / size_of::<RawFd>();
                let data = libc::CMSG_DATA(cmsg).cast::<RawFd>();
                for i in 0..count {
                    fds.push(OwnedFd::from_raw_fd(data.add(i).read_unaligned()));
                }
            }
            cmsg = libc::CMSG_NXTHDR(&msg, cmsg);
        }
    }
    if msg.msg_flags & (libc::MSG_CTRUNC | libc::MSG_TRUNC) != 0 {
        return Err("message from eyecam-grab was truncated".into());
    }
    data.truncate(n as usize);
    Ok((data, fds))
}

/// A read-only shared mapping of a buffer.
struct Mapping {
    ptr: *const u8,
    len: usize,
}

impl Mapping {
    fn new(fd: &OwnedFd, len: usize) -> Result<Self> {
        let p = unsafe {
            libc::mmap(std::ptr::null_mut(), len, libc::PROT_READ, libc::MAP_SHARED, fd.as_raw_fd(), 0)
        };
        if p == libc::MAP_FAILED {
            return Err(format!("mmap: {}", std::io::Error::last_os_error()).into());
        }
        Ok(Self { ptr: p as *const u8, len })
    }
}

impl Drop for Mapping {
    fn drop(&mut self) {
        unsafe { libc::munmap(self.ptr as *mut libc::c_void, self.len) };
    }
}

/// Where ValveWatch reads Valve's samples: its shared memory (`Shm`), or a stand-in in tests.
trait SampleSource {
    /// The latest sample, if there is a new one since the last call.
    fn poll(&mut self) -> Option<Sample>;
}

impl SampleSource for Shm {
    fn poll(&mut self) -> Option<Sample> {
        Shm::poll(self)
    }
}

/// Whether Valve's eye tracker is tracking: a new sample with producer_state 1 in its shared memory (read-only, as
/// Shm reads it) since the last look. The eye server publishes only while a client (frameeyeosc) asks for samples
/// and only while its cameras run, so this is false on a desk, and also whenever frameeyeosc is not running.
struct ValveWatch<S = Shm> {
    shm: Option<S>,
    /// Opens the source (`Shm::open`; tests count the attempts).
    open: Box<dyn FnMut() -> Option<S>>,
    retry: f64,
    last_new: f64,
}

impl Default for ValveWatch<Shm> {
    fn default() -> Self {
        Self::new(Box::new(|| Shm::open().ok()))
    }
}

impl<S: SampleSource> ValveWatch<S> {
    fn new(open: Box<dyn FnMut() -> Option<S>>) -> Self {
        Self { shm: None, open, retry: 0.0, last_new: 0.0 }
    }

    fn tracking(&mut self, now: f64) -> bool {
        // Reopen now and then while nothing new comes (the eye server may have replaced the file).
        if self.shm.is_some() && now - self.last_new > 30.0 {
            self.shm = None;
        }
        if self.shm.is_none() {
            if now < self.retry {
                return false;
            }
            self.retry = now + 5.0;
            self.last_new = now;
            self.shm = (self.open)();
            // The sample already there when it is opened can be old: only later ones count.
            self.shm.as_mut().and_then(S::poll);
            return false;
        }
        match self.shm.as_mut().and_then(S::poll) {
            Some(sample) => {
                self.last_new = now;
                sample.producer_state == 1
            }
            None => false,
        }
    }
}

/// Notices the eye tracker exiting (its buffers then stop being filled).
struct Tracker(Option<OwnedFd>);

impl Tracker {
    fn open(pid: i32) -> Self {
        let ret = unsafe { libc::syscall(libc::SYS_pidfd_open, pid, 0) };
        Self((ret >= 0).then(|| unsafe { OwnedFd::from_raw_fd(ret as RawFd) }))
    }

    fn exited(&self) -> bool {
        self.0.as_ref().is_some_and(|fd| {
            let mut pfd = libc::pollfd { fd: fd.as_raw_fd(), events: libc::POLLIN, revents: 0 };
            unsafe { libc::poll(&mut pfd, 1, 0) > 0 }
        })
    }
}

// ----------------------------------------------------------------------------------------------- finding frames

/// The proximity sensor between the lenses (read-only sysfs).
struct Proximity(Option<PathBuf>);

impl Proximity {
    fn find() -> Self {
        let found = (0..8).map(|i| PathBuf::from(format!("/sys/bus/iio/devices/iio:device{i}"))).find(|d| {
            fs::read_to_string(d.join("name")).is_ok_and(|n| n.contains("vcnl"))
        });
        Self(found.map(|d| d.join("in_proximity_raw")))
    }

    fn read(&self) -> Option<f64> {
        fs::read_to_string(self.0.as_ref()?).ok()?.trim().parse().ok()
    }
}

/// Where a search runs, which decides what it shows and how it answers control requests.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Ctx {
    /// Before a recording (state `searching`) or a calibration (state `calibrating`).
    Search(&'static str),
    /// Frames stopped during a recording or calibration, which carries on in this state with `locked` false.
    Relock(&'static str),
    /// Idle with live processing on: the idle loop owns the state, and any control request ends the search (it is
    /// left in `deferred` for the idle loop).
    Idle,
}

impl Ctx {
    fn busy_reply(self) -> &'static str {
        match self {
            Ctx::Search("calibrating") | Ctx::Relock("calibrating") => "err 校正中",
            _ => "err 録画中",
        }
    }

    fn stop_message(self) -> &'static str {
        match self {
            Ctx::Search("calibrating") | Ctx::Relock("calibrating") => "校正を中止した",
            Ctx::Relock(_) => "止めた（途中まで保存）",
            _ => "中止した",
        }
    }
}

/// Discovery's clock. Discovery takes about 1.6 s, so it also answers control requests between its polls (the
/// panel gives up on a reply after 2 s): `stop` ends the search, other requests are refused, or deferred when idle.
struct RealClock<'a> {
    ctl: Option<&'a Daemon>,
    ctx: Ctx,
    deferred: &'a mut Vec<Request>,
    stop_requested: bool,
}

impl Clock for RealClock<'_> {
    fn now(&mut self) -> f64 {
        now_raw()
    }

    fn wait(&mut self) {
        std::thread::sleep(Duration::from_millis(4));
        while let Some(req) = self.ctl.and_then(Daemon::request) {
            match (self.ctx, &req.command) {
                (Ctx::Idle, _) => {
                    self.deferred.push(req);
                    self.stop_requested = true;
                }
                (_, Command::Stop) => {
                    accept_stop(self.ctl, req, self.ctx.stop_message());
                    self.stop_requested = true;
                }
                _ => answer(req, self.ctx.busy_reply()),
            }
        }
    }

    fn stopped(&self) -> bool {
        STOP.load(Ordering::SeqCst) || self.stop_requested
    }
}

/// Search until a ring is found. `tick` is called about 10 times a second (a recording or calibration keeps its
/// cues and status going with it) and returns false once its time is up.
#[allow(clippy::too_many_arguments)]
fn lock_ring(
    b: &mut Buffers,
    args: &Args,
    prox: &mut Proximity,
    ctl: Option<&Daemon>,
    ctx: Ctx,
    tick: &mut dyn FnMut() -> bool,
    deferred: &mut Vec<Request>,
) -> std::result::Result<Ring, Abort> {
    b.arenas.iter_mut().for_each(Arena::reset_block_changes);
    let start = now_raw();
    let mut last_scan = start;
    let mut last_status = start - 60.0;
    let status_every = if ctx == Ctx::Idle { 60.0 } else { 5.0 };
    // The headset counts as worn once the proximity reading has stayed above --prox-min for WORN_FOR seconds.
    // That only decides how often to look and what to say: the search looks whether or not it is worn.
    let mut above_since: Option<f64> = None;
    // Whether the last look found only one camera's video (otherwise none), and whether there was one yet.
    let mut one_eye = false;
    let mut scanned = false;
    let mut changed_blocks = 0usize;
    loop {
        if STOP.load(Ordering::SeqCst) {
            return Err(Abort::Interrupted);
        }
        if now_raw() - start >= args.wait_lock {
            return Err(Abort::NotFound);
        }
        if !tick() {
            return Err(Abort::TimeUp);
        }
        if b.tracker.exited() {
            return Err(Abort::TrackerExited);
        }
        while let Some(req) = ctl.and_then(Daemon::request) {
            match (ctx, &req.command) {
                (Ctx::Idle, _) => deferred.push(req),
                (_, Command::Stop) => {
                    accept_stop(ctl, req, ctx.stop_message());
                    return Err(Abort::Stopped);
                }
                _ => answer(req, ctx.busy_reply()),
            }
        }
        if ctx == Ctx::Idle && !deferred.is_empty() {
            return Err(Abort::Busy);
        }
        let reading = prox.read();
        let now = now_raw();
        match reading {
            Some(v) if v > args.prox_min => {
                above_since.get_or_insert(now);
            }
            _ => above_since = None,
        }
        let worn = reading.is_none() || above_since.is_some_and(|t| now - t >= WORN_FOR);
        let search = search_reason(scanned, one_eye, worn, b.valve_tracking);
        if let Some(d) = ctl {
            let message = status::search_message(search).to_string();
            match ctx {
                Ctx::Idle => d.set(|s| {
                    // Keep an error from the last run on screen; otherwise say what is missing.
                    if s.state != "error" {
                        s.state = "idle";
                        s.message = message;
                    }
                    s.has_buffers = true;
                    s.locked = false;
                    s.prox = reading.unwrap_or(-1.0);
                    s.search = search;
                }),
                Ctx::Search(state) | Ctx::Relock(state) => d.set(|s| {
                    s.state = state;
                    s.message = message;
                    s.locked = false;
                    s.prox = reading.unwrap_or(-1.0);
                    s.search = search;
                }),
            }
        }
        if now - last_scan > if worn { SCAN_EVERY } else { SCAN_EVERY_NOT_WORN } {
            b.watch(reading.map(|v| v > args.prox_min))?;
            changed_blocks = b.arenas.iter().map(|a| a.block_changed.iter().filter(|&&c| c).count()).sum();
            let mut log = |line: String| eprintln!("  {line}");
            let mut clock = RealClock { ctl, ctx, deferred: &mut *deferred, stop_requested: false };
            let mut look = ring::Look::default();
            let found = ring::discover(&b.arenas, &mut clock, &mut log, &mut look);
            if clock.stop_requested {
                return Err(if ctx == Ctx::Idle { Abort::Busy } else { Abort::Stopped });
            }
            last_scan = now_raw();
            scanned = true;
            // What this look saw, for the panel's diagnostics (kept after a lock)
            if let Some(d) = ctl {
                d.set(|s| s.search_detail = Some(status::SearchDetail { look, changed_blocks, unix: unix_now() }));
            }
            match found {
                Some(r) if r.both_eyes || args.allow_one_eye => {
                    eprintln!(
                        "locked: {} slots at 0x{:x}, spacing {}, cameras {:?}{}; {}",
                        r.off.len(),
                        r.off[0],
                        r.pitch,
                        r.eye,
                        if r.both_eyes { "" } else { " (one eye only)" },
                        prox_at_look(reading, args.prox_min, above_since.map(|t| now - t))
                    );
                    return Ok(r);
                }
                Some(_) => one_eye = true,
                None => one_eye = false,
            }
            b.arenas.iter_mut().for_each(Arena::clear_block_changes);
        }
        if now_raw() - last_status >= status_every {
            last_status = now_raw();
            let p = reading.map_or("unknown".into(), |v| format!("{v:.1}"));
            eprintln!(
                "waiting for eye frames: {}. proximity {p} (worn above {} for {WORN_FOR} s){}",
                if search.is_empty() { "searching" } else { search },
                args.prox_min,
                if scanned { format!(", {changed_blocks} changed block(s) in the last look") } else { String::new() }
            );
        }
        // Idle searches run for as long as live processing is on: keep them cheap.
        thread::sleep(Duration::from_millis(if ctx == Ctx::Idle { 300 } else { 100 }));
    }
}

/// Why a search hasn't found the eye video (status.json `search`), from what it knows: whether it has looked yet,
/// whether the last look found only one camera's video, whether the proximity sensor says worn (`worn`: above
/// --prox-min for WORN_FOR, or unknown), and whether Valve's eye tracker delivered fresh samples at the last look.
/// "" before the first look. Not worn needs both the sensor and Valve's tracker to say so: the sensor reads low
/// on some faces (0-3 while worn on the developer's headset), and Valve's tracker only delivers while its cameras
/// see an eye, so then the headset is on and the video is what is missing.
fn search_reason(scanned: bool, one_eye: bool, worn: bool, valve_tracking: bool) -> &'static str {
    match (scanned, one_eye, worn || valve_tracking) {
        (false, _, _) => "",
        (true, true, _) => status::SEARCH_ONE_EYE,
        (true, false, true) => status::SEARCH_NO_VIDEO,
        (true, false, false) => status::SEARCH_NOT_WORN,
    }
}

/// The proximity reading at the start of a look, for the lock's log line: the reading, and whether it had been
/// above --prox-min for WORN_FOR yet (`above_for`: how long it had been above, None when it wasn't).
fn prox_at_look(reading: Option<f64>, prox_min: f64, above_for: Option<f64>) -> String {
    match (reading, above_for) {
        (None, _) => "proximity unknown".into(),
        (Some(v), Some(d)) if d >= WORN_FOR => format!("proximity {v:.1}, above {prox_min} for {WORN_FOR} s: worn"),
        (Some(v), Some(d)) => {
            format!("proximity {v:.1}, above {prox_min} for only {d:.1} s (worn after {WORN_FOR} s)")
        }
        (Some(v), None) => format!("proximity {v:.1}, not above {prox_min}: the sensor says not worn"),
    }
}

/// Whether a ring's slots are still being refilled (any change within 0.3 s).
fn ring_is_live(arena: &Arena, ring: &Ring) -> bool {
    let before: Vec<u64> = ring.off.iter().map(|&o| arena.fingerprint(o, FRAME_BYTES)).collect();
    let t = now_raw();
    while now_raw() - t < 0.3 {
        thread::sleep(Duration::from_millis(4));
        if ring.off.iter().zip(&before).any(|(&o, &f)| arena.fingerprint(o, FRAME_BYTES) != f) {
            return true;
        }
    }
    false
}

/// How a recording or calibration ended.
struct Ended {
    /// The session directory, if one was written.
    dir: Option<PathBuf>,
    abort: Option<Abort>,
    /// An I/O error that ended it (serve mode reports it rather than exiting).
    failure: Option<String>,
    /// It did not run its full time (meta.txt says aborted=1).
    aborted: bool,
    /// A calibration's outcome: the message to show, or why it has to be redone.
    calib: Option<std::result::Result<String, String>>,
}

impl Ended {
    fn early(abort: Abort) -> Self {
        Ended { dir: None, abort: Some(abort), failure: None, aborted: true, calib: None }
    }
}

/// What a run is: a recording into a session directory, or a calibration (no files).
#[derive(Clone, Copy, PartialEq, Eq)]
enum Mode {
    Record,
    Calib(CollectKind),
}

/// The built-in calibration protocols (each starts with a 3 s lead-in for the panel's countdown).
fn calib_steps(kind: CalibKind) -> Vec<Step> {
    let s = |seconds: f64, label: &str| Step { seconds, label: label.into() };
    match kind {
        CalibKind::Wear => {
            vec![s(3.0, "lead_in"), s(2.0, "close"), s(5.0, "normal"), s(3.0, "widen"), s(2.0, "normal"), s(3.0, "widen")]
        }
        CalibKind::User => vec![s(3.0, "lead_in"), s(5.0, "squint"), s(5.0, "look_up"), s(5.0, "look_down")],
    }
}

/// One recording or calibration: find (or reuse) the ring, then follow the protocol (beeps), feeding frames to the
/// live engine and, for a recording, writing frames, Valve samples and cues into a new session directory, until
/// the time is up, `stop`, Ctrl-C, or the frames are gone for good.
#[allow(clippy::too_many_arguments)]
fn record(
    b: &mut Buffers,
    cached: &mut Option<Ring>,
    args: &Args,
    steps: Option<Vec<Step>>,
    protocol: &str,
    prox: &mut Proximity,
    ctl: Option<&Daemon>,
    mut live: Option<&mut Live>,
    mode: Mode,
) -> Result<Ended> {
    let recording = mode == Mode::Record;
    let busy_state = if recording { "recording" } else { "calibrating" };
    let protocol_len: f64 = steps.iter().flatten().map(|s| s.seconds).sum();
    let seconds = if recording { recording_seconds(args, &steps) } else { protocol_len + 1.5 };
    let step_count = steps.as_ref().map_or(0, Vec::len);
    if let Some(d) = ctl {
        d.set(|s| {
            s.state = if recording { "searching" } else { "calibrating" };
            s.message = status::search_message("").into();
            s.search = "";
            s.clear_recording();
            s.step_count = step_count;
            s.total_s = seconds;
            s.protocol = protocol.into();
            s.session_dir.clear();
        });
    }
    if free_bytes(&args.out) < MIN_FREE {
        return Ok(Ended::early(Abort::DiskFull));
    }
    let mut deferred = Vec::new();
    let reuse = cached.take().filter(|r| ring_is_live(&b.arenas[r.arena], r));
    let search = Ctx::Search(if recording { "searching" } else { "calibrating" });
    let mut ring = match reuse {
        Some(r) => {
            eprintln!("keeping the lock: {} slots at 0x{:x}", r.off.len(), r.off[0]);
            r
        }
        None => match lock_ring(b, args, prox, ctl, search, &mut || true, &mut deferred) {
            Ok(r) => r,
            Err(abort) => return Ok(Ended::early(abort)),
        },
    };

    // Session files. Calibrations are saved the same way (calib_*) but, unless `dev` is on in settings.json, without
    // any eye images (no eye_*.raw, headers.bin, lock_dump.bin): only the small text files, among them the
    // calib_result.json that the widen history is rebuilt from. With `dev` a failed one can be looked at and replayed.
    let images = recording || Settings::load(&settings::path()).dev;
    let mut shm = match Shm::open() {
        Ok(s) => {
            eprintln!("reading Valve samples from {} (version {}, read-only)", shm::PATH, s.version);
            Some(s)
        }
        Err(e) => {
            eprintln!("WARNING: no Valve samples will be logged: {e}");
            None
        }
    };
    let d = args.out.join(format!("{}_{}", if recording { "rec" } else { "calib" }, local_stamp()));
    fs::create_dir_all(&d).map_err(|e| format!("{}: {e}", d.display()))?;
    let mut session = Some(Session::create(&d, args.full_width, images)?);
    let mut m = OpenOptions::new().create(true).append(true).open(d.join("meta.txt"))?;
    write_meta(&mut m, args, &b.header, &ring, &b.arenas, shm.as_ref(), prox.read(), protocol, "lock")?;
    writeln!(m, "images={}", if images { "kept" } else { "none (calibration without dev: no eye_*.raw, headers.bin, lock_dump.bin)" })?;
    if images {
        dump_ring(&d.join("lock_dump.bin"), &b.arenas[ring.arena], &ring, &mut m)?;
    }
    eprintln!("{} into {}", if recording { "recording" } else { "calibrating" }, d.display());
    let mut meta = Some(m);
    let dir = Some(d);
    let dir_text = dir.as_ref().map_or(String::new(), |d| d.display().to_string());

    let mut player = Player::new(&args.out.join(".beeps"), args.volume, args.beep && steps.is_some());
    if steps.is_some() {
        match player.program() {
            Some(p) => eprintln!("cues play with {}", p.display()),
            None if args.beep => eprintln!("WARNING: no pw-play/paplay/aplay found, cues are logged but silent"),
            None => {}
        }
    }
    // Labelled samples for the live engine: calibrations, and recordings with bright and dark steps (pupil range).
    let has = |l: &str| steps.iter().flatten().any(|s| s.label == l);
    let collect = match mode {
        Mode::Calib(kind) => Some(kind),
        Mode::Record if has("bright") && has("dark") => Some(CollectKind::Pupil),
        Mode::Record => None,
    };
    let mut live_collect = None;
    if let (Some(kind), Some(l)) = (collect, live.as_deref()) {
        l.send(Msg::Collect(kind));
        live_collect = Some(kind);
    }
    let mut cue = CueState::new(steps, dir.as_deref())?;

    let t_start = now_raw();
    if let Some(m) = meta.as_mut() {
        writeln!(m, "t_raw_record_start={t_start:.9}")?;
    }
    let mut slots = SlotStates::new(&b.arenas[ring.arena], &ring);
    let mut buf = vec![0u8; HEADER_BYTES + FRAME_BYTES];
    let mut count = [0u64; 2];
    let mut last_change = t_start;
    let mut last_report = t_start;
    let mut reported = [0u64; 3];
    let mut last_rate = (t_start, [0u64; 2]);
    let mut fps = [0f64; 2];
    let mut last_status = 0.0;
    let mut reading = prox.read();
    let mut valve_seen = 0u64;
    let mut relocks = 0;
    let mut abort = None;
    let mut pending_stop: Option<Request> = None;
    let reason: String = loop {
        let now = now_raw();
        if STOP.load(Ordering::SeqCst) {
            abort = Some(Abort::Interrupted);
            break "interrupted".into();
        }
        if now - t_start >= seconds {
            break "done".into();
        }
        if b.tracker.exited() {
            abort = Some(Abort::TrackerExited);
            break "the eye tracker exited (run eyecam-grab again for the new one)".into();
        }
        if let Some(d) = ctl {
            while let Some(req) = d.request() {
                match req.command {
                    // Answered once the run is closed (a few ms), with the state already `idle`.
                    Command::Stop if pending_stop.is_none() => pending_stop = Some(req),
                    Command::Stop => answer(req, "ok"),
                    _ => answer(req, Ctx::Relock(busy_state).busy_reply()),
                }
            }
            if pending_stop.is_some() {
                break "stopped".into();
            }
        }

        let arena = &b.arenas[ring.arena];
        for k in 0..ring.off.len() {
            match slots.poll(arena, &ring, k, now, &mut buf) {
                SlotEvent::None => {}
                SlotEvent::Changed => last_change = now,
                SlotEvent::Frame(t_first) => {
                    let camera = ring.eye[k] as usize ^ args.swap as usize;
                    count[camera] += 1;
                    if let Some(s) = session.as_mut() {
                        s.write_frame(camera, k, t_first, now, shm.as_ref().and_then(Shm::last_sequence), &buf)?;
                    }
                    if let Some(l) = live.as_deref_mut() {
                        l.frame(camera, &buf, now);
                    }
                }
            }
        }
        if let Some(l) = live.as_deref_mut() {
            l.poll_valve(now);
        }
        if let (Some(s), Some(sample)) = (session.as_mut(), shm.as_mut().and_then(Shm::poll)) {
            s.write_valve(&sample, now)?;
            valve_seen += 1;
        }
        if let Some((label, secs)) = cue.update(now - t_start, now, &mut player)?
            && let (Some(_), Some(l)) = (live_collect, live.as_deref())
        {
            l.send(Msg::Step { label, t0: now, seconds: secs });
        }

        if now - last_rate.0 >= 1.0 {
            for (e, f) in fps.iter_mut().enumerate() {
                *f = (count[e] - last_rate.1[e]) as f64 / (now - last_rate.0);
            }
            last_rate = (now, count);
            reading = prox.read();
        }
        if let Some(d) = ctl.filter(|_| now - last_status >= 0.1) {
            last_status = now;
            let (step_index, step_label, step_remaining) = cue.current(now - t_start);
            d.set(|s| {
                s.state = busy_state;
                s.message = if recording { "録画中" } else { "校正中" }.into();
                s.locked = true;
                s.search = "";
                s.fps = fps;
                s.step_index = step_index;
                s.step_label = step_label;
                s.step_remaining_s = step_remaining;
                s.elapsed_s = now - t_start;
                s.session_dir = dir_text.clone();
                s.prox = reading.unwrap_or(-1.0);
            });
        }

        if now - last_report >= 5.0 {
            let dt = now - last_report;
            let free = dir.as_deref().map_or(u64::MAX, free_bytes);
            let written = session.as_ref().map_or(0, |s| s.bytes);
            eprintln!(
                "{:5.0} s  L {:5.1} fps  R {:5.1} fps  valve {:5.1} Hz  written {} MB  free {} MB",
                now - t_start,
                (count[0] - reported[0]) as f64 / dt,
                (count[1] - reported[1]) as f64 / dt,
                (valve_seen - reported[2]) as f64 / dt,
                written >> 20,
                free >> 20
            );
            if shm.is_some() && valve_seen == reported[2] {
                eprintln!("  (no Valve samples: is frameeyeosc running? it is what requests them)");
            }
            reported = [count[0], count[1], valve_seen];
            last_report = now;
            if let Some(s) = session.as_mut() {
                s.flush()?;
            }
            if free < MIN_FREE {
                abort = Some(Abort::DiskFull);
                break "less than 1 GB of disk left".into();
            }
        }

        if now - last_change > LOST_AFTER {
            eprintln!("eye frames stopped; searching again ({busy_state} time keeps running)");
            if let Some(s) = session.as_mut() {
                s.flush()?;
            }
            if let Some(m) = meta.as_mut() {
                writeln!(m, "lost_at_t_raw={now:.9}")?;
            }
            if let Some(l) = live.as_deref() {
                l.send(Msg::Stale);
            }
            let mut tick = || {
                let now = now_raw();
                match cue.update(now - t_start, now, &mut player) {
                    Ok(Some((label, secs))) => {
                        if let (Some(_), Some(l)) = (live_collect, live.as_deref()) {
                            l.send(Msg::Step { label, t0: now, seconds: secs });
                        }
                    }
                    Ok(None) => {}
                    Err(e) => eprintln!("eyecam-rec: cues.csv: {e}"),
                }
                if let Some(d) = ctl {
                    let (step_index, step_label, step_remaining) = cue.current(now - t_start);
                    d.set(|s| {
                        s.fps = [0.0; 2];
                        s.step_index = step_index;
                        s.step_label = step_label;
                        s.step_remaining_s = step_remaining;
                        s.elapsed_s = now - t_start;
                    });
                }
                now - t_start < seconds
            };
            match lock_ring(b, args, prox, ctl, Ctx::Relock(busy_state), &mut tick, &mut deferred) {
                Ok(found) => {
                    ring = found;
                    relocks += 1;
                    if let Some(m) = meta.as_mut() {
                        let p = prox.read();
                        write_meta(m, args, &b.header, &ring, &b.arenas, shm.as_ref(), p, protocol, "relock")?;
                    }
                    slots = SlotStates::new(&b.arenas[ring.arena], &ring);
                    last_change = now_raw();
                }
                Err(Abort::Stopped) => break "stopped".into(),
                Err(Abort::TimeUp) => break "done".into(),
                Err(a) => {
                    abort = Some(a);
                    break match a {
                        Abort::Interrupted => "interrupted".into(),
                        Abort::NotFound => "eye frames did not come back".into(),
                        other => other.message().into(),
                    };
                }
            }
            continue;
        }
        thread::sleep(POLL);
    };

    let t_end = now_raw();
    // aborted=1: the run did not go its full time (stop, Ctrl-C, frames gone, disk full, tracker exit).
    let aborted = (reason != "done") as u8;
    if let (Some(s), Some(m)) = (session.as_mut(), meta.as_mut()) {
        s.flush()?;
        writeln!(
            m,
            "t_raw_end={t_end:.9}\nduration={:.3}\nframes_L={}\nframes_R={}\nvalve_samples={valve_seen}\nrelocks={relocks}\nstop_reason={reason}\naborted={aborted}",
            t_end - t_start,
            s.count[0],
            s.count[1]
        )?;
    }
    eprintln!(
        "{reason}: {:.1} s, {} L + {} R frames{}",
        t_end - t_start,
        count[0],
        count[1],
        dir.as_ref().map_or(String::new(), |d| format!(", {valve_seen} Valve samples in {}", d.display()))
    );
    if abort.is_none() && now_raw() - last_change < LOST_AFTER {
        *cached = Some(ring);
    }
    // The live engine fits what it collected if the run went its full time.
    let mut calib = None;
    if let (Some(kind), Some(l)) = (live_collect, live.as_deref()) {
        if aborted == 0 {
            let result = l.finish(if recording { None } else { dir.as_deref() });
            eprintln!("calibration ({kind:?}): {result:?}");
            if kind != CollectKind::Pupil {
                calib = Some(result);
            }
        } else {
            l.send(Msg::Abort(if recording { None } else { dir.clone() }));
        }
    }
    if let Some(d) = ctl {
        d.set(|s| {
            s.session_dir = dir_text.clone();
            if recording {
                s.last_session_aborted = aborted == 1;
            }
        });
    }
    if let Some(req) = pending_stop {
        let msg = match &dir {
            Some(d) if recording => {
                format!("保存した（途中で止めた）: {}", d.file_name().map_or(String::new(), |n| n.to_string_lossy().to_string()))
            }
            _ => "校正を中止した".into(),
        };
        accept_stop(ctl, req, &msg);
    }
    player.finish();
    Ok(Ended { dir, abort, failure: None, aborted: aborted == 1, calib })
}

// ------------------------------------------------------------------------------------------------- recording

enum SlotEvent {
    None,
    Changed,
    /// A complete new frame is in the buffer; it started changing at this time.
    Frame(f64),
}

/// Per slot: the last fingerprint and header seen, and since when it has been changing.
struct SlotStates {
    fp: Vec<u64>,
    header: Vec<[u8; HEADER_BYTES]>,
    dirty_since: Vec<Option<f64>>,
}

impl SlotStates {
    fn new(arena: &Arena, ring: &Ring) -> Self {
        let mut header = vec![[0u8; HEADER_BYTES]; ring.off.len()];
        for (h, &o) in header.iter_mut().zip(&ring.off) {
            arena.copy(o - HEADER_BYTES, h);
        }
        Self {
            fp: ring.off.iter().map(|&o| arena.fingerprint(o, FRAME_BYTES)).collect(),
            header,
            dirty_since: vec![None; ring.off.len()],
        }
    }

    /// A slot is copied once neither its frame nor its header changed over one poll, and only kept if it did not
    /// change while being copied, so a frame still being written is never recorded half-old.
    fn poll(&mut self, arena: &Arena, ring: &Ring, k: usize, now: f64, buf: &mut [u8]) -> SlotEvent {
        let off = ring.off[k];
        let fp = arena.fingerprint(off, FRAME_BYTES);
        let mut header = [0u8; HEADER_BYTES];
        arena.copy(off - HEADER_BYTES, &mut header);
        if fp != self.fp[k] || header != self.header[k] {
            self.fp[k] = fp;
            self.header[k] = header;
            self.dirty_since[k].get_or_insert(now);
            return SlotEvent::Changed;
        }
        let Some(t_first) = self.dirty_since[k] else { return SlotEvent::None };
        arena.copy(off - HEADER_BYTES, buf);
        if arena.fingerprint(off, FRAME_BYTES) != fp || buf[..HEADER_BYTES] != header {
            return SlotEvent::Changed;
        }
        self.dirty_since[k] = None;
        SlotEvent::Frame(t_first)
    }
}

struct Session {
    /// The eye images (eye_L.raw, eye_R.raw, headers.bin); None for a calibration without `dev`.
    images: Option<([BufWriter<File>; 2], BufWriter<File>)>,
    frames: BufWriter<File>,
    valve: BufWriter<File>,
    full_width: bool,
    row: Vec<u8>,
    count: [u64; 2],
    index: u64,
    bytes: u64,
}

impl Session {
    /// `images`: also write eye_L.raw, eye_R.raw and headers.bin (otherwise only frames.csv and valve.csv).
    fn create(dir: &Path, full_width: bool, images: bool) -> Result<Self> {
        let open = |name: &str| -> Result<BufWriter<File>> {
            Ok(BufWriter::with_capacity(1 << 20, File::create(dir.join(name))?))
        };
        let mut frames = open("frames.csv")?;
        writeln!(frames, "index,eye,eye_index,slot,t_cam,t_raw,t_copy,valve_seq")?;
        let mut valve = open("valve.csv")?;
        writeln!(valve, "{}", Sample::CSV_HEADER)?;
        let images = if images { Some(([open("eye_L.raw")?, open("eye_R.raw")?], open("headers.bin")?)) } else { None };
        Ok(Self {
            images,
            frames,
            valve,
            full_width,
            row: Vec::with_capacity(WIDTH * HEIGHT),
            count: [0; 2],
            index: 0,
            bytes: 0,
        })
    }

    /// `buf` is the slot's header followed by the 512-byte-pitch frame. `eye` 0 = L, 1 = R.
    fn write_frame(
        &mut self,
        eye: usize,
        slot: usize,
        t_first: f64,
        t_copy: f64,
        valve_seq: Option<u32>,
        buf: &[u8],
    ) -> Result<()> {
        let (header, frame) = buf.split_at(HEADER_BYTES);
        if let Some((eyes, headers)) = self.images.as_mut() {
            if self.full_width {
                eyes[eye].write_all(frame)?;
                self.bytes += frame.len() as u64;
            } else {
                self.row.clear();
                for r in 0..HEIGHT {
                    self.row.extend_from_slice(&frame[r * STRIDE..r * STRIDE + WIDTH]);
                }
                eyes[eye].write_all(&self.row)?;
                self.bytes += self.row.len() as u64;
            }
            headers.write_all(header)?;
        }
        let label = ["L", "R"][eye];
        let seq = valve_seq.map_or(String::new(), |s| s.to_string());
        // The header starts with the camera's timestamp, u64 nanoseconds of CLOCK_MONOTONIC_RAW.
        let t_cam = u64::from_le_bytes(header[..8].try_into().unwrap()) as f64 * 1e-9;
        writeln!(
            self.frames,
            "{},{label},{},{slot},{t_cam:.9},{t_first:.9},{t_copy:.9},{seq}",
            self.index, self.count[eye]
        )?;
        self.count[eye] += 1;
        self.index += 1;
        Ok(())
    }

    fn write_valve(&mut self, sample: &Sample, t_seen: f64) -> Result<()> {
        writeln!(self.valve, "{}", sample.csv_row(t_seen))?;
        Ok(())
    }

    fn flush(&mut self) -> Result<()> {
        if let Some((eyes, headers)) = self.images.as_mut() {
            for w in eyes {
                w.flush()?;
            }
            headers.flush()?;
        }
        self.frames.flush()?;
        self.valve.flush()?;
        Ok(())
    }
}

/// Plays the cue protocol against recording time and logs each step to cues.csv.
struct CueState {
    steps: Vec<Step>,
    active: bool,
    next: usize,
    at: f64,
    csv: Option<BufWriter<File>>,
}

impl CueState {
    /// `dir`: where cues.csv goes (a recording); None for a calibration.
    fn new(steps: Option<Vec<Step>>, dir: Option<&Path>) -> Result<Self> {
        let csv = match (&steps, dir) {
            (Some(_), Some(dir)) => {
                let mut w = BufWriter::new(File::create(dir.join("cues.csv"))?);
                writeln!(w, "index,t_raw,t_rel,label,seconds,beep")?;
                Some(w)
            }
            _ => None,
        };
        Ok(Self { active: steps.is_some(), steps: steps.unwrap_or_default(), next: 0, at: 0.0, csv })
    }

    /// Play the next step if its time has come; returns its label and length (the end returns "end", 0).
    fn update(&mut self, t_rel: f64, now: f64, player: &mut Player) -> Result<Option<(String, f64)>> {
        if !self.active || self.next > self.steps.len() || t_rel < self.at {
            return Ok(None);
        }
        let (label, seconds) = match self.steps.get(self.next) {
            Some(s) => (s.label.clone(), s.seconds),
            None => ("end".to_string(), 0.0),
        };
        let beep = player.play(&label);
        if let Some(csv) = self.csv.as_mut() {
            writeln!(csv, "{},{now:.9},{t_rel:.3},{label},{seconds},{beep}", self.next)?;
            csv.flush()?;
        }
        eprintln!("cue {:2}: {label} ({seconds} s, beep {beep})", self.next);
        self.at += seconds;
        self.next += 1;
        Ok(Some((label, seconds)))
    }

    /// The step under way: (index, label, seconds left). After the last step it is "end" with index = step count;
    /// without a protocol, (-1, "", 0).
    fn current(&self, t_rel: f64) -> (i64, String, f64) {
        if !self.active || self.next == 0 {
            return (-1, String::new(), 0.0);
        }
        match self.steps.get(self.next - 1) {
            Some(s) => ((self.next - 1) as i64, s.label.clone(), (self.at - t_rel).max(0.0)),
            None => (self.steps.len() as i64, "end".into(), 0.0),
        }
    }
}

// ------------------------------------------------------------------------------------------------- session log

#[allow(clippy::too_many_arguments)]
fn write_meta(
    meta: &mut File,
    args: &Args,
    header: &proto::Header,
    ring: &Ring,
    arenas: &[Arena],
    shm: Option<&Shm>,
    prox: Option<f64>,
    protocol: &str,
    what: &str,
) -> Result<()> {
    let hex: Vec<String> = ring.off.iter().map(|o| format!("0x{o:x}")).collect();
    let cams: Vec<String> = ring.eye.iter().map(|e| e.to_string()).collect();
    let labels = if args.swap { "camera0=R camera1=L" } else { "camera0=L camera1=R" };
    let width = if args.full_width { STRIDE } else { WIDTH };
    let eye_files = format!("{}={}", eyecam::replay::META_EYE_FILES, eyecam::replay::EYE_FILES_ANATOMICAL);
    let upside_down = ["L", "R"][eyecam::vision::UPSIDE_DOWN_EYE];
    writeln!(
        meta,
        "# {what}\nevent={what}\nlocal_time={}\nt_raw={:.9}\neyetracking_pid={}\nbuffer_sizes={:?}\n\
ring_buffer={}\nring_buffer_size={}\nring_pitch={}\nring_slots={}\nslot_offsets={}\n\
slot_header=64 bytes right before each offset: u64 LE camera timestamp in ns of CLOCK_MONOTONIC_RAW (t_cam in frames.csv), the rest zero so far; raw in headers.bin\n\
slot_camera={}\ncamera_assignment={}\nswap={}\nlabels={labels}\nboth_eyes={}\n\
framing=per_slot\nframing_shift={:?}\nframing_pad_dirty={:?}\nprotocol={protocol}\n\
frame_width={width}\nframe_height={HEIGHT}\nframe_bytes={}\n\
raw_format=eye_L.raw / eye_R.raw: {width}x{HEIGHT} 8-bit grey frames appended in recording order, unflipped; rows of frames.csv with eye L/R give each frame's time (eye_index = position in its file)\n\
{eye_files}\n\
eye_files_note=L is the left eye, R the right (slot_camera: 0 = L, 1 = R; the lower addresses are the right eye's camera). Sessions without eye_files (before 2026-10-07) called the lower-address camera L, so their eye_L.raw holds the right eye\n\
upside_down_eye={upside_down}: stored raw; flip vertically (not 180 degrees) to view upright\n\
clock=CLOCK_MONOTONIC_RAW seconds (same clock as Valve sample_time); t_cam = camera timestamp from the slot header, t_raw = when the slot started changing, t_copy = when it was copied\n\
shm_version={}\nproximity={}\n",
        local_stamp(),
        now_raw(),
        header.pid,
        header.sizes,
        ring.arena,
        arenas[ring.arena].len(),
        ring.pitch,
        ring.off.len(),
        hex.join(","),
        cams.join(","),
        ring.camera_reason,
        args.swap,
        ring.both_eyes,
        ring.framing.shift,
        ring.framing.pad_dirty.iter().map(|d| (d * 1e4).round() / 1e4).collect::<Vec<_>>(),
        width * HEIGHT,
        shm.map_or("unavailable".into(), |s| s.version.to_string()),
        prox.map_or("unknown".into(), |p| p.to_string()),
    )?;
    Ok(())
}

/// Save the raw ring area (64 KiB either side) as found, so the framing and header layout can be checked offline.
fn dump_ring(path: &Path, arena: &Arena, ring: &Ring, meta: &mut File) -> Result<()> {
    let from = ring.off[0].saturating_sub(HEADER_BYTES + 65536);
    let to = (ring.off[ring.off.len() - 1] + ring.pitch + 65536).min(arena.len());
    let mut bytes = vec![0u8; to - from];
    arena.copy(from, &mut bytes);
    fs::write(path, &bytes)?;
    writeln!(meta, "lock_dump=lock_dump.bin\nlock_dump_offset=0x{from:x}\nlock_dump_length={}", bytes.len())?;
    Ok(())
}

fn free_bytes(path: &Path) -> u64 {
    let Ok(c) = std::ffi::CString::new(path.as_os_str().as_encoded_bytes()) else { return u64::MAX };
    let mut st: libc::statvfs = unsafe { std::mem::zeroed() };
    if unsafe { libc::statvfs(c.as_ptr(), &mut st) } != 0 {
        return u64::MAX;
    }
    st.f_bavail as u64 * st.f_frsize as u64
}

/// Local time as YYYY-MM-DD_HH-MM-SS.
fn local_stamp() -> String {
    let t = unsafe { libc::time(std::ptr::null_mut()) };
    let mut tm: libc::tm = unsafe { std::mem::zeroed() };
    unsafe { libc::localtime_r(&t, &mut tm) };
    format!(
        "{:04}-{:02}-{:02}_{:02}-{:02}-{:02}",
        tm.tm_year + 1900,
        tm.tm_mon + 1,
        tm.tm_mday,
        tm.tm_hour,
        tm.tm_min,
        tm.tm_sec
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use eyecam::ring::{EXPECTED_PITCH, Framing};
    use std::os::unix::fs::FileExt;

    /// Frames handed over the way the polling loops do it (slot header + 512-byte rows through `Live::frame`) reach
    /// the engine, and status.json reports `live_ms`:
    /// `EYECAM_SESSION=dir cargo test --release -- --ignored live_frames_report_live_ms --nocapture`.
    #[test]
    #[ignore]
    fn live_frames_report_live_ms() {
        let s = eyecam::replay::Session::open(Path::new(&std::env::var("EYECAM_SESSION").unwrap()), false).unwrap();
        let dir = std::env::temp_dir().join(format!("eyecam-livems-test-{}", std::process::id()));
        let shared = Arc::new(livesvc::Shared::default());
        let daemon = Daemon::start(&dir, Some(shared.clone())).unwrap();
        let mut live = Live::start(&dir, None, shared.clone()).unwrap();
        live.send(Msg::Buffers(true));
        let mut img = vec![0u8; WIDTH * HEIGHT];
        let mut buf = vec![0u8; HEADER_BYTES + FRAME_BYTES];
        let t0 = std::time::Instant::now();
        let mut k = 1000;
        // Real time: 90 frames per eye per second for 2.5 s.
        while t0.elapsed() < Duration::from_millis(2500) {
            for eye in 0..2 {
                s.read(eye, k, &mut img).unwrap();
                if eyecam::vision::stored_upside_down(eye) {
                    img.reverse(); // back to stored orientation (rows and columns), close enough for a load test
                }
                let t = s.t_cam[eye][k];
                buf[..8].copy_from_slice(&((t * 1e9) as u64).to_le_bytes());
                for r in 0..HEIGHT {
                    buf[HEADER_BYTES + r * STRIDE..HEADER_BYTES + r * STRIDE + WIDTH].copy_from_slice(&img[r * WIDTH..(r + 1) * WIDTH]);
                }
                live.frame(eye, &buf, now_raw());
            }
            k += 1;
            thread::sleep(Duration::from_micros(11_111));
        }
        thread::sleep(Duration::from_millis(300));
        let json = fs::read_to_string(dir.join(status::STATUS_FILE)).unwrap();
        let frames = shared.frames.load(Ordering::Relaxed);
        live.stop();
        daemon.finish();
        fs::remove_dir_all(&dir).unwrap();
        eprintln!("{} frames processed, {} dropped; {json}", frames, live.dropped);
        assert!(frames > 300, "{frames}");
        let ms: f64 = json.split("\"live_ms\":").nth(1).unwrap().split(',').next().unwrap().parse().unwrap();
        assert!(ms > 0.1, "{json}");
    }

    /// Args as `--serve` has them, with status.json and everything else in `dir`.
    fn serve_args(dir: &Path) -> Args {
        Args {
            seconds: None,
            out: dir.join("out"),
            swap: false,
            cues: None,
            serve: true,
            fake: false,
            fake_search: None,
            fake_calib: None,
            fake_camfps: 90.0,
            run_dir: dir.to_path_buf(),
            replay: None,
            compat: false,
            limit: None,
            out_given: false,
            user_calib: None,
            calib_block: None,
            fit_user: None,
            use_wear: false,
            prewarm: false,
            resample: None,
            calib_replay: None,
            tune: eyecam::live::Tune::default(),
            wait_grab: 0.0,
            wait_lock: 300.0,
            prox_min: 20.0,
            allow_one_eye: false,
            full_width: false,
            beep: false,
            volume: 0.0,
        }
    }

    /// memfd stand-ins for the camera buffers (all zeros, never changing: the cameras off), "from" this process.
    fn still_buffers(sizes: &[u64]) -> Buffers {
        let mut maps = Vec::new();
        for &size in sizes {
            let fd = unsafe { OwnedFd::from_raw_fd(libc::memfd_create(c"eyecam-test".as_ptr(), libc::MFD_CLOEXEC)) };
            File::from(fd.try_clone().unwrap()).set_len(size).unwrap();
            maps.push(Mapping::new(&fd, size as usize).unwrap());
        }
        let arenas = maps.iter().map(|m| unsafe { Arena::new(m.ptr, m.len) }).collect();
        Buffers {
            header: proto::Header { count: sizes.len(), pid: std::process::id() as i32, sizes: sizes.to_vec() },
            arenas,
            _maps: maps,
            tracker: Tracker::open(std::process::id() as i32),
            last_activity: now_raw(),
            worn_since: None,
            valve: ValveWatch::default(),
            valve_tracking: false,
        }
    }

    /// Run idle() for `seconds` on still buffers of `sizes` with the real proximity sensor, then stop it. Returns
    /// the last status.json, and the process's CPU seconds (user + system) over the run.
    fn run_idle(name: &str, sizes: &'static [u64], seconds: f64) -> (String, f64) {
        let dir = std::env::temp_dir().join(format!("eyecam-{name}-{}", std::process::id()));
        let args = serve_args(&dir);
        let shared = Arc::new(livesvc::Shared::default());
        let daemon = Daemon::start(&dir, Some(shared.clone())).unwrap();
        daemon.set(|s| {
            s.state = "waiting_fds";
            s.message = INSTALL_TOOL.into();
        });
        let status_path = dir.join(status::STATUS_FILE);
        let live_dir = dir.clone();
        let cpu = || {
            let mut u: libc::rusage = unsafe { std::mem::zeroed() };
            unsafe { libc::getrusage(libc::RUSAGE_SELF, &mut u) };
            let t = |v: libc::timeval| v.tv_sec as f64 + v.tv_usec as f64 * 1e-6;
            t(u.ru_utime) + t(u.ru_stime)
        };
        let cpu0 = cpu();
        let handle = thread::spawn(move || {
            let mut live = Live::start(&live_dir, None, shared).unwrap();
            let mut b = still_buffers(sizes);
            let mut prox = Proximity::find();
            live.send(Msg::Buffers(true));
            let r = idle(&mut b, &args, &mut prox, &daemon, &mut live);
            live.stop();
            daemon.finish();
            r
        });
        thread::sleep(Duration::from_secs_f64(seconds));
        let json = fs::read_to_string(&status_path).unwrap();
        let used = cpu() - cpu0;
        STOP.store(true, Ordering::SeqCst);
        assert!(handle.join().unwrap().is_none());
        STOP.store(false, Ordering::SeqCst);
        fs::remove_dir_all(&dir).unwrap();
        (json, used)
    }

    /// Once the buffers are held, status.json must say so at once (idle, has_buffers), even while the headset is
    /// off and the first search is still running. Uses memfd stand-ins for the buffers and the real proximity
    /// sensor (run on the headset, not worn): `cargo test --release -- --ignored idle_reports_buffers`.
    #[test]
    #[ignore]
    fn idle_reports_buffers_at_once() {
        let (json, _) = run_idle("idle-test", &[16 << 20], 1.0);
        eprintln!("{json}");
        assert!(json.contains("\"state\":\"idle\""), "{json}");
        assert!(json.contains("\"has_buffers\":true"), "{json}");
        assert!(json.contains("\"live\":true"), "{json}");
        assert!(json.contains("HMD をかぶってね"), "{json}");
    }

    /// After the first look at still buffers (the cameras off), status.json says where it stopped: no candidates, with
    /// nothing changed, and the proximity threshold beside the reading. On the headset:
    /// `cargo test --release -- --ignored idle_says_what_the_look_saw`.
    #[test]
    #[ignore]
    fn idle_says_what_the_look_saw() {
        let (json, _) = run_idle("idle-look", &[16 << 20], 3.5);
        eprintln!("{json}");
        assert!(json.contains("\"prox_min\":20.000,"), "{json}");
        assert!(
            json.contains("\"search_detail\":{\"candidates\":0,\"refresh_hz\":0.000,\"slots\":0,\"both_eyes\":false,\"stopped_at\":\"no_candidates\",\"changed_blocks\":0,"),
            "{json}"
        );
    }

    /// What idling costs while the cameras are off (nothing changes in the buffers): the process's CPU over 30 s of
    /// idle() on still buffers the size of the real ones (16 + 32 MiB), with the real proximity sensor. On the
    /// headset, not worn: `cargo test --release -- --ignored idle_cpu --nocapture`.
    #[test]
    #[ignore]
    fn idle_cpu_while_the_cameras_are_off() {
        let seconds = 30.0;
        let (json, used) = run_idle("idle-cpu", &[16 << 20, 32 << 20], seconds);
        eprintln!("idle with the cameras off: {:.3} s CPU in {seconds} s ({:.2} % of a core)
{json}", used, used / seconds * 100.0);
    }

    /// A stand-in for Valve's shared memory: the samples `poll` hands out, one per call (None: nothing new).
    struct Samples(std::rc::Rc<std::cell::RefCell<Vec<Option<u32>>>>);

    impl SampleSource for Samples {
        fn poll(&mut self) -> Option<Sample> {
            let mut queue = self.0.borrow_mut();
            if queue.is_empty() {
                return None;
            }
            queue.remove(0).map(|producer_state| Sample { producer_state, ..Sample::default() })
        }
    }

    /// A ValveWatch over `Samples`: the queue its source reads (shared by every open), how many times it tried to
    /// open, and whether opening works.
    #[allow(clippy::type_complexity)]
    fn valve_watch() -> (
        ValveWatch<Samples>,
        std::rc::Rc<std::cell::RefCell<Vec<Option<u32>>>>,
        std::rc::Rc<std::cell::Cell<u32>>,
        std::rc::Rc<std::cell::Cell<bool>>,
    ) {
        let queue = std::rc::Rc::new(std::cell::RefCell::new(Vec::new()));
        let opens = std::rc::Rc::new(std::cell::Cell::new(0));
        let can_open = std::rc::Rc::new(std::cell::Cell::new(true));
        let (q, o, c) = (queue.clone(), opens.clone(), can_open.clone());
        let watch = ValveWatch::new(Box::new(move || {
            o.set(o.get() + 1);
            c.get().then(|| Samples(q.clone()))
        }));
        (watch, queue, opens, can_open)
    }

    #[test]
    fn valve_watch_retries_opening_every_5_s() {
        let (mut watch, _, opens, can_open) = valve_watch();
        can_open.set(false);
        assert!(!watch.tracking(100.0));
        assert_eq!(opens.get(), 1);
        // Not again before 5 s are up
        for t in [101.0, 103.0, 104.9] {
            assert!(!watch.tracking(t));
        }
        assert_eq!(opens.get(), 1);
        assert!(!watch.tracking(105.0));
        assert_eq!(opens.get(), 2);
        assert!(!watch.tracking(109.0));
        assert_eq!(opens.get(), 2);
        can_open.set(true);
        assert!(!watch.tracking(110.0));
        assert_eq!(opens.get(), 3);
        assert!(watch.shm.is_some());
    }

    #[test]
    fn valve_watch_counts_only_fresh_tracked_samples() {
        let (mut watch, queue, opens, _) = valve_watch();
        // The sample there at open doesn't count, even a tracked one
        queue.borrow_mut().extend([Some(1), Some(1), Some(0), None, Some(1)]);
        assert!(!watch.tracking(0.0));
        assert_eq!(queue.borrow().len(), 4);
        // A fresh one with producer_state 1 does; producer_state 0 or nothing new doesn't
        assert!(watch.tracking(1.0));
        assert!(!watch.tracking(2.0));
        assert!(!watch.tracking(3.0));
        assert!(watch.tracking(4.0));
        assert!(!watch.tracking(5.0));
        assert_eq!(opens.get(), 1);
    }

    #[test]
    fn valve_watch_reopens_after_30_s_without_new_samples() {
        let (mut watch, queue, opens, _) = valve_watch();
        assert!(!watch.tracking(0.0));
        assert_eq!(opens.get(), 1);
        // A sample that isn't tracked still counts as new: it keeps the file open
        queue.borrow_mut().push(Some(0));
        assert!(!watch.tracking(20.0));
        assert!(!watch.tracking(50.0));
        assert_eq!(opens.get(), 1);
        // Over 30 s since the last new one: closed and opened again (that look never counts)
        queue.borrow_mut().extend([Some(1), Some(1)]);
        assert!(!watch.tracking(50.1));
        assert_eq!(opens.get(), 2);
        assert!(watch.tracking(51.0));
        assert_eq!(opens.get(), 2);
    }

    #[test]
    fn search_reason_needs_a_look_and_both_sensors_for_not_worn() {
        // Nothing before the first look, whatever the sensor says
        for worn in [false, true] {
            assert_eq!(search_reason(false, false, worn, false), "");
            assert_eq!(search_reason(false, false, worn, true), "");
        }
        assert_eq!(search_reason(true, true, false, false), status::SEARCH_ONE_EYE);
        assert_eq!(search_reason(true, false, true, false), status::SEARCH_NO_VIDEO);
        // The sensor says off but Valve's eye tracker is delivering: worn, the video is what is missing
        assert_eq!(search_reason(true, false, false, true), status::SEARCH_NO_VIDEO);
        assert_eq!(search_reason(true, false, false, false), status::SEARCH_NOT_WORN);
    }

    #[test]
    fn prox_at_look_says_how_long_it_was_above() {
        assert_eq!(prox_at_look(None, 20.0, None), "proximity unknown");
        assert_eq!(prox_at_look(Some(31.0), 20.0, Some(4.0)), "proximity 31.0, above 20 for 1 s: worn");
        assert_eq!(
            prox_at_look(Some(39.0), 20.0, Some(0.4)),
            "proximity 39.0, above 20 for only 0.4 s (worn after 1 s)"
        );
        assert_eq!(prox_at_look(Some(2.9), 20.0, None), "proximity 2.9, not above 20: the sensor says not worn");
    }

    #[test]
    fn receives_descriptors_and_header() {
        let (tx, rx) = UnixStream::pair().unwrap();
        let fds: Vec<OwnedFd> = (0..2u8)
            .map(|i| {
                let fd = unsafe { OwnedFd::from_raw_fd(libc::memfd_create(c"eyecam".as_ptr(), libc::MFD_CLOEXEC)) };
                File::from(fd.try_clone().unwrap()).write_all(&[i + 1; 8]).unwrap();
                fd
            })
            .collect();
        let mut header = proto::MAGIC.to_vec();
        header.extend_from_slice(&2u32.to_le_bytes());
        header.extend_from_slice(&77u32.to_le_bytes());
        for size in [8u64, 8, 0, 0] {
            header.extend_from_slice(&size.to_le_bytes());
        }
        // The same sendmsg as eyecam-grab's send_fds.
        let raw: Vec<RawFd> = fds.iter().map(|f| f.as_raw_fd()).collect();
        let fd_bytes = size_of_val(&raw[..]) as u32;
        let mut control = vec![0u8; unsafe { libc::CMSG_SPACE(fd_bytes) } as usize];
        let mut iov = libc::iovec { iov_base: header.as_mut_ptr().cast(), iov_len: header.len() };
        let mut msg: libc::msghdr = unsafe { std::mem::zeroed() };
        msg.msg_iov = &mut iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control.as_mut_ptr().cast();
        msg.msg_controllen = control.len() as _;
        unsafe {
            let cmsg = libc::CMSG_FIRSTHDR(&msg);
            (*cmsg).cmsg_level = libc::SOL_SOCKET;
            (*cmsg).cmsg_type = libc::SCM_RIGHTS;
            (*cmsg).cmsg_len = libc::CMSG_LEN(fd_bytes) as _;
            std::ptr::copy_nonoverlapping(raw.as_ptr(), libc::CMSG_DATA(cmsg).cast::<RawFd>(), raw.len());
            assert_eq!(libc::sendmsg(tx.as_raw_fd(), &msg, 0), header.len() as isize);
        }
        let (bytes, got) = recv_fds(&rx).unwrap();
        let parsed = proto::parse_header(&bytes).unwrap();
        assert_eq!((parsed.count, parsed.pid, got.len()), (2, 77, 2));
        let mut content = [0u8; 8];
        File::from(got.into_iter().nth(1).unwrap()).read_exact_at(&mut content, 0).unwrap();
        assert_eq!(content, [2; 8]);
    }

    #[test]
    fn records_complete_frames_with_padding_stripped() {
        let mut mem = vec![0u8; 2 * EXPECTED_PITCH + 4096];
        let ptr = mem.as_mut_ptr();
        let arena = unsafe { Arena::new(ptr, mem.len()) };
        let ring = Ring {
            arena: 0,
            pitch: EXPECTED_PITCH,
            off: vec![HEADER_BYTES, HEADER_BYTES + EXPECTED_PITCH],
            eye: vec![0, 1],
            both_eyes: true,
            framing: Framing::default(),
            camera_reason: String::new(),
        };
        let mut slots = SlotStates::new(&arena, &ring);
        let mut buf = vec![0u8; HEADER_BYTES + FRAME_BYTES];
        assert!(matches!(slots.poll(&arena, &ring, 0, 0.5, &mut buf), SlotEvent::None));

        // The camera writes slot 1: header floats, then 400 pixels and 112 padding bytes per row.
        let base = ring.off[1];
        for (i, b) in 1_234_500_000_000u64.to_le_bytes().iter().enumerate() {
            unsafe { *ptr.add(base - HEADER_BYTES + i) = *b };
        }
        for r in 0..HEIGHT {
            for c in 0..STRIDE {
                unsafe { *ptr.add(base + r * STRIDE + c) = if c < WIDTH { ((r + c) % 251) as u8 } else { 0xee } };
            }
        }
        assert!(matches!(slots.poll(&arena, &ring, 1, 1.0, &mut buf), SlotEvent::Changed));
        let SlotEvent::Frame(t_first) = slots.poll(&arena, &ring, 1, 1.0025, &mut buf) else { panic!("no frame") };
        assert_eq!(t_first, 1.0);
        assert!(matches!(slots.poll(&arena, &ring, 1, 1.005, &mut buf), SlotEvent::None));

        let dir = std::env::temp_dir().join(format!("eyecam-test-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let mut session = Session::create(&dir, false, true).unwrap();
        session.write_frame(1, 1, t_first, 1.0025, Some(42), &buf).unwrap();
        session.flush().unwrap();
        drop(session);
        let raw = fs::read(dir.join("eye_R.raw")).unwrap();
        assert_eq!(raw.len(), WIDTH * HEIGHT);
        assert!(raw.iter().enumerate().all(|(i, &v)| v == ((i / WIDTH + i % WIDTH) % 251) as u8));
        assert_eq!(fs::read(dir.join("headers.bin")).unwrap().len(), HEADER_BYTES);
        let csv = fs::read_to_string(dir.join("frames.csv")).unwrap();
        let row: Vec<&str> = csv.lines().nth(1).unwrap().split(',').collect();
        assert_eq!(row, ["0", "R", "0", "1", "1234.500000000", "1.000000000", "1.002500000", "42"]);
        assert_eq!(row.len(), csv.lines().next().unwrap().split(',').count());
        fs::remove_dir_all(&dir).unwrap();

        // A calibration without dev: the same frames.csv row, but no file with image pixels.
        fs::create_dir_all(&dir).unwrap();
        let mut session = Session::create(&dir, false, false).unwrap();
        session.write_frame(1, 1, t_first, 1.0025, Some(42), &buf).unwrap();
        session.flush().unwrap();
        assert_eq!(session.bytes, 0);
        drop(session);
        let mut names: Vec<String> =
            fs::read_dir(&dir).unwrap().map(|e| e.unwrap().file_name().to_string_lossy().into_owned()).collect();
        names.sort();
        assert_eq!(names, ["frames.csv", "valve.csv"]);
        assert_eq!(fs::read_to_string(dir.join("frames.csv")).unwrap(), csv);
        fs::remove_dir_all(&dir).unwrap();
        drop(arena);
        drop(mem);
    }
}
