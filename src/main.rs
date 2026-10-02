//! Steam Frame eye bridge using the private shared-memory ABI (versions 4 and 5).

mod capture;
mod config;
mod dots;
mod livelink;
mod replay;
mod status;

use capture::{Capture, CaptureResult, CaptureState};
use clap::{CommandFactory, FromArgMatches, Parser};
use config::{ActiveType, Config, LidFit, OutputKind, Reload, Settings, Widen};
use memmap2::{MmapMut, MmapOptions};
use rosc::{OscMessage, OscPacket, OscType, encoder};
use status::{CalibrationStatus, RawValues, SentValues, Status, StatusFile};
use std::collections::{HashSet, VecDeque};
use std::error::Error;
use std::fs::{self, OpenOptions};
use std::os::unix::fs::MetadataExt;
use std::io;
use std::mem::{align_of, offset_of, size_of};
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr, ToSocketAddrs, UdpSocket};
use std::path::{Path, PathBuf};
use std::ptr;
use std::time::{Duration, Instant, SystemTime};

const SOURCE: &str = "/dev/shm/eye-server.mmap";
const TIMEOUT: Duration = Duration::from_secs(1);
// The eye server is waited on in slices this long, so the status file keeps updating while it is idle.
const POLL: Duration = Duration::from_millis(100);
// The status file's send rate counts the samples sent within this window.
const RATE_WINDOW: Duration = Duration::from_secs(1);
// Relaxed open is 0.75 in VRCFT units but 1.0 for the ETVR Tracking Module, which does not widen by default.
const ETVR_LID_SCALE: f32 = 1.0 / 0.75;
// While sending to VRCFT's LiveLink module without eye data, a neutral packet this often: the module logs "connection
// lost" after a second without packets, and only starts if one arrives within 180 s of VRCFT loading it.
const LIVELINK_IDLE_INTERVAL: Duration = Duration::from_millis(500);
// At most this many Live Link packets a second. VRCFT's LiveLink module (LiveLinkExtTrackingInterface.cs, Update)
// does `Thread.Sleep(10)` and then reads a single datagram per loop, and a 10 ms sleep takes 10-15.6 ms on Windows,
// so it takes in only about 64-100 packets a second. Sending every eye sample (90 Hz, up to ~136 Hz while streaming)
// filled its socket queue until the eyes lagged by seconds. Samples in between are dropped, never queued: each packet
// is the newest sample.
const LIVELINK_MAX_HZ: u32 = 50;
// The eye server produces samples at ~90 Hz.
const NOMINAL_DT: f32 = 1.0 / 90.0;
// Gaps longer than this restart the filters instead of smearing across them.
const MAX_GAP: f64 = 0.25;
// Blinks are fast, so eyelids track their speed with a quicker derivative filter than gaze.
const LID_D_CUTOFF: f32 = 1.0;
// The sideways gaze hold (--gaze-down-hold-x-deg) fades in over this many degrees further down.
const DOWN_HOLD_FADE_DEG: f32 = 10.0;
// The eye fit measures openness with the eyes on targets this far up and down (15° of 45°).
const LID_FIT_PITCH: f32 = 15.0 / 45.0;
// A fitted eye counts as closed from this share of the way from its closed reading to its open one,
// so readings a little above the eyes-shut average still close the eyelid. On the 2026-09-27 worn
// recording, 0.3 closed more blinks than no fit (53 vs 50 of 60), and 0.1 or 0.2 fewer.
const LID_FIT_CLOSED_MARGIN: f32 = 0.3;
// How far (raw openness) above a fitted eye's expected open reading widening starts, and where it is full, per
// --lid-widen. The Frame's openness rises only about 0.05 when the eyes are opened wide (two eye fits on 2026-09-30:
// +0.019 / -0.009 and +0.048 / +0.047), so widening can't be measured and is a chosen sensitivity. A relaxed open
// eye also wanders above its usual reading: over the 2026-09-27..30 recordings (eyes with room, looking within about
// 7° of straight ahead, relative to each recording's median) it was above +0.04 25.5% of the time, above +0.07
// 12.7% and above +0.10 6.0%. Visibly widened (40% of the way to full or more, VRCFT 0.85 or more) by accident
// that makes: high 14.9%, normal 6.3%, low 2.4%; a +0.10 widen shows fully (high), 43% (normal) or not (low).
const WIDEN_LOW: (f32, f32) = (0.10, 0.18);
const WIDEN_NORMAL: (f32, f32) = (0.07, 0.14);
const WIDEN_HIGH: (f32, f32) = (0.04, 0.10);
// The Frame's openness stops at 1.000. A fitted eye whose straight-ahead open reading puts the widening start above
// this has no room to widen (one user's left eye read 0.945-0.975 straight ahead, and 21-40% of its open samples sat
// at 1.000); it widens with the other eye. With "normal" that is an open reading above 0.90: the eyes with room in
// the recordings read 0.80-0.88, the saturated ones 0.92-0.97.
const WIDEN_ROOM_LIMIT: f32 = 0.97;
// Full widening needs at most this reading (where the openness stops).
const OPENNESS_CAP: f32 = 1.0;
// How often the Steam Link PC is looked up again, to follow reconnects over another network.
const RESOLVE_INTERVAL: Duration = Duration::from_secs(5);
// Eyelid auto calibration keeps a decaying histogram of each eye's open readings (0.005 wide bins).
const CAL_BINS: usize = 300;
const CAL_BIN_WIDTH: f32 = 0.005;
// Readings fade with a 10 minute half-life at ~90 Hz, so a short squint barely moves the estimate.
const CAL_HALF_LIFE_SAMPLES: f32 = 90.0 * 600.0;
// Only readings above this fraction of the current estimate count as "open". Higher gates resist
// squints better but ratchet the estimate upward on eyes whose readings spread widely.
const CAL_GATE: f32 = 0.75;
// Skip this long after tracking starts, while the headset is still being put on and adjusted.
const CAL_SETTLE: Duration = Duration::from_secs(20);
// Weight (~10 s of open eyes) before the histogram overrides the saved or default estimate.
const CAL_WARMUP_WEIGHT: f32 = 900.0;
const CAL_SCALE_RANGE: (f32, f32) = (0.75, 1.33);
const CAL_SAVE_INTERVAL: Duration = Duration::from_secs(60);

/// Where the eye record sits in one version of the shared memory, and how large that version's file is.
struct ShmLayout {
    version: u32,
    eye_data: usize,
    size: usize,
}

// Version 5 (SteamOS beta, 2026-10) inserts 5 bytes (constant `00 ff ff ff ff`) just before the eye record.
const SHM_LAYOUTS: &[ShmLayout] = &[
    ShmLayout { version: 4, eye_data: 0x152, size: 0x4f21a },
    ShmLayout { version: 5, eye_data: 0x157, size: 0x4f21f },
];

/// The control fields at the start of the shared memory, the same in every known version.
#[repr(C)]
struct EyeServerHeader {
    version: u32,
    initialized: u32,
    // The target glibc mutex slot is 48 bytes; host libc may define a smaller type.
    metadata_mutex: [u8; 0x30],
    sequence: u32,
    metadata_requested: u32,
}

// The record is packed, so its timestamp and vectors are not naturally aligned.
#[repr(C, packed)]
#[derive(Clone, Copy)]
struct EyeDataMmap {
    producer_state: u32,
    sample_flag: u8,
    sample_time: f64,
    // Left, right; after stereo fusion.
    gaze_direction: [[f32; 3]; 2],
    gaze_covariance_diag: [[f32; 3]; 2],
    // Head-relative, -Z forward, in metres.
    fixation_point: [f32; 3],
    pre_fusion_gaze: [[f32; 3]; 2],
    pre_fusion_cov_diag: [[f32; 3]; 2],
    openness: [f32; 2],
    estimate_extra: [f32; 8],
    reserved: [u8; 0xe1b],
}

const _: () = {
    assert!(offset_of!(EyeServerHeader, metadata_mutex) == 0x08);
    assert!(offset_of!(EyeServerHeader, sequence) == 0x38);
    assert!(offset_of!(EyeServerHeader, metadata_requested) == 0x3c);
    assert!(size_of::<EyeServerHeader>() == 0x40);
    assert!(offset_of!(EyeDataMmap, sample_time) == 0x05);
    assert!(offset_of!(EyeDataMmap, gaze_direction) == 0x0d);
    assert!(offset_of!(EyeDataMmap, gaze_covariance_diag) == 0x25);
    assert!(offset_of!(EyeDataMmap, fixation_point) == 0x3d);
    assert!(offset_of!(EyeDataMmap, pre_fusion_gaze) == 0x49);
    assert!(offset_of!(EyeDataMmap, pre_fusion_cov_diag) == 0x61);
    assert!(offset_of!(EyeDataMmap, openness) == 0x79);
    assert!(offset_of!(EyeDataMmap, estimate_extra) == 0x81);
    assert!(size_of::<EyeDataMmap>() == 0xebc);
    let mut i = 0;
    while i < SHM_LAYOUTS.len() {
        let layout = &SHM_LAYOUTS[i];
        assert!(layout.eye_data >= size_of::<EyeServerHeader>());
        assert!(layout.eye_data + size_of::<EyeDataMmap>() <= layout.size);
        i += 1;
    }
    assert!(size_of::<libc::pthread_mutex_t>() <= 0x30);
    assert!(8 % align_of::<libc::pthread_mutex_t>() == 0);
};

#[derive(Parser)]
#[command(about = "Send Steam Frame eye tracking from shared memory over OSC")]
struct Args {
    /// What to send: VRChat avatar parameters, VRCFaceTracking's ETVR Tracking Module format, or Live Link Face
    /// packets for VRCFaceTracking's LiveLink module
    #[arg(long, value_enum, default_value_t = OutputKind::Vrchat)]
    output: OutputKind,
    /// How EyeTrackingActive is sent in VRChat mode: a bool, a float (1.0 / 0.0; some avatars
    /// need it), or not at all
    #[arg(long, value_enum, default_value_t = ActiveType::Bool)]
    eye_tracking_active: ActiveType,
    /// How easily an eye with an eye fit widens: off, low, normal or high (eyes without one use --lid-widen-start
    /// and --lid-wide)
    #[arg(long, value_enum, default_value_t = Widen::Normal)]
    lid_widen: Widen,
    /// OSC destination as HOST:PORT, or "auto" for the PC that Steam Link is streaming from
    #[arg(long, default_value = "auto")]
    target: String,
    /// OSC port used with --target auto [default: 9000 for vrchat, 8889 for etvr, 11111 for livelink]
    #[arg(long)]
    port: Option<u16>,
    /// Parameter name prefix; "" or "/" for none
    #[arg(long, default_value = "/FT")]
    prefix: String,
    /// Send unsmoothed values (eyelid remapping still applies)
    #[arg(long)]
    raw: bool,
    /// One Euro minimum cutoff in Hz for gaze; lower is steadier at rest
    #[arg(long, default_value_t = 0.4)]
    gaze_min_cutoff: f32,
    /// One Euro beta for gaze; higher follows fast eye movements with less lag
    #[arg(long, default_value_t = 0.8)]
    gaze_beta: f32,
    /// One Euro derivative cutoff in Hz for gaze; lower keeps tracker noise from loosening the filter
    #[arg(long, default_value_t = 0.5)]
    gaze_d_cutoff: f32,
    /// Gaze changes smaller than this (1.0 = 45°) are ignored so the eyes stay put while fixating
    #[arg(long, default_value_t = 0.02)]
    gaze_deadzone: f32,
    /// Hold the gaze while either eye's Frame openness is below this; 0 disables
    #[arg(long, default_value_t = 0.5)]
    gaze_hold_below: f32,
    /// Send each eye's own gaze instead of the combined gaze for both eyes (jittery on the Frame)
    #[arg(long)]
    independent_eyes: bool,
    /// One Euro minimum cutoff in Hz for eyelids
    #[arg(long, default_value_t = 6.0)]
    lid_min_cutoff: f32,
    /// One Euro beta for eyelids
    #[arg(long, default_value_t = 5.0)]
    lid_beta: f32,
    /// Frame openness at or below this counts as fully closed
    #[arg(long, default_value_t = 0.30)]
    lid_closed: f32,
    /// Frame openness of a relaxed open eye (VRCFT 0.75)
    #[arg(long, default_value_t = 0.80)]
    lid_open: f32,
    /// Frame openness where widening begins; between --lid-open and this the eye stays at VRCFT 0.75
    #[arg(long, default_value_t = 0.92)]
    lid_widen_start: f32,
    /// Frame openness of a fully widened eye (VRCFT 1.0)
    #[arg(long, default_value_t = 1.00)]
    lid_wide: f32,
    /// Fixed multiplier on the left eye's Frame openness; overrides auto calibration for that eye. With an eye
    /// fit, a fine-tune on the fitted openness instead (0.9 = 10% less open)
    #[arg(long)]
    lid_scale_left: Option<f32>,
    /// Fixed multiplier on the right eye's Frame openness; overrides auto calibration for that eye. With an eye
    /// fit, a fine-tune on the fitted openness instead (0.9 = 10% less open)
    #[arg(long)]
    lid_scale_right: Option<f32>,
    /// Turn off learning each eye's relaxed openness (eyes without a fixed scale use 1.0)
    #[arg(long)]
    no_lid_calibration: bool,
    /// Where learned eyelid calibration is kept between runs [default: ~/.config/frameeyeosc/calibration]
    #[arg(long)]
    calibration_file: Option<PathBuf>,
    /// Pull both eyelids toward their average when they differ by less than this (VRCFT units);
    /// larger differences such as winks pass through untouched. 0 disables
    #[arg(long, default_value_t = 0.4)]
    lid_sync: f32,
    /// Ignore an eye's gaze while its covariance is above this; the other eye moves both,
    /// or the gaze is held if both are above. An optional safety net; 0 disables
    #[arg(long, default_value_t = 0.0)]
    gaze_quality_limit: f32,
    /// Keep a closed eyelid fully closed for at least this long, so short blinks reach other players. 0 disables
    #[arg(long, default_value_t = 80.0)]
    blink_hold_ms: f32,
    /// Turn off the 3-sample median that drops one-sample dropouts in gaze and openness
    #[arg(long)]
    no_despike: bool,
    /// Close both eyes when one is closed and the other is below this (VRCFT units); winks pass. 0 disables
    #[arg(long, default_value_t = 0.35)]
    blink_sync_below: f32,
    /// Gaze that counts as straight ahead, left/right (-0.5..0.5 on the -1..1 scale; 1.0 = 45°)
    #[arg(long, default_value_t = 0.0, allow_negative_numbers = true)]
    gaze_offset_x: f32,
    /// Gaze that counts as straight ahead, up/down (-0.5..0.5)
    #[arg(long, default_value_t = 0.0, allow_negative_numbers = true)]
    gaze_offset_y: f32,
    /// How far the gaze goes left and right, from --gaze-offset-x (0.5..2)
    #[arg(long, default_value_t = 1.0)]
    gaze_gain_x: f32,
    /// How far the gaze goes up, from --gaze-offset-y (0.5..2)
    #[arg(long, default_value_t = 1.0)]
    gaze_gain_up: f32,
    /// How far the gaze goes down, from --gaze-offset-y (0.5..2)
    #[arg(long, default_value_t = 1.0)]
    gaze_gain_down: f32,
    /// How far the headset sits tilted, in degrees (-20..20; positive: looking right reads higher). Undone
    /// around the zero point, before the gains
    #[arg(long, default_value_t = 0.0, allow_negative_numbers = true)]
    gaze_roll_deg: f32,
    /// Hold the sideways gaze when looking more than this many degrees down (fully 10° further down),
    /// where the Frame's x jumps; 0 disables
    #[arg(long, default_value_t = 24.0)]
    gaze_down_hold_x_deg: f32,
    /// The left eye's own sideways zero point for per-eye gaze [default: --gaze-offset-x]
    #[arg(long, allow_negative_numbers = true)]
    gaze_offset_x_left: Option<f32>,
    /// The right eye's own sideways zero point for per-eye gaze [default: --gaze-offset-x]
    #[arg(long, allow_negative_numbers = true)]
    gaze_offset_x_right: Option<f32>,
    /// The left eye's own sideways gain for per-eye gaze [default: --gaze-gain-x]
    #[arg(long)]
    gaze_gain_x_left: Option<f32>,
    /// The right eye's own sideways gain for per-eye gaze [default: --gaze-gain-x]
    #[arg(long)]
    gaze_gain_x_right: Option<f32>,
    /// Settings file, re-read while running; options given here win over it
    /// [default: ~/.config/frameeyeosc/config.json]
    #[arg(long)]
    config: Option<PathBuf>,
    /// Write the eye server's raw samples to this CSV file until stopped, instead of sending anything
    #[arg(long, value_name = "FILE", conflicts_with = "replay")]
    record: Option<PathBuf>,
    /// Run a recorded CSV file through the processing and print how the output behaves, instead of sending
    #[arg(long, value_name = "FILE")]
    replay: Option<PathBuf>,
    /// With --replay, also write every processed sample to this CSV file
    #[arg(long, value_name = "FILE", requires = "replay")]
    replay_out: Option<PathBuf>,
}

/// One Euro filter: smooths hard while the signal is still and loosens up as it moves fast.
#[derive(Clone, Copy)]
struct OneEuro {
    min_cutoff: f32,
    beta: f32,
    d_cutoff: f32,
    value: Option<f32>,
    velocity: f32,
}

impl OneEuro {
    fn new(min_cutoff: f32, beta: f32, d_cutoff: f32) -> Self {
        Self {
            min_cutoff,
            beta,
            d_cutoff,
            value: None,
            velocity: 0.0,
        }
    }

    fn alpha(cutoff: f32, dt: f32) -> f32 {
        let tau = 1.0 / (2.0 * std::f32::consts::PI * cutoff);
        1.0 / (1.0 + tau / dt)
    }

    fn filter(&mut self, x: f32, dt: f32) -> f32 {
        let Some(prev) = self.value else {
            self.value = Some(x);
            return x;
        };
        self.velocity += Self::alpha(self.d_cutoff, dt) * ((x - prev) / dt - self.velocity);
        let cutoff = self.min_cutoff + self.beta * self.velocity.abs();
        let y = prev + Self::alpha(cutoff, dt) * (x - prev);
        self.value = Some(y);
        y
    }

    fn reset(&mut self) {
        self.value = None;
        self.velocity = 0.0;
    }
}

/// Backlash deadzone: the output only moves once the input drifts more than `width` away,
/// which pins the eyes during fixation without adding delay to large movements.
#[derive(Clone, Copy)]
struct Deadzone {
    width: f32,
    value: Option<f32>,
}

impl Deadzone {
    fn new(width: f32) -> Self {
        Self { width, value: None }
    }

    fn apply(&mut self, x: f32) -> f32 {
        let y = self
            .value
            .map_or(x, |held| held.clamp(x - self.width, x + self.width));
        self.value = Some(y);
        y
    }

    fn reset(&mut self) {
        self.value = None;
    }
}

/// Filters for the six gaze values and two eyelids, clocked by the eye server's sample time.
struct Smoother {
    gaze: [OneEuro; 6],
    deadzones: [Deadzone; 6],
    lids: [OneEuro; 2],
    // The previous two samples' gaze angles and openness, for the 3-sample median.
    recent: [Option<[f32; 8]>; 2],
    // Sample time until which each eyelid is kept fully closed, and whether it went out closed last time.
    shut_until: [f64; 2],
    was_shut: [bool; 2],
    last_time: Option<f64>,
    // Left, right and combined gaze as last sent, to hold while the gaze is unreliable.
    last_gaze: [Option<[f32; 2]>; 3],
    // The up/down gaze the eyelid fit last used, kept while the gaze is held.
    lid_vertical: Option<f32>,
    // Left, right and combined raw x from the last sample above --gaze-down-hold-x-deg.
    down_hold_x: Option<[f32; 3]>,
}

impl Smoother {
    fn new(settings: &Settings) -> Self {
        Self {
            gaze: [OneEuro::new(settings.gaze_min_cutoff, settings.gaze_beta, settings.gaze_d_cutoff); 6],
            deadzones: [Deadzone::new(settings.gaze_deadzone); 6],
            lids: [OneEuro::new(settings.lid_min_cutoff, settings.lid_beta, LID_D_CUTOFF); 2],
            recent: [None; 2],
            shut_until: [f64::NEG_INFINITY; 2],
            was_shut: [false; 2],
            last_time: None,
            last_gaze: [None; 3],
            lid_vertical: None,
            down_hold_x: None,
        }
    }

    /// Take new filter parameters without dropping the filters' state, so the output does not jump.
    fn configure(&mut self, settings: &Settings) {
        for filter in &mut self.gaze {
            filter.min_cutoff = settings.gaze_min_cutoff;
            filter.beta = settings.gaze_beta;
            filter.d_cutoff = settings.gaze_d_cutoff;
        }
        for filter in &mut self.lids {
            filter.min_cutoff = settings.lid_min_cutoff;
            filter.beta = settings.lid_beta;
        }
        for deadzone in &mut self.deadzones {
            deadzone.width = settings.gaze_deadzone;
        }
    }

    /// Move the clock to `time` and return the time since the previous sample; a gap starts the filters over.
    fn advance(&mut self, time: f64) -> f32 {
        let dt = match self.last_time {
            Some(last) if time > last && time - last < MAX_GAP => (time - last) as f32,
            Some(last) if time <= last => NOMINAL_DT,
            _ => {
                self.reset();
                NOMINAL_DT
            }
        };
        // Set after the match: reset() clears last_time, and every later sample would reset again.
        self.last_time = Some(time);
        dt
    }

    /// Median of this sample and the previous two: a one-sample dropout disappears, and everything
    /// else comes out one sample (~11 ms) later.
    fn despike(&mut self, values: [f32; 8]) -> [f32; 8] {
        let median = match self.recent {
            [Some(older), Some(old)] => std::array::from_fn(|i| median3(older[i], old[i], values[i])),
            _ => values,
        };
        self.recent = [self.recent[1], Some(values)];
        median
    }

    /// `hold` keeps the left, right and combined gaze as last sent while it is unreliable: when the eyes
    /// are mostly shut, where the Frame's gaze jumps around, or when its covariance says so.
    fn filter(&mut self, dt: f32, gaze: &mut [f32; 6], lids: &mut [f32; 2], hold: [bool; 3]) {
        for (pair, held) in hold.into_iter().enumerate() {
            let values = &mut gaze[2 * pair..2 * pair + 2];
            match self.last_gaze[pair] {
                Some(last) if held => values.copy_from_slice(&last),
                _ => {
                    for (index, value) in (2 * pair..).zip(values.iter_mut()) {
                        *value = self.deadzones[index].apply(self.gaze[index].filter(*value, dt));
                    }
                    self.last_gaze[pair] = Some([values[0], values[1]]);
                }
            }
        }
        for (value, filter) in lids.iter_mut().zip(&mut self.lids) {
            *value = filter.filter(*value, dt);
        }
    }

    /// Which eyelids go out fully closed: `closed` ones (at or past --lid-closed), and both when one is
    /// closed and the other nearly so. Once an eye goes out closed it stays so for at least
    /// --blink-hold-ms, so a 30 ms blink is sent as 80 ms and a 200 ms one as 200 ms. Their filters
    /// restart from closed, so the eye opens smoothly afterwards.
    fn hold_shut(&mut self, time: f64, closed: [bool; 2], lids: [f32; 2], settings: &Settings) -> [bool; 2] {
        let hold = f64::from(settings.blink_hold_ms) / 1000.0;
        let held = [0, 1].map(|eye| closed[eye] || time < self.shut_until[eye]);
        let shut = sync_blinks(held, lids, settings.blink_sync_below);
        for (eye, shut) in shut.into_iter().enumerate() {
            if shut && !self.was_shut[eye] {
                self.shut_until[eye] = time + hold;
            }
            if shut {
                self.lids[eye].value = Some(0.0);
            }
        }
        self.was_shut = shut;
        shut
    }

    fn reset(&mut self) {
        self.gaze.iter_mut().chain(&mut self.lids).for_each(OneEuro::reset);
        self.deadzones.iter_mut().for_each(Deadzone::reset);
        self.recent = [None; 2];
        self.shut_until = [f64::NEG_INFINITY; 2];
        self.was_shut = [false; 2];
        self.last_time = None;
        self.last_gaze = [None; 3];
        self.lid_vertical = None;
        self.down_hold_x = None;
    }

    /// Below --gaze-down-hold-x-deg the Frame's sideways gaze jumps (by ~19° to the right when looking
    /// ~40° down, 2026-09-28), so the left, right and combined x fade into their values from just before
    /// the gaze went that low: not at all at the threshold, fully 10° further down. Judged on the raw
    /// combined vertical gaze, before the zero point and gains, so the threshold is the tracker's own
    /// degrees whatever the fit. Starting out that low, the held x is straight ahead as fitted. Only
    /// `trusted` eyes' x (open and reliable; the combined x if either is) are remembered, so a blink or an
    /// unreliable sample just before looking down is not what gets held.
    fn hold_down_x(&mut self, raw_gaze: [f32; 6], trusted: [bool; 2], settings: &Settings) -> [f32; 6] {
        let threshold = settings.gaze_down_hold_x_deg;
        let x = [raw_gaze[0], raw_gaze[2], raw_gaze[4]];
        let down_deg = -raw_gaze[5] * 45.0;
        let ahead = [
            settings.gaze_offset_x_left.unwrap_or(settings.gaze_offset_x),
            settings.gaze_offset_x_right.unwrap_or(settings.gaze_offset_x),
            settings.gaze_offset_x,
        ];
        if threshold <= 0.0 || down_deg <= threshold {
            let keep = [trusted[0], trusted[1], trusted[0] || trusted[1]];
            let mut remembered = self.down_hold_x.unwrap_or(ahead);
            for ((slot, value), keep) in remembered.iter_mut().zip(x).zip(keep) {
                if keep {
                    *slot = value;
                }
            }
            self.down_hold_x = Some(remembered);
            return raw_gaze;
        }
        let held = self.down_hold_x.unwrap_or(ahead);
        let live = (1.0 - (down_deg - threshold) / DOWN_HOLD_FADE_DEG).clamp(0.0, 1.0);
        let mut out = raw_gaze;
        for (pair, (held, x)) in held.into_iter().zip(x).enumerate() {
            out[pair * 2] = live * x + (1.0 - live) * held;
        }
        out
    }
}

fn median3(a: f32, b: f32, c: f32) -> f32 {
    a.max(b).min(a.min(b).max(c))
}

/// Learns each eye's relaxed openness while in use, so a face that opens one eye less than the
/// other still maps both eyes' normal state onto --lid-open.
#[derive(Clone)]
struct LidCalibration {
    histograms: [Vec<f32>; 2],
    relaxed: [f32; 2],
    decay: f32,
    path: Option<PathBuf>,
    saved: [f32; 2],
    last_save: Instant,
}

impl LidCalibration {
    /// Start from the saved calibration if there is one, else assume both eyes relax at `default`.
    fn load(path: Option<PathBuf>, default: f32) -> Self {
        let saved = path
            .as_deref()
            .and_then(|path| fs::read_to_string(path).ok())
            .map_or([default; 2], |text| parse_calibration(&text, default));
        Self {
            histograms: [vec![0.0; CAL_BINS], vec![0.0; CAL_BINS]],
            relaxed: saved,
            decay: 0.5_f32.powf(1.0 / CAL_HALF_LIFE_SAMPLES),
            path,
            saved,
            last_save: Instant::now(),
        }
    }

    fn observe(&mut self, openness: [f32; 2]) {
        let decay = self.decay;
        for ((histogram, relaxed), reading) in self
            .histograms
            .iter_mut()
            .zip(&mut self.relaxed)
            .zip(openness)
        {
            histogram.iter_mut().for_each(|weight| *weight *= decay);
            if reading > CAL_GATE * *relaxed && reading < 1.0 {
                histogram[((reading / CAL_BIN_WIDTH) as usize).min(CAL_BINS - 1)] += 1.0;
            }
            let total: f32 = histogram.iter().sum();
            if total < CAL_WARMUP_WEIGHT {
                continue;
            }
            let mut cumulative = 0.0;
            if let Some(bin) = histogram.iter().position(|weight| {
                cumulative += weight;
                cumulative >= total / 2.0
            }) {
                *relaxed = (bin as f32 + 0.5) * CAL_BIN_WIDTH;
            }
        }
    }

    /// Per-eye multipliers that bring each eye's relaxed openness to `lid_open`.
    fn scales(&self, lid_open: f32) -> [f32; 2] {
        self.relaxed
            .map(|relaxed| (lid_open / relaxed).clamp(CAL_SCALE_RANGE.0, CAL_SCALE_RANGE.1))
    }

    fn save_if_due(&mut self) {
        if self.path.is_none() || self.last_save.elapsed() < CAL_SAVE_INTERVAL {
            return;
        }
        self.last_save = Instant::now();
        if self
            .relaxed
            .iter()
            .zip(&self.saved)
            .all(|(now, saved)| (now - saved).abs() < 0.001)
        {
            return;
        }
        self.save();
    }

    fn save(&mut self) {
        let Some(path) = &self.path else {
            return;
        };
        match write_calibration(path, self.relaxed) {
            Ok(()) => {
                let [left, right] = self.relaxed;
                eprintln!("Saved eyelid calibration: left relaxes at {left:.3}, right at {right:.3}");
                self.saved = self.relaxed;
            }
            Err(error) => eprintln!("Could not save {}: {error}", path.display()),
        }
    }

    /// Forget what was learned and start over from `default`, saving that right away.
    fn reset(&mut self, default: f32) {
        self.histograms.iter_mut().for_each(|histogram| histogram.fill(0.0));
        self.relaxed = [default; 2];
        self.save();
    }
}

/// Saved as `left_relaxed=0.818` / `right_relaxed=0.777` lines; anything unreadable falls back to `default`.
fn parse_calibration(text: &str, default: f32) -> [f32; 2] {
    let value = |key: &str| {
        text.lines()
            .find_map(|line| line.strip_prefix(key)?.strip_prefix('=')?.trim().parse::<f32>().ok())
            .filter(|value| (0.3..=1.2).contains(value))
            .unwrap_or(default)
    };
    [value("left_relaxed"), value("right_relaxed")]
}

fn write_calibration(path: &Path, [left, right]: [f32; 2]) -> io::Result<()> {
    if let Some(dir) = path.parent() {
        fs::create_dir_all(dir)?;
    }
    let temporary = path.with_extension("tmp");
    fs::write(
        &temporary,
        format!("left_relaxed={left:.4}\nright_relaxed={right:.4}\n"),
    )?;
    fs::rename(temporary, path)
}

/// A fitted eye's expected open reading for an up/down gaze (-1..1): a line through its down, straight
/// ahead and up readings, carried on past them, and never below half the straight-ahead one.
fn expected_open(fit: &LidFit, vertical: f32) -> f32 {
    let slope = if vertical >= 0.0 { fit.up - fit.open } else { fit.open - fit.down };
    (fit.open + slope * vertical / LID_FIT_PITCH).max(0.5 * fit.open)
}

/// A fitted eye's openness on the --lid-closed / --lid-open scale: its closed reading (plus a margin)
/// maps to --lid-closed and its expected open reading for where the eyes look maps to --lid-open.
/// Higher readings go on past --lid-open toward widening as before. The range is at least half the
/// straight-ahead one, so far down, where the expected reading nears the closed one, a small wobble
/// does not flip the lid between open and shut.
fn fitted_openness(openness: f32, vertical: f32, fit: &LidFit, settings: &Settings) -> f32 {
    let range = open_reading(fit, vertical) - fit.closed;
    let fraction = (openness - fit.closed) / range;
    let fraction = (fraction - LID_FIT_CLOSED_MARGIN) / (1.0 - LID_FIT_CLOSED_MARGIN);
    settings.lid_closed + fraction * (settings.lid_open - settings.lid_closed)
}

/// The reading fitted_openness maps to --lid-open: the expected open reading, or further up far down, where the
/// range keeps its minimum (half the straight-ahead one). Relaxed open is this reading, and widening counts from it.
fn open_reading(fit: &LidFit, vertical: f32) -> f32 {
    fit.closed + (expected_open(fit, vertical) - fit.closed).max(0.5 * (fit.open - fit.closed))
}

/// Where widening starts and is full above a fitted eye's expected open reading, for --lid-widen (None: off).
fn widen_offsets(widen: Widen) -> Option<(f32, f32)> {
    match widen {
        Widen::Off => None,
        Widen::Low => Some(WIDEN_LOW),
        Widen::Normal => Some(WIDEN_NORMAL),
        Widen::High => Some(WIDEN_HIGH),
    }
}

/// Whether a fitted eye can widen by itself: widening on, and its straight-ahead open reading leaves room below
/// where the openness stops (see WIDEN_ROOM_LIMIT). Judged on the straight-ahead reading, not where the eyes look, so
/// an eye does not switch between widening itself and following the other one as the gaze moves.
fn widen_room(fit: &LidFit, widen: Widen) -> bool {
    widen_offsets(widen).is_some_and(|(start, _)| fit.open + start <= WIDEN_ROOM_LIMIT)
}

/// A fitted eye's openness on the --lid-* scale. Up to its open reading for where the eyes look (open_reading): as
/// fitted_openness (closed..open -> --lid-closed..--lid-open, VRCFT 0..0.75). Above it, with room to widen: up to
/// `start` above it onto --lid-open..--lid-widen-start (VRCFT stays 0.75), then up to `full` above it (or 1.000,
/// where the openness stops, if that comes first) onto --lid-widen-start..--lid-wide, so lid_to_vrcft gives 0.75
/// rising to 1 whatever marks 3 and 4 are, and carries on past it (clamped at 1). Without room, or with widening off,
/// it stays at --lid-open above the open reading (VRCFT 0.75; lid_inputs may lend it the other eye's widening).
/// Continuous and never falling: widening starts where the base mapping reaches --lid-open, which far down (where
/// fitted_openness keeps a minimum range) is above the expected reading. Starting there from the expected reading
/// instead made a step: 45° down with an expected 0.43, 0.430 sent VRCFT 0.31 and 0.431 sent 0.75.
fn fitted_lid(openness: f32, vertical: f32, fit: &LidFit, settings: &Settings) -> f32 {
    let expected = open_reading(fit, vertical);
    if openness <= expected {
        return fitted_openness(openness, vertical, fit, settings);
    }
    let Settings {
        lid_open,
        lid_widen_start,
        lid_wide,
        ..
    } = *settings;
    let Some((start, full)) = widen_offsets(settings.lid_widen).filter(|_| widen_room(fit, settings.lid_widen)) else {
        return lid_open;
    };
    let start_at = expected + start;
    // Looking up raises the expected reading, and the openness still stops at 1.000: keep some range
    let full_at = (expected + full).min(OPENNESS_CAP).max(start_at + 0.01);
    if openness <= start_at {
        lid_open + (lid_widen_start - lid_open) * (openness - expected) / start
    } else {
        lid_widen_start + (lid_wide - lid_widen_start) * (openness - start_at) / (full_at - start_at)
    }
}

/// Each eye's openness on the --lid-* scale, times the eye's scale (see lid_scales).
///
/// A fitted eye goes through fitted_lid. The scale then multiplies that value: the fitted openness is on the same
/// scale that --lid-closed / --lid-open compare against (the fit maps its closed reading to --lid-closed and its
/// expected open reading to --lid-open), so a scale of 0.9 makes that eye read 10% less open. It closes sooner and
/// widens less (it reaches marks 3 and 4 only further open; with the default marks 0.97 takes a full widen to about
/// 60%). 1.0 changes nothing.
///
/// A fitted eye without room to widen (see widen_room), while the other fitted eye has room, takes the other eye's
/// value whenever it reads at or above its own open reading (open_reading, where its own lid reaches relaxed open) (widening is almost always both eyes), but
/// never less than relaxed open (the other eye blinking or winking does not close it); below its open reading it
/// follows its own lid. Its own scale then applies. Neither with room: no widening.
///
/// An eye without an eye fit keeps the lid marks: its raw openness times its scale, widening between
/// --lid-widen-start and --lid-wide (--lid-widen does not apply, having no measured open reading to start from).
fn lid_inputs(openness: [f32; 2], vertical: f32, scales: [f32; 2], settings: &Settings) -> [f32; 2] {
    let fits = settings.lid_fit();
    let own = [0, 1].map(|eye| fits[eye].as_ref().map(|fit| fitted_lid(openness[eye], vertical, fit, settings)));
    [0, 1].map(|eye| {
        let (Some(fit), Some(value)) = (&fits[eye], own[eye]) else {
            return openness[eye] * scales[eye];
        };
        let other = 1 - eye;
        let borrowed = match (&fits[other], own[other]) {
            (Some(other_fit), Some(other_value))
                if !widen_room(fit, settings.lid_widen)
                    && widen_room(other_fit, settings.lid_widen)
                    && openness[eye] >= open_reading(fit, vertical) =>
            {
                Some(other_value.max(settings.lid_open))
            }
            _ => None,
        };
        borrowed.unwrap_or(value) * scales[eye]
    })
}

/// Blend the two eyelids together in proportion to how close they already are:
/// equal lids stay equal, small asymmetries fade out, and a wink (large difference) is left alone.
fn sync_lids([left, right]: [f32; 2], threshold: f32) -> [f32; 2] {
    if threshold <= 0.0 {
        return [left, right];
    }
    let weight = (1.0 - (left - right).abs() / threshold).clamp(0.0, 1.0);
    let average = (left + right) / 2.0;
    [left + weight * (average - left), right + weight * (average - right)]
}

/// Close both eyes when one is `shut` and the other's eyelid is below `below`: a blink the Frame caught
/// fully in one eye only. A wink, with the other eye open, passes through. 0 disables.
fn sync_blinks(shut: [bool; 2], lids: [f32; 2], below: f32) -> [bool; 2] {
    let [left, right] = shut;
    if below > 0.0 && ((left && lids[1] < below) || (right && lids[0] < below)) {
        [true; 2]
    } else {
        shut
    }
}

/// Whether each eye's gaze is reliable enough to use: both variances of its own (pre-fusion) estimate
/// must be at most `limit`. A missing or non-finite covariance counts as unreliable. 0 disables.
fn gaze_quality(data: &EyeData, limit: f32) -> [bool; 2] {
    if limit <= 0.0 {
        return [true; 2];
    }
    data.pre_fusion_covariance.map(|[x, y, _]| x <= limit && y <= limit)
}

/// The six gaze values to send (left x/y, right x/y, combined x/y) from the same layout of readings.
/// Each eye wobbles on its own (L/R changes correlate only ~0.35), so both share the combined gaze
/// unless --independent-eyes is given. An eye with unreliable gaze is left out of the combined gaze and the other eye stands in for it;
/// without --independent-eyes that also moves both eyes.
fn choose_gaze(angles: [f32; 6], reliable: [bool; 2], independent: bool) -> [f32; 6] {
    let [left_x, left_y, right_x, right_y, x, y] = angles;
    let [x, y] = match reliable {
        [true, false] => [left_x, left_y],
        [false, true] => [right_x, right_y],
        _ => [x, y],
    };
    if independent {
        [left_x, left_y, right_x, right_y, x, y]
    } else {
        [x, y, x, y, x, y]
    }
}

/// Map Frame eye openness onto VRCFT EyeLid, where 0 is closed, 0.75 relaxed open and 1 widened.
/// A held-closed eye reads ~0.2 on the Frame rather than 0, hence the closed threshold.
/// A relaxed eye wanders between ~0.75 and ~0.9, so widening only starts past a deadzone.
fn lid_to_vrcft(openness: f32, settings: &Settings) -> f32 {
    let Settings {
        lid_closed,
        lid_open,
        lid_widen_start,
        lid_wide,
        ..
    } = *settings;
    if openness <= lid_open {
        0.75 * ((openness - lid_closed) / (lid_open - lid_closed)).clamp(0.0, 1.0)
    } else if openness > lid_widen_start && lid_wide > lid_widen_start {
        let widen = (openness - lid_widen_start) / (lid_wide - lid_widen_start);
        0.75 + 0.25 * widen.clamp(0.0, 1.0)
    } else {
        0.75
    }
}

/// The ETVR Tracking Module treats 1.0 as a relaxed open eye, so widening is cut off there.
fn lid_to_etvr(vrcft: f32) -> f32 {
    (vrcft * ETVR_LID_SCALE).clamp(0.0, 1.0)
}

struct EyeSource {
    map: MmapMut,
    inode: u64,
    // Offset of the eye record for this file's version.
    eye_data: usize,
}

struct MutexGuard(*mut libc::pthread_mutex_t);

impl Drop for MutexGuard {
    fn drop(&mut self) {
        unsafe { libc::pthread_mutex_unlock(self.0) };
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
struct EyeData {
    sample_time: f64,
    gaze: [[f32; 3]; 2],
    // Diagonal of each eye's gaze covariance, after and before stereo fusion.
    gaze_covariance: [[f32; 3]; 2],
    fixation_point: [f32; 3],
    pre_fusion_gaze: [[f32; 3]; 2],
    pre_fusion_covariance: [[f32; 3]; 2],
    openness: [f32; 2],
    // Not yet understood; only recorded.
    extra: [f32; 8],
}

impl EyeData {
    fn is_finite(&self) -> bool {
        self.sample_time.is_finite()
            && self.gaze.iter().flatten().all(|value| value.is_finite())
            && self.fixation_point.iter().all(|value| value.is_finite())
            && self.openness.iter().all(|value| value.is_finite())
    }
}

enum Next {
    /// Nothing new within the timeout.
    Waiting,
    /// A new record, but the eye tracker is not producing.
    Stopped,
    Sample(EyeData),
}

impl EyeSource {
    fn open() -> Result<Self, Box<dyn Error>> {
        let file = OpenOptions::new().read(true).write(true).open(SOURCE)?;
        let metadata = file.metadata()?;
        let too_small = || format!("{SOURCE}: shared memory is too small");
        let len = usize::try_from(metadata.len()).map_err(|_| "shared memory is too large")?;
        if len < size_of::<EyeServerHeader>() {
            return Err(too_small().into());
        }
        // The whole file is mapped: its version, and so how much of it is needed, is only known once mapped.
        let map = unsafe { MmapOptions::new().len(len).map_mut(&file)? };
        let header: *const EyeServerHeader = map.as_ptr().cast();
        let version = u32::from_le(unsafe { ptr::read_volatile(&raw const (*header).version) });
        let Some(shm) = SHM_LAYOUTS.iter().find(|layout| layout.version == version) else {
            let known: Vec<String> = SHM_LAYOUTS.iter().map(|layout| layout.version.to_string()).collect();
            return Err(format!(
                "unsupported eye shared-memory version {version}; expected {}",
                known.join(" or ")
            )
            .into());
        };
        if len < shm.size {
            return Err(too_small().into());
        }
        if u32::from_le(unsafe { ptr::read_volatile(&raw const (*header).initialized) }) != 1 {
            return Err("eye shared memory is not initialized".into());
        }
        Ok(Self {
            map,
            inode: metadata.ino(),
            eye_data: shm.eye_data,
        })
    }

    /// True when the path now points at a different file (or none) than the one we mapped.
    fn is_stale(&self) -> bool {
        fs::metadata(SOURCE).map_or(true, |metadata| metadata.ino() != self.inode)
    }

    fn layout(&self) -> *const EyeServerHeader {
        self.map.as_ptr().cast()
    }

    fn layout_mut(&mut self) -> *mut EyeServerHeader {
        self.map.as_mut_ptr().cast()
    }

    fn eye_data(&self) -> *const EyeDataMmap {
        // In bounds: open() checked the file against this version's size, which covers the record.
        unsafe { self.map.as_ptr().add(self.eye_data).cast() }
    }

    fn lock(&mut self) -> io::Result<MutexGuard> {
        let mutex = unsafe { (&raw mut (*self.layout_mut()).metadata_mutex).cast() };
        let code = unsafe { libc::pthread_mutex_lock(mutex) };
        if code == libc::EOWNERDEAD {
            let result = unsafe { libc::pthread_mutex_consistent(mutex) };
            if result != 0 {
                unsafe { libc::pthread_mutex_unlock(mutex) };
                return Err(io::Error::from_raw_os_error(result));
            }
        } else if code != 0 {
            return Err(io::Error::from_raw_os_error(code));
        }
        Ok(MutexGuard(mutex))
    }

    fn next(&mut self, timeout: Duration) -> io::Result<Next> {
        let guard = self.lock()?;
        let sequence_ptr = unsafe { &raw const (*self.layout()).sequence };
        let sequence = unsafe { ptr::read_volatile(sequence_ptr) };
        let request_ptr = unsafe { &raw mut (*self.layout_mut()).metadata_requested };
        unsafe { ptr::write_volatile(request_ptr, 1) };
        drop(guard);

        let timespec = libc::timespec {
            tv_sec: timeout.as_secs() as libc::time_t,
            tv_nsec: timeout.subsec_nanos() as libc::c_long,
        };
        let result = unsafe {
            libc::syscall(
                libc::SYS_futex,
                sequence_ptr,
                libc::FUTEX_WAIT,
                sequence,
                &timespec as *const libc::timespec,
            )
        };
        if result == -1 {
            let error = io::Error::last_os_error();
            if !matches!(
                error.raw_os_error(),
                Some(libc::EAGAIN | libc::EINTR | libc::ETIMEDOUT)
            ) {
                return Err(error);
            }
        }

        let guard = self.lock()?;
        let data = if unsafe { ptr::read_volatile(sequence_ptr) } != sequence {
            let record_ptr = self.eye_data();
            let record = unsafe { ptr::read_unaligned(record_ptr) };
            if record.producer_state == 1 {
                Next::Sample(EyeData {
                    sample_time: record.sample_time,
                    gaze: record.gaze_direction,
                    gaze_covariance: record.gaze_covariance_diag,
                    fixation_point: record.fixation_point,
                    pre_fusion_gaze: record.pre_fusion_gaze,
                    pre_fusion_covariance: record.pre_fusion_cov_diag,
                    openness: record.openness,
                    extra: record.estimate_extra,
                })
            } else {
                Next::Stopped
            }
        } else {
            Next::Waiting
        };
        drop(guard);
        Ok(data)
    }
}

/// Where OSC goes: a fixed host, or the PC that Steam Link is currently streaming from.
#[derive(Clone, PartialEq)]
enum Target {
    Fixed { host: String, port: u16 },
    SteamLink { port: u16 },
}

impl Target {
    fn of(settings: &Settings) -> Self {
        let port = settings.port();
        if settings.host == "auto" {
            Self::SteamLink { port }
        } else {
            Self::Fixed {
                host: settings.host.clone(),
                port,
            }
        }
    }
}

// At most one "failed" / "works again" pair of lines this often, so a flapping link can't log a line per packet.
const SEND_LOG_COOLDOWN: Duration = Duration::from_secs(5);

/// Whether sending works, so a failure (no network yet, say) is logged when it starts and when it
/// ends, and never stops the process.
#[derive(Default)]
struct SendHealth {
    failing: Option<String>,
    // The current failure was logged (so its end is too).
    logged: bool,
    // When the last "failed" line went out, and the changes not logged since.
    last_logged: Option<Instant>,
    quiet: u32,
}

impl SendHealth {
    /// Note how one send went; returns the line to log, if any. "Connection refused" (nothing
    /// listening on the PC, e.g. VRChat closed) is neither a failure nor a recovery: it comes back
    /// on every other packet.
    fn record(&mut self, now: Instant, addr: SocketAddr, result: &io::Result<usize>) -> Option<String> {
        match result {
            Err(error) if error.kind() == io::ErrorKind::ConnectionRefused => None,
            Err(error) => {
                let reason = error.to_string();
                let changed = self.failing.as_ref() != Some(&reason);
                if changed {
                    self.failing = Some(reason.clone());
                    self.logged = false;
                }
                if self.logged {
                    return None;
                }
                if self.last_logged.is_some_and(|last| now.duration_since(last) < SEND_LOG_COOLDOWN) {
                    self.quiet += u32::from(changed);
                    return None;
                }
                self.logged = true;
                self.last_logged = Some(now);
                let quiet = std::mem::take(&mut self.quiet);
                let note = if quiet > 0 { format!(" ({quiet} more changes in the last few seconds not logged)") } else { String::new() };
                Some(format!("Sending OSC to {addr} failed ({reason}); retrying with every sample{note}"))
            }
            Ok(_) => {
                self.failing.take()?;
                // Only the end of a failure that was logged; the others are counted
                if std::mem::take(&mut self.logged) {
                    Some(format!("Sending OSC to {addr} works again"))
                } else {
                    self.quiet += 1;
                    None
                }
            }
        }
    }
}

/// UDP sender that re-resolves its target periodically and reconnects when it changes.
struct Output {
    target: Target,
    socket: Option<(UdpSocket, SocketAddr)>,
    last_resolve: Option<Instant>,
    health: SendHealth,
    // Why the socket to the target could not be set up, or the host could not be looked up, so each
    // is logged once per reason.
    connect_error: Option<String>,
    resolve_error: Option<String>,
}

impl Output {
    fn new(target: Target) -> Self {
        Self {
            target,
            socket: None,
            last_resolve: None,
            health: SendHealth::default(),
            connect_error: None,
            resolve_error: None,
        }
    }

    /// Switch to another target, looking it up on the next refresh instead of up to 5 s later.
    fn set_target(&mut self, target: Target) {
        if target != self.target {
            self.target = target;
            self.last_resolve = None;
        }
    }

    /// Look the target up again when due. Never fails: without a network the socket stays unset and
    /// the next look-up (RESOLVE_INTERVAL later) tries again.
    fn refresh(&mut self) {
        if let Some(resolved) = self.last_resolve {
            // A fixed host is looked up once (like before the config file existed) unless that failed.
            let settled = matches!(self.target, Target::Fixed { .. }) && self.socket.is_some();
            if settled || resolved.elapsed() < RESOLVE_INTERVAL {
                return;
            }
        }
        self.last_resolve = Some(Instant::now());
        let wanted = match &self.target {
            Target::Fixed { host, port } => match (host.as_str(), *port).to_socket_addrs() {
                Ok(mut addrs) => {
                    if self.resolve_error.take().is_some() {
                        eprintln!("Resolved {host} again");
                    }
                    addrs.next()
                }
                Err(error) => {
                    let reason = error.to_string();
                    if self.resolve_error.as_ref() != Some(&reason) {
                        eprintln!("Could not resolve {host}: {reason}; retrying every {} s", RESOLVE_INTERVAL.as_secs());
                        self.resolve_error = Some(reason);
                    }
                    None
                }
            },
            Target::SteamLink { port } => steam_link_peer().map(|ip| SocketAddr::new(ip, *port)),
        };
        if wanted == self.socket.as_ref().map(|(_, addr)| *addr) {
            return;
        }
        self.socket = match wanted {
            Some(addr) => match connect(addr) {
                Ok(socket) => {
                    eprintln!("Sending OSC to {addr}");
                    self.connect_error = None;
                    self.health = SendHealth::default();
                    Some((socket, addr))
                }
                // e.g. "Network is unreachable" while Wi-Fi comes up after boot
                Err(error) => {
                    let reason = error.to_string();
                    if self.connect_error.as_ref() != Some(&reason) {
                        eprintln!("Can't send OSC to {addr} yet ({reason}); retrying every {} s", RESOLVE_INTERVAL.as_secs());
                        self.connect_error = Some(reason);
                    }
                    None
                }
            },
            None if matches!(self.target, Target::SteamLink { .. }) => {
                eprintln!("No Steam Link connection found; waiting for one");
                None
            }
            None => None,
        };
    }

    fn addr(&self) -> Option<SocketAddr> {
        self.socket.as_ref().map(|(_, addr)| *addr)
    }

    /// Send one message. A network error (unreachable, refused, no address yet...) is logged when it
    /// starts and ends, and never returned: the next sample simply tries again.
    fn send(&mut self, addr: String, args: Vec<OscType>) -> Result<(), Box<dyn Error>> {
        if self.socket.is_none() {
            return Ok(());
        }
        let packet = OscPacket::Message(OscMessage { addr, args });
        self.send_datagram(&encoder::encode(&packet)?);
        Ok(())
    }

    /// Send one datagram as it is (an encoded OSC message or a Live Link packet); errors as in `send`.
    fn send_datagram(&mut self, datagram: &[u8]) {
        let Some((socket, target)) = &self.socket else {
            return;
        };
        let result = socket.send(datagram);
        if let Some(line) = self.health.record(Instant::now(), *target, &result) {
            eprintln!("{line}");
        }
    }
}

/// How many of these times fall in the last RATE_WINDOW (one second): a rate a second.
fn per_second(times: &VecDeque<Instant>) -> f32 {
    times.iter().filter(|time| time.elapsed() < RATE_WINDOW).count() as f32
}

/// A UDP socket connected to `addr` (connecting only sets where packets go; it fails without a route).
fn connect(addr: SocketAddr) -> io::Result<UdpSocket> {
    let socket = UdpSocket::bind(if addr.is_ipv4() { "0.0.0.0:0" } else { "[::]:0" })?;
    socket.connect(addr)?;
    Ok(socket)
}

/// The PC Steam Link is streaming from: the remote end of the `vrlink` client's connected UDP socket.
/// Over the bundled wireless adapter this is the PC's side of the direct link, not its home LAN address.
fn steam_link_peer() -> Option<IpAddr> {
    let inodes = vrlink_socket_inodes();
    if inodes.is_empty() {
        return None;
    }
    ["/proc/net/udp", "/proc/net/udp6"].iter().find_map(|path| {
        let table = fs::read_to_string(path).ok()?;
        connected_udp_peer(&table, &inodes)
    })
}

/// Socket inodes held by the Steam Link client process. Matched by executable name, because its
/// main thread renames itself (comm reads "vrlinkrunthread").
fn vrlink_socket_inodes() -> HashSet<u64> {
    let Ok(processes) = fs::read_dir("/proc") else {
        return HashSet::new();
    };
    processes
        .flatten()
        .filter(|process| {
            fs::read_link(process.path().join("exe"))
                .is_ok_and(|exe| exe.file_name().is_some_and(|name| name == "vrlink"))
        })
        .filter_map(|process| fs::read_dir(process.path().join("fd")).ok())
        .flatten()
        .flatten()
        .filter_map(|fd| {
            let link = fs::read_link(fd.path()).ok()?;
            link.to_str()?
                .strip_prefix("socket:[")?
                .strip_suffix(']')?
                .parse()
                .ok()
        })
        .collect()
}

/// Remote address of the first connected (state 01), non-loopback socket in a /proc/net/udp{,6}
/// table whose inode is in `inodes`.
fn connected_udp_peer(table: &str, inodes: &HashSet<u64>) -> Option<IpAddr> {
    table.lines().skip(1).find_map(|line| {
        let fields: Vec<&str> = line.split_whitespace().collect();
        let (remote, state, inode) = (fields.get(2)?, fields.get(3)?, fields.get(9)?);
        if *state != "01" || !inodes.contains(&inode.parse().ok()?) {
            return None;
        }
        let ip = parse_proc_ip(remote.split(':').next()?)?;
        (!ip.is_loopback() && !ip.is_unspecified()).then_some(ip)
    })
}

/// /proc/net writes addresses as 32-bit words in host (little-endian) byte order, in hex.
fn parse_proc_ip(hex: &str) -> Option<IpAddr> {
    if !hex.len().is_multiple_of(8) {
        return None;
    }
    let words = (0..hex.len() / 8)
        .map(|i| u32::from_str_radix(hex.get(i * 8..i * 8 + 8)?, 16).ok())
        .collect::<Option<Vec<u32>>>()?;
    match words[..] {
        [word] => Some(IpAddr::V4(Ipv4Addr::from(word.to_le_bytes()))),
        [_, _, _, _] => {
            let mut bytes = [0u8; 16];
            for (chunk, word) in bytes.chunks_mut(4).zip(&words) {
                chunk.copy_from_slice(&word.to_le_bytes());
            }
            let ip = Ipv6Addr::from(bytes);
            Some(ip.to_ipv4_mapped().map_or(IpAddr::V6(ip), IpAddr::V4))
        }
        _ => None,
    }
}

// Like Steam Link's OSC sender, ±45° maps to ±1; +Y is up (VRCFT convention, unverified on hardware).
fn gaze_angles([x, y, z]: [f32; 3]) -> [f32; 2] {
    let scale = 4.0 / std::f32::consts::PI;
    [
        (x.atan2(-z) * scale).clamp(-1.0, 1.0),
        (y.atan2(-z) * scale).clamp(-1.0, 1.0),
    ]
}

/// Move each gaze pair's zero point to --gaze-offset-x/y, undo the headset's tilt (--gaze-roll-deg) around it,
/// and scale how far it goes from there; up and down have their own gains, and each eye's x may have its own zero
/// point and gain (the combined x always uses the shared ones). The defaults leave the gaze exactly as it is.
fn correct_gaze(angles: [f32; 6], settings: &Settings) -> [f32; 6] {
    let eye_offsets = [settings.gaze_offset_x_left, settings.gaze_offset_x_right];
    let eye_gains = [settings.gaze_gain_x_left, settings.gaze_gain_x_right];
    // Tilted by θ, looking along the headset's own level line reads as (d, d·tanθ); turning back by θ makes it level
    let (sin, cos) = settings.gaze_roll_deg.to_radians().sin_cos();
    let mut corrected = [0.0; 6];
    for pair in 0..3 {
        let offset = eye_offsets.get(pair).copied().flatten().unwrap_or(settings.gaze_offset_x);
        let gain_x = eye_gains.get(pair).copied().flatten().unwrap_or(settings.gaze_gain_x);
        let dx = angles[pair * 2] - offset;
        let dy = angles[pair * 2 + 1] - settings.gaze_offset_y;
        // No tilt: exactly the values from before there was one
        let (x, y) = if settings.gaze_roll_deg == 0.0 {
            (dx, dy)
        } else {
            (dx * cos + dy * sin, -dx * sin + dy * cos)
        };
        let gain_y = if y >= 0.0 {
            settings.gaze_gain_up
        } else {
            settings.gaze_gain_down
        };
        corrected[pair * 2] = (x * gain_x).clamp(-1.0, 1.0);
        corrected[pair * 2 + 1] = (y * gain_y).clamp(-1.0, 1.0);
    }
    corrected
}

/// One eye-server sample worked through the eyelid mapping and the filters.
struct Sample {
    openness: [f32; 2],
    // After each eye's scale: what the --lid-* thresholds are compared against.
    openness_scaled: [f32; 2],
    // Left x/y, right x/y and combined x/y in -1..1, before smoothing.
    raw_gaze: [f32; 6],
    // The same layout, as sent.
    gaze: [f32; 6],
    // VRCFT eyelids, as sent to VRChat.
    lids: [f32; 2],
    // Whether each eye's gaze passed the quality check.
    reliable: [bool; 2],
    // Whether the combined gaze was held (eyes mostly shut, or both eyes unreliable). Only the tests read
    // it now; a gaze capture judges its samples itself (capture::usable).
    #[cfg_attr(not(test), allow(dead_code))]
    gaze_held: bool,
}

/// Per-eye multipliers as lid_inputs applies them. An unfitted eye: the fixed one, else the learned one, else 1.
/// A fitted eye: the fixed one as a fine-tune after the fit, else 1 (never the learned one, which fitted eyes
/// do not use).
fn lid_scales(settings: &Settings, calibration: &LidCalibration) -> [f32; 2] {
    let learned = if settings.lid_calibration {
        calibration.scales(settings.lid_open)
    } else {
        [1.0; 2]
    };
    let fits = settings.lid_fit();
    let fixed = [settings.lid_scale_left, settings.lid_scale_right];
    [0, 1].map(|eye| match (&fits[eye], fixed[eye]) {
        // A settings file from 0.5.x or earlier (no lid_widen in it): those versions ignored the scale of a fitted
        // eye, so an old value there (1.15, say) would suddenly make that eye 15% more open. The panel clears such
        // scales when it first reads the file and writes lid_widen, which ends this
        (Some(_), Some(_)) if settings.scales_predate_fit => 1.0,
        (Some(_), fixed) => fixed.unwrap_or(1.0),
        (None, fixed) => fixed.unwrap_or(learned[eye]),
    })
}

/// Let the eyelid calibration learn from a sample once `settled`, then work the sample through.
fn step(
    settings: &Settings,
    smoother: &mut Smoother,
    calibration: &mut LidCalibration,
    data: &EyeData,
    settled: bool,
) -> Sample {
    // Openness read while the gaze is unreliable is suspect too, so it does not teach the calibration.
    // Fitted eyes do not use it.
    if settings.lid_calibration
        && settled
        && settings.lid_fit().iter().any(Option::is_none)
        && gaze_quality(data, settings.gaze_quality_limit) == [true; 2]
    {
        calibration.observe(data.openness);
    }
    process(settings, smoother, lid_scales(settings, calibration), data)
}

fn process(settings: &Settings, smoother: &mut Smoother, scales: [f32; 2], data: &EyeData) -> Sample {
    let [x, y] = gaze_angles(data.fixation_point);
    let [left, right] = data.gaze.map(gaze_angles);
    let raw_gaze = [left[0], left[1], right[0], right[1], x, y];
    let reliable = gaze_quality(data, settings.gaze_quality_limit);
    let blink_stages = settings.blink_hold_ms > 0.0 || settings.blink_sync_below > 0.0;
    let (gaze, lids, gaze_held, openness_scaled) = if settings.raw {
        let corrected = correct_gaze(raw_gaze, settings);
        let gaze = choose_gaze(corrected, [true; 2], settings.independent_eyes);
        let openness_scaled = lid_inputs(data.openness, corrected[5], scales, settings);
        let mapped = openness_scaled.map(|openness| lid_to_vrcft(openness, settings));
        let shut = sync_blinks(mapped.map(|lid| lid <= 0.0), mapped, settings.blink_sync_below);
        let mut lids = sync_lids(mapped, settings.lid_sync);
        if blink_stages {
            shut_lids(&mut lids, shut);
        }
        let shut_eyes = data.openness.iter().any(|openness| *openness < settings.gaze_hold_below);
        (gaze, lids, shut_eyes, openness_scaled)
    } else {
        let dt = smoother.advance(data.sample_time);
        // Before anything else, so the filters see the gaze the way it will be sent.
        let open = data.openness.iter().all(|openness| *openness >= settings.gaze_hold_below);
        let trusted = reliable.map(|reliable| reliable && open);
        let corrected = correct_gaze(smoother.hold_down_x(raw_gaze, trusted, settings), settings);
        let readings: [f32; 8] =
            std::array::from_fn(|i| if i < 6 { corrected[i] } else { data.openness[i - 6] });
        // Fed even while off, so turning it on does not start from an empty history.
        let despiked = smoother.despike(readings);
        let readings = if settings.despike { despiked } else { readings };
        let angles: [f32; 6] = std::array::from_fn(|i| readings[i]);
        let openness = [readings[6], readings[7]];
        let mut gaze = choose_gaze(angles, reliable, settings.independent_eyes);
        let hold = if openness.iter().any(|openness| *openness < settings.gaze_hold_below)
            || reliable == [false; 2]
        {
            [true; 3]
        } else if settings.independent_eyes {
            [!reliable[0], !reliable[1], false]
        } else {
            [false; 3]
        };
        // Where the eyes look up or down, for the eyelid fit: kept while blinking, like the gaze.
        let vertical = match smoother.lid_vertical {
            Some(held) if hold[2] => held,
            _ => gaze[5],
        };
        smoother.lid_vertical = Some(vertical);
        let mapped = lid_inputs(openness, vertical, scales, settings).map(|openness| lid_to_vrcft(openness, settings));
        let mut lids = mapped;
        smoother.filter(dt, &mut gaze, &mut lids, hold);
        let mut lids = sync_lids(lids, settings.lid_sync);
        if blink_stages {
            let shut = smoother.hold_shut(data.sample_time, mapped.map(|lid| lid <= 0.0), mapped, settings);
            shut_lids(&mut lids, shut);
        }
        (gaze, lids, hold[2], lid_inputs(data.openness, vertical, scales, settings))
    };
    Sample {
        openness: data.openness,
        openness_scaled,
        raw_gaze,
        gaze,
        lids,
        reliable,
        gaze_held,
    }
}

fn shut_lids(lids: &mut [f32; 2], shut: [bool; 2]) {
    for (lid, shut) in lids.iter_mut().zip(shut) {
        if shut {
            *lid = 0.0;
        }
    }
}

/// Eyelids on the scale of the given output. LiveLink gets the VRCFT eyelids, split into blink and widening in the
/// packet (VRCFT puts the same eyelid back together).
fn output_lids(output: OutputKind, lids: [f32; 2]) -> [f32; 2] {
    match output {
        OutputKind::Vrchat | OutputKind::LiveLink => lids,
        OutputKind::Etvr => lids.map(lid_to_etvr),
    }
}

/// The Live Link packet for one sample, stamped `time` seconds into the day. Each eye's gaze is its own with
/// "move eyes separately", else the combined one (as for the other outputs). Nothing else goes to LiveLink: no
/// EyeTrackingActive and no parameter prefix, which only VRChat reads.
fn livelink_packet(sample: &Sample, time: f64) -> Vec<u8> {
    let [left_x, left_y, right_x, right_y, _, _] = sample.gaze;
    let lids = output_lids(OutputKind::LiveLink, sample.lids);
    livelink::packet(time, &livelink::shapes(lids, [left_x, left_y, right_x, right_y]))
}

/// The OSC messages for one sample. VRChat gets the full VRCFT v2 eye set. The ETVR Tracking Module
/// gets per-eye values only: EyeX/EyeY switch it to a single-eye mode that reads an eyelid we do not send.
fn osc_messages(settings: &Settings, sample: &Sample) -> Vec<(String, OscType)> {
    let prefix = format!("/avatar/parameters{}", settings.prefix);
    let [left_x, left_y, right_x, right_y, x, y] = sample.gaze;
    let [lid_left, lid_right] = output_lids(settings.output, sample.lids);
    let mut values = vec![
        ("EyeLeftX", left_x),
        ("EyeLeftY", left_y),
        ("EyeRightX", right_x),
        ("EyeRightY", right_y),
        ("EyeLidLeft", lid_left),
        ("EyeLidRight", lid_right),
    ];
    let mut messages = Vec::with_capacity(9);
    if settings.output == OutputKind::Vrchat {
        messages.extend(active_message(settings, true));
        values.extend([("EyeX", x), ("EyeY", y)]);
    }
    messages.extend(
        values
            .into_iter()
            .map(|(suffix, value)| (format!("{prefix}/v2/{suffix}"), OscType::Float(value))),
    );
    messages
}

/// `EyeTrackingActive` as the settings want it sent (a bool, a float, or nothing).
fn active_message(settings: &Settings, active: bool) -> Option<(String, OscType)> {
    let value = match settings.eye_tracking_active {
        ActiveType::Bool => OscType::Bool(active),
        ActiveType::Float => OscType::Float(if active { 1.0 } else { 0.0 }),
        ActiveType::Off => return None,
    };
    Some((format!("/avatar/parameters{}/EyeTrackingActive", settings.prefix), value))
}

/// The one-time "not active" on pausing, switching away or losing tracking (nothing with "off").
fn send_inactive(output: &mut Output, settings: &Settings) -> Result<(), Box<dyn Error>> {
    match active_message(settings, false) {
        Some((addr, value)) => output.send(addr, vec![value]),
        None => Ok(()),
    }
}

/// Where VRChat is being told that eye tracking is active, if anywhere. When this changes,
/// the old destination gets a final "inactive" so the avatar's eyes do not freeze.
fn vrchat_stream(settings: &Settings) -> Option<(&str, u16, &str, ActiveType)> {
    (settings.sending && settings.output == OutputKind::Vrchat).then(|| {
        (settings.host.as_str(), settings.port(), settings.prefix.as_str(), settings.eye_tracking_active)
    })
}

/// Lets a packet through at most once per interval, on a steady beat that neither bursts nor drifts: the next one is
/// due an interval after the last one was due, or after now if the samples paused for longer than that.
#[derive(Default)]
struct Throttle {
    next: Option<Instant>,
}

impl Throttle {
    /// Whether a packet may go out at `now` (it is then counted as sent).
    fn ready(&mut self, now: Instant, interval: Duration) -> bool {
        if self.next.is_some_and(|next| now < next) {
            return false;
        }
        self.next = Some(match self.next {
            Some(next) if now.duration_since(next) < interval => next + interval,
            _ => now + interval,
        });
        true
    }
}

/// Where VRCFT's LiveLink module is being sent to, if anywhere. When this changes while eye tracking runs, the old
/// destination gets a neutral packet (relaxed open eyes, straight ahead), since the module keeps the last values.
fn livelink_stream(settings: &Settings) -> Option<(&str, u16)> {
    (settings.sending && settings.output == OutputKind::LiveLink).then(|| (settings.host.as_str(), settings.port()))
}

/// Why the samples stopped, for the log line when tracking is lost.
fn lost_reason(next: &Next) -> String {
    match next {
        Next::Stopped => "eye server not producing".into(),
        Next::Sample(_) => "unreadable sample".into(),
        Next::Waiting => format!("no new samples for {} s", TIMEOUT.as_secs()),
    }
}

/// Everything the main loop keeps between samples.
struct Bridge {
    config: Config,
    settings: Settings,
    output: Output,
    smoother: Smoother,
    calibration: LidCalibration,
    started: SystemTime,
    active_since: Option<Instant>,
    last_data: Option<Instant>,
    latest: Option<Sample>,
    // When the samples of the last RATE_WINDOW went out, and when they came in from the tracker.
    sent: VecDeque<Instant>,
    received: VecDeque<Instant>,
    // The gaze capture the panel asked for, while it runs, and the latest one's result.
    capture: Option<Capture>,
    capture_result: Option<CaptureResult>,
    // The panel's debug gaze dots (only while gaze_debug_dots is on).
    dots: dots::DotStream,
    // When the last neutral Live Link packet went out.
    livelink_neutral: Option<Instant>,
    // Keeps the Live Link samples at or below LIVELINK_MAX_HZ.
    livelink_throttle: Throttle,
}

impl Bridge {
    /// Take changed settings without restarting.
    fn apply(&mut self, reload: Reload) -> Result<(), Box<dyn Error>> {
        let Reload {
            settings,
            reset_calibration,
            gaze_capture,
        } = reload;
        if let Some(request) = gaze_capture {
            eprintln!("Gaze capture {} ({}) asked for", request.id, request.target);
            let capture = Capture::new(request);
            self.capture_result = Some(capture.result(CaptureState::Running));
            self.capture = Some(capture);
        }
        let stream = vrchat_stream(&self.settings);
        if self.active_since.is_some() && stream.is_some() && stream != vrchat_stream(&settings) {
            send_inactive(&mut self.output, &self.settings)?;
        }
        let stream = livelink_stream(&self.settings);
        if self.active_since.is_some() && stream.is_some() && stream != livelink_stream(&settings) {
            self.send_livelink_neutral();
        }
        self.output.set_target(Target::of(&settings));
        self.smoother.configure(&settings);
        if reset_calibration {
            eprintln!("Starting eyelid calibration over");
            self.calibration.reset(settings.lid_open);
        }
        self.settings = settings;
        Ok(())
    }

    fn on_sample(&mut self, data: EyeData) -> Result<(), Box<dyn Error>> {
        let now = Instant::now();
        let previous = self.last_data.replace(now);
        if self.active_since.is_none() {
            match previous {
                Some(last) => eprintln!(
                    "Eye tracking resumed after {:.1} s",
                    now.duration_since(last).as_secs_f32()
                ),
                None => eprintln!("Eye tracking started"),
            }
        }
        let since = *self.active_since.get_or_insert(now);
        let settled = since.elapsed() >= CAL_SETTLE;
        let sample = step(&self.settings, &mut self.smoother, &mut self.calibration, &data, settled);
        if self.settings.lid_calibration && settled {
            self.calibration.save_if_due();
        }
        if self.settings.sending {
            let sent = if self.settings.output == OutputKind::LiveLink {
                // The newest sample, at most LIVELINK_MAX_HZ times a second; the ones in between are dropped
                let interval = Duration::from_secs(1) / LIVELINK_MAX_HZ;
                let ready = self.livelink_throttle.ready(now, interval);
                if ready {
                    self.output.send_datagram(&livelink_packet(&sample, livelink::time_of_day(SystemTime::now())));
                }
                ready
            } else {
                for (addr, arg) in osc_messages(&self.settings, &sample) {
                    self.output.send(addr, vec![arg])?;
                }
                true
            };
            if sent && self.output.addr().is_some() {
                self.sent.push_back(now);
            }
        }
        self.received.push_back(now);
        for times in [&mut self.sent, &mut self.received] {
            while times.front().is_some_and(|time| time.elapsed() >= RATE_WINDOW) {
                times.pop_front();
            }
        }
        let dot = dots::DotSample {
            time: data.sample_time,
            gaze: sample.gaze,
            independent: self.settings.independent_eyes,
        };
        self.dots.send(self.settings.gaze_debug_dots, &dot);
        if let Some(capture) = &mut self.capture {
            let gaze = [sample.raw_gaze[4], sample.raw_gaze[5]];
            let eye_x = [sample.raw_gaze[0], sample.raw_gaze[2]];
            let usable = capture::usable(data.openness, sample.reliable);
            if capture.add(data.sample_time, gaze, eye_x, data.openness, usable) {
                self.finish_capture();
            }
        }
        self.latest = Some(sample);
        Ok(())
    }

    /// End the running gaze capture, if it has waited too long for samples.
    fn check_capture(&mut self) {
        if self.capture.as_ref().is_some_and(Capture::timed_out) {
            self.finish_capture();
        }
    }

    fn finish_capture(&mut self) {
        if let Some(capture) = self.capture.take() {
            let result = capture.result(CaptureState::Done);
            eprintln!("{}", result.log_line());
            self.capture_result = Some(result);
        }
    }

    fn on_lost(&mut self, reason: &str) -> Result<(), Box<dyn Error>> {
        eprintln!("Eye tracking stopped ({reason})");
        if vrchat_stream(&self.settings).is_some() {
            send_inactive(&mut self.output, &self.settings)?;
        }
        if livelink_stream(&self.settings).is_some() {
            self.send_livelink_neutral();
        }
        self.smoother.reset();
        self.active_since = None;
        self.latest = None;
        Ok(())
    }

    /// Relaxed open eyes looking straight ahead, to VRCFT's LiveLink module.
    fn send_livelink_neutral(&mut self) {
        self.output.send_datagram(&livelink::packet(livelink::time_of_day(SystemTime::now()), &livelink::neutral()));
        self.livelink_neutral = Some(Instant::now());
    }

    /// While sending to VRCFT's LiveLink module without eye data (the headset off, the eye server stopped), keep
    /// sending it the neutral packet, so the module can start and does not log "connection lost" over and over.
    /// Paused, nothing goes out.
    fn keep_livelink_alive(&mut self) {
        let due = self.livelink_neutral.is_none_or(|sent| sent.elapsed() >= LIVELINK_IDLE_INTERVAL);
        if self.active_since.is_none() && livelink_stream(&self.settings).is_some() && due {
            self.send_livelink_neutral();
        }
    }

    fn status(&self) -> Status<'_> {
        let settings = &self.settings;
        let pair = |values: [f32; 6], first: usize| status::round([values[first], values[first + 1]]);
        Status {
            version: 1,
            pid: std::process::id(),
            time: status::unix_time(SystemTime::now()),
            started: status::unix_time(self.started),
            sending: settings.sending,
            output: settings.output,
            target_mode: if settings.host == "auto" { "auto" } else { "fixed" },
            target: self.output.addr().map(|addr| addr.to_string()),
            rate: per_second(&self.sent),
            tracker_rate: self
                .active_since
                .is_some_and(|since| since.elapsed() >= RATE_WINDOW)
                .then(|| per_second(&self.received)),
            tracking: self.active_since.is_some(),
            raw: self.latest.as_ref().map(|sample| RawValues {
                openness: status::round(sample.openness),
                openness_scaled: status::round(sample.openness_scaled),
                gaze: pair(sample.raw_gaze, 4),
                gaze_left: pair(sample.raw_gaze, 0),
                gaze_right: pair(sample.raw_gaze, 2),
            }),
            sent: self.latest.as_ref().map(|sample| SentValues {
                lids: status::round(output_lids(settings.output, sample.lids)),
                lids_vrcft: status::round(sample.lids),
                gaze: pair(sample.gaze, 4),
                gaze_left: pair(sample.gaze, 0),
                gaze_right: pair(sample.gaze, 2),
            }),
            calibration: CalibrationStatus {
                enabled: settings.lid_calibration,
                relaxed: status::round(self.calibration.relaxed),
                scales: status::round(lid_scales(settings, &self.calibration)),
                fitted: settings.lid_fit().map(|fit| fit.is_some()),
                learning: settings.lid_calibration
                    && settings.lid_fit().iter().any(Option::is_none)
                    && self
                        .active_since
                        .is_some_and(|since| since.elapsed() >= CAL_SETTLE),
            },
            config_path: self.config.path.as_deref(),
            calibration_path: self.calibration.path.as_deref(),
            config_error: self.config.error.as_deref(),
            locked: &self.config.locked,
            effective: settings,
            gaze_capture: self.capture_result.as_ref(),
        }
    }
}

/// Set by SIGINT / SIGTERM while recording, so the recording ends with every sample so far written out.
static STOP_RECORDING: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

extern "C" fn stop_recording(_: libc::c_int) {
    STOP_RECORDING.store(true, std::sync::atomic::Ordering::Relaxed);
}

/// Write every sample the eye server produces to `path` until stopped (Ctrl+C, SIGINT or SIGTERM: the file is then
/// complete up to the stop). Nothing is sent, and the status and calibration files are left alone, so this can run
/// next to the installed service (the panel's "Eye log" runs it).
fn record(path: &Path) -> Result<(), Box<dyn Error>> {
    // SAFETY: the handler only stores to an atomic, which is async-signal-safe.
    unsafe {
        let mut action: libc::sigaction = std::mem::zeroed();
        action.sa_sigaction = stop_recording as extern "C" fn(libc::c_int) as libc::sighandler_t;
        libc::sigemptyset(&mut action.sa_mask);
        for signal in [libc::SIGINT, libc::SIGTERM] {
            libc::sigaction(signal, &action, std::ptr::null_mut());
        }
    }
    let mut recorder = replay::Recorder::create(path)?;
    let mut source = EyeSource::open()?;
    eprintln!("Recording {SOURCE} to {}; stop with Ctrl+C", path.display());
    let mut last_flush = Instant::now();
    let mut reported = 0;
    loop {
        if STOP_RECORDING.load(std::sync::atomic::Ordering::Relaxed) {
            recorder.flush()?;
            eprintln!("Stopped: {} samples in {}", recorder.count, path.display());
            return Ok(());
        }
        match source.next(POLL)? {
            Next::Sample(data) => recorder.write(&data)?,
            Next::Waiting | Next::Stopped if source.is_stale() => {
                eprintln!("{SOURCE} was replaced; reopening");
                source = EyeSource::open()?;
            }
            _ => {}
        }
        // Flushed every second, so being killed loses at most that much (a stop signal loses nothing).
        if last_flush.elapsed() >= Duration::from_secs(1) {
            recorder.flush()?;
            last_flush = Instant::now();
            if recorder.count / 900 != reported {
                reported = recorder.count / 900;
                eprintln!("{} samples", recorder.count);
            }
        }
    }
}

fn main() -> Result<(), Box<dyn Error>> {
    let matches = Args::command().get_matches();
    let args = Args::from_arg_matches(&matches)?;
    if let Some(path) = &args.record {
        return record(path);
    }
    if args.target != "auto" && config::split_target(&args.target).is_none() {
        return Err("--target must be HOST:PORT or auto".into());
    }
    let in_config_dir = |name: &str| Some(config::config_dir()?.join(name));
    let calibration_path = args.calibration_file.clone().or_else(|| in_config_dir("calibration"));
    let config_path = args.config.clone().or_else(|| in_config_dir("config.json"));
    let replay = args.replay.clone().map(|input| (input, args.replay_out.clone()));
    let mut config = Config::new(config_path, args, config::given_options(&matches));
    let settings = config.load()?;
    if let Some((input, output)) = replay {
        // Starts from the saved calibration like a real run, but never writes it back.
        let mut calibration = LidCalibration::load(calibration_path, settings.lid_open);
        calibration.path = None;
        return replay::run(&input, output.as_deref(), &settings, &calibration);
    }
    let mut bridge = Bridge {
        output: Output::new(Target::of(&settings)),
        smoother: Smoother::new(&settings),
        calibration: LidCalibration::load(calibration_path, settings.lid_open),
        config,
        settings,
        started: SystemTime::now(),
        active_since: None,
        last_data: None,
        latest: None,
        sent: VecDeque::new(),
        received: VecDeque::new(),
        capture: None,
        capture_result: None,
        dots: dots::DotStream::new(status::status_path().parent().unwrap_or(Path::new("/tmp"))),
        livelink_neutral: None,
        livelink_throttle: Throttle::default(),
    };
    let mut status_file = StatusFile::new(status::status_path());
    let mut source = EyeSource::open()?;
    eprintln!("Reading {SOURCE}");
    // Whether the last attempt to reattach to a replaced shared memory failed, so it is logged once.
    let mut reopen_failed = false;
    loop {
        if let Some(reload) = bridge.config.poll() {
            bridge.apply(reload)?;
        }
        bridge.output.refresh();
        match source.next(POLL)? {
            Next::Sample(data) if data.is_finite() => bridge.on_sample(data)?,
            // Short waits are normal; only a whole second without data means tracking stopped.
            Next::Waiting if bridge.last_data.is_some_and(|last| last.elapsed() < TIMEOUT) => {}
            next if bridge.active_since.is_some() => bridge.on_lost(&lost_reason(&next))?,
            // Idle: if the eye server recreated its shared memory, our mapping would go silent forever.
            // While the new one is missing or not set up yet, keep trying instead of exiting.
            _ if source.is_stale() => match EyeSource::open() {
                Ok(reopened) => {
                    eprintln!("{SOURCE} was replaced; reattached");
                    source = reopened;
                    reopen_failed = false;
                }
                Err(error) if !reopen_failed => {
                    eprintln!("{SOURCE} was replaced and can't be opened yet ({error}); retrying");
                    reopen_failed = true;
                }
                Err(_) => {}
            },
            _ => {}
        }
        bridge.check_capture();
        bridge.keep_livelink_alive();
        if status_file.due() {
            status_file.write(&bridge.status());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn settings() -> Settings {
        Settings::default()
    }

    fn apply(smoother: &mut Smoother, time: f64, gaze: &mut [f32; 6], lids: &mut [f32; 2], hold: bool) {
        let dt = smoother.advance(time);
        smoother.filter(dt, gaze, lids, [hold; 3]);
    }

    /// Both eyes looking along (x, y) at sample `index`, with the given Frame openness.
    fn reading(index: usize, [x, y]: [f32; 2], openness: [f32; 2]) -> EyeData {
        let direction = [x, y, -1.0];
        EyeData {
            sample_time: index as f64 * f64::from(NOMINAL_DT),
            gaze: [direction; 2],
            fixation_point: direction,
            pre_fusion_gaze: [direction; 2],
            openness,
            ..EyeData::default()
        }
    }

    fn run(settings: &Settings, readings: &[EyeData]) -> Vec<Sample> {
        let mut smoother = Smoother::new(settings);
        readings
            .iter()
            .map(|data| process(settings, &mut smoother, [1.0; 2], data))
            .collect()
    }

    /// Open eyes, except for the samples listed with their openness.
    fn openness_track(length: usize, changes: &[(usize, [f32; 2])]) -> Vec<EyeData> {
        (0..length)
            .map(|i| {
                let openness = changes.iter().find(|(at, _)| *at == i).map_or([0.8; 2], |(_, open)| *open);
                reading(i, [0.0, 0.0], openness)
            })
            .collect()
    }

    fn without_new_stages() -> Settings {
        Settings {
            gaze_quality_limit: 0.0,
            blink_hold_ms: 0.0,
            despike: false,
            blink_sync_below: 0.0,
            ..settings()
        }
    }

    #[test]
    fn gaze_quality_reads_each_eyes_own_covariance() {
        let mut data = reading(0, [0.0, 0.0], [0.8; 2]);
        data.pre_fusion_covariance = [[0.01, 0.02, 0.5], [0.01, 0.05, 0.0]];
        // The z variance does not count.
        assert_eq!(gaze_quality(&data, 0.03), [true, false]);
        assert_eq!(gaze_quality(&data, 0.0), [true, true]);
        data.pre_fusion_covariance[0][0] = f32::NAN;
        assert_eq!(gaze_quality(&data, 0.03), [false, false]);
    }

    #[test]
    fn unreliable_eye_gives_way_to_the_other() {
        let angles = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0];
        assert_eq!(choose_gaze(angles, [true; 2], false), [5.0, 6.0, 5.0, 6.0, 5.0, 6.0]);
        assert_eq!(choose_gaze(angles, [true, false], false), [1.0, 2.0, 1.0, 2.0, 1.0, 2.0]);
        assert_eq!(choose_gaze(angles, [false, true], true), [1.0, 2.0, 3.0, 4.0, 3.0, 4.0]);
        assert_eq!(choose_gaze(angles, [false; 2], false), [5.0, 6.0, 5.0, 6.0, 5.0, 6.0]);
    }

    #[test]
    fn gaze_is_held_while_both_eyes_are_unreliable() {
        let settings = Settings {
            despike: false,
            gaze_quality_limit: 0.03,
            ..settings()
        };
        let mut readings: Vec<EyeData> = (0..10).map(|i| reading(i, [0.2, 0.0], [0.8; 2])).collect();
        let mut wild = reading(10, [-0.5, 0.3], [0.8; 2]);
        wild.pre_fusion_covariance = [[1.0; 3]; 2];
        readings.push(wild);
        let sent = run(&settings, &readings);
        assert_eq!(sent[10].gaze, sent[9].gaze);
        assert_eq!(sent[10].reliable, [false; 2]);

        // With per-eye gaze only the unreliable eye is held; the other one moves both combined values.
        let independent = Settings {
            independent_eyes: true,
            gaze_deadzone: 0.0,
            ..settings
        };
        readings[10].pre_fusion_covariance = [[1.0; 3], [0.0; 3]];
        let sent = run(&independent, &readings);
        assert_eq!(sent[10].gaze[..2], sent[9].gaze[..2]);
        assert!(sent[10].gaze[2] < sent[9].gaze[2] && sent[10].gaze[4] < sent[9].gaze[4]);
    }

    #[test]
    fn blink_hold_keeps_a_short_blink_closed() {
        let settings = Settings {
            despike: false,
            ..settings()
        };
        let readings = openness_track(30, &[(10, [0.2; 2])]);
        let sent = run(&settings, &readings);
        // 80 ms is 7.2 samples: closed at sample 10 and the seven after it.
        assert!(sent[10..18].iter().all(|sample| sample.lids == [0.0; 2]));
        assert!(sent[18].lids[0] > 0.0 && sent[18].lids[0] < 0.75);
        // It is a minimum, not an extension: a blink longer than the hold opens as soon as it ends.
        let long: Vec<(usize, [f32; 2])> = (10..30).map(|i| (i, [0.2; 2])).collect();
        let sent = run(&settings, &openness_track(40, &long));
        assert!(sent[10..30].iter().all(|sample| sample.lids == [0.0; 2]));
        assert!(sent[30].lids[0] > 0.0);
        // Without the new stages the filters never quite get there.
        let sent = run(&without_new_stages(), &readings);
        assert!(sent.iter().all(|sample| sample.lids[0] > 0.2), "{:?}", sent[10].lids);
    }

    #[test]
    fn despike_drops_a_one_sample_dropout() {
        let dropout = openness_track(30, &[(10, [0.2, 0.8])]);
        let sent = run(&settings(), &dropout);
        assert!(sent.iter().all(|sample| sample.lids[0] > 0.7));
        let sent = run(&without_new_stages(), &dropout);
        assert!(sent.iter().any(|sample| sample.lids[0] < 0.5));
        // Two samples are a real (short) blink, and come through one sample late.
        let blink = openness_track(30, &[(10, [0.2; 2]), (11, [0.2; 2])]);
        let sent = run(&settings(), &blink);
        assert!(sent[10].lids[0] > 0.7 && sent[11].lids == [0.0; 2]);
    }

    #[test]
    fn blink_sync_closes_both_but_keeps_winks() {
        assert_eq!(sync_blinks([true, false], [0.0, 0.2], 0.35), [true; 2]);
        assert_eq!(sync_blinks([false, true], [0.3, 0.0], 0.35), [true; 2]);
        assert_eq!(sync_blinks([true, false], [0.0, 0.75], 0.35), [true, false]);
        assert_eq!(sync_blinks([true, false], [0.0, 0.2], 0.0), [true, false]);
        // A wink stays a wink through the whole pipeline.
        let wink: Vec<(usize, [f32; 2])> = (10..30).map(|i| (i, [0.2, 0.8])).collect();
        let sent = run(&settings(), &openness_track(40, &wink));
        assert!(sent[20].lids[0] == 0.0 && sent[20].lids[1] > 0.7);
    }

    #[test]
    fn raw_mode_skips_the_timed_stages() {
        let raw = Settings {
            raw: true,
            ..settings()
        };
        let sent = run(&raw, &openness_track(20, &[(10, [0.2; 2])]));
        assert_eq!(sent[10].lids, [0.0; 2]);
        assert_eq!(sent[11].lids, [0.75; 2]);
    }

    #[test]
    fn gaze_correction_moves_the_zero_point_and_scales_each_direction() {
        let angles = [0.2, 0.3, -0.2, -0.3, 0.1, -0.1];
        assert_eq!(correct_gaze(angles, &settings()), angles);
        let fitted = Settings {
            gaze_offset_x: 0.1,
            gaze_offset_y: -0.1,
            gaze_gain_x: 2.0,
            gaze_gain_up: 1.5,
            gaze_gain_down: 0.5,
            ..settings()
        };
        let corrected = correct_gaze(angles, &fitted);
        let expected = [0.2, 0.6, -0.6, -0.1, 0.0, 0.0];
        for (value, expected) in corrected.iter().zip(expected) {
            assert!((value - expected).abs() < 1e-6, "{corrected:?}");
        }
        // Still within -1..1.
        assert_eq!(correct_gaze([1.0, 1.0, -1.0, -1.0, 0.0, 0.0], &fitted)[..4], [1.0, 1.0, -1.0, -0.45]);
    }

    #[test]
    fn gaze_correction_without_tilt_is_unchanged_to_the_bit() {
        // The correction as it was before gaze_roll_deg
        let before = |angles: [f32; 6], settings: &Settings| -> [f32; 6] {
            let eye_offsets = [settings.gaze_offset_x_left, settings.gaze_offset_x_right];
            let eye_gains = [settings.gaze_gain_x_left, settings.gaze_gain_x_right];
            std::array::from_fn(|i| {
                let value = angles[i];
                let corrected = if i % 2 == 0 {
                    let offset = eye_offsets.get(i / 2).copied().flatten().unwrap_or(settings.gaze_offset_x);
                    let gain = eye_gains.get(i / 2).copied().flatten().unwrap_or(settings.gaze_gain_x);
                    (value - offset) * gain
                } else {
                    let from_center = value - settings.gaze_offset_y;
                    let gain = if from_center >= 0.0 {
                        settings.gaze_gain_up
                    } else {
                        settings.gaze_gain_down
                    };
                    from_center * gain
                };
                corrected.clamp(-1.0, 1.0)
            })
        };
        let fitted = Settings {
            gaze_offset_x: 0.013,
            gaze_offset_y: -0.021,
            gaze_gain_x: 0.93,
            gaze_gain_up: 1.1,
            gaze_gain_down: 0.88,
            gaze_offset_x_left: Some(0.031),
            gaze_gain_x_right: Some(0.9),
            ..settings()
        };
        for settings in [settings(), fitted] {
            for step in -24i32..=24 {
                let a = step as f32 * 0.043;
                let angles = [a, -a * 0.7, a * 0.9, a * 0.3, -a, a, 0.0, -0.0, 0.013, -0.021];
                let angles: [f32; 6] = std::array::from_fn(|i| angles[(i + step.unsigned_abs() as usize) % 10]);
                let now = correct_gaze(angles, &settings).map(f32::to_bits);
                assert_eq!(now, before(angles, &settings).map(f32::to_bits), "{angles:?}");
            }
        }
    }

    #[test]
    fn gaze_correction_undoes_the_tilt_around_the_zero_point() {
        let tilted = Settings {
            gaze_offset_x: 0.02,
            gaze_offset_y: -0.03,
            gaze_roll_deg: 8.0,
            ..settings()
        };
        let tan = 8f32.to_radians().tan();
        let cos = 8f32.to_radians().cos();
        // Looking 20° right along the tilted headset's level line: it reads higher, and comes out level and as far
        let d = 20.0 / 45.0;
        let angles = [0.02 + d, -0.03 + d * tan, 0.02 - d, -0.03 - d * tan, 0.02 + d, -0.03 + d * tan];
        let corrected = correct_gaze(angles, &tilted);
        for (value, expected) in corrected.iter().zip([d / cos, 0.0, -d / cos, 0.0, d / cos, 0.0]) {
            assert!((value - expected).abs() < 1e-6, "{corrected:?}");
        }
        // Straight up along the tilted headset leans left (dx = -sinθ·dy); it comes out straight up
        let up = 15.0 / 45.0;
        let (sin, cos) = 8f32.to_radians().sin_cos();
        let corrected = correct_gaze([0.02 - sin * up, -0.03 + cos * up, 0.0, 0.0, 0.0, 0.0], &tilted);
        assert!(corrected[0].abs() < 1e-6 && (corrected[1] - up).abs() < 1e-6, "{corrected:?}");
        // The zero point stays put, and the gains apply after the turn (up / down by the turned y)
        let fitted = Settings {
            gaze_gain_x: 2.0,
            gaze_gain_up: 1.5,
            gaze_gain_down: 0.5,
            ..tilted.clone()
        };
        assert_eq!(correct_gaze([0.02, -0.03, 0.02, -0.03, 0.02, -0.03], &fitted), [0.0; 6]);
        let corrected = correct_gaze(angles, &fitted);
        assert!((corrected[0] - 2.0 * d / cos).abs() < 1e-6 && corrected[1].abs() < 1e-6, "{corrected:?}");
        let corrected = correct_gaze([0.02, 0.07, 0.02, -0.13, 0.0, 0.0], &fitted);
        assert!((corrected[1] - 1.5 * 0.1 * cos).abs() < 1e-6, "{corrected:?}");
        assert!((corrected[3] + 0.5 * 0.1 * cos).abs() < 1e-6, "{corrected:?}");
        // Each eye turns around its own sideways zero point; the combined gaze around the shared one
        let per_eye = Settings {
            gaze_offset_x_left: Some(0.05),
            gaze_offset_x_right: Some(-0.01),
            ..tilted
        };
        let corrected = correct_gaze([0.05 + d, -0.03 + d * tan, -0.01 + d, -0.03 + d * tan, 0.02, -0.03], &per_eye);
        for (value, expected) in corrected.iter().zip([d / cos, 0.0, d / cos, 0.0, 0.0, 0.0]) {
            assert!((value - expected).abs() < 1e-6, "{corrected:?}");
        }
    }

    #[test]
    fn corrected_gaze_is_sent_and_raw_gaze_reported() {
        let fitted = Settings {
            gaze_offset_y: -0.1,
            gaze_deadzone: 0.0,
            despike: false,
            ..settings()
        };
        let readings: Vec<EyeData> = (0..200).map(|i| reading(i, [0.0, 0.0], [0.8; 2])).collect();
        let sent = run(&fitted, &readings);
        let last = &sent[199];
        assert_eq!(last.raw_gaze[5], 0.0);
        assert!((last.gaze[5] - 0.1).abs() < 1e-3, "{:?}", last.gaze);
        let raw = run(&Settings { raw: true, ..fitted }, &readings);
        assert!((raw[0].gaze[5] - 0.1).abs() < 1e-6);
    }

    fn fitted() -> Settings {
        Settings {
            lid_fit_closed_left: Some(0.15),
            lid_fit_up_left: Some(0.95),
            lid_fit_open_left: Some(0.9),
            lid_fit_down_left: Some(0.7),
            lid_fit_closed_right: Some(0.25),
            lid_fit_up_right: Some(0.85),
            lid_fit_open_right: Some(0.8),
            lid_fit_down_right: Some(0.6),
            ..settings()
        }
    }

    #[test]
    fn expected_openness_follows_the_gaze_up_and_down() {
        let fit = fitted().lid_fit()[0].unwrap();
        assert!((expected_open(&fit, 0.0) - 0.9).abs() < 1e-6);
        assert!((expected_open(&fit, LID_FIT_PITCH) - 0.95).abs() < 1e-6);
        assert!((expected_open(&fit, -LID_FIT_PITCH) - 0.7).abs() < 1e-6);
        // Carried on past the down reading, but never below half the straight-ahead one.
        assert!((expected_open(&fit, -1.5 * LID_FIT_PITCH) - 0.6).abs() < 1e-6);
        assert!((expected_open(&fit, -1.0) - 0.45).abs() < 1e-6);
    }

    #[test]
    fn fitted_eyelids_stay_open_when_looking_down() {
        let settings = fitted();
        let fit = settings.lid_fit()[0].unwrap();
        // Looking 15° down, the reading drops to the fitted down value: still relaxed open.
        let down = fitted_openness(0.7, -LID_FIT_PITCH, &fit, &settings);
        assert!((lid_to_vrcft(down, &settings) - 0.75).abs() < 1e-5, "{down}");
        // The same reading straight ahead is a squint.
        let ahead = lid_to_vrcft(fitted_openness(0.7, 0.0, &fit, &settings), &settings);
        assert!(ahead > 0.4 && ahead < 0.6, "{ahead}");
        // Near the closed reading the eye is shut, straight ahead or looking down.
        assert_eq!(lid_to_vrcft(fitted_openness(0.2, 0.0, &fit, &settings), &settings), 0.0);
        assert_eq!(lid_to_vrcft(fitted_openness(0.2, -LID_FIT_PITCH, &fit, &settings), &settings), 0.0);
        // Wide open still widens.
        assert!(lid_to_vrcft(fitted_openness(1.2, 0.0, &fit, &settings), &settings) > 0.9);
        // Without a fit, the scale applies as before.
        assert_eq!(lid_inputs([0.7, 0.7], -0.3, [1.0, 1.1], &Settings::default()), [0.7, 0.7 * 1.1]);
    }

    #[test]
    fn a_scale_fine_tunes_a_fitted_eye() {
        let settings = fitted();
        let fits = settings.lid_fit();
        let (left, right) = (fits[0].unwrap(), fits[1].unwrap());
        let reading = [0.9, 0.8];
        let plain = [
            fitted_openness(reading[0], 0.0, &left, &settings),
            fitted_openness(reading[1], 0.0, &right, &settings),
        ];
        // Scale 1: the fit alone. 0.69 on the right: that eye reads 31% less open after the fit
        let tuned = lid_inputs(reading, 0.0, [1.0, 0.69], &settings);
        assert_eq!(tuned[0], plain[0]);
        assert!((tuned[1] - plain[1] * 0.69).abs() < 1e-6);
        // So a relaxed right eye is sent less open, and a wide one widens less
        let vrcft = |value: f32| lid_to_vrcft(value, &settings);
        assert!((vrcft(plain[1]) - 0.75).abs() < 1e-5);
        assert!(vrcft(tuned[1]) < 0.5, "{}", vrcft(tuned[1]));
        let wide = lid_inputs([0.9, 0.9], 0.0, [1.0, 1.0], &settings);
        let less_wide = lid_inputs([0.9, 0.9], 0.0, [1.0, 0.95], &settings);
        assert!(vrcft(wide[1]) > 0.75 && vrcft(less_wide[1]) < vrcft(wide[1]), "{:?} {:?}", wide, less_wide);
    }

    /// One user's fit (2026-09-30 logs): the left eye reads 0.945 straight ahead, too close to the Frame's 1.000 to
    /// widen, the right 0.835 with room; their lid marks; widening as given.
    fn user_fit(widen: Widen) -> Settings {
        Settings {
            lid_closed: 0.23,
            lid_open: 0.85,
            lid_widen_start: 0.95,
            lid_wide: 1.03,
            lid_widen: widen,
            lid_fit_closed_left: Some(0.165),
            lid_fit_up_left: Some(0.96),
            lid_fit_open_left: Some(0.945),
            lid_fit_down_left: Some(0.8),
            lid_fit_closed_right: Some(0.26),
            lid_fit_up_right: Some(0.85),
            lid_fit_open_right: Some(0.835),
            lid_fit_down_right: Some(0.7),
            ..settings()
        }
    }

    /// VRCFT eyelids for these readings straight ahead, with these scales.
    fn sent_lids(settings: &Settings, openness: [f32; 2], scales: [f32; 2]) -> [f32; 2] {
        lid_inputs(openness, 0.0, scales, settings).map(|value| lid_to_vrcft(value, settings))
    }

    #[test]
    fn a_fitted_eye_widens_by_its_preset_above_its_open_reading() {
        for (widen, (start, full)) in [(Widen::Low, WIDEN_LOW), (Widen::Normal, WIDEN_NORMAL), (Widen::High, WIDEN_HIGH)] {
            // A right eye reading 0.80 straight ahead (0.82 up), so even "low" is full below 1.000
            let settings = Settings {
                lid_fit_open_right: Some(0.8),
                lid_fit_up_right: Some(0.82),
                ..user_fit(widen)
            };
            let right = |raw: f32| sent_lids(&settings, [0.945, raw], [1.0; 2])[1];
            // At or below the open reading: as fitted_openness, the same for every preset
            let fit = settings.lid_fit()[1].unwrap();
            for raw in [0.3, 0.5, 0.7, 0.8] {
                assert_eq!(right(raw), lid_to_vrcft(fitted_openness(raw, 0.0, &fit, &settings), &settings));
            }
            assert!((right(0.8) - 0.75).abs() < 1e-5);
            // Relaxed up to `start` above it, half widened halfway to `full`, full at `full` and past it
            assert!((right(0.8 + start - 0.001) - 0.75).abs() < 1e-5, "{widen:?}");
            let half = 0.8 + (start + full) / 2.0;
            assert!((right(half) - 0.875).abs() < 1e-3, "{widen:?}: {}", right(half));
            assert!((right(0.8 + full) - 1.0).abs() < 1e-5, "{widen:?}");
            assert_eq!(right(1.0), 1.0);
            // Relative to the expected reading for where the eyes look: looking up the right eye reads 0.82
            let up = lid_inputs([0.96, 0.82 + start - 0.001], LID_FIT_PITCH, [1.0; 2], &settings);
            assert!((lid_to_vrcft(up[1], &settings) - 0.75).abs() < 1e-5);
        }
        // A +0.10 widen: fully with "high", partly with "normal", not with "low"
        let widened = |widen| sent_lids(&user_fit(widen), [0.945, 0.935], [1.0; 2])[1];
        assert_eq!(widened(Widen::High), 1.0);
        assert!(widened(Widen::Normal) > 0.8 && widened(Widen::Normal) < 0.9);
        assert!((widened(Widen::Low) - 0.75).abs() < 1e-5);
        // The user's right eye (0.835) with "low" would be full at 1.015: it is full where the openness stops
        assert_eq!(sent_lids(&user_fit(Widen::Low), [0.945, 1.0], [1.0; 2])[1], 1.0);
        // Off: never above relaxed open, however wide
        let off = user_fit(Widen::Off);
        assert_eq!(sent_lids(&off, [1.0, 1.0], [1.0; 2]), [0.75, 0.75]);
        assert!(sent_lids(&off, [0.3, 0.5], [1.0; 2])[1] < 0.75);
    }

    #[test]
    fn an_eye_without_room_to_widen_follows_the_other_eye() {
        let settings = user_fit(Widen::Normal);
        let fits = settings.lid_fit().map(Option::unwrap);
        assert!(!widen_room(&fits[0], Widen::Normal) && widen_room(&fits[1], Widen::Normal));
        // With "high" the left eye still has no room (0.945 + 0.04 > 0.97); an eye reading 0.90 has with "normal"
        assert!(!widen_room(&fits[0], Widen::High));
        assert!(widen_room(&LidFit { open: 0.9, ..fits[1] }, Widen::Normal));
        assert!(!widen_room(&LidFit { open: 0.91, ..fits[1] }, Widen::Normal));
        // The saturated left eye at 1.000 widens with the right...
        assert_eq!(sent_lids(&settings, [1.0, 0.835 + 0.14], [1.0; 2]), [1.0, 1.0]);
        let half = 0.835 + 0.105;
        let lids = sent_lids(&settings, [0.96, half], [1.0; 2]);
        assert!((lids[0] - lids[1]).abs() < 1e-5 && (lids[0] - 0.875).abs() < 1e-3, "{lids:?}");
        // ...stays relaxed while the right is relaxed or blinks...
        assert!((sent_lids(&settings, [1.0, 0.835], [1.0; 2])[0] - 0.75).abs() < 1e-5);
        assert!((sent_lids(&settings, [1.0, 0.3], [1.0; 2])[0] - 0.75).abs() < 1e-5);
        // ...and below its own open reading follows its own lid
        let closing = sent_lids(&settings, [0.4, 1.0], [1.0; 2]);
        assert_eq!(closing[0], lid_to_vrcft(fitted_openness(0.4, 0.0, &fits[0], &settings), &settings));
        assert!(closing[0] < 0.4);
        // Neither eye with room: no widening at all
        let both = Settings {
            lid_fit_open_right: Some(0.95),
            lid_fit_up_right: Some(0.96),
            ..user_fit(Widen::Normal)
        };
        assert_eq!(sent_lids(&both, [1.0, 1.0], [1.0; 2]), [0.75, 0.75]);
    }

    #[test]
    fn the_scale_still_fine_tunes_a_widening_eye() {
        let settings = user_fit(Widen::Normal);
        let full = 0.835 + 0.14;
        assert_eq!(sent_lids(&settings, [0.945, full], [1.0; 2])[1], 1.0);
        // 0.97 on the right: a full widen reaches (1.03 · 0.97 - 0.95) / 0.08 = 61% of the widening...
        let tuned = sent_lids(&settings, [0.945, full], [1.0, 0.97])[1];
        assert!((tuned - (0.75 + 0.25 * (1.03 * 0.97 - 0.95) / 0.08)).abs() < 1e-4, "{tuned}");
        // ...and it closes sooner: the value on the lid scale times 0.97 everywhere
        let plain = lid_inputs([0.5, 0.5], 0.0, [1.0; 2], &settings);
        assert!((lid_inputs([0.5, 0.5], 0.0, [1.0, 0.97], &settings)[1] - plain[1] * 0.97).abs() < 1e-6);
        // The left eye borrows the right's value and then takes its own scale
        let borrowed = lid_inputs([1.0, full], 0.0, [0.97, 1.0], &settings);
        assert!((borrowed[0] - borrowed[1] * 0.97).abs() < 1e-6);
    }

    #[test]
    fn unfitted_eyes_keep_the_lid_marks_whatever_the_preset() {
        let fitted_left = Settings {
            lid_fit_closed_right: None,
            lid_fit_up_right: None,
            lid_fit_open_right: None,
            lid_fit_down_right: None,
            ..user_fit(Widen::Off)
        };
        for widen in [Widen::Off, Widen::Low, Widen::Normal, Widen::High] {
            let settings = Settings {
                lid_widen: widen,
                ..fitted_left.clone()
            };
            // The right eye (no fit): raw openness times its scale against marks 3 and 4, as before
            assert_eq!(lid_inputs([0.9, 0.99], 0.0, [1.0, 1.1], &settings)[1], 0.99 * 1.1);
            assert_eq!(sent_lids(&settings, [0.9, 0.99], [1.0; 2])[1], lid_to_vrcft(0.99, &settings));
            // Nor does a fitted eye without room borrow from an unfitted one
            assert!((sent_lids(&settings, [1.0, 1.0], [1.0; 2])[0] - 0.75).abs() < 1e-5);
        }
    }

    #[test]
    fn scales_written_before_lid_widen_do_not_act_on_fitted_eyes() {
        let calibration = LidCalibration::load(None, 0.8);
        // A 0.5.x file: a scale next to a fitted left eye (ignored then) and an unfitted right eye's scale
        let old = Settings {
            lid_scale_left: Some(1.15),
            lid_scale_right: Some(0.9),
            lid_fit_closed_right: None,
            lid_fit_up_right: None,
            lid_fit_open_right: None,
            lid_fit_down_right: None,
            scales_predate_fit: true,
            ..user_fit(Widen::Normal)
        };
        assert_eq!(lid_scales(&old, &calibration), [1.0, 0.9]);
        // Once the file says lid_widen, the scale is the fine-tune after the fit
        assert_eq!(lid_scales(&Settings { scales_predate_fit: false, ..old }, &calibration), [1.15, 0.9]);
    }

    #[test]
    fn fitted_eyelids_are_continuous_and_never_fall() {
        let generic = Settings {
            lid_widen: Widen::Normal,
            lid_fit_open_right: Some(0.8),
            lid_fit_up_right: Some(0.82),
            ..fitted()
        };
        let settings_list = [user_fit(Widen::Normal), user_fit(Widen::High), user_fit(Widen::Off), generic];
        for settings in &settings_list {
            let fits = settings.lid_fit();
            for vertical in [-1.0, -0.8, -0.6, -0.45, -0.3, -0.15, 0.0, 0.2, 0.5] {
                for eye in 0..2 {
                    let fit = fits[eye].unwrap();
                    let mut last_value = f32::NAN;
                    let mut last_sent = f32::NAN;
                    // Up to 1.000, where the Frame's openness stops
                    for step in 0..=1000 {
                        let raw = step as f32 * 0.001;
                        let value = fitted_lid(raw, vertical, &fit, settings);
                        let sent = lid_to_vrcft(value, settings);
                        if step > 0 {
                            // Never falls, and no step: 0.001 of raw openness moves it at most 0.01
                            assert!(value >= last_value - 1e-6, "{vertical} eye {eye} at {raw}: {last_value} -> {value}");
                            assert!(value - last_value <= 0.01, "{vertical} eye {eye} at {raw}: {last_value} -> {value}");
                            assert!(sent - last_sent <= 0.02 && sent >= last_sent - 1e-6, "{vertical} {raw}: {last_sent} -> {sent}");
                        }
                        last_value = value;
                        last_sent = sent;
                    }
                }
                // Following the other eye: while the other one is relaxed, crossing its own open reading changes nothing
                let left = fits[0].unwrap();
                if settings.lid_widen != Widen::Off && !widen_room(&left, settings.lid_widen) {
                    let open_at = open_reading(&left, vertical);
                    let relaxed_right = open_reading(&fits[1].unwrap(), vertical);
                    let below = lid_inputs([open_at - 0.0005, relaxed_right], vertical, [1.0; 2], settings)[0];
                    let above = lid_inputs([open_at + 0.0005, relaxed_right], vertical, [1.0; 2], settings)[0];
                    assert!((lid_to_vrcft(below, settings) - lid_to_vrcft(above, settings)).abs() < 0.01, "{vertical}");
                }
            }
        }
        // The case that stepped before: 45° down, where the expected reading is far below the open one
        let settings = user_fit(Widen::Normal);
        let right = settings.lid_fit()[1].unwrap();
        let expected = expected_open(&right, -1.0);
        let at = lid_to_vrcft(fitted_lid(expected, -1.0, &right, &settings), &settings);
        let past = lid_to_vrcft(fitted_lid(expected + 0.001, -1.0, &right, &settings), &settings);
        assert!(at < 0.75 && past - at < 0.01, "{expected}: {at} -> {past}");
    }

    #[test]
    fn fitted_eyes_take_only_their_fixed_scale() {
        // Learned scales that are not 1 (relaxed 0.7 against lid_open 0.8)
        let calibration = LidCalibration::load(None, 0.7);
        let learned = calibration.scales(0.8);
        assert!(learned[0] > 1.0);
        // Unfitted: fixed, else learned (unchanged)
        let unfitted = Settings {
            lid_scale_left: Some(0.9),
            ..settings()
        };
        assert_eq!(lid_scales(&unfitted, &calibration), [0.9, learned[1]]);
        // Fitted: fixed as the fine-tune, else 1, never the learned one
        let fitted_unset = fitted();
        assert_eq!(lid_scales(&fitted_unset, &calibration), [1.0, 1.0]);
        let fitted_tuned = Settings {
            lid_scale_right: Some(0.69),
            ..fitted()
        };
        assert_eq!(lid_scales(&fitted_tuned, &calibration), [1.0, 0.69]);
        // One eye fitted: the other still learns its scale
        let half = Settings {
            lid_fit_closed_right: None,
            lid_fit_up_right: None,
            lid_fit_open_right: None,
            lid_fit_down_right: None,
            ..fitted()
        };
        assert_eq!(lid_scales(&half, &calibration), [1.0, learned[1]]);
    }

    #[test]
    fn fitted_eyelids_use_the_held_gaze_while_blinking() {
        let settings = Settings {
            despike: false,
            gaze_deadzone: 0.0,
            ..fitted()
        };
        // Looking 15° down (the fixation point at -tan 15°), then a blink where the gaze jumps up.
        let down = (15.0_f32).to_radians().tan();
        let mut readings: Vec<EyeData> = (0..20).map(|i| reading(i, [0.0, -down], [0.7, 0.6])).collect();
        readings.push(reading(20, [0.0, 0.5], [0.3, 0.3]));
        readings.push(reading(21, [0.0, 0.5], [0.3, 0.3]));
        let sent = run(&settings, &readings);
        assert!((sent[19].openness_scaled[0] - settings.lid_open).abs() < 0.01, "{:?}", sent[19].openness_scaled);
        // During the blink the fit keeps using "down": 0.3 is well below the down reading either way.
        assert!(sent[21].gaze_held && sent[21].openness_scaled[0] < settings.lid_closed + 0.1);
        let unfitted = run(&Settings { despike: false, ..Settings::default() }, &readings);
        assert!(unfitted[19].openness_scaled[0] < settings.lid_open);
    }

    #[test]
    fn fitted_eyes_are_not_learned() {
        let mut calibration = LidCalibration::load(None, 0.80);
        let mut smoother = Smoother::new(&fitted());
        for i in 0..900 {
            step(&fitted(), &mut smoother, &mut calibration, &reading(i, [0.0, 0.0], [0.6, 0.6]), true);
        }
        assert!(calibration.histograms.iter().flatten().all(|weight| *weight == 0.0));
    }

    #[test]
    fn samples_say_when_the_gaze_is_held() {
        let readings = openness_track(20, &[(10, [0.2; 2]), (11, [0.2; 2])]);
        let sent = run(&without_new_stages(), &readings);
        assert!(!sent[9].gaze_held && sent[10].gaze_held && !sent[12].gaze_held);
        let raw = run(&Settings { raw: true, ..settings() }, &readings);
        assert!(raw[10].gaze_held && !raw[12].gaze_held);
    }

    #[test]
    fn each_eye_may_have_its_own_sideways_fit() {
        let angles = [0.2, 0.1, -0.2, 0.1, 0.0, 0.1];
        let per_eye = Settings {
            gaze_offset_x: 0.01,
            gaze_gain_x: 2.0,
            gaze_offset_x_left: Some(0.1),
            gaze_gain_x_right: Some(0.5),
            ..settings()
        };
        let corrected = correct_gaze(angles, &per_eye);
        // Left: its own offset, the shared gain; right: the shared offset, its own gain; combined: shared.
        let expected = [0.2, 0.1, -0.105, 0.1, -0.02, 0.1];
        for (value, expected) in corrected.iter().zip(expected) {
            assert!((value - expected).abs() < 1e-6, "{corrected:?}");
        }
        // Unset per-eye values change nothing.
        let shared = Settings {
            gaze_offset_x: 0.01,
            gaze_gain_x: 2.0,
            ..settings()
        };
        assert_eq!(correct_gaze(angles, &shared)[0], (0.2 - 0.01) * 2.0);
    }

    #[test]
    fn looking_far_down_holds_the_sideways_gaze() {
        let settings = Settings {
            gaze_down_hold_x_deg: 28.0,
            ..settings()
        };
        let mut smoother = Smoother::new(&settings);
        let at = |down_deg: f32, x: f32| {
            let y = -down_deg / 45.0;
            [x, y, x + 0.1, y, x + 0.05, y]
        };
        let open = [true; 2];
        // Above the threshold nothing changes, and the x values are remembered.
        assert_eq!(smoother.hold_down_x(at(20.0, 0.1), open, &settings), at(20.0, 0.1));
        assert_eq!(smoother.hold_down_x(at(27.5, 0.1), open, &settings), at(27.5, 0.1));
        // Halfway into the fade, halfway to the held values; fully held 10° below the threshold.
        let half = smoother.hold_down_x(at(33.0, 0.5), open, &settings);
        assert!((half[0] - 0.3).abs() < 1e-5 && (half[2] - 0.4).abs() < 1e-5 && (half[4] - 0.35).abs() < 1e-5);
        let held = smoother.hold_down_x(at(42.0, 0.5), open, &settings);
        assert!((held[0] - 0.1).abs() < 1e-6 && (held[2] - 0.2).abs() < 1e-6 && (held[4] - 0.15).abs() < 1e-6);
        // The vertical gaze is left alone.
        assert_eq!([held[1], held[3], held[5]], [at(42.0, 0.5)[1]; 3]);
        // Starting out that low, straight ahead (as fitted, each eye's own) is held; after a reset too.
        smoother.reset();
        let fitted = Settings {
            gaze_offset_x: 0.02,
            gaze_offset_x_left: Some(0.012),
            gaze_offset_x_right: Some(0.049),
            ..settings.clone()
        };
        let start = smoother.hold_down_x(at(42.0, 0.5), open, &fitted);
        assert!((start[0] - 0.012).abs() < 1e-6 && (start[2] - 0.049).abs() < 1e-6 && (start[4] - 0.02).abs() < 1e-6);
        // 0 turns it off.
        let off = Settings {
            gaze_down_hold_x_deg: 0.0,
            ..settings.clone()
        };
        assert_eq!(smoother.hold_down_x(at(42.0, 0.5), open, &off), at(42.0, 0.5));
    }

    #[test]
    fn a_blink_before_looking_down_is_not_what_gets_held() {
        let settings = Settings {
            gaze_down_hold_x_deg: 24.0,
            ..settings()
        };
        let mut smoother = Smoother::new(&settings);
        let at = |down_deg: f32, x: f32| {
            let y = -down_deg / 45.0;
            [x, y, x + 0.1, y, x + 0.05, y]
        };
        // Looking 20° down at x 0.1, then a blink (or an unreliable sample) where x jumps to 0.6.
        smoother.hold_down_x(at(20.0, 0.1), [true; 2], &settings);
        smoother.hold_down_x(at(20.0, 0.6), [false; 2], &settings);
        // Only the right eye is trusted: only its x (and the combined one) is taken.
        smoother.hold_down_x(at(20.0, 0.2), [false, true], &settings);
        let held = smoother.hold_down_x(at(40.0, 0.9), [true; 2], &settings);
        assert!((held[0] - 0.1).abs() < 1e-6 && (held[2] - 0.3).abs() < 1e-6 && (held[4] - 0.25).abs() < 1e-6, "{held:?}");
        // Through the whole pipeline: a blink (openness below gaze_hold_below) at 20° down is not held.
        let down = |deg: f32| (deg.to_radians()).tan();
        let mut readings: Vec<EyeData> = (0..30).map(|i| reading(i, [0.0, -down(20.0)], [0.8; 2])).collect();
        readings.extend((30..34).map(|i| reading(i, [down(25.0), -down(20.0)], [0.2; 2])));
        readings.extend((34..60).map(|i| reading(i, [down(30.0), -down(40.0)], [0.8; 2])));
        // Fully held at 40° down: the x from before the blink (straight ahead), not the blink's 25°.
        let sent = run(&settings, &readings);
        let last = sent.last().unwrap();
        assert!(last.gaze[4].abs() < 0.02, "{:?}", last.gaze);
    }

    #[test]
    fn far_down_the_fitted_lid_does_not_flicker() {
        // A valid fit whose down reading is near its closed one.
        let settings = Settings {
            lid_fit_closed_left: Some(0.4),
            lid_fit_up_left: Some(0.95),
            lid_fit_open_left: Some(0.9),
            lid_fit_down_left: Some(0.5),
            ..settings()
        };
        let fit = settings.lid_fit()[0].unwrap();
        // 22° down the expected open reading is floored at 0.45, only 0.05 above closed; the range is
        // floored at half the straight-ahead one (0.25), so 0.05 of wobble moves the lid a fifth of the way.
        let vertical = -22.0 / 45.0;
        let step_lid = fitted_openness(0.5, vertical, &fit, &settings) - fitted_openness(0.45, vertical, &fit, &settings);
        let expected = 0.05 / 0.25 / (1.0 - LID_FIT_CLOSED_MARGIN) * (settings.lid_open - settings.lid_closed);
        assert!((step_lid - expected).abs() < 1e-5, "{step_lid} vs {expected}");
        // Straight ahead nothing changes: the range there is the whole one.
        let ahead = fitted_openness(0.9, 0.0, &fit, &settings);
        assert!((ahead - settings.lid_open).abs() < 1e-5);
    }

    #[test]
    fn a_send_error_is_logged_once_and_never_stops_the_process() {
        let addr: SocketAddr = "192.0.2.1:9000".parse().unwrap();
        let mut health = SendHealth::default();
        let unreachable = || Err(io::Error::from(io::ErrorKind::NetworkUnreachable));
        let t0 = Instant::now();
        let at = |ms: u64| t0 + Duration::from_millis(ms);
        assert_eq!(health.record(at(0), addr, &Ok(20)), None);
        // Wi-Fi not up yet: one line when it starts failing, then nothing while it keeps failing...
        let line = health.record(at(10), addr, &unreachable()).unwrap();
        assert!(line.starts_with("Sending OSC to 192.0.2.1:9000 failed (") && line.ends_with("retrying with every sample"), "{line}");
        assert_eq!(health.record(at(20), addr, &unreachable()), None);
        // ..."refused" (nothing listening) is neither a failure nor a recovery...
        assert_eq!(health.record(at(30), addr, &Err(io::Error::from(io::ErrorKind::ConnectionRefused))), None);
        // ...and one line when it works again.
        assert_eq!(health.record(at(40), addr, &Ok(20)).as_deref(), Some("Sending OSC to 192.0.2.1:9000 works again"));
        assert_eq!(health.record(at(50), addr, &Ok(20)), None);
        // A flapping link: within 5 s of that pair nothing more is logged, however often it flips...
        for i in 0..50 {
            assert_eq!(health.record(at(100 + i * 20), addr, &unreachable()), None);
            assert_eq!(health.record(at(110 + i * 20), addr, &Ok(20)), None);
        }
        // ...then a failure that lasts is logged, with how many changes were not...
        let line = health.record(at(5100), addr, &Err(io::Error::from(io::ErrorKind::HostUnreachable))).unwrap();
        assert!(line.ends_with("(100 more changes in the last few seconds not logged)"), "{line}");
        // ...and so is its end.
        assert!(health.record(at(5200), addr, &Ok(20)).is_some());
        // A failure that began quietly is logged once the 5 s are over, if it still lasts
        assert_eq!(health.record(at(5300), addr, &unreachable()), None);
        assert!(health.record(at(10200), addr, &unreachable()).is_some());

        // A real send to a port nothing listens on: never an error, however often.
        let listener = UdpSocket::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        drop(listener);
        let mut output = Output::new(Target::Fixed { host: "127.0.0.1".into(), port });
        output.refresh();
        assert_eq!(output.addr().map(|addr| addr.port()), Some(port));
        for _ in 0..5 {
            assert!(output.send("/avatar/parameters/FT/v2/EyeLeftX".into(), vec![OscType::Float(0.1)]).is_ok());
        }
        // A target that can't be looked up just waits (no socket, no error).
        let mut nowhere = Output::new(Target::Fixed { host: "no-such-host.invalid".into(), port: 9000 });
        nowhere.refresh();
        assert!(nowhere.addr().is_none());
        assert!(nowhere.send("/x".into(), vec![OscType::Bool(true)]).is_ok());
    }

    #[test]
    fn the_rate_counts_the_last_second() {
        let now = Instant::now();
        let times: VecDeque<Instant> = [now - Duration::from_millis(1500), now - Duration::from_millis(900), now]
            .into_iter()
            .collect();
        assert_eq!(per_second(&times), 2.0);
        assert_eq!(per_second(&VecDeque::new()), 0.0);
    }

    #[test]
    fn lost_tracking_says_why() {
        assert_eq!(lost_reason(&Next::Stopped), "eye server not producing");
        assert_eq!(lost_reason(&Next::Waiting), "no new samples for 1 s");
        let unreadable = EyeData {
            sample_time: f64::NAN,
            ..EyeData::default()
        };
        assert_eq!(lost_reason(&Next::Sample(unreadable)), "unreadable sample");
    }

    #[test]
    fn deadzone_ignores_small_moves_and_follows_large_ones() {
        let mut deadzone = Deadzone::new(0.03);
        assert_eq!(deadzone.apply(0.10), 0.10);
        assert_eq!(deadzone.apply(0.12), 0.10);
        assert_eq!(deadzone.apply(0.08), 0.10);
        assert!((deadzone.apply(0.30) - 0.27).abs() < 1e-6);
        assert!((deadzone.apply(0.29) - 0.27).abs() < 1e-6);
    }

    #[test]
    fn one_euro_damps_noise() {
        let mut filter = OneEuro::new(0.4, 0.8, 0.5);
        let mut last = 0.0;
        for i in 0..90 {
            let noise = if i % 2 == 0 { 0.02 } else { -0.02 };
            last = filter.filter(noise, NOMINAL_DT);
        }
        assert!(last.abs() < 0.005, "{last}");
    }

    #[test]
    fn smoother_keeps_filter_state_between_samples() {
        let mut smoother = Smoother::new(&Settings {
            gaze_deadzone: 0.0,
            ..settings()
        });
        let mut lids = [0.75; 2];
        let mut gaze = [0.0; 6];
        apply(&mut smoother, 0.0, &mut gaze, &mut lids, false);
        let mut jumped = [0.5; 6];
        apply(&mut smoother, NOMINAL_DT as f64, &mut jumped, &mut lids, false);
        assert!(jumped[0] < 0.5, "a filtered step must not pass through untouched: {}", jumped[0]);
    }

    #[test]
    fn gaze_is_held_while_eyes_are_shut() {
        let mut smoother = Smoother::new(&settings());
        let mut lids = [0.75; 2];
        let mut gaze = [0.2; 6];
        apply(&mut smoother, 0.0, &mut gaze, &mut lids, false);
        let before = gaze;
        let mut jumped = [-0.4; 6];
        apply(&mut smoother, NOMINAL_DT as f64, &mut jumped, &mut lids, true);
        assert_eq!(jumped, before);
    }

    #[test]
    fn proc_net_addresses_decode_in_host_byte_order() {
        assert_eq!(parse_proc_ip("1A4E230A"), Some(IpAddr::V4(Ipv4Addr::new(10, 35, 78, 26))));
        assert_eq!(
            parse_proc_ip("0000000000000000FFFF00001A4E230A"),
            Some(IpAddr::V4(Ipv4Addr::new(10, 35, 78, 26)))
        );
        assert_eq!(parse_proc_ip("xyz"), None);
    }

    #[test]
    fn steam_link_peer_is_the_connected_socket_of_vrlink() {
        let table = "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode ref pointer drops\n\
            \x20 10: 014E230A:28A0 00000000:0000 07 00000000:00000000 00:00000000 00000000  1000        0 111 2 0 0\n\
            \x20 11: 0100007F:28A0 0100007F:1F90 01 00000000:00000000 00:00000000 00000000  1000        0 222 2 0 0\n\
            \x20 12: 014E230A:28A0 1A4E230A:28A0 01 00000000:00000000 00:00000000 00000000  1000        0 333 2 0 0\n";
        let inodes: HashSet<u64> = [111, 222, 333].into();
        assert_eq!(
            connected_udp_peer(table, &inodes),
            Some(IpAddr::V4(Ipv4Addr::new(10, 35, 78, 26)))
        );
        assert_eq!(connected_udp_peer(table, &[111, 222].into()), None);
    }

    fn wobble(i: usize) -> f32 {
        [-0.02, 0.0, 0.02][i % 3]
    }

    #[test]
    fn lid_calibration_learns_each_eyes_relaxed_openness() {
        let mut calibration = LidCalibration::load(None, 0.80);
        for i in 0..90 * 30 {
            calibration.observe([0.82 + wobble(i), 0.78 + wobble(i)]);
        }
        let [left, right] = calibration.relaxed;
        assert!((left - 0.82).abs() < 0.01 && (right - 0.78).abs() < 0.01, "{left} {right}");
        let [scale_left, scale_right] = calibration.scales(0.80);
        assert!((0.82 * scale_left - 0.78 * scale_right).abs() < 0.01);
    }

    #[test]
    fn lid_calibration_shrugs_off_a_minute_of_squinting() {
        let mut calibration = LidCalibration::load(None, 0.80);
        for i in 0..90 * 600 {
            calibration.observe([0.82 + wobble(i); 2]);
        }
        for _ in 0..90 * 60 {
            calibration.observe([0.70, 0.55]);
        }
        let [left, right] = calibration.relaxed;
        // A light squint counts toward the estimate but barely moves it; a deep one is ignored.
        assert!((left - 0.82).abs() < 0.02, "{left}");
        assert!((right - 0.82).abs() < 0.01, "{right}");
    }

    #[test]
    fn lid_calibration_does_not_ratchet_up_on_widely_spread_readings() {
        let mut calibration = LidCalibration::load(None, 0.80);
        let spread = [0.63, 0.70, 0.76, 0.82, 0.87, 0.93, 0.97];
        for i in 0..90 * 120 {
            calibration.observe([spread[i % spread.len()]; 2]);
        }
        assert!((calibration.relaxed[0] - 0.82).abs() < 0.01, "{}", calibration.relaxed[0]);
    }

    #[test]
    fn lid_calibration_file_round_trips() {
        let dir = std::env::temp_dir().join(format!("frameeyeosc-test-{}", std::process::id()));
        let path = dir.join("calibration");
        write_calibration(&path, [0.818, 0.777]).unwrap();
        assert_eq!(LidCalibration::load(Some(path), 0.80).relaxed, [0.818, 0.777]);
        assert_eq!(parse_calibration("left_relaxed=abc\nright_relaxed=5\n", 0.80), [0.80, 0.80]);
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn lid_sync_evens_small_differences_but_keeps_winks() {
        let [left, right] = sync_lids([0.70, 0.75], 0.4);
        assert!((left - right).abs() < 0.01, "{left} {right}");
        assert_eq!(sync_lids([0.0, 0.75], 0.4), [0.0, 0.75]);
        assert_eq!(sync_lids([0.70, 0.75], 0.0), [0.70, 0.75]);
    }

    #[test]
    fn eyelids_map_onto_vrcft_scale() {
        let args = settings();
        assert_eq!(lid_to_vrcft(0.22, &args), 0.0);
        assert_eq!(lid_to_vrcft(0.80, &args), 0.75);
        assert_eq!(lid_to_vrcft(0.88, &args), 0.75);
        assert_eq!(lid_to_vrcft(1.00, &args), 1.0);
        assert!(lid_to_vrcft(0.55, &args) > 0.0 && lid_to_vrcft(0.55, &args) < 0.75);
    }

    #[test]
    fn etvr_eyelids_treat_relaxed_as_fully_open() {
        assert_eq!(lid_to_etvr(0.0), 0.0);
        assert_eq!(lid_to_etvr(0.375), 0.5);
        assert_eq!(lid_to_etvr(0.75), 1.0);
        // Widening does not reach the ETVR Tracking Module.
        assert_eq!(lid_to_etvr(1.0), 1.0);
        assert_eq!(output_lids(OutputKind::Vrchat, [0.375, 1.0]), [0.375, 1.0]);
        assert_eq!(output_lids(OutputKind::Etvr, [0.375, 1.0]), [0.5, 1.0]);
        // LiveLink keeps the VRCFT eyelids (the packet splits them into blink and widening)
        assert_eq!(output_lids(OutputKind::LiveLink, [0.375, 1.0]), [0.375, 1.0]);
    }

    fn sample() -> Sample {
        Sample {
            openness: [0.8; 2],
            openness_scaled: [0.8; 2],
            raw_gaze: [0.1, 0.2, 0.3, 0.4, 0.5, 0.6],
            gaze: [0.1, 0.2, 0.3, 0.4, 0.5, 0.6],
            lids: [0.375, 0.75],
            reliable: [true; 2],
            gaze_held: false,
        }
    }

    fn sent(settings: &Settings) -> Vec<(String, OscType)> {
        osc_messages(settings, &sample())
    }

    #[test]
    fn vrchat_gets_the_full_eye_set() {
        let messages = sent(&settings());
        let addrs: Vec<&str> = messages.iter().map(|(addr, _)| addr.as_str()).collect();
        assert_eq!(
            addrs,
            [
                "/avatar/parameters/FT/EyeTrackingActive",
                "/avatar/parameters/FT/v2/EyeLeftX",
                "/avatar/parameters/FT/v2/EyeLeftY",
                "/avatar/parameters/FT/v2/EyeRightX",
                "/avatar/parameters/FT/v2/EyeRightY",
                "/avatar/parameters/FT/v2/EyeLidLeft",
                "/avatar/parameters/FT/v2/EyeLidRight",
                "/avatar/parameters/FT/v2/EyeX",
                "/avatar/parameters/FT/v2/EyeY",
            ]
        );
        assert_eq!(messages[0].1, OscType::Bool(true));
        assert_eq!(messages[6].1, OscType::Float(0.75));
    }

    #[test]
    fn eye_tracking_active_goes_as_a_bool_a_float_or_not_at_all() {
        let with = |kind| Settings {
            eye_tracking_active: kind,
            ..settings()
        };
        let active = "/avatar/parameters/FT/EyeTrackingActive";
        // While sending
        let bool_messages = sent(&with(ActiveType::Bool));
        assert_eq!(bool_messages[0], (active.to_owned(), OscType::Bool(true)));
        let float_messages = sent(&with(ActiveType::Float));
        assert_eq!(float_messages[0], (active.to_owned(), OscType::Float(1.0)));
        assert_eq!(float_messages.len(), 9);
        let off_messages = sent(&with(ActiveType::Off));
        assert_eq!(off_messages.len(), 8);
        assert!(off_messages.iter().all(|(addr, _)| addr != active));
        // The eye values are the same in every mode
        assert_eq!(bool_messages[1..], float_messages[1..]);
        assert_eq!(bool_messages[1..], off_messages[..]);
        // The one-time "not active" on pausing or losing tracking
        assert_eq!(active_message(&with(ActiveType::Bool), false), Some((active.to_owned(), OscType::Bool(false))));
        assert_eq!(active_message(&with(ActiveType::Float), false), Some((active.to_owned(), OscType::Float(0.0))));
        assert_eq!(active_message(&with(ActiveType::Off), false), None);
        // Changing the type ends the old stream (it gets its own last "not active")
        assert_ne!(vrchat_stream(&with(ActiveType::Bool)), vrchat_stream(&with(ActiveType::Float)));
        // ETVR never gets it
        let etvr = Settings {
            output: OutputKind::Etvr,
            ..with(ActiveType::Float)
        };
        assert!(sent(&etvr).iter().all(|(addr, _)| !addr.ends_with("EyeTrackingActive")));
        assert!(vrchat_stream(&etvr).is_none());
    }

    #[test]
    fn a_pause_or_lost_tracking_sends_not_active_only_when_asked() {
        // A real socket: pausing with "float" sends 0.0, with "off" nothing
        let listener = UdpSocket::bind("127.0.0.1:0").unwrap();
        listener.set_read_timeout(Some(Duration::from_millis(500))).unwrap();
        let port = listener.local_addr().unwrap().port();
        let mut output = Output::new(Target::Fixed { host: "127.0.0.1".into(), port });
        output.refresh();
        let float = Settings {
            eye_tracking_active: ActiveType::Float,
            ..settings()
        };
        send_inactive(&mut output, &float).unwrap();
        let mut buffer = [0u8; 256];
        let size = listener.recv(&mut buffer).unwrap();
        let (_, packet) = rosc::decoder::decode_udp(&buffer[..size]).unwrap();
        let OscPacket::Message(message) = packet else { panic!("not a message") };
        assert_eq!(message.addr, "/avatar/parameters/FT/EyeTrackingActive");
        assert_eq!(message.args, [OscType::Float(0.0)]);
        let off = Settings {
            eye_tracking_active: ActiveType::Off,
            ..settings()
        };
        send_inactive(&mut output, &off).unwrap();
        listener.set_read_timeout(Some(Duration::from_millis(200))).unwrap();
        assert!(listener.recv(&mut buffer).is_err());
    }

    #[test]
    fn etvr_gets_only_the_six_per_eye_values() {
        let messages = sent(&Settings {
            output: OutputKind::Etvr,
            ..settings()
        });
        let addrs: Vec<&str> = messages.iter().map(|(addr, _)| addr.as_str()).collect();
        assert_eq!(
            addrs,
            [
                "/avatar/parameters/FT/v2/EyeLeftX",
                "/avatar/parameters/FT/v2/EyeLeftY",
                "/avatar/parameters/FT/v2/EyeRightX",
                "/avatar/parameters/FT/v2/EyeRightY",
                "/avatar/parameters/FT/v2/EyeLidLeft",
                "/avatar/parameters/FT/v2/EyeLidRight",
            ]
        );
        assert_eq!(messages[4].1, OscType::Float(0.5));
        assert_eq!(messages[5].1, OscType::Float(1.0));
    }

    #[test]
    fn livelink_gets_each_eyes_gaze_and_eyelid_in_one_packet() {
        let packet = livelink_packet(&sample(), 12.0);
        let [left, right] = livelink::tests::module_eyes(&packet).unwrap();
        // Eyelid 0.375: half closed; 0.75: relaxed open, not widened
        assert_eq!((left.0, left.1), (0.5, 0.0));
        assert_eq!((right.0, right.1), (1.0, 0.0));
        // The per-eye gaze, not the combined one (sample() has different values in each; choose_gaze puts the
        // combined gaze in both eyes unless "move eyes separately" is on)
        assert_eq!((left.2, left.3), (0.1, 0.2));
        assert_eq!((right.2, right.3), (0.3, 0.4));
        // Widened
        let wide = Sample {
            lids: [1.0, 0.875],
            ..sample()
        };
        let [left, right] = livelink::tests::module_eyes(&livelink_packet(&wide, 12.0)).unwrap();
        assert_eq!((left.0, left.1), (1.0, 1.0));
        assert_eq!((right.0, right.1), (1.0, 0.5));
    }

    /// A bridge sending to a local UDP listener, with no config file, calibration file or status folder.
    fn test_bridge(settings: Settings) -> Bridge {
        let mut bridge = Bridge {
            config: Config::new(None, Args::parse_from(["frameeyeosc"]), HashSet::new()),
            output: Output::new(Target::of(&settings)),
            smoother: Smoother::new(&settings),
            calibration: LidCalibration::load(None, settings.lid_open),
            settings,
            started: SystemTime::now(),
            active_since: None,
            last_data: None,
            latest: None,
            sent: VecDeque::new(),
            received: VecDeque::new(),
            capture: None,
            capture_result: None,
            dots: dots::DotStream::new(Path::new("/nonexistent")),
            livelink_neutral: None,
            livelink_throttle: Throttle::default(),
        };
        bridge.output.refresh();
        bridge
    }

    fn reload(settings: &Settings) -> Reload {
        Reload {
            settings: settings.clone(),
            reset_calibration: false,
            gaze_capture: None,
        }
    }

    /// The next datagram within 300 ms.
    fn receive(listener: &UdpSocket) -> Option<Vec<u8>> {
        let mut buffer = [0u8; 1024];
        listener.set_read_timeout(Some(Duration::from_millis(300))).unwrap();
        listener.recv(&mut buffer).ok().map(|size| buffer[..size].to_vec())
    }

    #[test]
    fn livelink_sends_a_packet_per_sample_and_neutral_when_the_eyes_stop() {
        let listener = UdpSocket::bind("127.0.0.1:0").unwrap();
        let live = Settings {
            output: OutputKind::LiveLink,
            host: "127.0.0.1".into(),
            port: Some(listener.local_addr().unwrap().port()),
            lid_calibration: false,
            ..settings()
        };
        let mut bridge = test_bridge(live.clone());
        let neutral = |packet: Vec<u8>| livelink::tests::module_eyes(&packet) == Some([(1.0, 0.0, 0.0, 0.0); 2]);
        // Before any eye data: the neutral packet, twice a second
        bridge.keep_livelink_alive();
        assert!(neutral(receive(&listener).unwrap()));
        bridge.keep_livelink_alive();
        assert!(receive(&listener).is_none());
        // Looking right and down (22.5°) with the left eye wide open: one packet per sample, as processed
        let look = [(std::f32::consts::PI / 8.0).tan(), -(std::f32::consts::PI / 8.0).tan()];
        for index in 0..3 {
            // (the rate limit is tested on its own below)
            bridge.livelink_throttle = Throttle::default();
            bridge.on_sample(reading(index, look, [1.0, 0.55])).unwrap();
            let packet = receive(&listener).unwrap();
            let [left, right] = livelink::tests::module_eyes(&packet).unwrap();
            let sample = bridge.latest.as_ref().unwrap();
            for ((openness, wide, x, y), (lid, gaze)) in [left, right].into_iter().zip([
                (sample.lids[0], [sample.gaze[0], sample.gaze[1]]),
                (sample.lids[1], [sample.gaze[2], sample.gaze[3]]),
            ]) {
                assert!((openness * 0.75 + wide * 0.25 - lid).abs() < 1e-6);
                assert_eq!([x, y], gaze);
            }
            assert!(left.1 > 0.0 && right.0 < 1.0);
            assert!(left.2 > 0.0 && left.3 < 0.0);
        }
        // Tracking runs, so no neutral packets in between
        bridge.keep_livelink_alive();
        assert!(receive(&listener).is_none());
        // Pausing sends one neutral packet, and nothing after it
        let paused = Settings {
            sending: false,
            ..live.clone()
        };
        bridge.apply(reload(&paused)).unwrap();
        assert!(neutral(receive(&listener).unwrap()));
        bridge.on_sample(reading(3, look, [1.0, 0.55])).unwrap();
        bridge.on_lost("test").unwrap();
        bridge.livelink_neutral = None;
        bridge.keep_livelink_alive();
        assert!(receive(&listener).is_none());
        // Losing the eye data sends one, then again only after LIVELINK_IDLE_INTERVAL
        bridge.apply(reload(&live)).unwrap();
        bridge.livelink_throttle = Throttle::default();
        bridge.on_sample(reading(4, look, [1.0, 0.55])).unwrap();
        assert!(!neutral(receive(&listener).unwrap()));
        bridge.on_lost("test").unwrap();
        assert!(neutral(receive(&listener).unwrap()));
        bridge.keep_livelink_alive();
        assert!(receive(&listener).is_none());
        bridge.livelink_neutral = Some(Instant::now() - LIVELINK_IDLE_INTERVAL);
        bridge.keep_livelink_alive();
        assert!(neutral(receive(&listener).unwrap()));
        // Switching to VRChat while tracking: a last neutral packet, then OSC
        bridge.livelink_throttle = Throttle::default();
        bridge.on_sample(reading(5, look, [1.0, 0.55])).unwrap();
        receive(&listener).unwrap();
        let vrchat = Settings {
            output: OutputKind::Vrchat,
            ..live.clone()
        };
        bridge.apply(reload(&vrchat)).unwrap();
        assert!(neutral(receive(&listener).unwrap()));
        bridge.on_sample(reading(6, look, [1.0, 0.55])).unwrap();
        let (_, packet) = rosc::decoder::decode_udp(&receive(&listener).unwrap()).unwrap();
        let OscPacket::Message(message) = packet else { panic!("not a message") };
        assert_eq!(message.addr, "/avatar/parameters/FT/EyeTrackingActive");
        while receive(&listener).is_some() {}
        // VRChat mode never gets the neutral packet while idle
        bridge.on_lost("test").unwrap();
        let (_, packet) = rosc::decoder::decode_udp(&receive(&listener).unwrap()).unwrap();
        let OscPacket::Message(message) = packet else { panic!("not a message") };
        assert_eq!(message.args, [OscType::Bool(false)]);
        bridge.livelink_neutral = None;
        bridge.keep_livelink_alive();
        assert!(receive(&listener).is_none());
    }

    #[test]
    fn the_livelink_rate_stays_at_or_below_the_limit() {
        let interval = Duration::from_secs(1) / LIVELINK_MAX_HZ;
        let start = Instant::now();
        // 90 Hz and 136 Hz for ten seconds: never more than 50 in any second, and no bursts (a late packet may be
        // followed by one a sample later, but any 200 ms hold at most one more than 200 ms / interval)
        for hz in [90u32, 136] {
            let mut throttle = Throttle::default();
            let times: Vec<Instant> = (0..hz * 10)
                .map(|i| start + Duration::from_secs(1) * i / hz)
                .filter(|&time| throttle.ready(time, interval))
                .collect();
            assert!(times.len() as u32 <= LIVELINK_MAX_HZ * 10, "{hz} Hz: {}", times.len());
            assert!(times.len() as u32 >= LIVELINK_MAX_HZ * 10 * 8 / 10, "{hz} Hz: {}", times.len());
            for (i, &from) in times.iter().enumerate() {
                let window = Duration::from_millis(200);
                let count = times[i..].iter().take_while(|&&time| time < from + window).count();
                assert!(count as u32 <= window.as_millis() as u32 / interval.as_millis() as u32 + 1, "{hz} Hz: {count}");
            }
            for second in 0..10 {
                let from = start + Duration::from_secs(second);
                let count = times.iter().filter(|&&time| time >= from && time < from + Duration::from_secs(1)).count();
                assert!(count as u32 <= LIVELINK_MAX_HZ, "{hz} Hz, second {second}: {count}");
            }
        }
        // Slower samples all go out, and a pause does not let a burst through afterwards
        let mut throttle = Throttle::default();
        let slow: Vec<bool> = (0..10u32).map(|i| throttle.ready(start + Duration::from_millis(30) * i, interval)).collect();
        assert!(slow.iter().all(|&ready| ready));
        let later = start + Duration::from_secs(5);
        assert!(throttle.ready(later, interval));
        assert!(!throttle.ready(later + Duration::from_millis(5), interval));
    }

    #[test]
    fn livelink_sends_the_newest_sample_and_the_rate_counts_only_what_went_out() {
        let listener = UdpSocket::bind("127.0.0.1:0").unwrap();
        let live = Settings {
            output: OutputKind::LiveLink,
            host: "127.0.0.1".into(),
            port: Some(listener.local_addr().unwrap().port()),
            raw: true,
            lid_calibration: false,
            ..settings()
        };
        let mut bridge = test_bridge(live);
        let gaze_of = |packet: Vec<u8>| livelink::tests::module_eyes(&packet).unwrap()[0].2;
        // Looking left, then (right away) ahead, then after the interval right
        let x = |deg: f32| deg.to_radians().tan();
        bridge.on_sample(reading(0, [x(-20.0), 0.0], [0.8; 2])).unwrap();
        bridge.on_sample(reading(1, [0.0, 0.0], [0.8; 2])).unwrap();
        assert!(gaze_of(receive(&listener).unwrap()) < 0.0);
        assert!(receive(&listener).is_none(), "the sample in between is dropped, not queued");
        std::thread::sleep(Duration::from_secs(1) / LIVELINK_MAX_HZ + Duration::from_millis(5));
        bridge.on_sample(reading(2, [x(20.0), 0.0], [0.8; 2])).unwrap();
        assert!(gaze_of(receive(&listener).unwrap()) > 0.0, "the newest sample goes out");
        // Three samples in, two packets out
        assert_eq!((bridge.received.len(), bridge.sent.len()), (3, 2));
        // The keepalive still goes out once the eye data stops
        bridge.on_lost("test").unwrap();
        assert!(livelink::tests::module_eyes(&receive(&listener).unwrap()) == Some([(1.0, 0.0, 0.0, 0.0); 2]));
        bridge.livelink_neutral = Some(Instant::now() - LIVELINK_IDLE_INTERVAL);
        bridge.keep_livelink_alive();
        assert!(receive(&listener).is_some());
    }

    #[test]
    fn empty_prefix_sends_bare_names() {
        let messages = sent(&Settings {
            prefix: String::new(),
            ..settings()
        });
        assert_eq!(messages[0].0, "/avatar/parameters/EyeTrackingActive");
        assert_eq!(messages[1].0, "/avatar/parameters/v2/EyeLeftX");
    }

    #[test]
    fn each_output_has_its_default_port() {
        assert_eq!(settings().port(), 9000);
        let etvr = Settings {
            output: OutputKind::Etvr,
            ..settings()
        };
        assert_eq!(etvr.port(), 8889);
        assert_eq!(Settings { port: Some(9100), ..etvr }.port(), 9100);
        assert!(Target::of(&settings()) == Target::SteamLink { port: 9000 });
        let livelink = Settings {
            output: OutputKind::LiveLink,
            ..settings()
        };
        assert_eq!(livelink.port(), 11111);
        assert_eq!(Settings { port: Some(11112), ..livelink.clone() }.port(), 11112);
        assert!(Target::of(&livelink) == Target::SteamLink { port: 11111 });
    }

    #[test]
    fn stopping_or_moving_the_vrchat_stream_is_noticed() {
        let base = settings();
        let paused = Settings {
            sending: false,
            ..settings()
        };
        let etvr = Settings {
            output: OutputKind::Etvr,
            ..settings()
        };
        let moved = Settings {
            port: Some(9001),
            ..settings()
        };
        assert!(vrchat_stream(&base).is_some());
        assert!(vrchat_stream(&paused).is_none() && vrchat_stream(&etvr).is_none());
        assert_ne!(vrchat_stream(&base), vrchat_stream(&moved));
        let tweaked = Settings {
            lid_open: 0.85,
            ..settings()
        };
        assert_eq!(vrchat_stream(&base), vrchat_stream(&tweaked));
        // The same for LiveLink
        let livelink = Settings {
            output: OutputKind::LiveLink,
            ..settings()
        };
        assert_eq!(livelink_stream(&livelink), Some(("auto", 11111)));
        assert!(livelink_stream(&base).is_none() && livelink_stream(&etvr).is_none());
        assert!(livelink_stream(&Settings { sending: false, ..livelink.clone() }).is_none());
        assert_ne!(livelink_stream(&livelink), livelink_stream(&Settings { port: Some(11112), ..livelink.clone() }));
        assert_eq!(livelink_stream(&livelink), livelink_stream(&Settings { lid_open: 0.85, ..livelink.clone() }));
        // LiveLink is not VRChat: no EyeTrackingActive stream to end
        assert!(vrchat_stream(&livelink).is_none());
    }

    #[test]
    fn lid_calibration_reset_starts_over_from_lid_open() {
        let mut calibration = LidCalibration::load(None, 0.80);
        for i in 0..90 * 30 {
            calibration.observe([0.90 + wobble(i); 2]);
        }
        calibration.reset(0.80);
        assert_eq!(calibration.relaxed, [0.80; 2]);
        assert!(calibration.histograms.iter().flatten().all(|weight| *weight == 0.0));
    }

    #[test]
    fn filters_take_new_parameters_without_losing_state() {
        let mut smoother = Smoother::new(&settings());
        let mut lids = [0.75; 2];
        let mut gaze = [0.0; 6];
        apply(&mut smoother, 0.0, &mut gaze, &mut lids, false);
        smoother.configure(&Settings {
            lid_min_cutoff: 10.0,
            gaze_deadzone: 0.0,
            ..settings()
        });
        assert_eq!(smoother.lids[0].min_cutoff, 10.0);
        assert_eq!(smoother.deadzones[0].width, 0.0);
        assert_eq!(smoother.lids[0].value, Some(0.75));
    }
}
