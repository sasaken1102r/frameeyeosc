//! Steam Frame eye bridge using the eye server's private shared-memory ABI (versions 4 and 5).

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
// While the eye server's shared memory can't be read (not there yet, an unsupported version, ...), it is tried again
// this often.
const REOPEN_INTERVAL: Duration = Duration::from_secs(1);
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
// A relaxed open eyelid in VRCFT units; above it the eye is widened.
const LID_RELAXED: f32 = 0.75;
// Widening shows only once an eyelid has been above relaxed open for this long (see Smoother::sustain_widen). Right
// before a blink the Frame's openness often jumps up, to 1.000 at times (72% of the samples pinned at 1.000 in a
// 60-minute recording were within 0.3 s of a blink), which "high" sends as wide eyes for a moment. Over the
// 2026-09-30..10-01 recordings an eye was visibly widened (VRCFT 0.85 or more) right before 16% of blinks; 448 of
// those 606 stretches lasted under 250 ms (median 178 ms), 3 lasted 250-300 ms and the rest over 300 ms (an eye held
// wide). Replayed with "high", blinks led in by a visible widen fell from 281 to 8 of 1113 (2026-10-01 01:09) and
// from 29 to 3 of 236 (00:21), while widens held for a second or more showed a median 0.16-0.23 s later.
const WIDEN_SUSTAIN: f64 = 0.25;
// The time above relaxed open counts on through dips back to relaxed of up to this long, so a widen that wobbles
// does not start over, and starts over once the eyelid falls below WIDEN_RESET_BELOW (a third closed, as in a
// blink), so a blink's lead-in and its reopening count separately.
const WIDEN_RELEASE: f64 = 0.1;
const WIDEN_RESET_BELOW: f32 = 0.5;
// SteamOS 0.4.3's eye tracker reads a relaxed open eye as 1.000, where the openness stops, so a widened eye can't read
// any higher and widening can't come through (one user's left eye, both eyes open: a median 0.754 and 2.0% of samples
// at 1.000 before, 1.000 and 75-93% after). An eye without a fit whose relaxed reading lands past mark 3 would even be
// sent widened all the time, so while it is saturated no eyelid goes out above relaxed open (see process). Told from
// the readings, not from a version, for that, the status file and the panel: over the last SATURATION_WINDOW seconds
// of samples with both eyes at SATURATION_OPEN or more, more than SATURATION_ON of them with either eye at
// SATURATED_READING or more turn it on, and fewer than SATURATION_OFF turn it off again; with fewer than
// SATURATION_MIN_SAMPLES such samples it stays as it was. On two 60-minute recordings
// before 0.4.3 that share was 0-30.5% (a median 3.1% and 4.5%), on three after it 50.4-99.5% (on from 6.7 s in).
const SATURATION_WINDOW: i64 = 60;
const SATURATION_OPEN: f32 = 0.6;
const SATURATED_READING: f32 = 0.999;
const SATURATION_ON: f64 = 0.5;
const SATURATION_OFF: f64 = 0.4;
const SATURATION_MIN_SAMPLES: u32 = 600;
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
// A jump in the eye server's sequence larger than this (10 s at 90 a second) is it starting over, not samples missed.
const MAX_MISSED_JUMP: u32 = 900;
// The panel shows the eye data rate in red below this. Held below it for LOW_RATE_LOG_AFTER, it is logged with the
// numbers that tell whether frameeyeosc or the eye tracker was the slow one.
const LOW_TRACKER_RATE: f32 = 60.0;
const LOW_RATE_LOG_AFTER: Duration = Duration::from_secs(10);
// At most one line this often about datagrams dropped because the network could not take them at once.
const DROP_LOG_INTERVAL: Duration = Duration::from_secs(60);

// The start of the shared memory, the same in every version read here. frameeyeosc only touches these fields and the
// sample record: it locks metadata_mutex, waits on and reads sequence, and sets metadata_requested to 1 (the eye
// server publishes a sample only while it is set, and clears it with each one).
#[repr(C)]
struct ShmControl {
    version: u32,
    initialized: u32,
    // The target glibc mutex slot is 48 bytes; host libc may define a smaller type.
    metadata_mutex: [u8; 0x30],
    sequence: u32,
    metadata_requested: u32,
}

// Version 4 (Frame 0.5.0).
#[repr(C)]
struct EyeServerMmap {
    control: ShmControl,
    other_control_fields: [u8; 0x112],
    eye_data: EyeDataMmap,
}

// Version 5 (SteamOS 0.4.3, build 20260930.6234839; the eye server binary of 2026-10-01). Read off the eye server's
// own client and server code (CEyeTrackingMmapClient / CEyeTrackingMmapServer) and checked against the file. The only
// change from version 4 is 5 bytes inserted at 0x152, in front of the sample record, which moves the record (and the
// camera images after it) 5 bytes on; the file grows from 0x4f21a to 0x4f21f bytes. The fields after the control
// block, none of which frameeyeosc touches:
//   0x040  u32       camera images requested (set by a client together with metadata_requested)
//   0x044  mutex     client-to-server mutex (0x30 bytes)
//   0x074  u32       client-to-server sequence (futex)
//   0x078  u32       client-to-server message pending
//   0x07c  8 x 25 B  client-to-server queue, read index at 0x144, write index at 0x148, full flag (u8) at 0x14c
//   0x14d  u8 + f32  a flag and value a client sets (90.0)
//   0x152  u8 + i32  new in version 5: "Track Dominant Eye Only" (VR Settings > General, advanced), on (1) or
//                    off (0), then which eye: 0 left, 1 right, -1 while off. Only read (see dominant_eye)
//   0x157  the sample record (0xebc bytes, laid out as in version 4)
//   0x1013 camera images: three u32 (size), then two 400 x 400 8-bit images at 0x101f and 0x2811f
#[repr(C)]
struct EyeServerMmapV5 {
    control: ShmControl,
    images_requested: u32,
    client_to_server: [u8; 0x10e],
    dominant_eye: [u8; 5],
    eye_data: EyeDataMmap,
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

/// What differs between the shared-memory versions: the file size, where the sample record starts, and where the
/// "Track Dominant Eye Only" setting is (version 5 on).
#[derive(Clone, Copy, Debug, PartialEq)]
struct ShmLayout {
    version: u32,
    size: usize,
    eye_data: usize,
    dominant_eye: Option<usize>,
}

const SHM_LAYOUTS: [ShmLayout; 2] = [
    ShmLayout {
        version: 4,
        size: 0x4f21a,
        eye_data: offset_of!(EyeServerMmap, eye_data),
        dominant_eye: None,
    },
    ShmLayout {
        version: 5,
        size: 0x4f21f,
        eye_data: offset_of!(EyeServerMmapV5, eye_data),
        dominant_eye: Some(offset_of!(EyeServerMmapV5, dominant_eye)),
    },
];

const _: () = {
    assert!(offset_of!(ShmControl, metadata_mutex) == 0x08);
    assert!(offset_of!(ShmControl, sequence) == 0x38);
    assert!(offset_of!(ShmControl, metadata_requested) == 0x3c);
    assert!(size_of::<ShmControl>() == 0x40);
    assert!(offset_of!(EyeServerMmap, control) == 0);
    assert!(offset_of!(EyeServerMmap, eye_data) == 0x152);
    assert!(offset_of!(EyeServerMmapV5, control) == 0);
    assert!(offset_of!(EyeServerMmapV5, images_requested) == 0x40);
    assert!(offset_of!(EyeServerMmapV5, client_to_server) == 0x44);
    assert!(offset_of!(EyeServerMmapV5, dominant_eye) == 0x152);
    assert!(offset_of!(EyeServerMmapV5, eye_data) == 0x157);
    assert!(offset_of!(EyeDataMmap, sample_time) == 0x05);
    assert!(offset_of!(EyeDataMmap, gaze_direction) == 0x0d);
    assert!(offset_of!(EyeDataMmap, gaze_covariance_diag) == 0x25);
    assert!(offset_of!(EyeDataMmap, fixation_point) == 0x3d);
    assert!(offset_of!(EyeDataMmap, pre_fusion_gaze) == 0x49);
    assert!(offset_of!(EyeDataMmap, pre_fusion_cov_diag) == 0x61);
    assert!(offset_of!(EyeDataMmap, openness) == 0x79);
    assert!(offset_of!(EyeDataMmap, estimate_extra) == 0x81);
    assert!(size_of::<EyeDataMmap>() == 0xebc);
    assert!(SHM_LAYOUTS[0].version == 4 && SHM_LAYOUTS[0].eye_data == 0x152);
    assert!(SHM_LAYOUTS[1].version == 5 && SHM_LAYOUTS[1].eye_data == 0x157);
    // The record ends where the camera images start (0x100e in version 4, 0x1013 in version 5).
    assert!(size_of::<EyeServerMmap>() <= SHM_LAYOUTS[0].size);
    assert!(size_of::<EyeServerMmapV5>() <= SHM_LAYOUTS[1].size);
    assert!(SHM_LAYOUTS[1].size - SHM_LAYOUTS[0].size == SHM_LAYOUTS[1].eye_data - SHM_LAYOUTS[0].eye_data);
    assert!(size_of::<libc::pthread_mutex_t>() <= 0x30);
    assert!(8 % align_of::<libc::pthread_mutex_t>() == 0);
};

/// The layout of a shared-memory version, or why it can't be read.
fn shm_layout(version: u32) -> Result<ShmLayout, String> {
    SHM_LAYOUTS.iter().copied().find(|layout| layout.version == version).ok_or_else(|| {
        let supported: Vec<String> = SHM_LAYOUTS
            .iter()
            .map(|layout| layout.version.to_string())
            .collect();
        format!(
            "unsupported eye shared-memory version {version}; supported: {}",
            supported.join(", ")
        )
    })
}

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
    #[arg(long, default_value_t = 0.3)]
    gaze_min_cutoff: f32,
    /// One Euro beta for gaze; higher follows fast eye movements with less lag
    #[arg(long, default_value_t = 1.5)]
    gaze_beta: f32,
    /// One Euro derivative cutoff in Hz for gaze; lower keeps tracker noise from loosening the filter
    #[arg(long, default_value_t = 0.5)]
    gaze_d_cutoff: f32,
    /// Gaze changes smaller than this (1.0 = 45°) are ignored so the eyes stay put while fixating
    #[arg(long, default_value_t = 0.005)]
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
    // Per eye: since when its eyelid has been above relaxed open (see sustain_widen), and when it last was.
    wide_since: [Option<f64>; 2],
    wide_last: [f64; 2],
    // Whether the openness is saturated, so nothing above relaxed open is widening. Not reset with the filters: it
    // belongs to the eye tracker, not to one stretch of tracking.
    saturation: Saturation,
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
            wide_since: [None; 2],
            wide_last: [f64::NEG_INFINITY; 2],
            saturation: Saturation::default(),
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
        self.wide_since = [None; 2];
        self.wide_last = [f64::NEG_INFINITY; 2];
    }

    /// Keep the filtered eyelids, and the eyelid filters themselves, at or below relaxed open.
    fn cap_lids(&mut self, lids: &mut [f32; 2]) {
        for (lid, filter) in lids.iter_mut().zip(&mut self.lids) {
            *lid = lid.min(LID_RELAXED);
            filter.value = filter.value.map(|value| value.min(LID_RELAXED));
        }
    }

    /// VRCFT eyelids with widening held back until it lasts: an eyelid above relaxed open is sent as relaxed open
    /// until it has been above it for WIDEN_SUSTAIN, counting through short dips (WIDEN_RELEASE) and starting over
    /// once it closes a third (WIDEN_RESET_BELOW). The Frame's brief jumps in openness around blinks then never show
    /// as wide eyes, and a widen that is held shows WIDEN_SUSTAIN late. Below relaxed open nothing changes.
    fn sustain_widen(&mut self, time: f64, lids: [f32; 2]) -> [f32; 2] {
        std::array::from_fn(|eye| {
            let lid = lids[eye];
            if lid > LID_RELAXED {
                self.wide_since[eye].get_or_insert(time);
                self.wide_last[eye] = time;
            } else if lid < WIDEN_RESET_BELOW || time - self.wide_last[eye] > WIDEN_RELEASE {
                self.wide_since[eye] = None;
            }
            match self.wide_since[eye] {
                Some(since) if time - since >= WIDEN_SUSTAIN => lid,
                _ => lid.min(LID_RELAXED),
            }
        })
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

/// A fitted eye's expected open reading for an up/down gaze (-1..1): a line through its down, straight-ahead and up
/// readings, held at the down and up readings beyond them (15° down and up), never above the straight-ahead reading
/// and never below half of it.
///
/// Never above the straight-ahead reading: fits often read an eye more open looking up or down than straight ahead
/// (one user's left eye 0.833 down, 0.703 ahead and 0.860 up), but over a 60-minute recording that eye's openness was
/// flat (0.74-0.77 from 20° down to 15° up). Expecting more there sent a relaxed eye as half closed 562 times an
/// hour and below 0.6 30.8% of the time; capped and held, 0 times and 8.9%. Looking down may still expect less, which
/// is what the fit is for. Held beyond the fitted readings: the line carried on past them made those mistakes larger
/// the further the eyes went.
fn expected_open(fit: &LidFit, vertical: f32) -> f32 {
    let vertical = vertical.clamp(-LID_FIT_PITCH, LID_FIT_PITCH);
    let slope = if vertical >= 0.0 { fit.up - fit.open } else { fit.open - fit.down };
    (fit.open + slope * vertical / LID_FIT_PITCH).clamp(0.5 * fit.open, fit.open)
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
    // An eye that reads near 1.000 straight ahead has little room below where the openness stops: keep some range
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
    path: PathBuf,
    inode: u64,
    layout: ShmLayout,
    // The sequence of the last record read (before the first, the one there when first asked), so a sample published
    // while the one before was being processed is read at once; None until the first call.
    last_sequence: Option<u32>,
    // Samples the eye server published that were not read (the sequence moved on by more than one), since take_missed.
    missed: u32,
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
        Self::open_at(Path::new(SOURCE))
    }

    /// Map the eye server's shared memory at `path`. The version is read (without writing anything) before the file
    /// is mapped, so a version this does not know is never written to.
    fn open_at(path: &Path) -> Result<Self, Box<dyn Error>> {
        let shown = path.display();
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .open(path)
            .map_err(|error| format!("{shown}: {error}"))?;
        let metadata = file.metadata()?;
        let mut header = [0; 4];
        std::os::unix::fs::FileExt::read_exact_at(&file, &mut header, 0)
            .map_err(|_| format!("{shown}: shared memory is too small"))?;
        let layout = shm_layout(u32::from_le_bytes(header))?;
        if metadata.len() < layout.size as u64 {
            return Err(format!("{shown}: shared memory is too small").into());
        }
        let map = unsafe { MmapOptions::new().len(layout.size).map_mut(&file)? };
        let source = Self {
            map,
            path: path.to_owned(),
            inode: metadata.ino(),
            layout,
            last_sequence: None,
            missed: 0,
        };
        let control = source.control();
        let version = u32::from_le(unsafe { ptr::read_volatile(&raw const (*control).version) });
        if version != layout.version {
            // Rewritten by a restarting eye server between the read and the mapping.
            return Err(shm_layout(version).map_or_else(
                |error| error,
                |_| format!("{shown}: the shared-memory version changed while opening it"),
            )
            .into());
        }
        if u32::from_le(unsafe { ptr::read_volatile(&raw const (*control).initialized) }) != 1 {
            return Err("eye shared memory is not initialized".into());
        }
        Ok(source)
    }

    /// True when the path now points at a different file (or none) than the one we mapped, or the eye server rewrote
    /// the same file as another version.
    fn is_stale(&self) -> bool {
        self.version_changed() || fs::metadata(&self.path).map_or(true, |metadata| metadata.ino() != self.inode)
    }

    /// True when the file no longer says the version it was opened as.
    fn version_changed(&self) -> bool {
        u32::from_le(unsafe { ptr::read_volatile(&raw const (*self.control()).version) }) != self.layout.version
    }

    fn control(&self) -> *const ShmControl {
        self.map.as_ptr().cast()
    }

    /// The "Track Dominant Eye Only" setting (see parse_dominant_eye; Some(None) in a version without it). Only read,
    /// without the lock: a client sets it, not the eye server, so a read is only torn while the setting changes, and
    /// it is read twice and must say the same both times.
    fn dominant_eye(&self) -> Option<Option<DominantEye>> {
        let Some(offset) = self.layout.dominant_eye else {
            return Some(None);
        };
        let read = || -> [u8; 5] {
            // The layout's offset lies within the mapping (checked at compile time against its size).
            std::array::from_fn(|i| unsafe { ptr::read_volatile(self.map.as_ptr().add(offset + i)) })
        };
        let first = read();
        (read() == first).then(|| parse_dominant_eye(first)).flatten()
    }

    fn control_mut(&mut self) -> *mut ShmControl {
        self.map.as_mut_ptr().cast()
    }

    fn lock(&mut self) -> io::Result<MutexGuard> {
        let mutex = unsafe { (&raw mut (*self.control_mut()).metadata_mutex).cast() };
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

    /// Wait up to `timeout` for the eye server's next sample. The server writes the record and bumps the sequence
    /// while holding metadata_mutex, and the record is copied out under the same mutex, so it is never torn.
    ///
    /// The eye server publishes a frame only if a sample was requested by then, and clears the request with it (read
    /// off its publish routine). So the next sample is requested in the same lock that copies this one out, and one
    /// published while this one is processed and sent is read at once on the next call. Requesting it only when the
    /// next call started lost every other frame (90 -> 45 a second) whenever waking up, processing and sending took
    /// longer than a frame (11.1 ms). The first call after opening still waits for a new sample rather than return the
    /// one already there.
    fn next(&mut self, timeout: Duration) -> io::Result<Next> {
        // Rewritten as another version: nothing is written to it (not even the lock), and is_stale() has it reopened.
        if self.version_changed() {
            return Ok(Next::Stopped);
        }
        let sequence_ptr = unsafe { &raw const (*self.control()).sequence };
        let guard = self.lock()?;
        let sequence = unsafe { ptr::read_volatile(sequence_ptr) };
        let seen = *self.last_sequence.get_or_insert(sequence);
        if sequence != seen {
            // Published while the last sample was being processed
            let next = self.take(sequence);
            drop(guard);
            return Ok(next);
        }
        let request_ptr = unsafe { &raw mut (*self.control_mut()).metadata_requested };
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
                seen,
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
        let sequence = unsafe { ptr::read_volatile(sequence_ptr) };
        let next = if sequence != seen { self.take(sequence) } else { Next::Waiting };
        drop(guard);
        Ok(next)
    }

    /// Copy out the record the eye server published as `sequence`, and request the next sample. Called with
    /// metadata_mutex held.
    fn take(&mut self, sequence: u32) -> Next {
        // The layout's record offset lies within the mapping (checked at compile time against its size).
        let record_ptr = unsafe { self.map.as_ptr().add(self.layout.eye_data) }.cast::<EyeDataMmap>();
        let record = unsafe { ptr::read_unaligned(record_ptr) };
        let request_ptr = unsafe { &raw mut (*self.control_mut()).metadata_requested };
        unsafe { ptr::write_volatile(request_ptr, 1) };
        if let Some(last) = self.last_sequence {
            let jump = sequence.wrapping_sub(last);
            // A larger jump (or one back) is the sequence starting over, not samples gone by
            if (2..=MAX_MISSED_JUMP).contains(&jump) {
                self.missed = self.missed.saturating_add(jump - 1);
            }
        }
        self.last_sequence = Some(sequence);
        decode(record)
    }

    /// The samples the eye server published that were not read, since the last call.
    fn take_missed(&mut self) -> u32 {
        std::mem::take(&mut self.missed)
    }
}

/// Which eye alone the Frame tracks while "Track Dominant Eye Only" is on. Its fused gaze of both eyes is then
/// that eye's (the other eye's fused gaze is a copy, about 0.15° off), while each eye's gaze before fusion and its
/// openness are still its own.
#[derive(Clone, Copy, Debug, PartialEq)]
enum DominantEye {
    Left,
    Right,
}

impl DominantEye {
    fn name(self) -> &'static str {
        match self {
            Self::Left => "left",
            Self::Right => "right",
        }
    }
}

/// The setting's 5 bytes (a flag, then the eye as a little-endian i32), as they are in the shared memory: Some(None)
/// while it is off, Some(Some(eye)) while it is on, None for anything else, such as a read torn by the setting changing
/// halfway or a meaning that has changed.
fn parse_dominant_eye(bytes: [u8; 5]) -> Option<Option<DominantEye>> {
    let eye = i32::from_le_bytes([bytes[1], bytes[2], bytes[3], bytes[4]]);
    match (bytes[0], eye) {
        (0, -1..=1) => Some(None),
        (1, 0) => Some(Some(DominantEye::Left)),
        (1, 1) => Some(Some(DominantEye::Right)),
        _ => None,
    }
}

/// Whether the Frame's openness is saturated: a relaxed open eye reading 1.000 (see SATURATION_WINDOW).
#[derive(Default)]
struct Saturation {
    // Whole seconds of sample time, each with its samples with both eyes open and how many of those read saturated.
    seconds: VecDeque<(i64, u32, u32)>,
    open: u32,
    saturated: u32,
    last_time: Option<f64>,
    on: bool,
}

impl Saturation {
    fn add(&mut self, data: &EyeData) {
        let mut time = data.sample_time;
        if let Some(last) = self.last_time {
            if time < last - 1.0 {
                // The eye server's clock started over
                self.seconds.clear();
                self.open = 0;
                self.saturated = 0;
            } else if time < last {
                // A sample a few ms out of order (Smoother::advance allows for it too): count it as the newest
                // second, so the window keeps its history instead of starting the warm-up over
                time = last;
            }
        }
        self.last_time = Some(time);
        let second = time.floor() as i64;
        while let Some(&(start, open, saturated)) = self.seconds.front() {
            if start > second - SATURATION_WINDOW {
                break;
            }
            self.seconds.pop_front();
            self.open -= open;
            self.saturated -= saturated;
        }
        if data.openness.iter().all(|openness| *openness >= SATURATION_OPEN) {
            let saturated = u32::from(data.openness.iter().any(|openness| *openness >= SATURATED_READING));
            match self.seconds.back_mut() {
                Some((start, open, count)) if *start == second => {
                    *open += 1;
                    *count += saturated;
                }
                _ => self.seconds.push_back((second, 1, saturated)),
            }
            self.open += 1;
            self.saturated += saturated;
        }
        if self.open >= SATURATION_MIN_SAMPLES {
            let share = f64::from(self.saturated) / f64::from(self.open);
            if share > SATURATION_ON {
                self.on = true;
            } else if share < SATURATION_OFF {
                self.on = false;
            }
        }
    }
}

/// The sample in a record the eye server published, the same for every version.
fn decode(record: EyeDataMmap) -> Next {
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
}

/// The eye server's shared memory, kept open while it can be read. While it can't (SteamVR not started yet, an
/// unsupported version, replaced by the eye server, ...), frameeyeosc keeps running: it is tried again every
/// REOPEN_INTERVAL, and `error` says why, for the status file and so the panel.
struct EyeReader {
    path: PathBuf,
    source: Option<EyeSource>,
    // Why the shared memory can't be read; None while it can.
    error: Option<String>,
    next_open: Instant,
    // The eye "Track Dominant Eye Only" has the Frame track alone, as last read; None while it is off, or the
    // shared memory can't be read or has no such setting (version 4).
    dominant_eye: Option<DominantEye>,
}

impl EyeReader {
    fn new(path: &Path, now: Instant) -> Self {
        Self {
            path: path.to_owned(),
            source: None,
            error: None,
            next_open: now,
            dominant_eye: None,
        }
    }

    /// Read the "Track Dominant Eye Only" setting again (a read that can't be trusted keeps the last one). Returns what
    /// to log when it changed while the shared memory is open.
    fn refresh_dominant_eye(&mut self) -> Option<String> {
        let now = match &self.source {
            Some(source) => source.dominant_eye().unwrap_or(self.dominant_eye),
            None => None,
        };
        if now == self.dominant_eye {
            return None;
        }
        self.dominant_eye = now;
        self.source.as_ref()?;
        Some(match now {
            Some(eye) => format!("Track Dominant Eye Only is on: the Frame tracks the {} eye alone", eye.name()),
            None => "Track Dominant Eye Only is off".to_owned(),
        })
    }

    /// Open the shared memory if it isn't open and a try is due. Returns what to log: the version once it is open, or
    /// why it can't be, only when that reason is new (not every second).
    fn open_if_due(&mut self, now: Instant) -> Option<String> {
        if self.source.is_some() || now < self.next_open {
            return None;
        }
        match EyeSource::open_at(&self.path) {
            Ok(source) => {
                let line = format!("Reading {} (version {})", self.path.display(), source.layout.version);
                self.source = Some(source);
                self.error = None;
                Some(line)
            }
            Err(error) => {
                let error = error.to_string();
                self.next_open = now + REOPEN_INTERVAL;
                if self.error.as_ref() == Some(&error) {
                    return None;
                }
                let line = format!(
                    "Can't read eye data ({error}); retrying every {} s",
                    REOPEN_INTERVAL.as_secs()
                );
                self.error = Some(error);
                Some(line)
            }
        }
    }

    /// Let go of a shared memory that was replaced; the next open_if_due opens the new one.
    fn close(&mut self, now: Instant) {
        self.source = None;
        self.next_open = now;
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

/// Datagrams dropped because the network could not take them at once (the socket's send buffer was full, say while
/// Steam Link's video fills the Wi-Fi). Sending never waits, so a busy network can't hold up reading the eye tracker;
/// a dropped datagram is counted, and logged at most once every DROP_LOG_INTERVAL.
#[derive(Default)]
struct Drops {
    // When each drop of the last RATE_WINDOW happened.
    times: VecDeque<Instant>,
    // Drops not logged yet, and when the first of them happened.
    unlogged: u32,
    first_unlogged: Option<Instant>,
    last_logged: Option<Instant>,
}

impl Drops {
    /// Count a dropped datagram to `addr`; returns the line to log, if one is due.
    fn record(&mut self, now: Instant, addr: SocketAddr) -> Option<String> {
        self.times.push_back(now);
        self.prune(now);
        self.unlogged += 1;
        let first = *self.first_unlogged.get_or_insert(now);
        if self.last_logged.is_some_and(|last| now.duration_since(last) < DROP_LOG_INTERVAL) {
            return None;
        }
        self.last_logged = Some(now);
        self.first_unlogged = None;
        let count = std::mem::take(&mut self.unlogged);
        Some(format!(
            "Dropped {count} datagram{} to {addr} over {:.0} s: the network was too busy to take {} at once \
             (said at most once a minute)",
            if count == 1 { "" } else { "s" },
            now.duration_since(first).as_secs_f32(),
            if count == 1 { "it" } else { "them" },
        ))
    }

    /// Drops in the last RATE_WINDOW.
    fn per_second(&self, now: Instant) -> f32 {
        self.times.iter().filter(|time| now.duration_since(**time) < RATE_WINDOW).count() as f32
    }

    fn prune(&mut self, now: Instant) {
        while self.times.front().is_some_and(|time| now.duration_since(*time) >= RATE_WINDOW) {
            self.times.pop_front();
        }
    }
}

/// UDP sender that re-resolves its target periodically and reconnects when it changes.
struct Output {
    target: Target,
    socket: Option<(UdpSocket, SocketAddr)>,
    last_resolve: Option<Instant>,
    health: SendHealth,
    drops: Drops,
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
            drops: Drops::default(),
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
        let (target, result) = (*target, socket.send(datagram));
        if let Some(line) = self.note(Instant::now(), target, &result) {
            eprintln!("{line}");
        }
    }

    /// Note how sending one datagram to `target` went; returns the line to log, if any. A datagram the network could
    /// not take at once is dropped and counted, which is not a failure to send.
    fn note(&mut self, now: Instant, target: SocketAddr, result: &io::Result<usize>) -> Option<String> {
        match result {
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => self.drops.record(now, target),
            _ => self.health.record(now, target, result),
        }
    }
}

/// How many of these times fall in the last RATE_WINDOW (one second): a rate a second.
fn per_second(times: &VecDeque<Instant>) -> f32 {
    times.iter().filter(|time| time.elapsed() < RATE_WINDOW).count() as f32
}

/// A UDP socket connected to `addr` (connecting only sets where packets go; it fails without a route). It never
/// blocks: a datagram the network can't take at once is dropped (see Drops) rather than holding up the eye data.
fn connect(addr: SocketAddr) -> io::Result<UdpSocket> {
    let socket = UdpSocket::bind(if addr.is_ipv4() { "0.0.0.0:0" } else { "[::]:0" })?;
    socket.connect(addr)?;
    socket.set_nonblocking(true)?;
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
    // Judged in --raw too (for the status file), though raw eyelids are never capped
    smoother.saturation.add(data);
    let saturated = smoother.saturation.on;
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
        // While the openness is saturated, nothing above relaxed open is widening. Capped before the widen sustain and
        // the filter, so the filter rests at relaxed open (a closing eye starts closing at once) and a widen starts over
        // once it isn't saturated any more; and the filter itself right after it, for the sample it turns on while a
        // widen is still in the filter. The filter and lid_sync only average, so nothing goes above it meanwhile.
        // Closing, half-closed and blinks are below relaxed open and untouched.
        let mut lids = if saturated { mapped.map(|lid| lid.min(LID_RELAXED)) } else { mapped };
        lids = smoother.sustain_widen(data.sample_time, lids);
        smoother.filter(dt, &mut gaze, &mut lids, hold);
        if saturated {
            smoother.cap_lids(&mut lids);
        }
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

/// How well frameeyeosc keeps up with the eye tracker, to tell apart the two reasons the eye data rate can be low:
/// frameeyeosc too slow to take each sample (samples published but not read, or long over one), or the eye tracker
/// itself delivering few.
#[derive(Default)]
struct Pace {
    // Samples the eye server published and frameeyeosc did not read, as noticed over the last RATE_WINDOW.
    missed: VecDeque<(Instant, u32)>,
    // How long each sample of the last RATE_WINDOW took, from reading it until ready to read the next (processing,
    // sending, the status file).
    busy: VecDeque<(Instant, Duration)>,
    // Since when the eye data rate has been below LOW_TRACKER_RATE, and whether that was logged.
    low_since: Option<Instant>,
    low_logged: bool,
}

impl Pace {
    /// Note the samples missed before the one just read.
    fn add_missed(&mut self, now: Instant, missed: u32) {
        if missed > 0 {
            self.missed.push_back((now, missed));
        }
        self.prune(now);
    }

    /// Note how long one sample took.
    fn add_busy(&mut self, now: Instant, busy: Duration) {
        self.busy.push_back((now, busy));
        self.prune(now);
    }

    /// Samples missed in the last RATE_WINDOW.
    fn missed_rate(&self, now: Instant) -> f32 {
        let recent = self.missed.iter().filter(|(time, _)| now.duration_since(*time) < RATE_WINDOW);
        recent.map(|(_, missed)| *missed as f32).sum()
    }

    /// The longest one sample took in the last RATE_WINDOW, in ms (0 without samples), to 0.1 ms.
    fn max_busy_ms(&self, now: Instant) -> f32 {
        let recent = self.busy.iter().filter(|(time, _)| now.duration_since(*time) < RATE_WINDOW);
        let longest = recent.map(|(_, busy)| *busy).max().unwrap_or_default();
        (longest.as_secs_f32() * 10_000.0).round() / 10.0
    }

    fn prune(&mut self, now: Instant) {
        while self.missed.front().is_some_and(|(time, _)| now.duration_since(*time) >= RATE_WINDOW) {
            self.missed.pop_front();
        }
        while self.busy.front().is_some_and(|(time, _)| now.duration_since(*time) >= RATE_WINDOW) {
            self.busy.pop_front();
        }
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
    // Whether frameeyeosc keeps up with the eye tracker.
    pace: Pace,
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

    /// Samples from the eye tracker in the last second; None until tracking has run for a second.
    fn tracker_rate(&self) -> Option<f32> {
        self.active_since
            .is_some_and(|since| since.elapsed() >= RATE_WINDOW)
            .then(|| per_second(&self.received))
    }

    /// Returns the line to log once the eye data rate has stayed below LOW_TRACKER_RATE for LOW_RATE_LOG_AFTER (once
    /// each time it goes low), with the numbers that tell who was slow.
    fn check_low_rate(&mut self, now: Instant) -> Option<String> {
        let Some(rate) = self.tracker_rate().filter(|rate| *rate < LOW_TRACKER_RATE) else {
            self.pace.low_since = None;
            self.pace.low_logged = false;
            return None;
        };
        let since = *self.pace.low_since.get_or_insert(now);
        if self.pace.low_logged || now.duration_since(since) < LOW_RATE_LOG_AFTER {
            return None;
        }
        self.pace.low_logged = true;
        Some(format!(
            "Eye data has been low for {} s: {rate:.0} samples/s; in the last second {:.0} published samples were \
             missed, the slowest sample took {:.1} ms, and {:.0} datagrams were dropped",
            LOW_RATE_LOG_AFTER.as_secs(),
            self.pace.missed_rate(now),
            self.pace.max_busy_ms(now),
            self.output.drops.per_second(now),
        ))
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

    /// `source_error`: why the eye tracker's shared memory can't be read, if it can't; `dominant_eye`: the eye the
    /// Frame tracks alone, if "Track Dominant Eye Only" is on.
    fn status<'a>(&'a self, source_error: Option<&'a str>, dominant_eye: Option<DominantEye>) -> Status<'a> {
        let now = Instant::now();
        let tracker_rate = self.tracker_rate();
        let missed_rate = tracker_rate.map(|_| self.pace.missed_rate(now));
        let max_processing_ms = tracker_rate.map(|_| self.pace.max_busy_ms(now));
        let dropped_rate = self.output.drops.per_second(now);
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
            tracker_rate,
            missed_rate,
            max_processing_ms,
            dropped_rate,
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
            source_error,
            dominant_eye: dominant_eye.map(DominantEye::name),
            openness_saturated: self.smoother.saturation.on,
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
    eprintln!(
        "Recording {SOURCE} (version {}) to {}; stop with Ctrl+C",
        source.layout.version,
        path.display()
    );
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
        pace: Pace::default(),
    };
    let mut status_file = StatusFile::new(status::status_path());
    // Until the shared memory can be read, frameeyeosc keeps running and trying again, and the status file says why,
    // so the panel shows the reason instead of "not running". Config reloads, the status file and the LiveLink
    // keepalive go on meanwhile.
    let mut eye = EyeReader::new(Path::new(SOURCE), Instant::now());
    // When the last sample was read, until ready to read the next one.
    let mut busy_since: Option<Instant> = None;
    loop {
        if let Some(reload) = bridge.config.poll() {
            bridge.apply(reload)?;
        }
        bridge.output.refresh();
        if let Some(line) = eye.open_if_due(Instant::now()) {
            eprintln!("{line}");
        }
        if let Some(since) = busy_since.take() {
            bridge.pace.add_busy(Instant::now(), since.elapsed());
        }
        match eye.source.as_mut() {
            Some(source) => match source.next(POLL)? {
                Next::Sample(data) if data.is_finite() => {
                    let now = Instant::now();
                    busy_since = Some(now);
                    bridge.pace.add_missed(now, source.take_missed());
                    bridge.on_sample(data)?;
                }
                // Short waits are normal; only a whole second without data means tracking stopped.
                Next::Waiting if bridge.last_data.is_some_and(|last| last.elapsed() < TIMEOUT) => {}
                next if bridge.active_since.is_some() => bridge.on_lost(&lost_reason(&next))?,
                // Idle: if the eye server recreated its shared memory (or rewrote it as another version), our mapping
                // would go silent forever. It is opened again above, retrying while the new one can't be read.
                _ if source.is_stale() => {
                    eprintln!("{SOURCE} was replaced; reopening");
                    eye.close(Instant::now());
                }
                _ => {}
            },
            // Nothing to wait on; the status file and the LiveLink keepalive keep their pace.
            None => std::thread::sleep(POLL),
        }
        bridge.check_capture();
        bridge.keep_livelink_alive();
        if status_file.due() {
            if let Some(line) = eye.refresh_dominant_eye() {
                eprintln!("{line}");
            }
            if let Some(line) = bridge.check_low_rate(Instant::now()) {
                eprintln!("{line}");
            }
            status_file.write(&bridge.status(eye.error.as_deref(), eye.dominant_eye));
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
    fn expected_openness_follows_the_gaze_down_but_never_tops_straight_ahead() {
        // Left: 0.95 up, 0.9 straight ahead, 0.7 down
        let fit = fitted().lid_fit()[0].unwrap();
        assert!((expected_open(&fit, 0.0) - 0.9).abs() < 1e-6);
        // Down: along the line to the down reading, and held there beyond it (15° down)
        assert!((expected_open(&fit, -0.5 * LID_FIT_PITCH) - 0.8).abs() < 1e-6);
        assert!((expected_open(&fit, -LID_FIT_PITCH) - 0.7).abs() < 1e-6);
        for vertical in [-1.5 * LID_FIT_PITCH, -0.75, -1.0] {
            assert!((expected_open(&fit, vertical) - 0.7).abs() < 1e-6, "{vertical}");
        }
        // Up reads more open in the fit, but no more than straight ahead is ever expected
        for vertical in [0.1, LID_FIT_PITCH, 0.5, 1.0] {
            assert_eq!(expected_open(&fit, vertical), 0.9, "{vertical}");
        }
        // A fit more open up and down than straight ahead (one user's left eye, 2026-10-01): flat everywhere
        let v_shape = LidFit {
            closed: 0.205,
            up: 0.86,
            open: 0.703,
            down: 0.833,
        };
        for vertical in [-1.0, -LID_FIT_PITCH, -0.1, 0.0, 0.1, LID_FIT_PITCH, 1.0] {
            assert_eq!(expected_open(&v_shape, vertical), 0.703, "{vertical}");
        }
        // An eye that reads less open looking up follows that, and holds it beyond 15° up
        let lower_up = LidFit { up: 0.8, ..fit };
        assert!((expected_open(&lower_up, 0.5 * LID_FIT_PITCH) - 0.85).abs() < 1e-6);
        for vertical in [LID_FIT_PITCH, 0.6, 1.0] {
            assert!((expected_open(&lower_up, vertical) - 0.8).abs() < 1e-6, "{vertical}");
        }
        // Never below half the straight-ahead reading, however low the down one
        let steep = LidFit {
            closed: 0.1,
            up: 0.9,
            open: 0.9,
            down: 0.3,
        };
        assert!((expected_open(&steep, -LID_FIT_PITCH) - 0.45).abs() < 1e-6);
        assert!((expected_open(&steep, -1.0) - 0.45).abs() < 1e-6);
        // From straight ahead outwards it only ever falls or stays, within the fitted readings
        for fit in [fit, v_shape, lower_up, steep] {
            for direction in [-1.0_f32, 1.0] {
                let mut last = expected_open(&fit, 0.0);
                for step in 1..=100 {
                    let value = expected_open(&fit, direction * step as f32 / 100.0);
                    assert!(value <= last + 1e-6 && value <= fit.open, "{fit:?} {direction} {step}");
                    assert!(value >= fit.up.min(fit.down).max(0.5 * fit.open).min(fit.open) - 1e-6, "{fit:?}");
                    last = value;
                }
            }
        }
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
            // Relative to the expected reading for where the eyes look, which is never above the straight-ahead one:
            // looking up (fitted 0.82 there) widening starts and is full at the same readings as straight ahead...
            let up = |raw: f32| lid_to_vrcft(lid_inputs([0.96, raw], LID_FIT_PITCH, [1.0; 2], &settings)[1], &settings);
            assert!((up(0.8 + start - 0.001) - 0.75).abs() < 1e-5, "{widen:?}");
            assert!(up(0.8 + start + 0.01) > 0.75, "{widen:?}");
            assert!((up(0.8 + full) - 1.0).abs() < 1e-5, "{widen:?}");
            // ...and looking down (0.7 there) from the down reading
            let down =
                |raw: f32| lid_to_vrcft(lid_inputs([0.96, raw], -LID_FIT_PITCH, [1.0; 2], &settings)[1], &settings);
            assert!((down(0.7 + start - 0.001) - 0.75).abs() < 1e-5, "{widen:?}");
            assert!((down(0.7 + full) - 1.0).abs() < 1e-5, "{widen:?}");
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
        // The case that stepped before: far down, where the expected reading is below the floor of the range (a down
        // reading of 0.4 against 0.835 straight ahead)
        let settings = Settings {
            lid_fit_down_right: Some(0.4),
            ..user_fit(Widen::Normal)
        };
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
        // 22° down the expected open reading is held at the down reading, 0.5, only 0.1 above closed; the range is
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
    fn widening_stays_reachable_wherever_the_eyes_look() {
        let generic = Settings {
            lid_fit_open_right: Some(0.8),
            lid_fit_up_right: Some(0.82),
            ..fitted()
        };
        for widen in [Widen::Low, Widen::Normal, Widen::High] {
            for base in [user_fit(widen), generic.clone()] {
                let settings = Settings { lid_widen: widen, ..base };
                let fit = settings.lid_fit()[1].unwrap();
                assert!(widen_room(&fit, widen));
                let (start, _) = widen_offsets(widen).unwrap();
                for vertical in [-1.0, -0.5, -LID_FIT_PITCH, -0.1, 0.0, 0.1, LID_FIT_PITCH, 0.5, 1.0] {
                    let lid = |raw: f32| lid_to_vrcft(fitted_lid(raw, vertical, &fit, &settings), &settings);
                    // Widening starts at most `start` above the straight-ahead reading, and is full by 1.000
                    let begins = open_reading(&fit, vertical) + start;
                    assert!(begins <= fit.open + start + 1e-6, "{widen:?} {vertical}");
                    assert!(lid(begins + 0.01) > 0.75, "{widen:?} {vertical}");
                    assert_eq!(lid(OPENNESS_CAP), 1.0, "{widen:?} {vertical}");
                }
            }
        }
    }

    /// Sent eyelids of unfitted eyes against the default lid marks (0.8 relaxed, 1.0 fully wide), open except for
    /// the listed changes.
    fn sent_lid_track(raw: bool, length: usize, changes: &[(usize, [f32; 2])]) -> Vec<[f32; 2]> {
        let settings = Settings { raw, ..settings() };
        run(&settings, &openness_track(length, changes)).iter().map(|sample| sample.lids).collect()
    }

    #[test]
    fn a_widen_shows_only_once_it_lasts() {
        let sustain = (WIDEN_SUSTAIN / f64::from(NOMINAL_DT)).round() as usize;
        let wide = |from: usize, to: usize| (from..to).map(|i| (i, [1.0; 2])).collect::<Vec<_>>();
        let relaxed = |lids: &[f32; 2]| lids.iter().all(|lid| *lid <= LID_RELAXED + 1e-6);
        // A 200 ms jump to 1.000 right before a blink, and a 170 ms one right after it: never above relaxed open
        let mut blink = wide(40, 58);
        blink.extend((58..70).map(|i| (i, [0.1; 2])));
        blink.extend(wide(70, 85));
        let sent = sent_lid_track(false, 120, &blink);
        assert!(sent.iter().all(relaxed), "{sent:?}");
        // Held wide: relaxed open until it has lasted WIDEN_SUSTAIN (a sample later for the despike), then wide...
        let mut held = wide(40, 140);
        // ...on through a 45 ms dip back to relaxed open...
        held.retain(|(i, _)| !(100..104).contains(i));
        let sent = sent_lid_track(false, 160, &held);
        assert!(sent[..40 + sustain].iter().all(relaxed), "{:?}", &sent[..40 + sustain]);
        assert!(sent[40 + sustain + 12].iter().all(|lid| *lid > 0.95), "{:?}", sent[40 + sustain + 12]);
        assert!(sent[110..140].iter().all(|lids| lids.iter().all(|lid| *lid > 0.9)), "{:?}", &sent[100..140]);
        // ...and starting over after a blink
        let mut again = wide(40, 100);
        again.extend((100..110).map(|i| (i, [0.1; 2])));
        again.extend(wide(110, 130));
        let sent = sent_lid_track(false, 150, &again);
        assert!(sent[90].iter().all(|lid| *lid > 0.95), "{:?}", sent[90]);
        assert!(sent[110..].iter().all(relaxed), "{:?}", &sent[110..]);
        // Unsmoothed output has no timed stages: wide at once
        assert_eq!(sent_lid_track(true, 20, &wide(5, 6))[5], [1.0; 2]);
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
    fn sending_never_blocks_and_a_full_buffer_drops_quietly() {
        // The socket never blocks
        let listener = UdpSocket::bind("127.0.0.1:0").unwrap();
        let socket = connect(listener.local_addr().unwrap()).unwrap();
        let flags = unsafe { libc::fcntl(std::os::fd::AsRawFd::as_raw_fd(&socket), libc::F_GETFL) };
        assert_ne!(flags & libc::O_NONBLOCK, 0);

        // A datagram the network can't take at once is dropped and counted: one line, then at most one a minute with
        // how many since, and never "sending failed"
        let addr: SocketAddr = "192.0.2.1:9000".parse().unwrap();
        let mut output = Output::new(Target::Fixed { host: "192.0.2.1".into(), port: 9000 });
        let full = || Err(io::Error::from(io::ErrorKind::WouldBlock));
        let t0 = Instant::now();
        let at = |ms: u64| t0 + Duration::from_millis(ms);
        let line = output.note(at(0), addr, &full()).unwrap();
        assert_eq!(
            line,
            "Dropped 1 datagram to 192.0.2.1:9000 over 0 s: the network was too busy to take it at once \
             (said at most once a minute)"
        );
        for i in 1..=200 {
            assert_eq!(output.note(at(i * 100), addr, &full()), None);
        }
        assert_eq!(output.drops.per_second(at(20_000)), 10.0);
        assert_eq!(output.health.failing, None);
        // Sending works in between: nothing to say
        assert_eq!(output.note(at(20_050), addr, &Ok(20)), None);
        let line = output.note(at(60_000), addr, &full()).unwrap();
        assert!(line.starts_with("Dropped 201 datagrams to 192.0.2.1:9000 over 60 s: "), "{line}");
        assert_eq!(output.note(at(60_100), addr, &full()), None);
        assert_eq!(output.drops.per_second(at(70_000)), 0.0);
        // A real failure is still one
        assert!(output.note(at(70_000), addr, &Err(io::Error::from(io::ErrorKind::NetworkUnreachable))).is_some());
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
            pace: Pace::default(),
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
        // Looking right and down (22.5°) with the left eye wide open: one packet per sample, as processed (the
        // widening shows once it has lasted WIDEN_SUSTAIN)
        let look = [(std::f32::consts::PI / 8.0).tan(), -(std::f32::consts::PI / 8.0).tan()];
        let widened_from = (WIDEN_SUSTAIN / f64::from(NOMINAL_DT)).ceil() as usize;
        for index in 0..widened_from + 3 {
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
            assert!(right.0 < 1.0 && left.2 > 0.0 && left.3 < 0.0);
            assert_eq!(left.1 > 0.0, index >= widened_from, "{index}");
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

    /// A file shaped like the eye server's shared memory of one version: zeroed (a zeroed glibc mutex is an unlocked
    /// default one), initialized, with nothing published yet. Removed when dropped.
    struct FakeShm {
        path: PathBuf,
    }

    impl FakeShm {
        fn new(name: &str, version: u32, size: usize) -> Self {
            let mut bytes = vec![0u8; size];
            bytes[0..4].copy_from_slice(&version.to_le_bytes());
            bytes[4..8].copy_from_slice(&1u32.to_le_bytes());
            Self::with(name, &bytes)
        }

        fn with(name: &str, bytes: &[u8]) -> Self {
            let path = std::env::temp_dir().join(format!("frameeyeosc-test-{}-{name}", std::process::id()));
            fs::write(&path, bytes).unwrap();
            Self { path }
        }

        fn bytes(&self) -> Vec<u8> {
            fs::read(&self.path).unwrap()
        }
    }

    impl Drop for FakeShm {
        fn drop(&mut self) {
            let _ = fs::remove_file(&self.path);
        }
    }

    // A sample record's floats, from the gaze (0x0d) to the last extra value (ending at 0xa1).
    const RECORD_FLOATS: usize = 37;

    /// Write a record at `base` the way the eye server lays it out (offsets written out, not taken from the struct):
    /// producer state, sample time, then the floats in order, each `seed` plus its index / 100.
    fn write_record(bytes: &mut [u8], base: usize, state: u32, seed: f32) {
        bytes[base..base + 4].copy_from_slice(&state.to_le_bytes());
        bytes[base + 5..base + 0x0d].copy_from_slice(&(100.0 + f64::from(seed)).to_le_bytes());
        for i in 0..RECORD_FLOATS {
            let offset = base + 0x0d + 4 * i;
            bytes[offset..offset + 4].copy_from_slice(&(seed + i as f32 / 100.0).to_le_bytes());
        }
    }

    /// The sample `write_record` writes.
    fn record_sample(seed: f32) -> EyeData {
        let value = |i: usize| seed + i as f32 / 100.0;
        let vector = |i: usize| [value(i), value(i + 1), value(i + 2)];
        EyeData {
            sample_time: 100.0 + f64::from(seed),
            gaze: [vector(0), vector(3)],
            gaze_covariance: [vector(6), vector(9)],
            fixation_point: vector(12),
            pre_fusion_gaze: [vector(15), vector(18)],
            pre_fusion_covariance: [vector(21), vector(24)],
            openness: [value(27), value(28)],
            extra: std::array::from_fn(|i| value(29 + i)),
        }
    }

    fn decode_at(bytes: &[u8], offset: usize) -> Next {
        assert!(offset + size_of::<EyeDataMmap>() <= bytes.len());
        decode(unsafe { ptr::read_unaligned(bytes.as_ptr().add(offset).cast::<EyeDataMmap>()) })
    }

    #[test]
    fn shm_versions_have_their_layouts() {
        assert_eq!(shm_layout(4).unwrap().eye_data, 0x152);
        assert_eq!(shm_layout(4).unwrap().size, 0x4f21a);
        assert_eq!(shm_layout(5).unwrap().eye_data, 0x157);
        assert_eq!(shm_layout(5).unwrap().size, 0x4f21f);
        assert_eq!(
            shm_layout(6).unwrap_err(),
            "unsupported eye shared-memory version 6; supported: 4, 5"
        );
    }

    #[test]
    fn records_decode_at_each_versions_offset() {
        for layout in SHM_LAYOUTS {
            let mut bytes = vec![0u8; layout.size];
            write_record(&mut bytes, layout.eye_data, 1, 0.25);
            match decode_at(&bytes, layout.eye_data) {
                Next::Sample(data) => assert_eq!(data, record_sample(0.25), "version {}", layout.version),
                _ => panic!("version {}: no sample", layout.version),
            }
            // Read at the other version's offset, it is not the sample: the offsets really differ.
            let other = SHM_LAYOUTS.iter().find(|other| other.version != layout.version).unwrap();
            assert!(!matches!(decode_at(&bytes, other.eye_data), Next::Sample(data) if data == record_sample(0.25)));
            write_record(&mut bytes, layout.eye_data, 0, 0.25);
            assert!(matches!(decode_at(&bytes, layout.eye_data), Next::Stopped));
        }
    }

    /// The start of /dev/shm/eye-server.mmap as the version-5 eye server left it (SteamOS 0.4.3, 2026-10-02, headset
    /// off); everything after it was zero.
    const V5_SNAPSHOT: [&str; 11] = [
        "050000000100000000000000010000000000000000000000900000000000000000000000000000000000000000000000",
        "000000000000000083030000010000000000000000000000010000000000000000000000900000000000000000000000",
        "00000000000000000000000000000000000000000d00000000000000009da24e3919ef4a40dc0ed63d8de469beaaa6a9",
        "bf2d01000000e141537972fe4a40feafc23dcc7b72be3981a9bf2e01000000bef697a0b0084b407900da3d9ede6fbe39",
        "9fa9bf2d01000000e594ca25df164b40e2e0a43da00d6ebe1439a9bf2e010000002ade1c3c006f4a4082fcc9bdbbfeaf",
        "be4417a6bf2d01000000c955b56f317e4a404969aebdba5babbe4188a6bf2e010000003ed8e33725a94a4056b62f3c4b",
        "9fcfbeaf3caabf2d0100000030430c0952b64a4013f2053cf79acdbe842faabf2e010000040000000400000000000000",
        "b44200ffffffff01000000001e362ef09dfe4c4072396fbda75602bde26e7fbf738279bda35102bd0e657fbf6078f93b",
        "a31bda3bc135d53b88bdc03ba31bda3b362fa93b84e9bebf85a34bbfd58ac7c18ce706bec046583c9c1d7abf0008b939",
        "4ae889bd69727cbf6078f93b51b56b3cc135d53b88bdc03b16f44a3c362fa93b9fc0fc3e0000803f34f2e53cbbf41d3d",
        "068bd43d7eb6a6bd53d9923bec77c73bb84cfe3b957fb13b",
    ];

    #[test]
    fn real_version_5_file_decodes() {
        let mut bytes = vec![0u8; 0x4f21f];
        let prefix: Vec<u8> = V5_SNAPSHOT
            .concat()
            .as_bytes()
            .chunks(2)
            .map(|pair| u8::from_str_radix(std::str::from_utf8(pair).unwrap(), 16).unwrap())
            .collect();
        bytes[..prefix.len()].copy_from_slice(&prefix);
        let shm = FakeShm::with("real-v5", &bytes);
        let source = EyeSource::open_at(&shm.path).unwrap();
        assert_eq!(source.layout.version, 5);
        let Next::Sample(data) = decode_at(&bytes, source.layout.eye_data) else {
            panic!("no sample");
        };
        assert_eq!(data.sample_time, 57.989194891513975);
        let length = |gaze: &[f32; 3]| gaze.iter().map(|value| value * value).sum::<f32>().sqrt();
        for gaze in &data.gaze {
            assert!((length(gaze) - 1.0).abs() < 1e-3, "{gaze:?}");
            assert!(gaze[2] < -0.9, "looks ahead (-Z): {gaze:?}");
        }
        // Before fusion the gaze is only about unit length, in version 4 too (0.99-1.00 in a worn recording).
        for gaze in &data.pre_fusion_gaze {
            assert!((0.95..1.001).contains(&length(gaze)), "{gaze:?}");
            assert!(gaze[2] < -0.9, "looks ahead (-Z): {gaze:?}");
        }
        assert_eq!(data.openness, [0.49365708, 1.0]);
        assert!(data.gaze_covariance.iter().flatten().all(|value| (0.0..0.05).contains(value)));
        assert!(data.fixation_point[2] < 0.0);
        // Opening wrote nothing into the file.
        drop(source);
        assert_eq!(shm.bytes(), bytes);
    }

    #[test]
    fn unknown_or_short_shared_memory_is_refused_untouched() {
        let unknown = FakeShm::new("v6", 6, 0x4f21f);
        let before = unknown.bytes();
        let error = EyeSource::open_at(&unknown.path).err().unwrap().to_string();
        assert!(error.starts_with("unsupported eye shared-memory version 6; supported: 4"), "{error}");
        assert_eq!(unknown.bytes(), before);

        // A version-5 file of the version-4 size can't hold the version-5 record and images.
        let short = FakeShm::new("v5-short", 5, 0x4f21a);
        let error = EyeSource::open_at(&short.path).err().unwrap().to_string();
        assert!(error.ends_with("shared memory is too small"), "{error}");

        let uninitialized = FakeShm::new("v5-uninit", 5, 0x4f21f);
        let mut bytes = uninitialized.bytes();
        bytes[4] = 0;
        fs::write(&uninitialized.path, &bytes).unwrap();
        let error = EyeSource::open_at(&uninitialized.path).err().unwrap().to_string();
        assert_eq!(error, "eye shared memory is not initialized");
    }

    #[test]
    fn reader_follows_the_sequence_and_only_sets_the_request() {
        for layout in SHM_LAYOUTS {
            let version = layout.version;
            let shm = FakeShm::new(&format!("v{version}-protocol"), version, layout.size);
            let mut expected = shm.bytes();
            let mut source = EyeSource::open_at(&shm.path).unwrap();

            // Nothing published: the reader waits, and all it changed is metadata_requested (the mutex is unlocked
            // again).
            assert!(matches!(source.next(Duration::from_millis(10)).unwrap(), Next::Waiting));
            expected[0x3c] = 1;
            assert_eq!(shm.bytes(), expected, "version {version}");

            // A stand-in for the eye server, through its own mapping: once a sample is requested, it writes the
            // record, bumps the sequence, clears the request and wakes the reader.
            let file = OpenOptions::new().read(true).write(true).open(&shm.path).unwrap();
            let mut server = unsafe { MmapOptions::new().len(layout.size).map_mut(&file).unwrap() };
            server[0x3c] = 0;
            let eye_data = layout.eye_data;
            let publisher = std::thread::spawn(move || {
                while unsafe { ptr::read_volatile(server.as_ptr().add(0x3c).cast::<u32>()) } == 0 {
                    std::thread::yield_now();
                }
                write_record(&mut server, eye_data, 1, 0.5);
                let base = server.as_mut_ptr();
                let sequence = unsafe { base.add(0x38).cast::<u32>() };
                unsafe {
                    ptr::write_volatile(sequence, ptr::read_volatile(sequence) + 1);
                    ptr::write_volatile(base.add(0x3c).cast::<u32>(), 0);
                    libc::syscall(libc::SYS_futex, sequence, libc::FUTEX_WAKE, i32::MAX);
                }
                server
            });
            let next = source.next(Duration::from_secs(5)).unwrap();
            drop(publisher.join().unwrap());
            match next {
                Next::Sample(data) => assert_eq!(data, record_sample(0.5), "version {version}"),
                _ => panic!("version {version}: no sample"),
            }

            // The same sequence again: no new sample, though the record is still there.
            assert!(matches!(source.next(Duration::from_millis(10)).unwrap(), Next::Waiting));
            write_record(&mut expected, eye_data, 1, 0.5);
            expected[0x38] = 1;
            expected[0x3c] = 1;
            assert_eq!(shm.bytes(), expected, "version {version}");
        }
    }

    #[test]
    fn reader_keeps_trying_and_logs_each_reason_once() {
        let shm = FakeShm::new("reader", 6, 0x4f21f);
        let unsupported = shm.bytes();
        fs::remove_file(&shm.path).unwrap();
        let start = Instant::now();
        let at = |seconds: u32| start + REOPEN_INTERVAL * seconds;
        let mut reader = EyeReader::new(&shm.path, start);

        // Missing (SteamVR not started yet): said once, with the path, and tried again each interval.
        let line = reader.open_if_due(at(0)).unwrap();
        assert!(line.starts_with("Can't read eye data (") && line.ends_with("; retrying every 1 s"), "{line}");
        let error = reader.error.clone().unwrap();
        assert!(error.starts_with(&format!("{}: ", shm.path.display())), "{error}");
        assert_eq!(reader.open_if_due(at(0)), None);
        assert_eq!(reader.next_open, at(1));
        assert_eq!(reader.open_if_due(at(1)), None);
        assert_eq!(reader.next_open, at(2));
        assert_eq!(reader.error.as_deref(), Some(error.as_str()));

        // A version this doesn't know: a new reason, so said again, and the file is left untouched.
        fs::write(&shm.path, &unsupported).unwrap();
        let line = reader.open_if_due(at(2)).unwrap();
        assert!(line.contains("unsupported eye shared-memory version 6; supported: 4"), "{line}");
        assert_eq!(reader.open_if_due(at(3)), None);
        assert!(reader.source.is_none());
        assert!(reader.error.as_deref().unwrap().starts_with("unsupported eye shared-memory version 6"));
        assert_eq!(shm.bytes(), unsupported);

        // Readable: opened at the next try, and the reason is gone.
        let readable = FakeShm::new("reader", 5, 0x4f21f);
        assert_eq!(readable.path, shm.path);
        assert_eq!(reader.open_if_due(at(3) + POLL), None);
        let line = reader.open_if_due(at(4)).unwrap();
        assert_eq!(line, format!("Reading {} (version 5)", shm.path.display()));
        assert!(reader.source.is_some());
        assert_eq!(reader.error, None);
        assert_eq!(reader.open_if_due(at(5)), None);

        // Let go of (replaced): opened again right away, and said again.
        reader.close(at(5));
        assert!(reader.source.is_none());
        assert_eq!(reader.open_if_due(at(5)), Some(line));
    }

    /// Publish a sample through the eye server's `server` mapping the way the eye server does, without the lock: write
    /// the record, bump the sequence, clear the request. The caller wakes the readers.
    fn publish(server: &mut [u8], eye_data: usize, seed: f32) {
        write_record(server, eye_data, 1, seed);
        let base = server.as_mut_ptr();
        unsafe {
            let sequence = base.add(0x38).cast::<u32>();
            ptr::write_volatile(sequence, ptr::read_volatile(sequence).wrapping_add(1));
            ptr::write_volatile(base.add(0x3c).cast::<u32>(), 0);
        }
    }

    fn wake(server: &mut [u8]) {
        unsafe { libc::syscall(libc::SYS_futex, server.as_mut_ptr().add(0x38), libc::FUTEX_WAKE, i32::MAX) };
    }

    #[test]
    fn a_sample_published_while_busy_is_read_at_once_and_skips_are_counted() {
        let layout = SHM_LAYOUTS[1];
        // A record is already there when frameeyeosc starts (sequence 5, nothing requested)
        let mut bytes = vec![0u8; layout.size];
        bytes[0..4].copy_from_slice(&5u32.to_le_bytes());
        bytes[4..8].copy_from_slice(&1u32.to_le_bytes());
        bytes[0x38..0x3c].copy_from_slice(&5u32.to_le_bytes());
        write_record(&mut bytes, layout.eye_data, 1, 0.1);
        let shm = FakeShm::with("pipeline", &bytes);
        let mut source = EyeSource::open_at(&shm.path).unwrap();
        let file = OpenOptions::new().read(true).write(true).open(&shm.path).unwrap();
        let mut server = unsafe { MmapOptions::new().len(layout.size).map_mut(&file).unwrap() };

        // The first call doesn't return the stale record; it requests a sample and waits for a new one.
        assert!(matches!(source.next(Duration::from_millis(10)).unwrap(), Next::Waiting));
        assert_eq!(server[0x3c], 1);
        publish(&mut server, layout.eye_data, 0.2);
        wake(&mut server);
        assert!(matches!(source.next(Duration::from_secs(1)).unwrap(), Next::Sample(data) if data == record_sample(0.2)));
        // The next one is requested in the same lock the sample was read in, before the next call
        assert_eq!(server[0x3c], 1);
        assert_eq!(source.take_missed(), 0);

        // Published while that sample was processed (nobody woken, as when the wake came before the next wait): read at
        // once, without waiting for the one after
        publish(&mut server, layout.eye_data, 0.3);
        let start = Instant::now();
        assert!(matches!(source.next(Duration::from_secs(1)).unwrap(), Next::Sample(data) if data == record_sample(0.3)));
        assert!(start.elapsed() < Duration::from_millis(500), "{:?}", start.elapsed());
        assert_eq!(server[0x3c], 1);

        // Two published while busy (another client asked for them too): the newer is read, the older counted as missed
        publish(&mut server, layout.eye_data, 0.4);
        publish(&mut server, layout.eye_data, 0.5);
        assert!(matches!(source.next(Duration::from_secs(1)).unwrap(), Next::Sample(data) if data == record_sample(0.5)));
        assert_eq!(source.take_missed(), 1);
        assert_eq!(source.take_missed(), 0);
        // Nothing new: waits again
        assert!(matches!(source.next(Duration::from_millis(10)).unwrap(), Next::Waiting));

        // The sequence starting over (a jump back, or one too large) is not counted as missed
        unsafe { ptr::write_volatile(server.as_mut_ptr().add(0x38).cast::<u32>(), 2) };
        assert!(matches!(source.next(Duration::from_secs(1)).unwrap(), Next::Sample(_)));
        unsafe { ptr::write_volatile(server.as_mut_ptr().add(0x38).cast::<u32>(), 2 + MAX_MISSED_JUMP + 1) };
        assert!(matches!(source.next(Duration::from_secs(1)).unwrap(), Next::Sample(_)));
        assert_eq!(source.take_missed(), 0);

        // Only the lock, the request and nothing else was written: the file is what the test wrote, with the request set
        let mut expected = bytes.clone();
        write_record(&mut expected, layout.eye_data, 1, 0.5);
        expected[0x38..0x3c].copy_from_slice(&(2 + MAX_MISSED_JUMP + 1).to_le_bytes());
        expected[0x3c] = 1;
        drop(server);
        assert_eq!(shm.bytes(), expected);
    }

    /// Run the eye server as read off its disassembly for `frames` frames of 11.1 ms (90 a second), next to a reader
    /// that takes `processing` over each sample; returns (samples read, samples the server published). Each frame the
    /// server locks metadata_mutex and, only if a sample was requested by then, writes the record, bumps the sequence
    /// and clears the request; then it unlocks and wakes the readers, `wake_delay` late (the reader's thread woken late,
    /// as on a busy headset).
    fn run_paced(frames: u32, wake_delay: Duration, processing: Duration) -> (u32, u32) {
        let layout = SHM_LAYOUTS[1];
        let shm = FakeShm::new("paced", 5, layout.size);
        let mut source = EyeSource::open_at(&shm.path).unwrap();
        let file = OpenOptions::new().read(true).write(true).open(&shm.path).unwrap();
        let mut server = unsafe { MmapOptions::new().len(layout.size).map_mut(&file).unwrap() };
        // The mutex is a process-private one in this file, so the server locks it at the reader's own address
        let mutex = unsafe { source.map.as_mut_ptr().add(0x08) } as usize;
        let frame = Duration::from_secs(1) / 90;
        let publisher = std::thread::spawn(move || {
            let mutex = mutex as *mut libc::pthread_mutex_t;
            let start = Instant::now();
            let mut published = 0;
            for i in 1..=frames {
                if let Some(wait) = (start + frame * i).checked_duration_since(Instant::now()) {
                    std::thread::sleep(wait);
                }
                assert_eq!(unsafe { libc::pthread_mutex_lock(mutex) }, 0);
                let requested = unsafe { ptr::read_volatile(server.as_ptr().add(0x3c).cast::<u32>()) } != 0;
                if requested {
                    publish(&mut server, layout.eye_data, i as f32);
                    published += 1;
                }
                unsafe { libc::pthread_mutex_unlock(mutex) };
                if requested {
                    std::thread::sleep(wake_delay);
                    wake(&mut server);
                }
            }
            published
        });
        let mut read = 0;
        loop {
            match source.next(Duration::from_millis(100)).unwrap() {
                Next::Sample(_) => {
                    read += 1;
                    std::thread::sleep(processing);
                }
                Next::Waiting if publisher.is_finished() => break,
                Next::Waiting => {}
                Next::Stopped => panic!("stopped"),
            }
        }
        let published = publisher.join().unwrap();
        assert_eq!(source.take_missed(), 0, "the only reader misses nothing the server published");
        (read, published)
    }

    #[test]
    fn a_reader_slower_than_a_frame_still_gets_every_sample() {
        // 8 ms to wake up and 7 ms over each sample: 15 ms from one sample being published to the reader asking for the
        // next, more than a frame. Asking only then, the server skipped every other frame (about 45 of 90 read); asked
        // for while the sample is copied out, every frame is published and read.
        let frames = 90;
        let (read, published) = run_paced(frames, Duration::from_millis(8), Duration::from_millis(7));
        eprintln!("read {read} of {frames} frames ({published} published)");
        assert_eq!(read, published);
        assert!(read >= frames * 85 / 100, "read {read} of {frames} frames");
    }

    #[test]
    fn a_replaced_or_rewritten_shared_memory_is_stale_and_left_alone() {
        let shm = FakeShm::new("stale", 5, 0x4f21f);
        let mut source = EyeSource::open_at(&shm.path).unwrap();
        assert!(!source.is_stale());

        // The same file rewritten as a version this doesn't know: stale, and nothing is written to it any more.
        let file = OpenOptions::new().write(true).open(&shm.path).unwrap();
        std::os::unix::fs::FileExt::write_all_at(&file, &6u32.to_le_bytes(), 0).unwrap();
        let before = shm.bytes();
        assert!(source.is_stale());
        assert!(matches!(source.next(Duration::from_millis(10)).unwrap(), Next::Stopped));
        assert_eq!(shm.bytes(), before);

        // Back to version 5, then replaced by a new file at the same path.
        std::os::unix::fs::FileExt::write_all_at(&file, &5u32.to_le_bytes(), 0).unwrap();
        assert!(!source.is_stale());
        fs::remove_file(&shm.path).unwrap();
        assert!(source.is_stale());
        let _replacement = FakeShm::new("stale", 5, 0x4f21f);
        assert!(source.is_stale());
    }

    #[test]
    fn status_says_why_eye_data_cant_be_read() {
        let bridge = test_bridge(settings());
        let json = serde_json::to_value(bridge.status(None, None)).unwrap();
        assert!(json["source_error"].is_null());
        let error = "unsupported eye shared-memory version 6; supported: 4, 5";
        let json = serde_json::to_value(bridge.status(Some(error), None)).unwrap();
        assert_eq!(json["source_error"], error);
    }

    #[test]
    fn dominant_eye_is_read_only_from_states_it_knows() {
        let read = |flag: u8, eye: i32| {
            let [a, b, c, d] = eye.to_le_bytes();
            parse_dominant_eye([flag, a, b, c, d])
        };
        // As read off the headset: off, on with the left eye, on with the right eye
        assert_eq!(read(0, -1), Some(None));
        assert_eq!(read(1, 0), Some(Some(DominantEye::Left)));
        assert_eq!(read(1, 1), Some(Some(DominantEye::Right)));
        // Off, whichever eye was picked before
        assert_eq!(read(0, 0), Some(None));
        assert_eq!(read(0, 1), Some(None));
        // Anything else is not trusted: on without an eye, other flags or eyes, halves of two states
        assert_eq!(read(1, -1), None);
        assert_eq!(read(2, 1), None);
        assert_eq!(read(1, 2), None);
        assert_eq!(parse_dominant_eye([1, 0xff, 0xff, 0, 0]), None);
        assert_eq!(parse_dominant_eye([1, 1, 0xff, 0xff, 0xff]), None);
        assert_eq!(parse_dominant_eye([0xff; 5]), None);
    }

    #[test]
    fn dominant_eye_is_read_without_writing_and_only_in_version_5() {
        // The real version-5 file has the setting off
        let mut bytes = vec![0u8; 0x4f21f];
        let prefix: Vec<u8> = V5_SNAPSHOT
            .concat()
            .as_bytes()
            .chunks(2)
            .map(|pair| u8::from_str_radix(std::str::from_utf8(pair).unwrap(), 16).unwrap())
            .collect();
        bytes[..prefix.len()].copy_from_slice(&prefix);
        assert_eq!(bytes[0x152..0x157], [0x00, 0xff, 0xff, 0xff, 0xff]);
        let shm = FakeShm::with("dominant-v5", &bytes);
        let start = Instant::now();
        let mut reader = EyeReader::new(&shm.path, start);
        assert!(reader.open_if_due(start).is_some());
        assert_eq!(reader.source.as_ref().unwrap().dominant_eye(), Some(None));
        assert_eq!(reader.refresh_dominant_eye(), None);
        assert_eq!(reader.dominant_eye, None);

        // Turned on in SteamVR with the right eye, then the left one: said once each
        let file = OpenOptions::new().write(true).open(&shm.path).unwrap();
        let set = |value: [u8; 5]| std::os::unix::fs::FileExt::write_all_at(&file, &value, 0x152).unwrap();
        set([1, 1, 0, 0, 0]);
        let line = reader.refresh_dominant_eye().unwrap();
        assert_eq!(line, "Track Dominant Eye Only is on: the Frame tracks the right eye alone");
        assert_eq!(reader.dominant_eye, Some(DominantEye::Right));
        assert_eq!(reader.refresh_dominant_eye(), None);
        set([1, 0, 0, 0, 0]);
        assert!(reader.refresh_dominant_eye().unwrap().contains("left eye"));
        assert_eq!(reader.dominant_eye, Some(DominantEye::Left));
        // Caught halfway through a change: the last state stays
        set([1, 0xff, 0xff, 0xff, 0xff]);
        assert_eq!(reader.refresh_dominant_eye(), None);
        assert_eq!(reader.dominant_eye, Some(DominantEye::Left));
        set([0, 0xff, 0xff, 0xff, 0xff]);
        assert_eq!(reader.refresh_dominant_eye().unwrap(), "Track Dominant Eye Only is off");
        assert_eq!(reader.dominant_eye, None);
        // Nothing but the test's own writes changed the file
        let mut expected = bytes.clone();
        expected[0x152..0x157].copy_from_slice(&[0, 0xff, 0xff, 0xff, 0xff]);
        assert_eq!(shm.bytes(), expected);

        // Lost along with the shared memory, without a line of its own
        set([1, 1, 0, 0, 0]);
        reader.refresh_dominant_eye();
        reader.close(start);
        assert_eq!(reader.refresh_dominant_eye(), None);
        assert_eq!(reader.dominant_eye, None);

        // Version 4 has no such setting, whatever its bytes there (the start of its sample record)
        let mut v4 = vec![0u8; 0x4f21a];
        v4[0..4].copy_from_slice(&4u32.to_le_bytes());
        v4[4..8].copy_from_slice(&1u32.to_le_bytes());
        v4[0x152..0x157].copy_from_slice(&[1, 1, 0, 0, 0]);
        let shm4 = FakeShm::with("dominant-v4", &v4);
        let source = EyeSource::open_at(&shm4.path).unwrap();
        assert_eq!(source.dominant_eye(), Some(None));
        drop(source);
        assert_eq!(shm4.bytes(), v4);
    }

    #[test]
    fn status_says_whether_frameeyeosc_keeps_up() {
        let mut bridge = test_bridge(settings());
        let json = serde_json::to_value(bridge.status(None, None)).unwrap();
        // Not tracking: nothing to tell yet, and no drops
        assert!(json["missed_rate"].is_null() && json["max_processing_ms"].is_null());
        assert_eq!(json["dropped_rate"], 0.0);

        let now = Instant::now();
        bridge.active_since = Some(now - Duration::from_secs(5));
        bridge.received.extend((0..46).map(|i| now - Duration::from_millis(20 * i)));
        bridge.pace.add_missed(now - Duration::from_millis(1500), 30);
        bridge.pace.add_missed(now - Duration::from_millis(300), 1);
        bridge.pace.add_missed(now, 43);
        bridge.pace.add_missed(now, 0);
        bridge.pace.add_busy(now - Duration::from_millis(1200), Duration::from_millis(40));
        bridge.pace.add_busy(now - Duration::from_millis(500), Duration::from_micros(15_240));
        bridge.pace.add_busy(now, Duration::from_millis(2));
        let addr: SocketAddr = "192.0.2.1:9000".parse().unwrap();
        for ms in [1500, 400, 20] {
            bridge.output.drops.record(now - Duration::from_millis(ms), addr);
        }
        let json = serde_json::to_value(bridge.status(None, None)).unwrap();
        assert_eq!(json["tracker_rate"], 46.0);
        assert_eq!(json["missed_rate"], 44.0);
        assert_eq!(json["max_processing_ms"].as_f64().unwrap() as f32, 15.2);
        assert_eq!(json["dropped_rate"], 2.0);
    }

    #[test]
    fn a_rate_low_for_ten_seconds_is_logged_once_with_its_numbers() {
        let mut bridge = test_bridge(settings());
        let start = Instant::now();
        bridge.active_since = Some(start - Duration::from_secs(5));
        bridge.received.extend((0..46).map(|i| start - Duration::from_millis(20 * i)));
        assert_eq!(bridge.check_low_rate(start), None);
        assert_eq!(bridge.check_low_rate(start + Duration::from_secs(9)), None);
        let at = start + Duration::from_secs(10);
        bridge.pace.add_missed(at, 44);
        bridge.pace.add_busy(at, Duration::from_micros(15_240));
        let line = bridge.check_low_rate(at).unwrap();
        assert_eq!(
            line,
            "Eye data has been low for 10 s: 46 samples/s; in the last second 44 published samples were missed, \
             the slowest sample took 15.2 ms, and 0 datagrams were dropped"
        );
        assert_eq!(bridge.check_low_rate(start + Duration::from_secs(20)), None);
        // Fine again, then low again: logged again after another 10 s
        bridge.received.extend((0..44).map(|_| start));
        assert_eq!(bridge.check_low_rate(start + Duration::from_secs(21)), None);
        bridge.received.truncate(46);
        assert_eq!(bridge.check_low_rate(start + Duration::from_secs(22)), None);
        assert!(bridge.check_low_rate(start + Duration::from_secs(32)).is_some());
        // Not tracking: no rate, nothing to say
        bridge.active_since = None;
        assert_eq!(bridge.check_low_rate(start + Duration::from_secs(50)), None);
        assert_eq!(bridge.pace.low_since, None);
    }

    #[test]
    fn status_says_which_eye_is_tracked_alone_and_whether_openness_is_saturated() {
        let mut bridge = test_bridge(settings());
        let json = serde_json::to_value(bridge.status(None, None)).unwrap();
        assert!(json["dominant_eye"].is_null());
        assert_eq!(json["openness_saturated"], false);
        bridge.smoother.saturation.on = true;
        let json = serde_json::to_value(bridge.status(None, Some(DominantEye::Right))).unwrap();
        assert_eq!(json["dominant_eye"], "right");
        assert_eq!(json["openness_saturated"], true);
        let json = serde_json::to_value(bridge.status(None, Some(DominantEye::Left))).unwrap();
        assert_eq!(json["dominant_eye"], "left");
    }

    /// Feed `seconds` of 90 Hz samples from `start`, each eye's openness from `openness(sample index)`.
    fn feed(saturation: &mut Saturation, start: f64, seconds: f64, openness: impl Fn(usize) -> [f32; 2]) {
        for i in 0..(seconds * 90.0) as usize {
            let data = EyeData {
                sample_time: start + i as f64 / 90.0,
                openness: openness(i),
                ..EyeData::default()
            };
            saturation.add(&data);
        }
    }

    /// Eyelids sent for `seconds` of 90 Hz samples from `start` looking straight ahead, with each eye's openness from
    /// `openness(sample index)`; also whether the openness counted as saturated at each.
    fn lids_over(
        settings: &Settings,
        smoother: &mut Smoother,
        start: f64,
        seconds: f64,
        openness: impl Fn(usize) -> [f32; 2],
    ) -> Vec<([f32; 2], bool)> {
        (0..(seconds * 90.0) as usize)
            .map(|i| {
                let data = EyeData {
                    sample_time: start + i as f64 / 90.0,
                    gaze: [[0.0, 0.0, -1.0]; 2],
                    fixation_point: [0.0, 0.0, -1.0],
                    openness: openness(i),
                    ..EyeData::default()
                };
                (process(settings, smoother, [1.0; 2], &data).lids, smoother.saturation.on)
            })
            .collect()
    }

    #[test]
    fn saturated_openness_sends_no_widening() {
        // An eye without a fit and no calibration, as on SteamOS 0.4.3: relaxed open reads 1.000, past mark 4
        let settings = Settings {
            lid_calibration: false,
            ..settings()
        };
        let mut smoother = Smoother::new(&settings);
        let relaxed = lids_over(&settings, &mut smoother, 100.0, 10.0, |_| [1.0, 1.0]);
        // Until the openness is told to be saturated (600 samples) it widens as before, after WIDEN_SUSTAIN
        assert!(!relaxed[598].1 && relaxed[598].0.iter().all(|lid| *lid > 0.95), "{:?}", relaxed[598]);
        // From then on not above relaxed open, not even for the one sample the filter still holds a widen
        let on = relaxed.iter().position(|(_, saturated)| *saturated).unwrap();
        assert_eq!(on, 599);
        assert!(relaxed[on..].iter().all(|(lids, _)| lids.iter().all(|lid| *lid <= LID_RELAXED)));
        assert_eq!(relaxed.last().unwrap().0, [LID_RELAXED; 2]);
        // A blink still closes (and is held, and reopens) and a half-closed eye still reads half closed, at once
        let blink = lids_over(&settings, &mut smoother, 110.0, 1.0, |i| match i {
            0..=4 => [0.1, 0.1],
            30.. => [0.55, 0.55],
            _ => [1.0, 1.0],
        });
        // (one sample late, through the 3-sample median)
        assert_eq!(blink[1].0, [0.0; 2]);
        assert!(blink[5..30].iter().all(|(lids, _)| lids.iter().all(|lid| *lid <= LID_RELAXED)));
        let half = lid_to_vrcft(0.55, &settings);
        assert!(blink[31].0.iter().all(|lid| *lid < LID_RELAXED - 0.05), "{:?}", blink[31]);
        assert!((blink.last().unwrap().0[0] - half).abs() < 0.01, "{:?} {half}", blink.last());
        assert!(blink.iter().all(|(_, saturated)| *saturated));

        // --raw is left raw
        let raw = Settings { raw: true, ..settings.clone() };
        let mut smoother = Smoother::new(&raw);
        let sent = lids_over(&raw, &mut smoother, 100.0, 8.0, |_| [1.0, 1.0]);
        assert!(sent.last().unwrap().1 && sent.last().unwrap().0 == [1.0; 2], "{:?}", sent.last());
    }

    #[test]
    fn widening_is_capped_only_while_saturated() {
        let settings = Settings {
            lid_calibration: false,
            ..settings()
        };
        // Widened at 0.995 (below 0.999): never saturated, and sent widened as before
        let mut smoother = Smoother::new(&settings);
        let wide = lids_over(&settings, &mut smoother, 100.0, 8.0, |_| [0.995, 0.995]);
        assert!(wide.iter().all(|(_, saturated)| !*saturated));
        assert!(wide.last().unwrap().0.iter().all(|lid| *lid > 0.95), "{:?}", wide.last());
        // Then reading 1.000: once more than half the minute is, it turns on, and the widen still in the filter is cut
        // off at once
        let after = lids_over(&settings, &mut smoother, 108.0, 12.0, |_| [1.0, 1.0]);
        let on = after.iter().position(|(_, saturated)| *saturated).unwrap();
        assert!(on > 0 && after[on - 1].0.iter().all(|lid| *lid > 0.95), "{on} {:?}", after[on - 1]);
        assert!(after[on..].iter().all(|(lids, _)| lids.iter().all(|lid| *lid <= LID_RELAXED)));
        // Widening is back by itself once the minute reads below 1.000 again, after WIDEN_SUSTAIN
        let back = lids_over(&settings, &mut smoother, 120.0, 70.0, |_| [0.995, 0.995]);
        let off = back.iter().position(|(_, saturated)| !*saturated).unwrap();
        assert!(back[off..off + 20].iter().all(|(lids, _)| lids.iter().all(|lid| *lid <= LID_RELAXED)), "{off} {:?}", &back[off - 2..off + 25]);
        assert!(back.last().unwrap().0.iter().all(|lid| *lid > 0.95), "{:?}", back.last());
    }

    #[test]
    fn openness_saturation_is_told_from_the_readings() {
        // Before SteamOS 0.4.3: relaxed open around 0.72, with the 1.000 jump of a blink's lead-in now and then
        let mut before = Saturation::default();
        feed(&mut before, 100.0, 300.0, |i| match i % 300 {
            0..=4 => [1.0, 1.0],
            5..=20 => [0.1, 0.1],
            _ => [0.72, 0.74],
        });
        assert!(!before.on);
        // After it: relaxed open reads 1.000; on once 600 open samples are in (6.7 s)
        let mut after = Saturation::default();
        feed(&mut after, 100.0, 6.0, |_| [1.0, 0.98]);
        assert!(!after.on, "too few samples yet");
        feed(&mut after, 106.0, 1.0, |_| [1.0, 0.98]);
        assert!(after.on);
        // Either eye at 1.000 counts; blinks (eyes not both open) don't count either way
        let mut one_eye = Saturation::default();
        feed(&mut one_eye, 0.0, 20.0, |i| if i % 10 < 3 { [0.2, 0.2] } else { [0.95, 1.0] });
        assert!(one_eye.on);
        // Between the two shares it stays as it was, below the lower one it goes off again, as the old samples leave
        // the minute
        feed(&mut one_eye, 20.0, 120.0, |i| if i % 20 < 9 { [1.0, 1.0] } else { [0.8, 0.8] });
        assert!(one_eye.on, "45% saturated");
        feed(&mut one_eye, 140.0, 61.0, |i| if i % 10 < 3 { [1.0, 1.0] } else { [0.8, 0.8] });
        assert!(!one_eye.on, "30% saturated");
        // With the headset off nothing comes in and it stays; a clock that starts over starts the count over
        feed(&mut one_eye, 5.0, 5.0, |_| [1.0, 1.0]);
        assert!(!one_eye.on && one_eye.open == 450, "{}", one_eye.open);
        feed(&mut one_eye, 10.0, 2.0, |_| [1.0, 1.0]);
        assert!(one_eye.on);
    }

    #[test]
    fn openness_saturation_keeps_its_history_through_samples_slightly_out_of_order() {
        // Every tenth sample 5 ms earlier than the one before: the warm-up still finishes after 600 open samples
        let mut saturation = Saturation::default();
        for i in 0..700 {
            let jitter = if i % 10 == 9 { -0.016 } else { 0.0 };
            saturation.add(&EyeData {
                sample_time: 100.0 + i as f64 / 90.0 + jitter,
                openness: [1.0, 1.0],
                ..EyeData::default()
            });
        }
        assert!(saturation.on, "{} open samples", saturation.open);
        assert_eq!(saturation.open, 700);
    }
}
