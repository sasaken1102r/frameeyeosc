//! Steam Frame eye bridge using the eye server's private shared-memory ABI (versions 4 and 5).

mod capture;
mod config;
mod dots;
mod eyecam_ctl;
mod eyecam_live;
mod feo;
mod livelink;
mod replay;
mod status;

use capture::{Capture, CaptureResult, CaptureState};
use clap::{CommandFactory, FromArgMatches, Parser};
use config::{ActiveType, Config, LidFit, OutputKind, Reload, Settings, Widen};
use eyecam_live::{Live, LiveReader};
use memmap2::{MmapMut, MmapOptions};
use rosc::{OscMessage, OscPacket, OscType, encoder};
use status::{CalibrationStatus, CameraStatus, RawValues, SentValues, Status, StatusFile};
use std::collections::{HashSet, VecDeque};
use std::error::Error;
use std::fs::{self, OpenOptions};
use std::os::unix::fs::MetadataExt;
use std::io;
use std::mem::{align_of, offset_of, size_of};
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr, ToSocketAddrs, UdpSocket};
use std::path::{Path, PathBuf};
use std::ptr;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
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
// While sending to VRCFT's FrameEyeOSC module without eye data, a packet without TRACKING (relaxed open eyes looking
// ahead) this often, so the module tells "no eye data" from "frameeyeosc gone" (it relaxes the eyes after a second
// without packets). Every eye sample goes to it: the module takes in all queued packets and keeps the newest.
const FEO_IDLE_INTERVAL: Duration = Duration::from_millis(500);
// The eye server produces samples at ~90 Hz.
const NOMINAL_DT: f32 = 1.0 / 90.0;
// Gaps longer than this restart the filters instead of smearing across them.
const MAX_GAP: f64 = 0.25;
// Blinks are fast, so eyelids track their speed with a quicker derivative filter than gaze.
const LID_D_CUTOFF: f32 = 1.0;
// The sideways gaze hold (--gaze-down-hold-x-deg) fades in over this many degrees further down.
const DOWN_HOLD_FADE_DEG: f32 = 10.0;
// With --independent-eyes, how far the eyes turn in from each other (degrees, both together): the eyes on something
// about 2 m away. Fixed, because the Frame's own per-eye sideways gaze can't show it: on 2026-10-08, with the eyes on
// a fit dot 0.9 m away (4.4° at a 69.5 mm IPD), its left - right read 0.1-1.1° looking up, 0.6-17.3° straight ahead
// and 3.9-9.5° looking down, and while recording it swung between "far" (under 1°) and "near" (over 4°) 20-32 times
// a minute, with medians of 5-7° and a p95 of 12-18°. Sent as it was, the avatar went cross-eyed (p95 of 13-19°)
// and every eye fit baked a different +4° to -13° into it. The eye cameras' pupils agree better with the fused gaze
// than with that eye's own reading (R² 0.84-0.89 against 0.78-0.80 on the left, 0.69-0.80 against 0.54-0.73 on the
// right).
const VERGENCE_DEG: f32 = 2.0;
// Intentional cross-eye (a finger to the bridge of the nose) still shows with --independent-eyes. On 2026-10-08 the
// author's eleven tries read 21-35° left - right on the Frame (its fixation 0.10-0.14 m away), with the eyes fully open
// and no more than 12° down. In 37 minutes of everyday recordings, with both eyes at least CROSS_EYE_ENGAGE_OPEN open,
// Valve's left - right stayed at or above 20° for at most 0.40 s and at or above 25° for at most 0.16 s (with only
// half-open eyes: 1.54 s and 0.29 s, at the end of one recording, looking 15-27° down). So it is let through once,
// with both eyes reliable and at least that open, it has stayed at or above CROSS_EYE_ENGAGE_DEG for
// CROSS_EYE_ENGAGE_S, or at or above CROSS_EYE_FAST_DEG for CROSS_EYE_FAST_S (both raised in a wear that reads
// higher, and only from an ordinary reading, see CROSS_EYE_LEVEL_S): ten of the eleven tries, 0.43-1.01 s after they
// reached 15°, and never in the everyday recordings. The one missed stayed at or above 20° for 0.39 s (peak 22°),
// which the everyday noise matches...
const CROSS_EYE_ENGAGE_DEG: f32 = 20.0;
const CROSS_EYE_ENGAGE_S: f64 = 0.5;
const CROSS_EYE_FAST_DEG: f32 = 25.0;
const CROSS_EYE_FAST_S: f64 = 0.25;
const CROSS_EYE_ENGAGE_OPEN: f32 = 0.7;
// ...until it has stayed below CROSS_EYE_EXIT_DEG (raised with them) for CROSS_EYE_EXIT_S (looking down at a phone
// read 6-12°, which is not told apart from the noise and stays at VERGENCE_DEG; 12° kept the tries no more than 0.2 s
// longer, but let the way back down show as cross-eye for up to 0.6 s instead of 0.2 s)...
const CROSS_EYE_EXIT_DEG: f32 = 15.0;
const CROSS_EYE_EXIT_S: f64 = 0.15;
// ...smoothed over this long (s), and blending in and out over this long (s), from and back to VERGENCE_DEG.
const CROSS_EYE_SMOOTH_S: f32 = 0.1;
const CROSS_EYE_RAMP_S: f32 = 0.2;
// Eyes shut or unreliable while it shows hold the vergence for up to this long (s); longer, and it stops.
const CROSS_EYE_HOLD_S: f64 = 0.3;
// Both eyes count as open for holding and smoothing it from this Frame openness, or from --gaze-hold-below when that is
// set (CROSS_EYE_ENGAGE_OPEN to start it).
const CROSS_EYE_OPEN: f32 = 0.5;
// Some wears read far more turned in all along. In three on 2026-10-10 (10, 8 and 51 minutes, the eyes often half
// shut), Valve's left - right with the eyes open had a median of about 16° in the first two (p90 24-26°, 28-37% of the
// time at 20° or more) and 11-15° in the first minutes of the third, against 2-6° in other wears; the lines above let
// it through 55 times (146 s) though the eyes were crossed on purpose once at most, and neither Valve's eyelids nor the
// eye cameras' told those stretches apart. So the lines are raised to what the wear reads: CROSS_EYE_ENGAGE_DEG,
// CROSS_EYE_FAST_DEG and CROSS_EYE_EXIT_DEG go up together until CROSS_EYE_ENGAGE_DEG is CROSS_EYE_ABOVE_LEVEL_DEG
// above the median of Valve's left - right over the last CROSS_EYE_LEVEL_S of samples with both eyes usable (not while
// let through). It is worked out every CROSS_EYE_LEVEL_EVERY_S once there are CROSS_EYE_LEVEL_MIN of them (10 s at
// ~90 Hz; until then the lines stay as they are), and with fewer later the last one holds (in those wears the eyes
// were half shut for minutes), also over gaps in the samples. With what follows, 2 of the stretches not meant came
// through; with 8° above it, a median over 120 s or the 40th percentile, 5-8; with 12° above or 30 s, the same 2.
const CROSS_EYE_LEVEL_S: f64 = 60.0;
const CROSS_EYE_LEVEL_MIN: usize = 900;
const CROSS_EYE_LEVEL_EVERY_S: f64 = 1.0;
const CROSS_EYE_ABOVE_LEVEL_DEG: f32 = 10.0;
// Only a stretch with the eyes no more than this far down counts (degrees, Valve's own up / down, before the eye fit):
// the tries read a median of 3-11° down around being let through, those wears most of theirs 10-25° down, much like
// shut eyes (23-27° left - right, 17-21° down). Without it, 8 of the stretches not meant came through; at -13° or
// -17°, the same 2...
const CROSS_EYE_LOWEST_PITCH_DEG: f32 = -15.0;
// ...if it got there within this long (s) of Valve's left - right being below CROSS_EYE_EXIT_DEG (not raised) or the
// eyes being shut or unreliable: the tries took 0.43-1.01 s, while in those wears it often crept up over 2.6-3.0 s
// (5 of the stretches not meant came through without this, 3 with 3 s)...
const CROSS_EYE_RISE_S: f64 = 2.5;
// ...and started no sooner than this long (s) after both eyes were open again (CROSS_EYE_ENGAGE_OPEN): right after a
// blink Valve can still read the shut eyes' turn-in, 30-37° for 0.3 s from 0.26 s after the eyes opened. With all of
// this, those three wears let it through 3 times (8.4 s), one of them 40-50° for 2.4 s right after a blink, much like
// crossing on purpose; the tries show as before, as do three from 2026-10-10 crossed right after shutting the eyes for
// 2 s (34-44°, 0.49-0.92 s after reaching 15°), and the everyday recordings never let it through.
const CROSS_EYE_SETTLE_S: f64 = 0.3;
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
// SATURATED_READING or more turn it on; with fewer than SATURATION_MIN_SAMPLES such samples it waits. On two
// 60-minute recordings before 0.4.3 that share was 0-30.5% (a median 3.1% and 4.5%), on three after it 50.4-99.5% (on
// from 6.7 s in). Once on it stays on while frameeyeosc runs, through the eye server starting over: it is how this eye
// tracker reads (a SteamOS update that changes it comes with a reboot), while the share drifts with where you look.
// One 37-minute 0.4.3 recording dipped to 13%, and each dip below the 40% that used to turn it off again let a relaxed
// eye, reading 1.000 while looking up, through as widened.
const SATURATION_WINDOW: i64 = 60;
const SATURATION_OPEN: f32 = 0.6;
const SATURATED_READING: f32 = 0.999;
const SATURATION_ON: f64 = 0.5;
const SATURATION_MIN_SAMPLES: u32 = 600;
// The eye camera's squint and pupil (see eyecam_live) are smoothed per eye sample (~90 Hz) by these shares of each
// new value: the squint settles within a few samples (a time constant of about 25 ms), the pupil, which wobbles from
// frame to frame and changes slowly, over about 10 (105 ms). The camera's eyelid is not: the eyelid filter takes it.
const CAMERA_SQUINT_ALPHA: f32 = 0.35;
const CAMERA_PUPIL_ALPHA: f32 = 0.1;
// Where the eye camera's eyelid takes over from the eye server's below relaxed open (see Smoother::check_lids). The eye
// server often reads an eye that is only narrowed as closed: on 2026-10-07 a squinted eye read 0.08-0.18 (sent closed)
// while the camera saw it narrowed, and an eye that squinted along while the other was held shut read 0.00-0.41 and
// went out closed too, pulled shut by the blink sync besides. Blinks are short and squints are not: the eye server's
// shut stretches last about 70-200 ms, and over the 2026-10-04 recording (90 s) its eyelid was below this line while
// the camera saw the eye open for a median 22-44 ms per stretch (p90 78-144 ms), held squints for 0.8-1.3 s. So once
// the eye server has read an eye below CAMERA_DOUBT_BELOW (or below --blink-sync-below, if that is higher: an eye the
// blink sync could pull shut) while the camera saw it open for CAMERA_DOUBT_GRACE in all, the eyelid is the camera's.
// At least CAMERA_DOUBT_FRAMES of the camera's frame intervals, though: at 15 frames a second its closed flag comes
// about 3 frames (200 ms) late and its eyelid is a 5-frame median, so a blink would otherwise go out open.
const CAMERA_DOUBT_BELOW: f32 = 0.25;
const CAMERA_DOUBT_GRACE: f64 = 0.3;
const CAMERA_DOUBT_FRAMES: f64 = 8.0;
// The eyelid goes back to the eye server's once the two have agreed for CAMERA_AGREE_HOLD: both closed, or the eye
// server's at or above the doubt line and at most CAMERA_AGREE_GAP below the camera's (up to relaxed open). Not at
// the first sample they agree: during a squint the eye server's openness jumps up for 100-150 ms at a time (to 1.000
// on 2026-10-07), and going back to it for such a jump would send the eye closed for another grace period after it.
// Time spent agreeing for less than that, or disagreeing the other way (the eye server reading the eye open but well
// below the camera), neither adds to the doubt nor clears it.
const CAMERA_AGREE_GAP: f32 = 0.15;
const CAMERA_AGREE_HOLD: f64 = 0.2;
// SteamOS 0.4.x's eye tracker often reads an open eye well below its open reading (1.000 on 0.4.3): 0.6-0.95, mostly
// while looking down, at times for seconds straight ahead (mostly the right eye; 2026-10-07 23:23 and 10-08 20:47,
// the camera seeing the eye open throughout). Through a fit that open at 1.000 and LID_FIT_CLOSED_MARGIN, a reading
// of 0.85 went out as VRCFT 0.59, and the avatar looked drowsy. So:
// - With the eye camera's eyelid for that eye (see raised_to_camera): an eyelid the eye server reads open but below
//   relaxed open (from the doubt line up) goes out no lower than the camera's, up to relaxed open. Only ever raised,
//   so a squint the camera sees, a blink (below the doubt line) and a wink stay the eye server's.
// - Without it (see snapped_open): an eyelid at or above --lid-open-snap (lid_open_snap, VRCFT) eases smoothly up to
//   relaxed open, which it reaches LID_OPEN_SNAP_RAMP of the way from there. A fitted eye's eyelid is a straight line
//   in its reading, so with the ramp ending halfway this is exactly the zone this started as, from 0.80 to 0.90 of the
//   eye's open reading, wherever it starts. The default, 0.53, is where 0.80 lands through a fit that reads 1.000 open
//   and 0.00-0.05 shut (0.536-0.525; the author's two eyes); an eye without a fit at the default lid marks lands at
//   0.51. A squint mostly reads lower than 0.8 there; the cost is a squint the eye tracker reads at 0.80-0.90, which
//   goes out more open. A fitted eye's scale (a fine-tune after the fit) applies after it, so an eye scaled to 0.9
//   still goes out 10% less open (see snapped_lids); for an eye without a fit the scale is the mapping itself (the
//   calibration), so it comes first.
// - Without it, too (see fits_at): further than LID_DOWN_FROM_DEG down, a fitted eye's expected open reading carries
//   on falling by LID_DOWN_PER_DEG a degree (no lower than half its straight-ahead reading, as expected_open), where
//   it used to hold at the reading measured 15° down. Looking 24° down an open eye read about 0.82 there and went
//   out half closed. Both numbers come from the author's two recordings (2026-10-07 23:23 and 10-08 20:47).
const LID_OPEN_SNAP_RAMP: f32 = 0.5;
const LID_DOWN_FROM_DEG: f32 = 17.0;
const LID_DOWN_PER_DEG: f32 = 0.02;
// An eye the eye server reads at SATURATED_READING or more says no more than "open": on SteamOS 0.4.3 a relaxed open
// eye already reads 1.000 (99.3% of the samples on 2026-10-07 23:23, 16 minutes in VRChat). An eye held half closed
// (the camera at 0.3-0.5, sent about 0.45) went out 0.72-0.75 for the 2-3 samples its reading jumped to 1.000, and
// back, many times there; the 3-sample median lets such jumps through. So while the camera's values for that eye are
// used and the camera sees it clearly narrowed, a fresh jump there is lowered to the camera's eyelid (see
// Smoother::check_lids and lowered_at_ceiling); everything after (the floor, the widen sustain, the filter, the blinks)
// goes on from there as usual. Only lowered, and only so far:
// - Along a ramp of readings from CEILING_FROM, not at 1.000 alone: an open eye's reading dips to 0.90-0.99 now and
//   then (8.6% of the samples there, 70.7% at 1.000), and switching at 1.000 jumped the eyelid back up at each dip.
// - Only while the camera's eyelid is below CEILING_NARROWED.1, all the way from CEILING_NARROWED.0 down: the camera
//   reads a relaxed eye between about 0.6 and 0.8 (p10 0.66, median 0.75 there), and following it for every reading
//   at 1.000 sent an open eye below VRCFT 0.7 in 31% / 32% of the samples (left / right) instead of 17% / 22%.
// - Only for the first CEILING_JUMP.0 of a stretch near the ceiling, none after CEILING_JUMP.1: the jumps last 2-3
//   samples, while an eye that reads 1.000 for long is open (looking down, the camera sees its eyelid lower: limited
//   by the camera alone, VRCFT 0.7 or less went to 24% / 29% and "closing while looking down" from 291 to 346).
// Replayed with all three: eyelids that jumped up by 0.12 or more and came back within 0.1 s, 84 / 96 -> 68 / 42; VRCFT
// 0.7 or less 18.9% / 23.8% (most of what is added is within 0.3 s of a blink, where the reading jumps to 1.000 as the
// eye starts closing and the camera already sees it closing); closing while looking down 303.
const CEILING_FROM: f32 = 0.9;
const CEILING_NARROWED: (f32, f32) = (0.6, 0.7);
const CEILING_JUMP: (f64, f64) = (0.1, 0.2);
// The camera's widening fades in over its first CAMERA_WIDEN_FADE above relaxed open, and where the eye server reads
// the eye lower, over VALVE_WIDEN_FADE above WIDEN_RESET_BELOW (see camera_widen_share). The camera's eyelid wobbles
// around relaxed open (0.750 <-> 0.773 at a frame), and with an on/off switch an eye the eye server read at VRCFT 0.6
// went out 0.60 <-> 0.74 with it (2026-10-07 23:23, 438 s). Replayed, that second's eyelid movement went from 0.99 to
// 0.61 and another's (461 s) from 0.50 to 0.22, with no more of the eyelids at VRCFT 0.7 or less (17.2% / 22.6%) and
// the flicker while open 0.0049 (0.0050 before).
const CAMERA_WIDEN_FADE: f32 = 0.05;
// An eye's camera eyelid that stops coming (stale for a few frames, the pupil lost for a moment) keeps the eye on the
// rules for eyes with it for up to CAMERA_GAP_HOLD, with the eye server's eyelid still raised to the camera's last one
// (raised_to_camera; never lowered, closed or widened by it, since the camera often drops out just as the eye
// narrows), before the eye goes over to the rules for eyes without it (the nearly open easing, the far-down slope);
// and one that comes back after that is taken once it has kept coming for as long. Switching at once stepped an eye
// the eye server reads low between the camera's raise and the eased eyelid at every such gap. The same length as
// CAMERA_AGREE_HOLD, for the same reason: the camera's values come and go for 100-150 ms at a time.
const CAMERA_GAP_HOLD: f64 = 0.2;
const VALVE_WIDEN_FADE: f32 = 0.1;
// An eye turned far outward (toward the ear: the left eye looking left, the right eye looking right) takes its pupil
// into the dark temporal side of the eye camera's picture, and eyecam-rec read such an open eye as closed: over eleven
// recordings of 2026-10-08/09 (81 minutes) the left eye went out closed for 231 s in 1114 stretches while the eye
// server read both eyes open, 22 of them 2 s or longer (31 s at most). eyecam-rec no longer does (its EDGE_LOST_PX), but
// as a safety net (an older eyecam-rec, a face that shades differently): while the eye server reads an eye looking more
// than CAMERA_SIDE_DEG to the side, the camera's "closed" for that eye is taken as its last open eyelid, unless the eye
// server read that same eye below CAMERA_SIDE_SHUT within CAMERA_SIDE_SHUT_RECENT (a blink, a wink). Only that eye: on
// 2026-10-09 15:23 the left eye's winks while looking right let the right eye's false close through for 3.3 s when
// either eye counted. Replayed: the left eye 40 s in 538 stretches (0.2 s at most), the right eye 60 s (not looking
// outward, as before), the 15:23 right look's right eye 9.0 -> 0.8 s; blinks sent below 0.3 1814 -> 1817 (left) and
// 1454 -> 1453 (right) of 2094 / 1687, winks the same.
// Inward too: on 2026-10-09 18:34 (the headset worn deeper) the right eye turned 30 degrees inward lost its pupil well
// before the lens-edge band (its last one 27-54 px from it, the lids 0.95-1.26 iris radii apart, as narrow as a closing
// eye's there), and eyecam-rec read it closed for 0.88, 0.33 and 0.52 s while the eye server read both eyes open.
// A blink the eye server reads in the other eye only (it read the eye in question 0.64-0.86 through three blinks on
// 2026-10-09 18:17 while the camera saw it shut for 0.09-0.13 s) still closes: the camera's "closed" goes through while
// it began within CAMERA_SIDE_BLINK_SYNC of the other eye's closing, for at most CAMERA_SIDE_BLINK_MAX from its start,
// and only while the other eye's closing has lasted no longer than that (a wink is longer). See SideCheck.
// Replayed (fourteen recordings, seventeen eye videos): looking inward, false closes 31.0 -> 0.1 s (right) and 2.0 -> 0.1 s
// (left), the videos' 7.3 / 3.3 s -> 0; 18:34's right eye 1.06 / 0.41 / 0.62 -> 0 s; 18:17's three blinks back
// (0.11-0.13 s each). A long false close that starts with the other eye's closing still goes out for its first
// CAMERA_SIDE_BLINK_MAX (15:23: three of 1.1-2.6 s went out for 0.31-0.39 s).
const CAMERA_SIDE_DEG: f32 = 15.0;
const CAMERA_SIDE_SHUT: f32 = 0.5;
const CAMERA_SIDE_SHUT_RECENT: f64 = 0.15;
const CAMERA_SIDE_BLINK_SYNC: f64 = 0.15;
const CAMERA_SIDE_BLINK_MAX: f64 = 0.3;
// Lying down, the eye camera loses an eye's pupil at its search window's temporal edge without the eye looking to the
// side, and eyecam-rec then judges "closed" from a box that sits on skin: it read the right eye of a lying wear
// (2026-10-12 01:01, an hour) as closed for seconds at a time while it was open (84-127 s into it, on and off), and
// the eye went out half-shut or shut all along. The eye server read the eye at 0.65-1.0 through those stretches, and
// its own per-eye (pre-fusion) gaze variance, the larger of x and y as gaze_quality and the replay's covariance line
// read it, was as low as for an open eye: over that hour above 0.01 in 0.6-1.2% of the samples where both read an eye
// open, against 66-72% where the eye server read it closed. The eye server never read an eye that was really closed as
// open for longer than 0.5 s in our recordings (the guided "close" steps read 0.00). So the camera's "closed" for an
// eye is also taken as its last open eyelid (at most relaxed open, as with the side check) once the camera has read it
// closed for longer than CAMERA_CLOSED_DOUBT_S in a row (blinks last 70-400 ms and close as before) while the eye
// server has read it at CAMERA_CLOSED_DOUBT_OPEN or more and its variance for that eye is below
// CAMERA_CLOSED_DOUBT_VARIANCE; not within CAMERA_SIDE_SHUT_RECENT of the eye server reading the eye shut, nor with the
// other eye's blink (blink_with_other), as for the side. See SideCheck.
// - A gap in the camera's values up to CAMERA_GAP_HOLD after its last "closed" doesn't end the stretch: its doubt goes
//   on through the gap and after it (started over, each hiccup of a few frames closed a doubted eye again for
//   CAMERA_CLOSED_DOUBT_S). A longer gap starts a new one.
// - The eye server may read the eye below CAMERA_CLOSED_DOUBT_OPEN in the first CAMERA_CLOSED_DOUBT_BLINK_S of the
//   stretch (the blink it began with: lying down, the camera often loses the pupil as the eye closes and does not find
//   it again once it opens), and CAMERA_CLOSED_DOUBT_S counts from its last such reading there. Any such reading after
//   that keeps the eye closed to the end of the stretch: a drowsy eye whose reading goes up and down around 0.5 is
//   one the camera is likely right about (10-12 01:01: 110 / 176 s of the camera's "closed" with the eye server at 0.5
//   or more were in such stretches, left / right). Counting from the last such reading anywhere instead would have
//   taken 54 / 71 s of that hour as open, much of it in 12-25 s stretches with 8-13 dips.
// - Once the eye is taken as open, the eye server's variance going above the line closes it again only after it has
//   stayed there for CAMERA_CLOSED_DOUBT_UNSURE_S, and the eye opens again once it is back below (for as long as the
//   camera's "closed" goes on): taken as it came, each spike of a sample or two closed the eye for a blink's length
//   (--blink-hold-ms), and at 10-11 00:52 the right eye went below VRCFT 0.3 87 times in those stretches instead of 49.
// Replayed on nine recordings of 2026-10-08 to 10-12 (four lying, five sitting): the camera's "closed" taken as open
// this way (where looking to the side did not already) for 8.4 / 48.0 s (left / right) of 10-12 01:01, 7.2 / 49.3 s of
// 10-11 00:52 and 10.7 / 15.8 s of 10-10 02:31 (an hour each, lying), 0-4.6 s in the others. Where the camera read an
// eye closed for longer than 0.5 s while the eye server read it at 0.5 or more but in its first 0.4 s, the right eye
// went out below VRCFT 0.3 for 49.1 -> 13.6 s, 79.6 -> 48.3 s and 26.6 -> 17.0 s of those three (left 8.8 -> 5.4,
// 12.0 -> 6.8 and 16.1 -> 9.1 s); what is left is mostly each stretch's first 0.5 s. In the stretches without such a
// blink it went below 0.3 45 -> 28, 49 -> 50 and 35 -> 24 times (left 11 -> 10, 14 -> 12, 16 -> 9), and anywhere with
// the eye server at 0.65 or more for 176.2 -> 131.5 s, 237.2 -> 195.3 s and 136.9 -> 122.9 s. Closing while looking
// down 141 / 130 -> 134 / 123, 419 / 411 -> 413 / 403 and 138 / 135 -> 128 / 125 stretches. Both-eye blinks fully
// closed (935 / 954 of 1283 at 10-12 01:01, the same or one more elsewhere) and cross-eye are as before.
const CAMERA_CLOSED_DOUBT_S: f64 = 0.5;
const CAMERA_CLOSED_DOUBT_OPEN: f32 = 0.5;
const CAMERA_CLOSED_DOUBT_VARIANCE: f32 = 0.01;
const CAMERA_CLOSED_DOUBT_BLINK_S: f64 = 0.4;
const CAMERA_CLOSED_DOUBT_UNSURE_S: f64 = 0.25;
// The camera's eyelid wobbles by 0.1-0.4 at 2-3 times a second while the eye is open (the right eye on 2026-10-09 15:23:
// median 0.68, p10-p90 0.32-0.75), and an eyelid raised to it (raised_to_camera) followed every wobble: that eye went out
// between 0.55 and 0.70 for 40 s of 105, trembling. So the eye server's eyelid is raised to the camera's highest over
// the last CAMERA_RAISE_PEAK_S instead (RaisePeak), and to the camera's own one again from the moment the camera sees
// the eye closed or the eye server reads it below CAMERA_RAISE_SHUT, so a blink or wink goes as before. Only the
// raising and the ceiling rule (lowered_at_ceiling goes by the same peak, so one low camera reading no longer makes a
// dip: the left eye on 2026-10-09 16:36, its camera eyelid low, changed by 0.0036 -> 0.0032 a sample while open, and
// 0.0022 -> 0.0019 once eyecam's aperture baseline followed; single-sample dips of the eye server's reading that went
// out at 0.00-0.21 stay at 0.31-0.36): where the camera's eyelid stands for the eye's (CameraOpen, widening) it is the
// camera's own.
// Replayed over eleven recordings and fourteen eye videos: that right eye partly closed (0.25-0.72 while the eye server
// read 0.8 or more) 19.9 -> 12.6 s of 105, its change from sample to sample while open 0.0041 -> 0.0024 (all
// recordings 0.0022 -> 0.0017 for the right eye, 0.0025 -> 0.0020 for the left); blinks and winks the same (1817 ->
// 1815 and 1453 -> 1452 sent below 0.3, from the same sample, p90 22 ms either way). Averaging instead (0.1-0.3 s)
// kept the wobble's low points: 15-50% more time partly closed.
const CAMERA_RAISE_PEAK_S: f64 = 0.3;
const CAMERA_RAISE_SHUT: f32 = 0.5;
// The eye camera's "closed" for one eye (see CloseGate, camera_close_gate): used at once while the other eye's camera
// sees its eye closed too or did within CLOSE_PAIR_S (a blink: in one wearer's recordings the two cameras' flags came
// 10-40 ms apart), or the eye tracker reads the other eye closing (openness below CLOSE_WINK_OPEN: it catches one eye
// of some blinks); in one eye only, once it has lasted CLOSE_WINK_S (a wink). Before that, the camera goes on as seeing
// the eye as it last saw it open. Where the camera found an eye's pupil in only part of the frames (one wearer's right
// eye: 1-2% of the frames), its "closed" fired while the eye was open, Valve reading it open with the pupil tracked: 61
// times in 99 s, median 56 ms; too short for CAMERA_CLOSED_DOUBT_S, and not looking to the side. Winks need no word
// from the eye tracker: on SteamOS 0.4.3 it read winked eyes 0.85-1.00 (6 cued winks of 2 s; the eye cameras saw every
// one, 100% of each, in the winking eye only). Looking down further than CLOSE_DOWN_DEG (the eye tracker's own pitch,
// before the eye fit) the upper lid follows the gaze over the pupil, and both cameras read the eyes closed (in cued
// glances down: 57% of the time below -25 degrees, 6% from -25 to -18, under 0.3% above); there no "closed" of the
// cameras is used unless the eye tracker reads an eye closing (a blink while looking down). The gate takes the camera's
// eyelids as SideCheck leaves them (it goes first, from the camera's own).
const CLOSE_PAIR_S: f64 = 0.15;
const CLOSE_WINK_S: f64 = 0.2;
const CLOSE_WINK_OPEN: f32 = 0.5;
const CLOSE_DOWN_DEG: f32 = 18.0;
// The camera's own eyelid goes nearly to 0 in a hard squint (0.00-0.06 with its squint at 1.0 on 2026-10-07), which
// looks closed on an avatar, so an eye the camera sees open is held at --camera-lid-floor (camera_lid_floor) and the
// squint parameter carries the rest; see Smoother::check_lids for when.
// The pupil diameter goes out in cm (VRCFT's PupilDiameter, 0..1).
const PUPIL_MM_PER_UNIT: f32 = 10.0;
// The pupil dilation's bits (pupil_bits) go out only when they change, and all of them again this often, since a
// datagram can be lost.
const PUPIL_BITS_RESEND: Duration = Duration::from_secs(1);
// How often the Steam Link PC is looked up again, to follow reconnects over another network.
const RESOLVE_INTERVAL: Duration = Duration::from_secs(5);
/// The shortest time between two checks of the route to a target that a send failing or working again asks for.
const ROUTE_CHECK_MIN: Duration = Duration::from_secs(1);
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
    /// What to send: VRChat avatar parameters, VRCFaceTracking's ETVR Tracking Module format, Live Link Face
    /// packets for VRCFaceTracking's LiveLink module, or FEO1 packets for VRCFaceTracking's FrameEyeOSC module
    #[arg(long, value_enum, default_value_t = OutputKind::Vrchat)]
    output: OutputKind,
    /// How EyeTrackingActive is sent in VRChat mode: a bool, a float (1.0 / 0.0; some avatars
    /// need it), or not at all
    #[arg(long, value_enum, default_value_t = ActiveType::Bool)]
    eye_tracking_active: ActiveType,
    /// In VRChat mode, also send the avatar parameters SteamVR's Steam Link OSC sends (LeftEyeX, RightEyeLid, ...;
    /// no prefix), for avatars made for it
    #[arg(long)]
    steamlink_params: bool,
    /// In VRChat mode, also send VRChat's own eye tracking input (/tracking/eye/*), which moves the eyes of
    /// avatars without VRCFT parameters
    #[arg(long)]
    native_eyes: bool,
    /// Also send the pupil dilation to VRChat as this many bool parameters (PupilDilation1, 2, 4, 8), for avatars
    /// that take it bit-packed; 0 sends only the float
    #[arg(long, default_value_t = 0, value_parser = clap::value_parser!(u8).range(0..=4))]
    pupil_bits: u8,
    /// How easily an eye with an eye fit widens: off, low, normal or high (eyes without one use --lid-widen-start
    /// and --lid-wide)
    #[arg(long, value_enum, default_value_t = Widen::Normal)]
    lid_widen: Widen,
    /// OSC destination as HOST:PORT, or "auto" for the PC that Steam Link is streaming from
    #[arg(long, default_value = "auto")]
    target: String,
    /// OSC port used with --target auto [default: 9000 for vrchat, 8889 for etvr, 11111 for livelink, 11112 for
    /// frameeyeosc]
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
    /// Turn the eyes in a little toward each other (about 2 m away) instead of sending the combined gaze for both eyes;
    /// each keeps its own up / down (the Frame's own per-eye sideways gaze is too unsteady to send). With
    /// --eye-behavior 1, each eye's own gaze as the Frame reads it
    #[arg(long)]
    independent_eyes: bool,
    /// How the eyes move: 2 (the default) as now; 1 as up to 0.7.5 (each eye's own gaze with --independent-eyes, no
    /// cross-eye gate, and none of the eyelid rules added since: the eye camera only widens, nothing eases nearly open
    /// eyelids up, no --camera-lid-floor or --lid-open-snap)
    #[arg(long, default_value_t = 2, value_parser = clap::value_parser!(u8).range(1..=2))]
    eye_behavior: u8,
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
    /// Don't use eyecam-rec's eye-camera values (widening and squint from relaxed open up, pupils)
    #[arg(long)]
    no_camera_lids: bool,
    /// In LiveLink mode, don't send the eye camera's pupils straight to VRChat (port 9000 of the target's host)
    #[arg(long)]
    no_pupils_to_vrchat: bool,
    /// Act on every "closed" of the eye camera at once (by default one in one eye only waits until it is a blink or a
    /// wink, and while looking down none is used unless the eye tracker reads an eye closing)
    #[arg(long)]
    no_camera_close_gate: bool,
    /// Don't take the eye camera's "closed" as open where the eye server has read the eye open, and been sure of it,
    /// for longer than a blink (for a headset whose eye server reads winks and shut eyes as open)
    #[arg(long)]
    no_camera_closed_doubt: bool,
    /// Where eyecam-rec's eye-camera values are read from [default: $XDG_RUNTIME_DIR/eyecam/live]
    #[arg(long, value_name = "PATH")]
    eyecam_live: Option<PathBuf>,
    /// Close both eyes when one is closed and the other is below this (VRCFT units); winks pass. 0 disables
    #[arg(long, default_value_t = 0.35)]
    blink_sync_below: f32,
    /// While the eye camera sees an eye open, send its eyelid no lower than this (VRCFT units, 0..0.75), so a hard
    /// squint shows narrowed; blinks and an eye the camera sees closed still close. 0 disables
    #[arg(long, default_value_t = 0.0)]
    camera_lid_floor: f32,
    /// For eyes without the eye camera's eyelid: an eyelid at least this open (VRCFT, 0..0.75) eases smoothly up to
    /// relaxed open (0.75), reached halfway there. 0.75 disables
    #[arg(long, default_value_t = 0.53)]
    lid_open_snap: f32,
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
    /// Your own multiplier on how far the gaze goes left, after the eye fit (0.05..2); the fit never changes it
    #[arg(long, default_value_t = 1.0)]
    gaze_scale_left: f32,
    /// Your own multiplier on how far the gaze goes right, after the eye fit (0.05..2)
    #[arg(long, default_value_t = 1.0)]
    gaze_scale_right: f32,
    /// Your own multiplier on how far the gaze goes up, after the eye fit (0.05..2)
    #[arg(long, default_value_t = 1.0)]
    gaze_scale_up: f32,
    /// Your own multiplier on how far the gaze goes down, after the eye fit (0.05..2)
    #[arg(long, default_value_t = 1.0)]
    gaze_scale_down: f32,
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
    // The eye camera's squint and pupils, smoothed.
    camera: CameraSmoother,
    // Whether intentional cross-eye is let through, and how far.
    cross_eye: CrossEye,
    // Per eye: how the eye server's eyelid and the camera's have agreed lately (see check_lids).
    lid_doubt: [LidDoubt; 2],
    // Per eye: since when the eye server's reading has been near the ceiling (see check_lids).
    ceiling_since: [Option<f64>; 2],
    // Per eye: the camera's eyelid held through short gaps (see CAMERA_GAP_HOLD).
    camera_gap: [CameraGap; 2],
    // The camera's "closed" doubted while looking to the side (see CAMERA_SIDE_DEG), or while the eye server reads the
    // eye open and is sure of it (see CAMERA_CLOSED_DOUBT_S).
    side: SideCheck,
    // The camera's eyelid the eye server's is raised to (see CAMERA_RAISE_PEAK_S).
    raise_peak: RaisePeak,
    // The camera's "closed" in one eye held back until it is a blink or a wink (see CLOSE_PAIR_S).
    close_gate: CloseGate,
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
            camera: CameraSmoother::default(),
            cross_eye: CrossEye::default(),
            lid_doubt: [LidDoubt::default(); 2],
            ceiling_since: [None; 2],
            camera_gap: [CameraGap::default(); 2],
            side: SideCheck::default(),
            raise_peak: RaisePeak::default(),
            close_gate: CloseGate::default(),
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
        self.camera = CameraSmoother::default();
        self.cross_eye.restart();
        self.lid_doubt = [LidDoubt::default(); 2];
        self.ceiling_since = [None; 2];
        self.camera_gap = [CameraGap::default(); 2];
        self.side = SideCheck::default();
        self.raise_peak = RaisePeak::default();
        self.close_gate = CloseGate::default();
    }

    /// Start over what only --eye-behavior 2 keeps (the camera's eyelid's doubt and gaps, the ceiling rule, the
    /// cross-eye gate but for what the wear reads, the camera's held-back "closed"), for a switch between it and 1; the
    /// filters go on, so the eyes don't jump.
    fn reset_behavior(&mut self) {
        self.lid_doubt = [LidDoubt::default(); 2];
        self.ceiling_since = [None; 2];
        self.cross_eye.restart();
        self.camera_gap = [CameraGap::default(); 2];
        self.side = SideCheck::default();
        self.raise_peak = RaisePeak::default();
        self.close_gate = CloseGate::default();
    }

    /// The camera's eyelids with a "closed" in one eye that is not yet a blink or a wink held back (see CloseGate), and
    /// which eyes that held back; as they are with --eye-behavior 1 (which leaves the camera's "closed" alone) or
    /// camera_close_gate off.
    fn gate_closed(
        &mut self,
        settings: &Settings,
        data: &EyeData,
        lids: [Option<CameraLid>; 2],
    ) -> ([Option<CameraLid>; 2], [bool; 2]) {
        if settings.v2() && settings.camera_close_gate {
            let pitch = gaze_angles(data.fixation_point)[1] * 45.0;
            self.close_gate.check(data.sample_time, lids, data.openness, pitch)
        } else {
            (lids, [false; 2])
        }
    }

    /// Each eye's camera eyelid at `time` with its short gaps bridged, and the one held through a gap (see
    /// CAMERA_GAP_HOLD, CameraGap::steady).
    fn steady_camera(&mut self, time: f64, read: [Option<CameraLid>; 2]) -> ([Option<CameraLid>; 2], [Option<f32>; 2]) {
        let steady: [(Option<CameraLid>, Option<f32>); 2] =
            std::array::from_fn(|eye| self.camera_gap[eye].steady(time, read[eye]));
        (steady.map(|(lid, _)| lid), steady.map(|(_, held)| held))
    }

    /// The eye server's VRCFT eyelids (`mapped`) corrected by the eye camera's (`camera`, per eye while it may be used),
    /// and where each came from. An eye the camera sees closed is closed (the eye server misses one eye of many
    /// blinks). An eye the eye server has read closing (below CAMERA_DOUBT_BELOW or --blink-sync-below) while the camera
    /// saw it open for longer than a blink (CAMERA_DOUBT_GRACE, at least CAMERA_DOUBT_FRAMES of the camera's frames,
    /// added up) takes the camera's eyelid, until the two have agreed for CAMERA_AGREE_HOLD. Before that the eye
    /// server's stands, so a blink the camera catches late (or not at all) still closes the eye, and the blink sync
    /// still closes an eye the eye server caught in one eye only. An eye without the camera's values is the eye
    /// server's, as before, and starts over. Whichever it is, an eye the camera sees open goes no lower than
    /// --camera-lid-floor, except while the eye server reads it closing within the grace period (a blink); the third
    /// value says which eyes the floor holds. An eye the eye server has just read jumping to the ceiling (`ceiling`, its
    /// ceiling_share; for CEILING_JUMP) is lowered toward the camera's eyelid where that is clearly narrowed (see
    /// lowered_at_ceiling); the agreeing above goes by the eye server's reading all the same, so a jump to 1.000 during
    /// a squint counts as before. Otherwise an eye the eye server reads open but below relaxed open goes no lower than
    /// the camera's eyelid (see raised_to_camera). Both go by `raise_to` where given (RaisePeak).
    fn check_lids(
        &mut self,
        time: f64,
        dt: f32,
        mapped: [f32; 2],
        ceiling: [f32; 2],
        camera: [Option<CameraLid>; 2],
        raise_to: [Option<f32>; 2],
        settings: &Settings,
    ) -> ([f32; 2], [LidFrom; 2], [bool; 2]) {
        let line = doubt_line(settings);
        let floor = settings.camera_lid_floor;
        let mut from = [LidFrom::EyeServer; 2];
        let mut floored = [false; 2];
        let lids = std::array::from_fn(|eye| {
            let valve = mapped[eye];
            // How fresh a jump to the ceiling is: all of it for CEILING_JUMP.0, none from CEILING_JUMP.1
            let since = if ceiling[eye] > 0.0 {
                *self.ceiling_since[eye].get_or_insert(time)
            } else {
                self.ceiling_since[eye] = None;
                time
            };
            let (fresh, stale) = CEILING_JUMP;
            let jump = 1.0 - ((time - since - fresh) / (stale - fresh)).clamp(0.0, 1.0) as f32;
            let state = &mut self.lid_doubt[eye];
            let Some(camera) = camera[eye] else {
                *state = LidDoubt::default();
                return valve;
            };
            let closing = valve < line;
            let agree = if camera.closed {
                closing
            } else {
                !closing && valve >= camera.lid.min(LID_RELAXED) - CAMERA_AGREE_GAP
            };
            if closing && !camera.closed {
                state.doubted += f64::from(dt);
                state.agree_since = None;
            } else if agree {
                let since = *state.agree_since.get_or_insert(time);
                if time - since >= CAMERA_AGREE_HOLD {
                    state.doubted = 0.0;
                    state.camera = false;
                }
            } else {
                state.agree_since = None;
            }
            if state.doubted >= CAMERA_DOUBT_GRACE.max(CAMERA_DOUBT_FRAMES * camera.frame) {
                state.camera = true;
            }
            if camera.closed {
                from[eye] = if valve > 0.0 { LidFrom::CameraClosed } else { LidFrom::EyeServer };
                return 0.0;
            }
            floored[eye] = floor > 0.0 && (state.camera || !closing);
            let lid = if state.camera {
                from[eye] = LidFrom::CameraOpen;
                camera.lid
            } else {
                let (lid, share) = lowered_at_ceiling(valve, ceiling[eye] * jump, raise_to[eye].unwrap_or(camera.lid));
                if share >= 0.5 {
                    from[eye] = LidFrom::CameraAtCeiling;
                    lid
                } else {
                    let (lid, raised) = raised_to_camera(lid, raise_to[eye].unwrap_or(camera.lid), line);
                    if raised {
                        from[eye] = LidFrom::CameraRaised;
                    }
                    lid
                }
            };
            if floored[eye] { lid.max(floor) } else { lid }
        });
        (lids, from, floored)
    }

    /// Keep the `capped` filtered eyelids, and their eyelid filters themselves, at or below relaxed open.
    fn cap_lids(&mut self, lids: &mut [f32; 2], capped: [bool; 2]) {
        for ((lid, filter), capped) in lids.iter_mut().zip(&mut self.lids).zip(capped) {
            if capped {
                *lid = lid.min(LID_RELAXED);
                filter.value = filter.value.map(|value| value.min(LID_RELAXED));
            }
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

/// Intentional cross-eye for --independent-eyes: Valve's own left - right is let through only while it stays far beyond
/// what its noise reaches, in this wear too (see CROSS_EYE_ENGAGE_DEG and CROSS_EYE_LEVEL_S); otherwise the eyes keep
/// VERGENCE_DEG.
#[derive(Default)]
struct CrossEye {
    // Valve's left - right (degrees), smoothed over CROSS_EYE_SMOOTH_S while both eyes are open and reliable.
    smoothed: Option<f32>,
    // Since when it has been past the lines that switch it: CROSS_EYE_ENGAGE_DEG and CROSS_EYE_FAST_DEG up, or
    // CROSS_EYE_EXIT_DEG down (the first).
    since: [Option<f64>; 2],
    // While it shows: since when the eyes have been shut or unreliable.
    held_since: Option<f64>,
    engaged: bool,
    // How much of it shows, 0 (VERGENCE_DEG) to 1, moving at 1 / CROSS_EYE_RAMP_S.
    share: f32,
    // Since when both eyes have been usable and at least CROSS_EYE_ENGAGE_OPEN open (for CROSS_EYE_SETTLE_S).
    open_since: Option<f64>,
    // When Valve's left - right was last below CROSS_EYE_EXIT_DEG, or the eyes shut or unreliable (for
    // CROSS_EYE_RISE_S); None if it has not been since the start.
    low_at: Option<f64>,
    // What this wear reads; kept when the rest starts over (see restart).
    level: CrossEyeLevel,
}

impl CrossEye {
    /// Start over (after a gap in the samples, or a switch of --eye-behavior), keeping what this wear reads.
    fn restart(&mut self) {
        *self = CrossEye {
            level: std::mem::take(&mut self.level),
            ..CrossEye::default()
        };
    }

    /// The left - right to send (degrees) after a sample at `time`, `dt` after the one before, with Valve's left - right
    /// `raw` and up / down `pitch` (degrees, before the eye fit), whether both eyes are open and reliable (`usable`), and
    /// whether they are open enough to start it (`wide`: CROSS_EYE_ENGAGE_OPEN).
    fn step(&mut self, time: f64, dt: f32, raw: f32, pitch: f32, usable: bool, wide: bool) -> f32 {
        // The lines, raised together until CROSS_EYE_ENGAGE_DEG is CROSS_EYE_ABOVE_LEVEL_DEG above what this wear reads
        let level = self.level.add(time, raw, usable && !self.engaged);
        let raise = level.map_or(0.0, |level| (level + CROSS_EYE_ABOVE_LEVEL_DEG - CROSS_EYE_ENGAGE_DEG).max(0.0));
        if !(usable && wide) {
            self.open_since = None;
        } else if self.open_since.is_none() {
            self.open_since = Some(time);
        }
        if !usable || raw < CROSS_EYE_EXIT_DEG {
            self.low_at = Some(time);
        }
        if usable {
            self.held_since = None;
            let alpha = 1.0 - (-dt / CROSS_EYE_SMOOTH_S).exp();
            self.smoothed = Some(self.smoothed.map_or(raw, |smoothed| smoothed + alpha * (raw - smoothed)));
            let lines = if self.engaged {
                [(raw < CROSS_EYE_EXIT_DEG + raise, CROSS_EYE_EXIT_S), (false, 0.0)]
            } else {
                // Not far down, not long after an ordinary reading, and not right after a blink
                let start = wide
                    && pitch >= CROSS_EYE_LOWEST_PITCH_DEG
                    && self.low_at.is_some_and(|low| time - low <= CROSS_EYE_RISE_S + 1e-9)
                    && self.open_since.is_some_and(|open| time - open >= CROSS_EYE_SETTLE_S - 1e-9);
                [
                    (start && raw >= CROSS_EYE_ENGAGE_DEG + raise, CROSS_EYE_ENGAGE_S),
                    (start && raw >= CROSS_EYE_FAST_DEG + raise, CROSS_EYE_FAST_S),
                ]
            };
            let mut switch = false;
            for ((past, wait), since) in lines.into_iter().zip(&mut self.since) {
                if !past {
                    *since = None;
                } else if time - *since.get_or_insert(time) >= wait - 1e-9 {
                    switch = true;
                }
            }
            if switch {
                self.engaged = !self.engaged;
                self.since = [None; 2];
            }
        } else {
            // A blink breaks the stretch toward engaging; while engaged, the vergence holds for a while
            self.since = [None; 2];
            if self.engaged && time - *self.held_since.get_or_insert(time) > CROSS_EYE_HOLD_S {
                self.engaged = false;
                self.held_since = None;
            }
        }
        let step = dt / CROSS_EYE_RAMP_S;
        self.share = if self.engaged { (self.share + step).min(1.0) } else { (self.share - step).max(0.0) };
        let through = self.smoothed.unwrap_or(VERGENCE_DEG);
        VERGENCE_DEG + self.share * (through - VERGENCE_DEG)
    }
}

/// What Valve's left - right reads in this wear (see CROSS_EYE_LEVEL_S).
#[derive(Default)]
struct CrossEyeLevel {
    // The samples it is taken from, (time, degrees), oldest first.
    recent: VecDeque<(f64, f32)>,
    // Their median, once there were CROSS_EYE_LEVEL_MIN of them.
    level: Option<f32>,
    // When it is worked out next.
    next: Option<f64>,
}

impl CrossEyeLevel {
    /// Take in Valve's left - right `raw` (degrees) at `time` if `learn` (both eyes usable and it not let through), and
    /// return the level.
    fn add(&mut self, time: f64, raw: f32, learn: bool) -> Option<f32> {
        if learn {
            self.recent.push_back((time, raw));
        }
        // Also those from after `time`, and a next time too far ahead: the clock went back (the eye server restarted)
        while self.recent.front().is_some_and(|(at, _)| time - at > CROSS_EYE_LEVEL_S || *at > time) {
            self.recent.pop_front();
        }
        if self.next.is_none_or(|next| time >= next || next - time > CROSS_EYE_LEVEL_EVERY_S) {
            self.next = Some(time + CROSS_EYE_LEVEL_EVERY_S);
            // With fewer, the last level holds
            if self.recent.len() >= CROSS_EYE_LEVEL_MIN {
                let mut values: Vec<f32> = self.recent.iter().map(|(_, value)| *value).collect();
                let middle = values.len() / 2;
                self.level = Some(*values.select_nth_unstable_by(middle, f32::total_cmp).1);
            }
        }
        self.level
    }
}

fn median3(a: f32, b: f32, c: f32) -> f32 {
    a.max(b).min(a.min(b).max(c))
}

/// What the eye camera adds to one sample (see eyecam_live): each eye's squint while the camera drives its eyelid,
/// and its pupil while that is used; None for an eye whose values are not.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
struct CameraValues {
    squint: [Option<f32>; 2],
    pupil_dilation: [Option<f32>; 2],
    // In mm.
    pupil_diameter: [Option<f32>; 2],
}

/// One eye's eyelid from the eye camera, while it may be used.
#[derive(Clone, Copy, Debug, PartialEq)]
struct CameraLid {
    // VRCFT scale: 0 closed, 0.75 relaxed open, 1 fully widened.
    lid: f32,
    closed: bool,
    // The camera's frame interval in seconds, 0 while it is not known.
    frame: f64,
}

/// One eye's camera eyelid across short gaps (see CAMERA_GAP_HOLD).
#[derive(Clone, Copy, Debug, Default)]
struct CameraGap {
    // Whether the eye is on the camera's eyelid (None before the first sample: whatever that says).
    on: Option<bool>,
    // The eyelid last taken, and when.
    last: Option<(CameraLid, f64)>,
    // While off: since when the camera's eyelid has been coming again.
    back_since: Option<f64>,
}

impl CameraGap {
    /// At `time`, from the camera's eyelid as read (`read`): the one to use (None in a gap, and while one that came
    /// back is not taken yet), and through a gap the last one, to raise the eye server's to only.
    fn steady(&mut self, time: f64, read: Option<CameraLid>) -> (Option<CameraLid>, Option<f32>) {
        match (self.on, read) {
            (None, _) | (Some(true), Some(_)) => {
                self.on = Some(read.is_some());
                self.last = read.map(|lid| (lid, time));
                self.back_since = None;
                (read, None)
            }
            (Some(true), None) => match self.last {
                Some((lid, at)) if time - at <= CAMERA_GAP_HOLD => (None, Some(lid.lid)),
                _ => {
                    self.on = Some(false);
                    (None, None)
                }
            },
            (Some(false), Some(lid)) => {
                if time - *self.back_since.get_or_insert(time) < CAMERA_GAP_HOLD {
                    return (None, None);
                }
                self.on = Some(true);
                self.last = Some((lid, time));
                self.back_since = None;
                (read, None)
            }
            (Some(false), None) => {
                self.back_since = None;
                (None, None)
            }
        }
    }
}

/// The eye camera's "closed" doubted for an eye that looks far to the side (see CAMERA_SIDE_DEG), or that the eye server
/// has read open, and been sure of, for longer than a blink (see CAMERA_CLOSED_DOUBT_S).
#[derive(Clone, Copy, Debug, Default)]
struct SideCheck {
    // Per eye: when the eye server last read it below CAMERA_SIDE_SHUT, since when it has (or last had) in a row, and
    // whether it did at the last sample.
    shut_at: [Option<f64>; 2],
    shut_since: [Option<f64>; 2],
    shut: [bool; 2],
    // Per eye: since when the camera has read it closed; from when CAMERA_CLOSED_DOUBT_S counts in that stretch (its
    // start, or the eye server's last reading below CAMERA_CLOSED_DOUBT_OPEN in its first CAMERA_CLOSED_DOUBT_BLINK_S);
    // whether the eye server has read it below that after those; whether the camera's "closed" is taken as open for it
    // (CAMERA_CLOSED_DOUBT_S); since when the eye server's variance has been at or above the line in a row; and when
    // the camera last read it closed (a gap in its values up to CAMERA_GAP_HOLD after that doesn't end the stretch).
    closed_since: [Option<f64>; 2],
    doubt_from: [f64; 2],
    dipped: [bool; 2],
    doubting: [bool; 2],
    unsure_since: [Option<f64>; 2],
    closed_at: [Option<f64>; 2],
    // Each eye's last camera eyelid that was not closed.
    open_lid: [Option<f32>; 2],
    // Per eye: whether its camera "closed" was taken as open at the last sample because the eye server read the eye
    // open and was sure of it (CAMERA_CLOSED_DOUBT_S), where looking to the side did not already; for the replay.
    overruled: [bool; 2],
    // camera_closed_doubt off: a "closed" is taken as open only where the eye looks to the side.
    doubt_off: bool,
}

impl SideCheck {
    /// At `time`, with the eye server's `openness`, the larger of each eye's pre-fusion gaze variances (`variance`, see
    /// gaze_variance) and how far each eye looks outward (`outward_deg`, see outward_degrees; inward is negative): the
    /// camera's eyelids as read (`camera`), with a "closed" turned into that eye's last open one (relaxed open if there
    /// was none, and at most) where the eye looks to the side, or where the camera has read it closed for longer than
    /// CAMERA_CLOSED_DOUBT_S while the eye server read it open (but for the blink it began with) and was sure of it
    /// (see doubt_closed; not with `doubt_off`, camera_closed_doubt); either unless the eye server read that eye shut
    /// lately or the other eye closing with it in a blink (see CAMERA_SIDE_BLINK_SYNC).
    fn step(
        &mut self,
        time: f64,
        openness: [f32; 2],
        variance: [f32; 2],
        outward_deg: [f32; 2],
        camera: [Option<CameraLid>; 2],
    ) -> [Option<CameraLid>; 2] {
        for eye in 0..2 {
            let shut = openness[eye] < CAMERA_SIDE_SHUT;
            if shut {
                if !self.shut[eye] {
                    self.shut_since[eye] = Some(time);
                }
                self.shut_at[eye] = Some(time);
            }
            self.shut[eye] = shut;
            match camera[eye] {
                Some(lid) if lid.closed => {
                    self.doubt_closed(eye, time, openness[eye], variance[eye]);
                    self.closed_at[eye] = Some(time);
                }
                // A gap in the camera's values (stale for a few frames) no longer than CAMERA_GAP_HOLD: the stretch, its
                // doubt and what the eye server reads in it go on (started over, a hiccup closed a doubted eye again
                // for CAMERA_CLOSED_DOUBT_S)
                None if self.closed_at[eye].is_some_and(|at| time - at <= CAMERA_GAP_HOLD) => {
                    self.doubt_closed(eye, time, openness[eye], variance[eye]);
                }
                _ => {
                    self.closed_since[eye] = None;
                    self.closed_at[eye] = None;
                    self.doubting[eye] = false;
                }
            }
        }
        self.overruled = [false; 2];
        std::array::from_fn(|eye| {
            let lid = camera[eye]?;
            if !lid.closed {
                self.open_lid[eye] = Some(lid.lid);
                return Some(lid);
            }
            let shut = self.shut_at[eye].is_some_and(|at| time - at <= CAMERA_SIDE_SHUT_RECENT);
            let side = outward_deg[eye].abs() > CAMERA_SIDE_DEG;
            if (side || (self.doubting[eye] && !self.doubt_off)) && !shut && !self.blink_with_other(eye, time) {
                self.overruled[eye] = !side;
                // No wider than relaxed open, like eyecam-rec's own hold at the lens edge
                let held = self.open_lid[eye].unwrap_or(LID_RELAXED).min(LID_RELAXED);
                Some(CameraLid { lid: held, closed: false, ..lid })
            } else {
                Some(lid)
            }
        })
    }

    /// Follow `eye`'s camera "closed" stretch at `time` with the eye server's `openness` and `variance` of that eye (see
    /// CAMERA_CLOSED_DOUBT_S), and whether it is taken as open now (`doubting`). An openness or variance that is not a
    /// number is not open or sure.
    fn doubt_closed(&mut self, eye: usize, time: f64, openness: f32, variance: f32) {
        let since = match self.closed_since[eye] {
            Some(since) => since,
            None => {
                self.closed_since[eye] = Some(time);
                self.doubt_from[eye] = time;
                self.dipped[eye] = false;
                self.doubting[eye] = false;
                time
            }
        };
        if !(openness >= CAMERA_CLOSED_DOUBT_OPEN) {
            if time - since <= CAMERA_CLOSED_DOUBT_BLINK_S {
                self.doubt_from[eye] = time;
            } else {
                self.dipped[eye] = true;
            }
        }
        let unsure = !(variance < CAMERA_CLOSED_DOUBT_VARIANCE);
        if unsure {
            self.unsure_since[eye].get_or_insert(time);
        } else {
            self.unsure_since[eye] = None;
        }
        let ready = time - self.doubt_from[eye] > CAMERA_CLOSED_DOUBT_S && !self.dipped[eye];
        // Once open, it stays so through the variance's spikes shorter than CAMERA_CLOSED_DOUBT_UNSURE_S
        let sure = if self.doubting[eye] {
            self.unsure_since[eye].is_none_or(|from| time - from < CAMERA_CLOSED_DOUBT_UNSURE_S)
        } else {
            !unsure
        };
        self.doubting[eye] = ready && sure;
    }

    /// Whether `eye`'s camera "closed" at `time` goes with a blink the eye server reads in the other eye: it began within
    /// CAMERA_SIDE_BLINK_SYNC of the other eye's closing, CAMERA_SIDE_BLINK_MAX ago at most, and the other eye's closing
    /// has lasted no longer than that.
    fn blink_with_other(&self, eye: usize, time: f64) -> bool {
        let other = 1 - eye;
        let (Some(closed), Some(start), Some(last)) = (self.closed_since[eye], self.shut_since[other], self.shut_at[other])
        else {
            return false;
        };
        let end = if self.shut[other] { time } else { last };
        time - closed <= CAMERA_SIDE_BLINK_MAX
            && (start - closed).abs() <= CAMERA_SIDE_BLINK_SYNC
            && end - start <= CAMERA_SIDE_BLINK_MAX
    }
}

/// The camera's eyelid to raise the eye server's to, per eye (see CAMERA_RAISE_PEAK_S): the highest of the last
/// CAMERA_RAISE_PEAK_S of open eyelids, forgotten whenever the camera sees the eye closed or the eye server reads it
/// below CAMERA_RAISE_SHUT; through a gap in the camera's values, what is left of them.
#[derive(Clone, Debug, Default)]
struct RaisePeak {
    recent: [VecDeque<(f64, f32)>; 2],
}

impl RaisePeak {
    /// At `time`, with the eye server's `openness` and the camera's eyelids (`camera`): the eyelid each eye is raised
    /// to, None where there is none (the camera's own then).
    fn step(&mut self, time: f64, openness: [f32; 2], camera: [Option<CameraLid>; 2]) -> [Option<f32>; 2] {
        std::array::from_fn(|eye| {
            let recent = &mut self.recent[eye];
            match camera[eye] {
                Some(lid) if lid.closed || openness[eye] < CAMERA_RAISE_SHUT => recent.clear(),
                Some(lid) => recent.push_back((time, lid.lid)),
                None => {}
            }
            while recent.front().is_some_and(|(at, _)| time - at > CAMERA_RAISE_PEAK_S || *at > time) {
                recent.pop_front();
            }
            recent.iter().map(|(_, lid)| *lid).reduce(f32::max)
        })
    }
}

/// How far each eye looks outward (toward its ear) by the eye server's own gaze, in degrees (negative: inward; NaN
/// without a gaze): the left eye looking left, the right eye looking right (gaze_angles' x is positive to the right).
fn outward_degrees(data: &EyeData) -> [f32; 2] {
    let [left, right] = data.gaze.map(gaze_angles);
    [-left[0] * 45.0, right[0] * 45.0]
}

/// The larger of each eye's two variances of its own (pre-fusion) gaze, the ones gaze_quality checks: how unsure the eye
/// server is of that eye. Infinite where either is not a number, which gaze_quality counts as unreliable too.
fn gaze_variance(data: &EyeData) -> [f32; 2] {
    data.pre_fusion_covariance.map(|[x, y, _]| if x.is_finite() && y.is_finite() { x.max(y) } else { f32::INFINITY })
}

/// The eye camera's "closed" in one eye only, held back unless it is a blink or a held wink (see CLOSE_PAIR_S).
#[derive(Clone, Copy, Debug)]
struct CloseGate {
    // Per eye: since when the camera has said closed, when it last did, whether this closing is a blink, and the
    // camera's eyelid when it last saw the eye not closed.
    since: [Option<f64>; 2],
    last: [f64; 2],
    paired: [bool; 2],
    open_lid: [f32; 2],
}

impl Default for CloseGate {
    fn default() -> Self {
        Self {
            since: [None; 2],
            last: [f64::NEG_INFINITY; 2],
            paired: [false; 2],
            open_lid: [LID_RELAXED; 2],
        }
    }
}

/// Whether the eye tracker reads an eye closing, for CloseGate: openness below CLOSE_WINK_OPEN. Not its own gaze
/// variance (its pupil hidden at 0.02 or more): on one headset the left eye's runs 0.0125 at the median and 0.023 at
/// p90 while open (over 150 s), so it said "closing" a tenth of the time and paired false right-eye "closed" with it as
/// blinks.
fn tracker_reads_closing(openness: f32) -> bool {
    openness < CLOSE_WINK_OPEN
}

impl CloseGate {
    /// The camera's eyelids at `time` (`lids`, as CameraSmoother::take gives them) with a "closed" that is not a blink
    /// or a wink replaced by the eye's last eyelid before it (so the camera carries on as seeing the eye open, not as a
    /// gap, and a wink that is let through closes at once), and which eyes that held back. `openness` is the eye
    /// tracker's per eye and `pitch` its gaze up/down (degrees, + up) for the same sample. A blink, let through to the
    /// end of the closing: the other eye's camera saw its eye closed now or within CLOSE_PAIR_S, or the eye tracker
    /// reads the other eye closing. A wink: CLOSE_WINK_S on. Neither while looking down (CLOSE_DOWN_DEG) unless the eye
    /// tracker reads an eye closing.
    fn check(
        &mut self,
        time: f64,
        lids: [Option<CameraLid>; 2],
        openness: [f32; 2],
        pitch: f32,
    ) -> ([Option<CameraLid>; 2], [bool; 2]) {
        for (eye, lid) in lids.iter().enumerate() {
            match lid {
                Some(lid) if lid.closed => {
                    if self.since[eye].is_none() {
                        self.since[eye] = Some(time);
                        self.paired[eye] = false;
                    }
                    self.last[eye] = time;
                }
                Some(lid) => {
                    self.since[eye] = None;
                    self.open_lid[eye] = lid.lid;
                }
                None => self.since[eye] = None,
            }
        }
        let tracker = openness.map(tracker_reads_closing);
        let down = pitch < -CLOSE_DOWN_DEG && !tracker.contains(&true);
        let mut held = [false; 2];
        let lids = std::array::from_fn(|eye| {
            let lid = lids[eye]?;
            let Some(since) = self.since[eye] else {
                return Some(lid);
            };
            // Without the other eye's camera values there is nothing to pair with: as without the gate
            let other = 1 - eye;
            self.paired[eye] |= lids[other].is_none() || time - self.last[other] <= CLOSE_PAIR_S || tracker[other];
            let wink = time - since >= CLOSE_WINK_S - 1e-9;
            held[eye] = down || !(self.paired[eye] || wink);
            Some(if held[eye] { CameraLid { lid: self.open_lid[eye], closed: false, ..lid } } else { lid })
        });
        (lids, held)
    }
}

/// Per eye, how the eye server's eyelid and the eye camera's have agreed lately (see Smoother::check_lids).
#[derive(Clone, Copy, Debug, Default)]
struct LidDoubt {
    // Seconds the eye server has read the eye closing while the camera saw it open, added up since they last agreed.
    doubted: f64,
    // Since when they have agreed.
    agree_since: Option<f64>,
    // Whether the eyelid is the camera's.
    camera: bool,
}

/// Where a sent eyelid came from, beyond the usual mix (the eye server's below relaxed open, the camera's from there
/// up; see mix_lid). The panel's eye preview gets it as its place in this order (dot_sample).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
enum LidFrom {
    #[default]
    EyeServer,
    // The camera's: the eye server read the eye closing for longer than a blink while the camera saw it open.
    CameraOpen,
    // Closed: the camera saw it closed and the eye server did not.
    CameraClosed,
    // The camera's, narrowed: the eye server's reading had just jumped to (or most of the way to) the ceiling, 1.000,
    // which says only "open" (see lowered_at_ceiling).
    CameraAtCeiling,
    // The camera's widening, where the eye server read the eye below relaxed open.
    CameraWidened,
    // Raised to the camera's: the eye server read the eye open but lower than the camera did, below relaxed open
    // (see raised_to_camera).
    CameraRaised,
}

/// Per-eye smoothing of the eye camera's squint and pupils. An eye whose values are not used starts over, so one that
/// comes back after a gap starts from its new value instead of from before the gap.
#[derive(Clone, Copy, Default)]
struct CameraSmoother {
    squint: [Option<f32>; 2],
    pupil_dilation: [Option<f32>; 2],
    pupil_diameter: [Option<f32>; 2],
}

impl CameraSmoother {
    /// Each eye's camera eyelid while it may be used (see Live::lids_usable; None otherwise, and always without
    /// `camera_lids`), and the squint and pupils to send, smoothed unless `smooth` is false (--raw).
    fn take(&mut self, settings: &Settings, camera: Option<&Live>, smooth: bool) -> ([Option<CameraLid>; 2], CameraValues) {
        let camera = camera.filter(|_| settings.camera_lids);
        let lids_usable = camera.map_or([false; 2], Live::lids_usable);
        let pupil_usable = camera.map_or([false; 2], Live::pupil_usable);
        let mut values = CameraValues::default();
        let mut lids = [None; 2];
        for eye in 0..2 {
            let read = camera.map(|camera| camera.eyes[eye]).unwrap_or_default();
            let (lid_used, pupil_used) = (lids_usable[eye], pupil_usable[eye]);
            lids[eye] = lid_used.then(|| CameraLid {
                lid: read.lid,
                closed: read.closed,
                frame: camera.map_or(0.0, |camera| camera.frame_ns[eye] as f64 / 1e9),
            });
            let squint = read.squint.clamp(0.0, 1.0);
            values.squint[eye] = smoothed(&mut self.squint[eye], lid_used, squint, CAMERA_SQUINT_ALPHA, smooth);
            let dilation = read.pupil_dilation.clamp(0.0, 1.0);
            values.pupil_dilation[eye] =
                smoothed(&mut self.pupil_dilation[eye], pupil_used, dilation, CAMERA_PUPIL_ALPHA, smooth);
            let diameter = read.pupil_mm.max(0.0);
            values.pupil_diameter[eye] =
                smoothed(&mut self.pupil_diameter[eye], pupil_used, diameter, CAMERA_PUPIL_ALPHA, smooth);
        }
        (lids, values)
    }
}

/// An exponential moving average kept in `state`, taking `alpha` of each new `value`: None (and started over) while
/// not `used`, the value itself without `smooth`.
fn smoothed(state: &mut Option<f32>, used: bool, value: f32, alpha: f32, smooth: bool) -> Option<f32> {
    *state = match *state {
        _ if !used => None,
        Some(last) if smooth => Some(last + alpha * (value - last)),
        _ => Some(value),
    };
    *state
}

/// An eyelid from the eye server (VRCFT `valve`, as Smoother::check_lids left it), with the eye camera's (`camera`,
/// while it may be used): from relaxed open up, the camera's widening, which the eye server can't see (see
/// SATURATION_WINDOW), and below it the eye server's, which catches closing and blinks faster and more reliably, but
/// for the camera_widen_share of the way up to the camera's widening.
fn mix_lid(valve: f32, camera: Option<f32>) -> f32 {
    mix_lid_by(valve, valve, camera)
}

/// mix_lid as it was up to 0.7.5, for --eye-behavior 1: from relaxed open up, the camera's eyelid, kept at relaxed open
/// or above; below it, the eye server's.
fn mix_lid_v1(valve: f32, camera: Option<f32>) -> f32 {
    match camera {
        Some(camera) if valve >= LID_RELAXED => camera.clamp(LID_RELAXED, 1.0),
        _ => valve,
    }
}

/// mix_lid with the eye server's side of camera_widen_share going by `read` instead of `valve`: for an eyelid
/// raised_to_camera raised, the eyelid before that, so the raising doesn't let all of the camera's widening through
/// where the eye server read the eye well below relaxed open (on 2026-10-08 22:20, 2 of the right eye's 16% of samples
/// sent widened).
fn mix_lid_by(valve: f32, read: f32, camera: Option<f32>) -> f32 {
    let Some(camera) = camera else {
        return valve;
    };
    let below = valve.min(LID_RELAXED);
    below + camera_widen_share(read, camera) * (camera.min(1.0) - below)
}

/// The eyelids camera_widen_share goes by (see mix_lid_by): the eye server's before raised_to_camera (`valve`) for the
/// eyes it raised, the corrected ones (`mapped`) otherwise.
fn widen_basis(mapped: [f32; 2], valve: [f32; 2], lid_from: [LidFrom; 2]) -> [f32; 2] {
    std::array::from_fn(|eye| if lid_from[eye] == LidFrom::CameraRaised { valve[eye] } else { mapped[eye] })
}

/// How much of the camera's widening (`camera`) an eyelid gets, from 0 to 1: as much as the camera sees the eye widened
/// (none at relaxed open, all from CAMERA_WIDEN_FADE above it) and the eye server does not read it closing (none at
/// WIDEN_RESET_BELOW, all from VALVE_WIDEN_FADE above it; an eye at relaxed open or above, all). One eye often reads
/// lower than the other through its fit: on 2026-10-07, with both eyes widened and the camera seeing both fully so, one
/// eye's eyelid stayed below relaxed open and went out at 0.58-0.74 while the other went out widened.
fn camera_widen_share(valve: f32, camera: f32) -> f32 {
    let seen = ((camera - LID_RELAXED) / CAMERA_WIDEN_FADE).clamp(0.0, 1.0);
    let open = ((valve.min(LID_RELAXED) - WIDEN_RESET_BELOW) / VALVE_WIDEN_FADE).clamp(0.0, 1.0);
    seen * open
}

/// The eye server's eyelid (VRCFT `valve`) lowered toward the camera's (`camera`) at a reading near the ceiling
/// (`reading`, its ceiling_share), and that share: all of it while the camera sees the eye clearly narrowed
/// (CEILING_NARROWED.0 or less), none from CEILING_NARROWED.1 up, in a straight line between; and only ever down.
fn lowered_at_ceiling(valve: f32, reading: f32, camera: f32) -> (f32, f32) {
    let (narrowed, open) = CEILING_NARROWED;
    let share = reading * ((open - camera) / (open - narrowed)).clamp(0.0, 1.0);
    (valve - share * (valve - camera).max(0.0), share)
}

/// The eye server's eyelid (VRCFT `lid`) raised to the camera's (`camera`, up to relaxed open) where the eye server
/// reads the eye open (at or above `line`, the doubt line) but below relaxed open, and whether it was raised (see
/// LID_OPEN_SNAP_RAMP). Only ever raised: where the camera sees the eye narrowed (a squint) the eye server's lower
/// eyelid stands, and below the doubt line (a blink, a wink) it is untouched.
fn raised_to_camera(lid: f32, camera: f32, line: f32) -> (f32, bool) {
    if lid < line || lid >= LID_RELAXED {
        return (lid, false);
    }
    let raised = lid.max(camera.min(LID_RELAXED));
    (raised, raised > lid)
}

/// Each eye's VRCFT eyelid (as the fit, or the lid marks, and the eye's scale map it) eased up to relaxed open from
/// --lid-open-snap, for the eyes in `zoned` (those without the eye camera's eyelid; see LID_OPEN_SNAP_RAMP):
/// unchanged up to it, relaxed open from LID_OPEN_SNAP_RAMP of the way from it to relaxed open (widening above it
/// unchanged), and in between a cubic that leaves --lid-open-snap at the same slope and meets relaxed open flat, so
/// it is smooth at both ends and never falls. With --lid-open-snap at relaxed open (0.75) nothing changes (off).
fn snapped_open(lids: [f32; 2], zoned: [bool; 2], settings: &Settings) -> [f32; 2] {
    let from = settings.lid_open_snap;
    let to = from + LID_OPEN_SNAP_RAMP * (LID_RELAXED - from);
    std::array::from_fn(|eye| {
        let lid = lids[eye];
        if !zoned[eye] || from >= LID_RELAXED || lid <= from {
            lid
        } else if lid >= to {
            lid.max(LID_RELAXED)
        } else {
            let t = (lid - from) / (to - from);
            let (t2, t3) = (t * t, t * t * t);
            (2.0 * t3 - 3.0 * t2 + 1.0) * from + (t3 - 2.0 * t2 + t) * (to - from) + (3.0 * t2 - 2.0 * t3) * LID_RELAXED
        }
    })
}

/// Each eye's VRCFT eyelid from its openness (lid_inputs_with's, through `fits`), eased up by snapped_open for the eyes
/// in `zoned`: for a fitted eye before its scale (a fine-tune after the fit, which then still applies), for one without
/// a fit after it (its scale is the calibration that maps it). Where nothing is eased, exactly lid_inputs_with's.
fn snapped_lids(
    openness: [f32; 2],
    vertical: f32,
    scales: [f32; 2],
    settings: &Settings,
    fits: [Option<LidFit>; 2],
    zoned: [bool; 2],
) -> [f32; 2] {
    let fitted = fits.map(|fit| fit.is_some());
    let first = std::array::from_fn(|eye| if fitted[eye] { 1.0 } else { scales[eye] });
    let unscaled = lid_inputs_with(openness, vertical, first, settings, fits);
    let mapped = unscaled.map(|value| lid_to_vrcft(value, settings));
    let snapped = snapped_open(mapped, zoned, settings);
    std::array::from_fn(|eye| {
        if !fitted[eye] {
            return snapped[eye];
        }
        // Back onto the --lid-* scale (below relaxed open lid_to_vrcft is a straight line), never below where it was
        let value = if snapped[eye] > mapped[eye] {
            let marks = settings.lid_open - settings.lid_closed;
            (settings.lid_closed + snapped[eye] / LID_RELAXED * marks).max(unscaled[eye])
        } else {
            unscaled[eye]
        };
        lid_to_vrcft(value * scales[eye], settings)
    })
}

/// Which eyes' readings near 1.000 say no more than "open", for the ceiling rule (see CEILING_FROM): a fitted eye whose
/// open reading is at or above CEILING_FROM, and one without a fit while the openness is saturated (see Saturation).
/// An eye that reads 0.85 open has room above it, and 0.90 or more there is widening, which the rule held back for up
/// to CEILING_JUMP.1.
fn ceiling_eyes(settings: &Settings, saturated: bool) -> [bool; 2] {
    settings.lid_fit().map(|fit| fit.map_or(saturated, |fit| fit.open >= CEILING_FROM))
}

/// How far an eye server's reading is toward the ceiling, for Smoother::check_lids: none up to CEILING_FROM, all of the
/// way from SATURATED_READING, in a straight line between.
fn ceiling_share(reading: f32) -> f32 {
    ((reading - CEILING_FROM) / (SATURATED_READING - CEILING_FROM)).clamp(0.0, 1.0)
}

/// Eyelids (VRCFT, as mix_lid gave them) widened no further above relaxed open than the mean of both cameras'
/// widening (`camera`, each eye's eyelid while it may be used), while both eyes have one; unchanged otherwise. On
/// 2026-10-07 the camera's widening was right on a scripted check (in 0% of the normal, squint and look steps' samples,
/// 83% of the widen steps'), but over 16 minutes in VRChat 21 of 122 widen stretches of the left eye and 27 of 130 of
/// the right were that eye alone (a median 0.45 s), the other eye's camera seeing none, while widening is nearly always
/// both eyes. With the mean, a widen in one eye shows at half and one in both unchanged, and an uneven one meets in
/// the middle (the less widened eye keeps its own). Not the other eye's plus a margin: that would take a one-eye widen
/// away altogether however wide it is, where the mean only weakens it, and both are continuous in either value.
fn share_widening(lids: [f32; 2], camera: [Option<f32>; 2]) -> [f32; 2] {
    let [Some(left), Some(right)] = camera else {
        return lids;
    };
    let widening = |lid: f32| (lid.min(1.0) - LID_RELAXED).max(0.0);
    let limit = LID_RELAXED + (widening(left) + widening(right)) / 2.0;
    lids.map(|lid| lid.min(limit))
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
            Err(error) => status::log_error(&format!("Could not save {}: {error}", path.display())),
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
    lid_inputs_with(openness, vertical, scales, settings, settings.lid_fit())
}

/// lid_inputs with these eye fits (fits_at's, for where the eyes look).
fn lid_inputs_with(
    openness: [f32; 2],
    vertical: f32,
    scales: [f32; 2],
    settings: &Settings,
    fits: [Option<LidFit>; 2],
) -> [f32; 2] {
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

/// The eye fits for a sample with the eyes at `vertical` (-1..1): as fitted, but for the eyes in `extended` (those
/// without the eye camera's eyelid) further than LID_DOWN_FROM_DEG down, where the expected open reading carries on
/// falling by LID_DOWN_PER_DEG a degree (see LID_OPEN_SNAP_RAMP). expected_open holds the down reading from 15° down, so
/// that lower reading stands in for it there, and its own floor (half the straight-ahead reading) still applies. Only
/// for an eye that can't widen (widen_room: --lid-widen off, or an open reading near the openness ceiling, as on SteamOS
/// 0.4.3 where it reads 1.000): the expected open reading is also where widening starts (fitted_lid), and lowering it
/// made an eye with room to widen go out fully widened looking 24° down at a reading of 0.80.
fn fits_at(settings: &Settings, vertical: f32, extended: [bool; 2]) -> [Option<LidFit>; 2] {
    let beyond = -vertical * 45.0 - LID_DOWN_FROM_DEG;
    let fits = settings.lid_fit();
    std::array::from_fn(|eye| {
        fits[eye].map(|fit| {
            if !extended[eye] || beyond <= 0.0 || widen_room(&fit, settings.lid_widen) {
                return fit;
            }
            LidFit {
                down: expected_open(&fit, vertical) - LID_DOWN_PER_DEG * beyond,
                ..fit
            }
        })
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
/// unless --independent-eyes is given. An eye with unreliable gaze is left out of the combined gaze and the other eye
/// stands in for it. With --independent-eyes each eye looks the combined way, turned in by half of VERGENCE_DEG (the
/// Frame's own per-eye x is no good for that), and keeps its own up / down; an unreliable eye takes the other's. With
/// --eye-behavior 1 (`v2` false), each eye's own gaze as it is, as up to 0.7.5 (an unreliable one is held instead).
fn choose_gaze(angles: [f32; 6], reliable: [bool; 2], independent: bool, v2: bool) -> [f32; 6] {
    let [left_x, left_y, right_x, right_y, x, y] = angles;
    let [x, y] = match reliable {
        [true, false] => [left_x, left_y],
        [false, true] => [right_x, right_y],
        _ => [x, y],
    };
    if independent && !v2 {
        [left_x, left_y, right_x, right_y, x, y]
    } else if independent {
        let [left_y, right_y] = match reliable {
            [true, false] => [left_y, left_y],
            [false, true] => [right_y, right_y],
            _ => [left_y, right_y],
        };
        let half = VERGENCE_DEG / 2.0 / 45.0;
        [(x + half).clamp(-1.0, 1.0), left_y, (x - half).clamp(-1.0, 1.0), right_y, x, y]
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

/// UDP sender that re-resolves its target periodically and reconnects when it changes, or when the route to it
/// now starts from another local address.
struct Output {
    target: Target,
    socket: Option<(UdpSocket, SocketAddr)>,
    last_resolve: Option<Instant>,
    // When the route to the target was last checked (see check_route), and whether a send failing or working again
    // asks for the next check at once.
    last_route_check: Option<Instant>,
    route_check_due: bool,
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
            last_route_check: None,
            route_check_due: false,
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

    /// Look the target up again, and check the route to it, when due. Never fails: without a network the socket
    /// stays unset and the next look-up (RESOLVE_INTERVAL later) tries again.
    fn refresh(&mut self) {
        let now = Instant::now();
        // A fixed host is looked up once (like before the config file existed) unless that failed.
        let settled = matches!(self.target, Target::Fixed { .. }) && self.socket.is_some();
        if self.last_resolve.is_none_or(|resolved| !settled && now.duration_since(resolved) >= RESOLVE_INTERVAL) {
            self.look_up(now);
        }
        // Even when the target stays the same, the network under it can change. A send failing or working again asks
        // for a check sooner, but not more than once every ROUTE_CHECK_MIN (sends that keep flipping between the two
        // would otherwise make a socket at every sample)
        let since = |checked: Instant| now.duration_since(checked);
        if self.last_route_check.is_none_or(|checked| {
            since(checked) >= RESOLVE_INTERVAL || (self.route_check_due && since(checked) >= ROUTE_CHECK_MIN)
        }) {
            self.check_route(now);
        }
    }

    /// Look the target up and connect to it if it moved.
    fn look_up(&mut self, now: Instant) {
        self.last_resolve = Some(now);
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
                        status::log_error(&format!(
                            "Could not resolve {host}: {reason}; retrying every {} s",
                            RESOLVE_INTERVAL.as_secs()
                        ));
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
                    // A new socket starts from the current route
                    self.last_route_check = Some(now);
                    self.route_check_due = false;
                    Some((socket, addr))
                }
                // e.g. "Network is unreachable" while Wi-Fi comes up after boot
                Err(error) => {
                    let reason = error.to_string();
                    if self.connect_error.as_ref() != Some(&reason) {
                        status::log_error(&format!(
                            "Can't send OSC to {addr} yet ({reason}); retrying every {} s",
                            RESOLVE_INTERVAL.as_secs()
                        ));
                        self.connect_error = Some(reason);
                    }
                    None
                }
            },
            None if matches!(self.target, Target::SteamLink { .. }) => {
                status::log_error("No Steam Link connection found; waiting for one");
                None
            }
            None => None,
        };
    }

    /// Reconnect when the route to the target now starts from another local address than the socket's. A connected
    /// UDP socket keeps the source address chosen when it connected: after the Frame moves to another network (home
    /// Wi-Fi to the Steam Link adapter, say) it would go on sending "successfully" from an address the new network
    /// doesn't route back, and nothing arrives. Costs one unsent socket per check, never one per sample. Without a
    /// route (the network is switching) the socket is kept, quietly, and checked again later.
    fn check_route(&mut self, now: Instant) {
        self.last_route_check = Some(now);
        self.route_check_due = false;
        let Some((socket, addr)) = &self.socket else {
            return;
        };
        let addr = *addr;
        let Ok(current) = socket.local_addr().map(|local| local.ip()) else {
            return;
        };
        let Some(moved) = moved_source(current, &route_source(addr)) else {
            return;
        };
        // Should connecting fail after all, the old socket stays until the next check
        if let Ok(socket) = connect(addr) {
            eprintln!("Source address for {addr} changed from {current} to {moved}; reconnecting");
            self.socket = Some((socket, addr));
            self.connect_error = None;
            self.health = SendHealth::default();
        }
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
            // A failure to send is kept for the panel; its end, and dropped datagrams, are only logged
            if result.as_ref().is_err_and(|error| error.kind() != io::ErrorKind::WouldBlock) {
                status::log_error(&line);
            } else {
                eprintln!("{line}");
            }
        }
    }

    /// Note how sending one datagram to `target` went; returns the line to log, if any. A datagram the network could
    /// not take at once is dropped and counted, which is not a failure to send.
    fn note(&mut self, now: Instant, target: SocketAddr, result: &io::Result<usize>) -> Option<String> {
        match result {
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => self.drops.record(now, target),
            _ => {
                let was_failing = self.health.failing.is_some();
                let line = self.health.record(now, target, result);
                // Sending starting to fail, or working again, can mean the network changed under the socket: check the
                // route at the next refresh rather than up to RESOLVE_INTERVAL later. Once per change, not per sample
                // ("refused" changes nothing).
                if self.health.failing.is_some() != was_failing {
                    self.route_check_due = true;
                }
                line
            }
        }
    }
}

/// The local address the system would send from to reach `addr` now: a socket connected to it (which sends nothing)
/// is given one. Fails without a route, e.g. "Network is unreachable" while the network switches.
fn route_source(addr: SocketAddr) -> io::Result<IpAddr> {
    let probe = UdpSocket::bind(if addr.is_ipv4() { "0.0.0.0:0" } else { "[::]:0" })?;
    probe.connect(addr)?;
    Ok(probe.local_addr()?.ip())
}

/// The new source address, when a socket sending from `current` should be replaced given where the route goes now
/// (`route`, from route_source): only when there is a route and it starts from another address. Without one the
/// socket is kept.
fn moved_source(current: IpAddr, route: &io::Result<IpAddr>) -> Option<IpAddr> {
    match route {
        Ok(ip) if *ip != current && !ip.is_unspecified() => Some(*ip),
        _ => None,
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

/// The user's own gaze scale (--gaze-scale-left/right/up/down) on the gaze about to be sent (left x/y, right x/y,
/// combined x/y): looking left (x below 0) goes `gaze_scale_left` times as far from the fitted zero point, and so on.
///
/// Where: on the shared (combined) direction, once everything else is done (after the filters and, with
/// --independent-eyes in v2, after the cross-eye gate), in both eye behaviours. The combined x/y is scaled, and each
/// eye moves by the same amount, so whatever separates the eyes stays as it was: v2's fixed VERGENCE_DEG and an
/// intentional cross-eye let through, v1's per-eye readings (an eye held while unreliable moves with the combined
/// gaze), and each eye's own up / down. After the filters, so they (the deadzone, how fast the One Euro filter
/// loosens) and the eyelid fit's look up / down (`vertical`, taken before this) behave the same whatever the scale; a
/// gaze capture reads the raw gaze, so the eye fit never sees it either.
///
/// Like the fit's gains in `correct_gaze`, it multiplies the -1..1 scale (an angle: 1.0 = 45°), not its tangent.
/// All four at 1.0 return the gaze untouched, bit for bit.
fn scale_gaze(gaze: [f32; 6], settings: &Settings) -> [f32; 6] {
    let Settings {
        gaze_scale_left: left,
        gaze_scale_right: right,
        gaze_scale_up: up,
        gaze_scale_down: down,
        ..
    } = *settings;
    if !settings.gaze_scaled() {
        return gaze;
    }
    let scaled = |value: f32, below: f32, above: f32| {
        let scale = if value < 0.0 { below } else { above };
        if scale == 1.0 { value } else { (value * scale).clamp(-1.0, 1.0) }
    };
    let [x, y] = [gaze[4], gaze[5]];
    let shared = [scaled(x, left, right), scaled(y, down, up)];
    let mut out = gaze;
    for (index, value) in out.iter_mut().enumerate() {
        let axis = index % 2;
        let fused = [x, y][axis];
        // An eye looking the combined way (both eyes share it unless --independent-eyes) takes it exactly
        *value = if *value == fused { shared[axis] } else { (*value + (shared[axis] - fused)).clamp(-1.0, 1.0) };
    }
    out
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
    // The same, before the user's gaze scale (scale_gaze); equal to `gaze` while the scale is 1 on all sides. Only for
    // the panel's gaze dots.
    unscaled_gaze: [f32; 6],
    // VRCFT eyelids, as sent to VRChat.
    lids: [f32; 2],
    // Whether each eye's gaze passed the quality check.
    reliable: [bool; 2],
    // Whether the combined gaze was held (eyes mostly shut, or both eyes unreliable). Only the tests read
    // it now; a gaze capture judges its samples itself (capture::usable).
    #[cfg_attr(not(test), allow(dead_code))]
    gaze_held: bool,
    // The eye camera's squint and pupils, for the eyes they are used for.
    camera: CameraValues,
    // Where each eyelid came from, where the camera overrode the usual mix.
    lid_from: [LidFrom; 2],
    // Whether each eye's camera "closed" was taken as open because the eye server read the eye open and was sure of it
    // (see CAMERA_CLOSED_DOUBT_S); for the replay's summary.
    closed_overruled: [bool; 2],
    // Whether intentional cross-eye was let through (--independent-eyes; see CrossEye).
    cross_eye: bool,
    // Per eye: the camera's "closed" was held back, not a blink or a wink yet (see CloseGate). The replay reads it.
    #[cfg_attr(not(test), allow(dead_code))]
    close_held: [bool; 2],
}

impl Sample {
    /// Whether its gaze counts in a gaze capture (capture::usable). An eye whose eyelid is the camera's because the eye
    /// server reads it closing while the camera has seen it open for longer than a blink (LidFrom::CameraOpen, see
    /// Smoother::check_lids) counts as open: the eye server's openness may be stuck at 0 for the whole wear.
    fn capture_usable(&self) -> bool {
        capture::usable(self.openness, self.camera_open(), self.reliable)
    }

    /// Which eyes' eyelids are the camera's because it sees them open while the eye server reads them closing.
    fn camera_open(&self) -> [bool; 2] {
        self.lid_from.map(|from| from == LidFrom::CameraOpen)
    }
}

/// A sample's copy for the panel (its gaze dots and eye preview; dots.rs): the gaze as sent, before smoothing and before
/// the user's gaze scale (with whether that scale is in use: not 1 on all sides), and
/// each eye's eyelid as sent, Valve's openness, squint and pupil as sent, and the eye camera's own values as read for it
/// (`camera`; NaN for an eye without fresh ones). Only for display: nothing here goes to VRChat or VRCFT.
fn dot_sample(settings: &Settings, data: &EyeData, sample: &Sample, camera: Option<&Live>) -> dots::DotSample {
    let nan = f32::NAN;
    dots::DotSample {
        time: data.sample_time,
        gaze: sample.gaze,
        raw_gaze: sample.raw_gaze,
        unscaled_gaze: sample.unscaled_gaze,
        independent: settings.independent_eyes,
        scaled: settings.gaze_scaled(),
        eyes: std::array::from_fn(|eye| {
            let seen = camera.filter(|live| live.fresh[eye]).map(|live| live.eyes[eye]);
            dots::DotEye {
                lid: sample.lids[eye],
                openness: sample.openness[eye],
                openness_scaled: sample.openness_scaled[eye],
                camera_lid: seen.map_or(nan, |seen| seen.lid),
                squint: sample.camera.squint[eye].unwrap_or(nan),
                pupil_mm: sample.camera.pupil_diameter[eye].unwrap_or(nan),
                camera_squint: seen.map_or(nan, |seen| seen.squint),
                camera_pupil_mm: seen.map_or(nan, |seen| seen.pupil_mm),
                // (LidFrom's order: 0 the eye server's usual mix, the camera's from 1 on)
                lid_from: sample.lid_from[eye] as u8,
                camera_closed: seen.is_some_and(|seen| seen.closed),
            }
        }),
    }
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

/// Let the eyelid calibration learn from a sample once `settled`, then work the sample through, with the eye camera's
/// values as read for it (None without them).
fn step(
    settings: &Settings,
    smoother: &mut Smoother,
    calibration: &mut LidCalibration,
    data: &EyeData,
    settled: bool,
    camera: Option<&Live>,
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
    process(settings, smoother, lid_scales(settings, calibration), data, camera)
}

/// Work one eye-server sample through, with the eye camera's values as read for it (`camera`; None without them).
fn process(
    settings: &Settings,
    smoother: &mut Smoother,
    scales: [f32; 2],
    data: &EyeData,
    camera: Option<&Live>,
) -> Sample {
    // Judged in --raw too (for the status file), though raw eyelids are never capped
    smoother.saturation.add(data);
    let saturated = smoother.saturation.on;
    let [x, y] = gaze_angles(data.fixation_point);
    let [left, right] = data.gaze.map(gaze_angles);
    let raw_gaze = [left[0], left[1], right[0], right[1], x, y];
    let reliable = gaze_quality(data, settings.gaze_quality_limit);
    let blink_stages = settings.blink_hold_ms > 0.0 || settings.blink_sync_below > 0.0;
    // --eye-behavior 1 is 0.7.5's processing: each eye's own gaze, and of the eyelid rules since, none (no nearly open
    // easing or far-down slope, the camera only for widening, no floor, no ceiling rule, no shared widening)
    let v2 = settings.v2();
    let mut cross_eye = false;
    let (gaze, unscaled_gaze, lids, gaze_held, openness_scaled, camera, lid_from, close_held) = if settings.raw {
        let (camera_lids, camera) = smoother.camera.take(settings, camera, false);
        let camera_lids = side_checked(smoother, data, camera_lids, v2, settings.camera_closed_doubt);
        let (camera_lids, close_held) = smoother.gate_closed(settings, data, camera_lids);
        let corrected = correct_gaze(raw_gaze, settings);
        let unscaled_gaze = choose_gaze(corrected, [true; 2], settings.independent_eyes, v2);
        let gaze = scale_gaze(unscaled_gaze, settings);
        let zoned = camera_lids.map(|camera| v2 && camera.is_none());
        let fits = fits_at(settings, corrected[5], zoned);
        let openness_scaled = lid_inputs_with(data.openness, corrected[5], scales, settings, fits);
        let mapped = if v2 {
            snapped_lids(data.openness, corrected[5], scales, settings, fits, zoned)
        } else {
            openness_scaled.map(|openness| lid_to_vrcft(openness, settings))
        };
        let valve = mapped;
        // Without the timed stages, the camera's eyelid does not take over from a closing eye server's (that waits
        // longer than a blink); an eye it sees closed still closes, and one it sees open while the eye server does
        // not read it closing is raised to it and keeps the floor
        let mut lid_from = [LidFrom::EyeServer; 2];
        let floor = settings.camera_lid_floor;
        let line = doubt_line(settings);
        let floored = [0, 1].map(|eye| {
            v2 && floor > 0.0 && camera_lids[eye].is_some_and(|camera| !camera.closed && mapped[eye] >= line)
        });
        let mapped = [0, 1].map(|eye| match camera_lids[eye].filter(|_| v2) {
            Some(camera) if camera.closed => {
                if mapped[eye] > 0.0 {
                    lid_from[eye] = LidFrom::CameraClosed;
                }
                0.0
            }
            Some(camera) => {
                let (lid, raised) = raised_to_camera(mapped[eye], camera.lid, line);
                if raised {
                    lid_from[eye] = LidFrom::CameraRaised;
                }
                if floored[eye] { lid.max(floor) } else { lid }
            }
            None => mapped[eye],
        });
        let camera_lid = camera_lids.map(|camera| camera.map(|camera| camera.lid));
        let mixed = mixed_lids(mapped, valve, camera_lid, &mut lid_from, v2);
        let shut = sync_blinks(mapped.map(|lid| lid <= 0.0), mapped, settings.blink_sync_below);
        let mut lids = sync_lids(mixed, settings.lid_sync);
        keep_floor(&mut lids, mixed, floored, floor);
        if blink_stages {
            shut_lids(&mut lids, shut);
        }
        let shut_eyes = data.openness.iter().any(|openness| *openness < settings.gaze_hold_below);
        (gaze, unscaled_gaze, lids, shut_eyes, openness_scaled, camera, lid_from, close_held)
    } else {
        let dt = smoother.advance(data.sample_time);
        // After advance(), which starts the smoothing over after a gap
        let (camera_lids, camera) = smoother.camera.take(settings, camera, true);
        let camera_lids = side_checked(smoother, data, camera_lids, v2, settings.camera_closed_doubt);
        // A "closed" in one eye that is not a blink or a wink yet: the camera's last open eyelid (see CloseGate)
        let (camera_lids, close_held) = smoother.gate_closed(settings, data, camera_lids);
        let raise_to = if v2 { smoother.raise_peak.step(data.sample_time, data.openness, camera_lids) } else { [None; 2] };
        // With --eye-behavior 2 short gaps in the camera's eyelid are bridged (see CAMERA_GAP_HOLD)
        let (camera_lids, held) =
            if v2 { smoother.steady_camera(data.sample_time, camera_lids) } else { (camera_lids, [None; 2]) };
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
        let mut gaze = choose_gaze(angles, reliable, settings.independent_eyes, v2);
        // One unreliable eye takes the other's gaze (choose_gaze), so only both being unreliable holds it; with
        // --eye-behavior 1 each eye's own gaze is held while it is unreliable
        let hold = if openness.iter().any(|openness| *openness < settings.gaze_hold_below)
            || reliable == [false; 2]
        {
            [true; 3]
        } else if settings.independent_eyes && !v2 {
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
        // For eyes without the camera's eyelid, eyelids near relaxed open count as open, and far down less is
        // expected (see LID_OPEN_SNAP_RAMP)
        let zoned = [0, 1].map(|eye| v2 && camera_lids[eye].is_none() && held[eye].is_none());
        let fits = fits_at(settings, vertical, zoned);
        let mapped = if v2 {
            snapped_lids(openness, vertical, scales, settings, fits, zoned)
        } else {
            lid_inputs(openness, vertical, scales, settings).map(|openness| lid_to_vrcft(openness, settings))
        };
        let valve = mapped;
        // Corrected by the camera where it sees an eye closed, or open for longer than a blink while the eye server
        // reads it closing; everything after this (closing, the blink sync, lid_sync) goes by the corrected eyelids
        // (and where the eye server reads it at the ceiling, which says no more than open)
        // Only for eyes whose open reading is itself at the ceiling (see ceiling_eyes)
        let at_ceiling = ceiling_eyes(settings, saturated);
        let ceiling = [0, 1].map(|eye| if at_ceiling[eye] { ceiling_share(openness[eye]) } else { 0.0 });
        let (mapped, mut lid_from, floored) = if v2 {
            smoother.check_lids(data.sample_time, dt, mapped, ceiling, camera_lids, raise_to, settings)
        } else {
            (mapped, [LidFrom::EyeServer; 2], [false; 2])
        };
        // Through a short gap in the camera's eyelid, the eye server's is still raised to the camera's last one
        let mapped = [0, 1].map(|eye| match held[eye] {
            Some(camera) => {
                let (lid, raised) = raised_to_camera(mapped[eye], raise_to[eye].unwrap_or(camera), doubt_line(settings));
                if raised {
                    lid_from[eye] = LidFrom::CameraRaised;
                }
                lid
            }
            None => mapped[eye],
        });
        // From relaxed open up (or widened on the camera while not closing), an eye the camera sees takes the camera's
        // eyelid; a closing one stays the eye server's. Widening in one eye only is halved (see share_widening)
        let camera_lid = camera_lids.map(|camera| camera.map(|camera| camera.lid));
        let mixed = mixed_lids(mapped, valve, camera_lid, &mut lid_from, v2);
        // While the openness is saturated, nothing above relaxed open is widening, except where the camera says so.
        // Capped before the widen sustain and the filter, so the filter rests at relaxed open (a closing eye starts
        // closing at once) and a widen starts over once it isn't saturated any more; and the filter itself right after
        // it, for the sample it turns on (or the camera stops) while a widen is still in the filter. The filter only
        // averages, so nothing goes above it meanwhile; lid_sync could lift a capped eye toward the other eye's camera
        // widening, so it is capped again after that. Closing, half-closed and blinks are below relaxed open and
        // untouched. The widen sustain applies to the camera's widening too.
        let capped = camera_lids.map(|camera| saturated && camera.is_none());
        let mut lids = [0, 1].map(|eye| if capped[eye] { mixed[eye].min(LID_RELAXED) } else { mixed[eye] });
        lids = smoother.sustain_widen(data.sample_time, lids);
        smoother.filter(dt, &mut gaze, &mut lids, hold);
        // Intentional cross-eye: Valve's own left - right, split evenly around the combined gaze, after the gaze filter
        // (which would only slow it down)
        let open_line = if settings.gaze_hold_below > 0.0 { settings.gaze_hold_below } else { CROSS_EYE_OPEN };
        let usable = reliable == [true; 2] && data.openness.iter().all(|openness| *openness >= open_line);
        let wide = data.openness.iter().all(|openness| *openness >= CROSS_EYE_ENGAGE_OPEN);
        let vergence = smoother.cross_eye.step(
            data.sample_time,
            dt,
            (raw_gaze[0] - raw_gaze[2]) * 45.0,
            raw_gaze[5] * 45.0,
            usable,
            wide,
        );
        if settings.independent_eyes && v2 {
            let extra = (vergence - VERGENCE_DEG) / 2.0 / 45.0;
            gaze[0] = (gaze[0] + extra).clamp(-1.0, 1.0);
            gaze[2] = (gaze[2] - extra).clamp(-1.0, 1.0);
            cross_eye = smoother.cross_eye.engaged;
        }
        // The user's own scale last, on the combined direction (see scale_gaze)
        let unscaled_gaze = gaze;
        let gaze = scale_gaze(gaze, settings);
        smoother.cap_lids(&mut lids, capped);
        let filtered = lids;
        let mut lids = sync_lids(lids, settings.lid_sync);
        for (lid, capped) in lids.iter_mut().zip(capped) {
            if capped {
                *lid = lid.min(LID_RELAXED);
            }
        }
        keep_floor(&mut lids, filtered, floored, settings.camera_lid_floor);
        if blink_stages {
            // An eye whose eyelid is the camera's because it sees the eye open (or the eye server reads it at the
            // ceiling) is neither closed at 0 (the camera's eyelid reaches 0 in a hard squint) nor pulled shut with
            // the other eye
            let seen_open = lid_from.map(|from| from == LidFrom::CameraOpen || from == LidFrom::CameraAtCeiling);
            let closed = [0, 1].map(|eye| mapped[eye] <= 0.0 && !seen_open[eye]);
            let sync = [0, 1].map(|eye| if seen_open[eye] { 1.0 } else { mapped[eye] });
            let shut = smoother.hold_shut(data.sample_time, closed, sync, settings);
            shut_lids(&mut lids, shut);
        }
        let openness_scaled = lid_inputs_with(data.openness, vertical, scales, settings, fits);
        (gaze, unscaled_gaze, lids, hold[2], openness_scaled, camera, lid_from, close_held)
    };
    Sample {
        openness: data.openness,
        openness_scaled,
        raw_gaze,
        gaze,
        unscaled_gaze,
        lids,
        reliable,
        gaze_held,
        camera,
        lid_from,
        closed_overruled: if v2 { smoother.side.overruled } else { [false; 2] },
        cross_eye,
        close_held,
    }
}

/// The camera's eyelids with a "closed" while looking to the side, or while the eye server reads the eye open and is
/// sure of it, doubted (SideCheck); with --eye-behavior 1 (`v2` false) as read.
fn side_checked(
    smoother: &mut Smoother,
    data: &EyeData,
    camera: [Option<CameraLid>; 2],
    v2: bool,
    doubt: bool,
) -> [Option<CameraLid>; 2] {
    if v2 {
        smoother.side.doubt_off = !doubt;
        smoother.side.step(data.sample_time, data.openness, gaze_variance(data), outward_degrees(data), camera)
    } else {
        camera
    }
}

/// The eyelids with the eye camera's widening mixed in (`camera`, each eye's camera eyelid while it may be used), from
/// the corrected ones (`mapped`) and the eye server's before that (`valve`), marking where widening came from the
/// camera: camera_widen_share's fade and share_widening. With --eye-behavior 1 (`v2` false), 0.7.5's mix_lid_v1.
fn mixed_lids(
    mapped: [f32; 2],
    valve: [f32; 2],
    camera: [Option<f32>; 2],
    lid_from: &mut [LidFrom; 2],
    v2: bool,
) -> [f32; 2] {
    if !v2 {
        return [0, 1].map(|eye| mix_lid_v1(mapped[eye], camera[eye]));
    }
    let basis = widen_basis(mapped, valve, *lid_from);
    let mixed = share_widening([0, 1].map(|eye| mix_lid_by(mapped[eye], basis[eye], camera[eye])), camera);
    mark_widened(lid_from, basis, camera);
    mixed
}

/// Below this VRCFT eyelid the eye server reads an eye as closing, for Smoother::check_lids: CAMERA_DOUBT_BELOW, or
/// --blink-sync-below if that is higher (an eye the blink sync could pull shut).
fn doubt_line(settings: &Settings) -> f32 {
    CAMERA_DOUBT_BELOW.max(settings.blink_sync_below)
}

/// Keep the `floored` eyes at --camera-lid-floor after lid_sync, which may pull them toward a closed other eye: no
/// lower than the floor, or than they were before it (`before`) while their filters rise to it from closed.
fn keep_floor(lids: &mut [f32; 2], before: [f32; 2], floored: [bool; 2], floor: f32) {
    for eye in 0..2 {
        if floored[eye] {
            lids[eye] = lids[eye].max(floor.min(before[eye]));
        }
    }
}

/// Mark the eyes whose eyelid is the camera's widening although the eye server read them below relaxed open (also
/// where that was raised to the camera's first), by the eyelids widening goes by (`mapped`, widen_basis).
fn mark_widened(lid_from: &mut [LidFrom; 2], mapped: [f32; 2], camera: [Option<f32>; 2]) {
    for eye in 0..2 {
        let widened = camera[eye].is_some_and(|camera| camera_widen_share(mapped[eye], camera) >= 0.5);
        // An eyelid raised to the camera's was below relaxed open before (`mapped` is the one before, widen_basis)
        let below = matches!(lid_from[eye], LidFrom::EyeServer | LidFrom::CameraRaised) && mapped[eye] < LID_RELAXED;
        if below && widened {
            lid_from[eye] = LidFrom::CameraWidened;
        }
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
/// packet (VRCFT puts the same eyelid back together), and the FrameEyeOSC module the VRCFT eyelids as they are.
fn output_lids(output: OutputKind, lids: [f32; 2]) -> [f32; 2] {
    match output {
        OutputKind::Vrchat | OutputKind::LiveLink | OutputKind::FrameEyeOsc => lids,
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

/// The FrameEyeOSC packet for one sample: the same gaze and eyelids as the Live Link packet (each eye's gaze its own
/// with "move eyes separately", else the combined one), with the eye camera's squint and pupils for the eyes they are
/// used for. VRCFT then sends the pupils to each avatar in the form it takes them, so none go straight to VRChat.
fn feo_packet(sample: &Sample, session: u32, sequence: u32, time_us: u64) -> [u8; feo::PACKET_LEN] {
    let [left_x, left_y, right_x, right_y, _, _] = sample.gaze;
    let lids = output_lids(OutputKind::FrameEyeOsc, sample.lids);
    let camera = &sample.camera;
    let eye = |side: usize, gaze: [f32; 2]| feo::Eye {
        gaze,
        lid: lids[side],
        squint: camera.squint[side],
        pupil: camera.pupil_diameter[side].zip(camera.pupil_dilation[side]),
    };
    feo::packet(session, sequence, true, time_us, [eye(0, [left_x, left_y]), eye(1, [right_x, right_y])])
}

/// The OSC messages for one sample. VRChat gets the full VRCFT v2 eye set with the eye camera's squint and pupils (see
/// camera_params), and with `steamlink_params` Steam Link's names after it. The ETVR Tracking Module gets per-eye
/// values only: EyeX/EyeY switch it to a single-eye mode that reads an eyelid we do not send.
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
    let mut messages = Vec::with_capacity(21);
    if settings.output == OutputKind::Vrchat {
        messages.extend(active_message(settings, true));
        values.extend([("EyeX", x), ("EyeY", y)]);
        values.extend(camera_params(&sample.camera));
    }
    messages.extend(
        values
            .into_iter()
            .map(|(suffix, value)| (format!("{prefix}/v2/{suffix}"), OscType::Float(value))),
    );
    if settings.output == OutputKind::Vrchat && settings.steamlink_params {
        messages.extend(steamlink_messages(sample.gaze, sample.lids));
    }
    messages
}

/// The eye camera's values as VRCFT v2 parameters: each eye's squint while the camera drives its eyelid, and the
/// combined squint (the mean of those eyes); the pupil dilation and each eye's pupil diameter (in cm, 0..1) for the
/// eyes whose pupil is used, and the combined diameter (the mean of those). Nothing for an eye the camera's values are
/// not used for: see squint_release for the squint it leaves behind.
fn camera_params(camera: &CameraValues) -> Vec<(&'static str, f32)> {
    let mut params = per_eye_params(["EyeSquintLeft", "EyeSquintRight", "EyeSquint"], camera.squint);
    params.extend(pupil_params(camera));
    params
}

/// The pupil part of camera_params: the dilation and each eye's pupil diameter (in cm, 0..1) for the eyes whose pupil
/// is used, and the combined diameter.
fn pupil_params(camera: &CameraValues) -> Vec<(&'static str, f32)> {
    let diameter = camera.pupil_diameter.map(|mm| mm.map(|mm| (mm / PUPIL_MM_PER_UNIT).min(1.0)));
    let mut params: Vec<_> = pupil_dilation(camera).map(|dilation| ("PupilDilation", dilation)).into_iter().collect();
    params.extend(per_eye_params(["PupilDiameterLeft", "PupilDiameterRight", "PupilDiameter"], diameter));
    params
}

/// The pupil dilation that goes out (PupilDilation, and its bits): the mean of the eyes whose pupil is used.
fn pupil_dilation(camera: &CameraValues) -> Option<f32> {
    mean(camera.pupil_dilation)
}

/// A value 0..1 as `bits` bools the way VRCFT's BinaryBaseParameter makes them (VRCFaceTracking.Core, OSC/DataTypes/
/// BinaryBaseParameter.cs, ProcessBinary): above 0.99999 every bit is set, else bit k of (int)(value * 2^bits).
/// Negative values give no bits: pupils are never negative, so there is no Negative parameter.
fn binary_bits(value: f32, bits: u8) -> Vec<bool> {
    let all = value > 0.99999;
    // as i64 truncates toward zero like C#'s (int), and saturates instead of overflowing
    let big = if value > 0.0 { (value * f32::from(1u8 << bits)) as i64 } else { 0 };
    (0..bits).map(|k| all || (big >> k) & 1 == 1).collect()
}

/// The pupil dilation as bool parameters, for avatars that take it bit-packed (pupil_bits): PupilDilation1, 2, 4
/// and 8 under the same prefix as the float, next to it wherever pupils go to VRChat. Only the bits that changed go
/// out, and all of them again every PUPIL_BITS_RESEND. When the pupils stop, nothing is sent: the avatar holds them,
/// like the floats.
#[derive(Default)]
struct PupilBits {
    // The bits as they last went out, and when all of them last did.
    sent: Vec<bool>,
    sent_all: Option<Instant>,
}

impl PupilBits {
    /// The messages for this sample's dilation (None: no eye's pupil is used).
    fn messages(&mut self, settings: &Settings, dilation: Option<f32>, now: Instant) -> Vec<(String, OscType)> {
        let Some(dilation) = dilation.filter(|_| settings.pupil_bits > 0) else {
            return Vec::new();
        };
        let bits = binary_bits(dilation, settings.pupil_bits);
        let all = bits.len() != self.sent.len()
            || self.sent_all.is_none_or(|at| now.saturating_duration_since(at) >= PUPIL_BITS_RESEND);
        let prefix = format!("/avatar/parameters{}/v2/PupilDilation", settings.prefix);
        let messages = bits
            .iter()
            .enumerate()
            .filter(|&(k, bit)| all || self.sent[k] != *bit)
            .map(|(k, &bit)| (format!("{prefix}{}", 1u8 << k), OscType::Bool(bit)))
            .collect();
        if all {
            self.sent_all = Some(now);
        }
        self.sent = bits;
        messages
    }

    /// Send all of them with the next sample (the settings or the target changed).
    fn reset(&mut self) {
        *self = Self::default();
    }
}

/// The left, right and combined (`names`) of per-eye values, for the eyes that have one; the combined one is their
/// mean.
fn per_eye_params(names: [&'static str; 3], values: [Option<f32>; 2]) -> Vec<(&'static str, f32)> {
    let [left, right] = values;
    [left, right, mean(values)]
        .into_iter()
        .zip(names)
        .filter_map(|(value, name)| Some((name, value?)))
        .collect()
}

/// In LiveLink mode, the eye camera's pupils as VRChat avatar parameters (the ones `--output vrchat` sends, with the
/// prefix), to go straight to VRChat next to the Live Link packets: VRCFT's LiveLink module has no pupils of its own
/// (it gives a fixed 5 mm, which VRCFT sends once, as it only sends a parameter when it changes, so these don't
/// fight it). Only pupils: the eyelids and gaze go through VRCFT. Nothing when no eye's pupil is used.
fn pupil_messages(settings: &Settings, camera: &CameraValues) -> Vec<(String, OscType)> {
    let prefix = format!("/avatar/parameters{}/v2", settings.prefix);
    pupil_params(camera)
        .into_iter()
        .map(|(name, value)| (format!("{prefix}/{name}"), OscType::Float(value)))
        .collect()
}

/// Where the eye camera's pupils go straight to VRChat, if anywhere: in LiveLink mode with pupils_to_vrchat, VRChat's
/// OSC port (9000) on the LiveLink target's host. VRCFT runs next to VRChat, so that host is VRChat's PC: the Steam
/// Link PC with "auto", else the host typed. The LiveLink port setting is VRCFT's, not VRChat's.
fn pupil_stream(settings: &Settings) -> Option<(&str, u16)> {
    (settings.sending && settings.output == OutputKind::LiveLink && settings.pupils_to_vrchat)
        .then(|| (settings.host.as_str(), OutputKind::Vrchat.default_port()))
}

/// The target of pupil_stream (also while it is off, so it is ready when it comes on).
fn pupil_target(settings: &Settings) -> Target {
    let port = OutputKind::Vrchat.default_port();
    match Target::of(settings) {
        Target::Fixed { host, .. } => Target::Fixed { host, port },
        Target::SteamLink { .. } => Target::SteamLink { port },
    }
}

/// The mean of the values there are, if any.
fn mean(values: [Option<f32>; 2]) -> Option<f32> {
    match values {
        [Some(left), Some(right)] => Some((left + right) / 2.0),
        [one, None] | [None, one] => one,
    }
}

/// Squint 0 for each eye whose squint went out (`out`) but does not with this sample (`squint`), and for the combined
/// one once no eye's does: VRChat keeps a parameter's last value, so the avatar would otherwise stay squinting.
fn squint_release(settings: &Settings, out: [bool; 2], squint: [Option<f32>; 2]) -> Vec<(String, OscType)> {
    let stopped = [0, 1].map(|eye| out[eye] && squint[eye].is_none());
    let names = [
        ("EyeSquintLeft", stopped[0]),
        ("EyeSquintRight", stopped[1]),
        ("EyeSquint", out.contains(&true) && squint == [None; 2]),
    ];
    names
        .into_iter()
        .filter(|(_, stopped)| *stopped)
        .map(|(name, _)| (format!("/avatar/parameters{}/v2/{name}", settings.prefix), OscType::Float(0.0)))
        .collect()
}

/// How closed an eye is the way Steam Link sends it (0 open, 1 shut), from a VRCFT eyelid: 0.75 (relaxed open) and
/// anything widened above it are 0, and 0.75..0 closes it evenly to 1.
fn steamlink_closed(lid: f32) -> f32 {
    (1.0 - lid / LID_RELAXED).clamp(0.0, 1.0)
}

/// The avatar parameters SteamVR's Steam Link sends from its own OSC (measured on SteamVR 2.18.2), in its order and
/// without a prefix, for avatars made for it. Its X is ours (±1 = ±45°, + right), but its Y is ours turned over
/// (+ is down). Its eyelid is how closed the eye is; the squeeze and widen values follow from that as Steam Link
/// works them out (it always sends WidenToggle 1). Each eye's gaze is its own with "move eyes separately", like
/// the VRCFT ones.
fn steamlink_messages(gaze: [f32; 6], lids: [f32; 2]) -> Vec<(String, OscType)> {
    let [left_x, left_y, right_x, right_y, _, _] = gaze;
    let [closed_left, closed_right] = lids.map(steamlink_closed);
    let param = |name: &str, value: OscType| (format!("/avatar/parameters/{name}"), value);
    let mut messages = vec![
        param("LeftEyeX", OscType::Float(left_x)),
        param("LeftEyeY", OscType::Float(-left_y)),
        param("RightEyeX", OscType::Float(right_x)),
        param("RightEyeY", OscType::Float(-right_y)),
    ];
    for (eye, closed) in [("Right", closed_right), ("Left", closed_left)] {
        let shut = closed > 0.5;
        messages.extend([
            param(&format!("{eye}EyeLid"), OscType::Float(closed)),
            param(&format!("{eye}EyeLidExpandedSqueeze"), OscType::Float(if shut { 0.0 } else { 0.8 })),
            param(&format!("{eye}EyeSqueezeToggle"), OscType::Int(i32::from(shut))),
            param(&format!("{eye}EyeWidenToggle"), OscType::Int(1)),
        ]);
    }
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

/// Unit vector in VRChat's HMD space (Unity: +X right, +Y up, +Z forward) for a gaze pair in -1..1; the inverse
/// of `gaze_angles`, so 1 is 45°.
fn gaze_vector([x, y]: [f32; 2]) -> [f32; 3] {
    let quarter = std::f32::consts::FRAC_PI_4;
    let (right, up) = ((x * quarter).tan(), (y * quarter).tan());
    let length = (right * right + up * up + 1.0).sqrt();
    [right / length, up / length, 1.0 / length]
}

/// VRChat's own eye tracking input for the gaze and VRCFT eyelids as sent: it drives the avatar descriptor's Eye
/// Look on any avatar. What it does to an avatar built for VRCFT depends on that avatar's animator (Tracking Control
/// for Eyes & Eyelids): most hand their eyes to animation while EyeTrackingActive is true and keep following the
/// VRCFT parameters, but one that also has Eyelids set up in Eye Look can close its eyelids twice as far (turn
/// native_eyes off for it). SteamVR's own Steam Link OSC sends these same addresses, so it should stay off. Each eye's
/// gaze is its own with "move eyes separately", else the combined one.
fn native_messages(settings: &Settings, gaze: [f32; 6], lids: [f32; 2]) -> Vec<(String, Vec<OscType>)> {
    let floats = |values: &[f32]| -> Vec<OscType> { values.iter().copied().map(OscType::Float).collect() };
    let [left_x, left_y, right_x, right_y, x, y] = gaze;
    let look = if settings.independent_eyes {
        let [left, right] = [[left_x, left_y], [right_x, right_y]].map(gaze_vector);
        ("/tracking/eye/LeftRightVec", floats(&[left, right].concat()))
    } else {
        ("/tracking/eye/CenterVec", floats(&gaze_vector([x, y])))
    };
    // One value for both eyes, 0 open to 1 closed; VRChat has nothing for widening or winks.
    let open = (lids[0] + lids[1]) / 2.0 / 0.75;
    vec![
        (look.0.into(), look.1),
        ("/tracking/eye/EyesClosedAmount".into(), floats(&[(1.0 - open).clamp(0.0, 1.0)])),
    ]
}

/// Where Steam Link's avatar parameters are being sent to, if anywhere. When this changes while eye tracking runs,
/// the old destination gets relaxed open eyes looking ahead (see `send_steamlink_neutral`).
fn steamlink_stream(settings: &Settings) -> Option<(&str, u16)> {
    (settings.sending && settings.output == OutputKind::Vrchat && settings.steamlink_params)
        .then(|| (settings.host.as_str(), settings.port()))
}

/// Relaxed open eyes looking straight ahead, as Steam Link's avatar parameters. Avatars made for Steam Link's OSC
/// have no `EyeTrackingActive` to fall back on, and VRChat keeps the last values, so the eyes would otherwise stay
/// where they were (shut, if tracking was lost in a blink).
fn send_steamlink_neutral(output: &mut Output) -> Result<(), Box<dyn Error>> {
    for (addr, arg) in steamlink_messages([0.0; 6], [LID_RELAXED; 2]) {
        output.send(addr, vec![arg])?;
    }
    Ok(())
}

/// Where VRChat's own eye tracking input is being sent to, if anywhere. When this changes while eye tracking
/// runs, the old destination gets relaxed open eyes looking ahead (see `send_native_neutral`).
fn native_stream(settings: &Settings) -> Option<(&str, u16)> {
    (settings.sending && settings.output == OutputKind::Vrchat && settings.native_eyes)
        .then(|| (settings.host.as_str(), settings.port()))
}

/// Relaxed open eyes looking straight ahead, as VRChat's own eye tracking input. VRChat has no "not active" for
/// it and keeps the last values until its own timeout hands the eyes back to its automatic eye movement, so the
/// eyes would otherwise stay where they were (shut, if tracking was lost in a blink) until then.
fn send_native_neutral(output: &mut Output, settings: &Settings) -> Result<(), Box<dyn Error>> {
    for (addr, args) in native_messages(settings, [0.0; 6], [0.75; 2]) {
        output.send(addr, args)?;
    }
    Ok(())
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

/// Where VRCFT's FrameEyeOSC module is being sent to, if anywhere. When this changes while eye tracking runs, the old
/// destination gets a packet without TRACKING (relaxed open eyes, straight ahead) at once, instead of after the
/// module's one-second timeout.
fn feo_stream(settings: &Settings) -> Option<(&str, u16)> {
    (settings.sending && settings.output == OutputKind::FrameEyeOsc).then(|| (settings.host.as_str(), settings.port()))
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
    // The panel's debug gaze dots and eye preview (only while gaze_debug_dots is on or the panel listens).
    dots: dots::DotStream,
    // When the last neutral Live Link packet went out.
    livelink_neutral: Option<Instant>,
    // Keeps the Live Link samples at or below LIVELINK_MAX_HZ.
    livelink_throttle: Throttle,
    // This run's FrameEyeOSC session, the next packet's sequence number, and when the last one without TRACKING went
    // out.
    feo_session: u32,
    feo_sequence: u32,
    feo_idle: Option<Instant>,
    // Whether frameeyeosc keeps up with the eye tracker.
    pace: Pace,
    // eyecam-rec's live eye-camera values, and the newest read of them.
    camera: LiveReader,
    camera_live: Option<Live>,
    // Which eyes' squint went out to VRChat with the last sample, so it can be set back to 0 when it stops.
    squint_out: [bool; 2],
    // In LiveLink mode, the eye camera's pupils straight to VRChat (see pupil_stream): a sender of its own, so the
    // Live Link rate and drops stay the LiveLink module's.
    pupil_output: Output,
    // The pupil dilation's bits that went out (pupil_bits), wherever pupils go to VRChat.
    pupil_bits: PupilBits,
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
        if self.squint_out.contains(&true) && stream != vrchat_stream(&settings) {
            self.release_squint()?;
        }
        let stream = steamlink_stream(&self.settings);
        if self.active_since.is_some() && stream.is_some() && stream != steamlink_stream(&settings) {
            send_steamlink_neutral(&mut self.output)?;
        }
        let stream = native_stream(&self.settings);
        if self.active_since.is_some() && stream.is_some() && stream != native_stream(&settings) {
            send_native_neutral(&mut self.output, &self.settings)?;
        }
        let stream = livelink_stream(&self.settings);
        if self.active_since.is_some() && stream.is_some() && stream != livelink_stream(&settings) {
            self.send_livelink_neutral();
        }
        let stream = feo_stream(&self.settings);
        if self.active_since.is_some() && stream.is_some() && stream != feo_stream(&settings) {
            self.send_feo_idle();
        }
        self.output.set_target(Target::of(&settings));
        // Nothing to set back when the pupils stop going there: they just hold
        self.pupil_output.set_target(pupil_target(&settings));
        // All the bits again with the next sample, wherever they go now and however many
        self.pupil_bits.reset();
        self.smoother.configure(&settings);
        if settings.eye_behavior != self.settings.eye_behavior {
            self.smoother.reset_behavior();
        }
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
        self.read_camera();
        let camera = self.camera_live.as_ref();
        let sample = step(&self.settings, &mut self.smoother, &mut self.calibration, &data, settled, camera);
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
                    // At the same pace
                    if pupil_stream(&self.settings).is_some() {
                        let bits = self.pupil_bits.messages(&self.settings, pupil_dilation(&sample.camera), now);
                        for (addr, arg) in pupil_messages(&self.settings, &sample.camera).into_iter().chain(bits) {
                            self.pupil_output.send(addr, vec![arg])?;
                        }
                    }
                }
                ready
            } else if self.settings.output == OutputKind::FrameEyeOsc {
                let sequence = self.next_feo_sequence();
                let packet = feo_packet(&sample, self.feo_session, sequence, feo::time_us(SystemTime::now()));
                self.output.send_datagram(&packet);
                true
            } else {
                for (addr, arg) in osc_messages(&self.settings, &sample) {
                    self.output.send(addr, vec![arg])?;
                }
                if self.settings.output == OutputKind::Vrchat {
                    for (addr, arg) in self.pupil_bits.messages(&self.settings, pupil_dilation(&sample.camera), now) {
                        self.output.send(addr, vec![arg])?;
                    }
                    for (addr, arg) in squint_release(&self.settings, self.squint_out, sample.camera.squint) {
                        self.output.send(addr, vec![arg])?;
                    }
                    self.squint_out = sample.camera.squint.map(|squint| squint.is_some());
                }
                if native_stream(&self.settings).is_some() {
                    for (addr, args) in native_messages(&self.settings, sample.gaze, sample.lids) {
                        self.output.send(addr, args)?;
                    }
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
        if self.dots.wanted(self.settings.gaze_debug_dots, now) {
            self.dots.send(&dot_sample(&self.settings, &data, &sample, self.camera_live.as_ref()));
        }
        if let Some(capture) = &mut self.capture {
            let gaze = [sample.raw_gaze[4], sample.raw_gaze[5]];
            let eye_x = [sample.raw_gaze[0], sample.raw_gaze[2]];
            let usable = sample.capture_usable();
            if capture.add(data.sample_time, gaze, eye_x, data.openness, sample.camera_open(), usable) {
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
        if self.squint_out.contains(&true) {
            self.release_squint()?;
        }
        if steamlink_stream(&self.settings).is_some() {
            send_steamlink_neutral(&mut self.output)?;
        }
        if native_stream(&self.settings).is_some() {
            send_native_neutral(&mut self.output, &self.settings)?;
        }
        if livelink_stream(&self.settings).is_some() {
            self.send_livelink_neutral();
        }
        if feo_stream(&self.settings).is_some() {
            self.send_feo_idle();
        }
        self.smoother.reset();
        // The next wear may read differently; reset keeps what this one reads only over short gaps
        self.smoother.cross_eye.level = CrossEyeLevel::default();
        self.active_since = None;
        self.latest = None;
        Ok(())
    }

    /// Set the squint that went out back to 0 (see squint_release), where it went with the current settings.
    fn release_squint(&mut self) -> Result<(), Box<dyn Error>> {
        for (addr, arg) in squint_release(&self.settings, self.squint_out, [None; 2]) {
            self.output.send(addr, vec![arg])?;
        }
        self.squint_out = [false; 2];
        Ok(())
    }

    /// Check on eyecam-rec's live file when due (logging what changed), and read its newest values.
    fn read_camera(&mut self) {
        if let Some(line) = self.camera.check(Instant::now()) {
            eprintln!("{line}");
        }
        self.camera_live = self.camera.read(eyecam_live::monotonic_ns());
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

    /// The sequence number for the next FrameEyeOSC packet.
    fn next_feo_sequence(&mut self) -> u32 {
        let sequence = self.feo_sequence;
        self.feo_sequence = sequence.wrapping_add(1);
        sequence
    }

    /// A FrameEyeOSC packet without TRACKING: relaxed open eyes looking straight ahead.
    fn send_feo_idle(&mut self) {
        let sequence = self.next_feo_sequence();
        let time_us = feo::time_us(SystemTime::now());
        let packet = feo::packet(self.feo_session, sequence, false, time_us, [feo::Eye::NEUTRAL; 2]);
        self.output.send_datagram(&packet);
        self.feo_idle = Some(Instant::now());
    }

    /// While sending to VRCFT's FrameEyeOSC module without eye data, keep telling it so (see FEO_IDLE_INTERVAL).
    /// Paused, nothing goes out.
    fn keep_feo_alive(&mut self) {
        let due = self.feo_idle.is_none_or(|sent| sent.elapsed() >= FEO_IDLE_INTERVAL);
        if self.active_since.is_none() && feo_stream(&self.settings).is_some() && due {
            self.send_feo_idle();
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
        let vrchat = settings.output == OutputKind::Vrchat;
        // The FrameEyeOSC module gets the squint and pupils like VRChat directly
        let camera_out = vrchat || settings.output == OutputKind::FrameEyeOsc;
        let pupils = pupil_stream(settings).is_some();
        let camera = self.latest.as_ref().map(|sample| sample.camera).unwrap_or_default();
        let live = self.camera_live.as_ref();
        Status {
            version: 1,
            pid: std::process::id(),
            time: status::unix_time(SystemTime::now()),
            started: status::unix_time(self.started),
            sending: settings.sending,
            output: settings.output,
            target_mode: if settings.host == "auto" { "auto" } else { "fixed" },
            target: self.output.addr().map(|addr| addr.to_string()),
            pupil_target: self.pupil_output.addr().filter(|_| pupils).map(|addr| addr.to_string()),
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
                squint: (camera_out && camera.squint != [None; 2])
                    .then(|| status::round(camera.squint.map(|squint| squint.unwrap_or(0.0)))),
                pupil_dilation: mean(camera.pupil_dilation).filter(|_| camera_out || pupils).map(status::round_one),
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
            camera: self.camera.opened().then(|| CameraStatus {
                present: live.is_some_and(Live::present),
                calib_state: live.map_or(0, |live| live.calib_state),
                recalib_suggested: live.is_some_and(|live| live.recalib_suggested),
                used: camera.squint.map(|squint| squint.is_some()),
                pupil_used: camera.pupil_dilation.map(|dilation| dilation.is_some()),
                error: self.camera.error(),
            }),
            locked: &self.config.locked,
            effective: settings,
            gaze_capture: self.capture_result.as_ref(),
            last_error: status::ERRORS.last(),
        }
    }
}

/// Set by SIGINT / SIGTERM while recording, so the recording ends with every sample so far written out.
static STOP_RECORDING: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

extern "C" fn stop_recording(_: libc::c_int) {
    STOP_RECORDING.store(true, std::sync::atomic::Ordering::Relaxed);
}

/// Write every sample the eye server produces to `path` until stopped (Ctrl+C, SIGINT or SIGTERM: the file is then
/// complete up to the stop), with eyecam-rec's eye-camera values from `camera_path` as read with each sample (only
/// read, like the live loop does). Nothing is sent, and the status and calibration files are left alone, so this can
/// run next to the installed service (the panel's "Eye log" runs it).
fn record(path: &Path, camera_path: &Path) -> Result<(), Box<dyn Error>> {
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
    // Says in the log whether there are eye-camera values, from which eyecam-rec, and when that changes
    let mut camera = LiveReader::new(camera_path.to_owned(), Instant::now());
    let mut last_flush = Instant::now();
    let mut reported = 0;
    loop {
        if STOP_RECORDING.load(std::sync::atomic::Ordering::Relaxed) {
            recorder.flush()?;
            eprintln!(
                "Stopped: {} samples in {} ({} with eye-camera values)",
                recorder.count,
                path.display(),
                recorder.camera_count
            );
            return Ok(());
        }
        if let Some(line) = camera.check(Instant::now()) {
            eprintln!("{line}");
        }
        match source.next(POLL)? {
            Next::Sample(data) => {
                let now_ns = eyecam_live::monotonic_ns();
                let live = camera.read(now_ns);
                recorder.write(&data, live.as_ref().map(|live| (live, now_ns)))?;
            }
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
        return record(path, &args.eyecam_live.clone().unwrap_or_else(eyecam_live::live_path));
    }
    if args.target != "auto" && config::split_target(&args.target).is_none() {
        return Err("--target must be HOST:PORT or auto".into());
    }
    let in_config_dir = |name: &str| Some(config::config_dir()?.join(name));
    let calibration_path = args.calibration_file.clone().or_else(|| in_config_dir("calibration"));
    let config_path = args.config.clone().or_else(|| in_config_dir("config.json"));
    let replay = args.replay.clone().map(|input| (input, args.replay_out.clone()));
    let camera_path = args.eyecam_live.clone().unwrap_or_else(eyecam_live::live_path);
    let mut config = Config::new(config_path, args, config::given_options(&matches));
    let settings = config.load()?;
    if let Some((input, output)) = replay {
        // Starts from the saved calibration like a real run, but never writes it back.
        let mut calibration = LidCalibration::load(calibration_path, settings.lid_open);
        calibration.path = None;
        return replay::run(&input, output.as_deref(), &settings, &calibration);
    }
    // camera_lids for eyecam-rec too ("live off" / "live on" on its control socket, next to its live file), on a
    // thread of its own
    let live_wanted = Arc::new(AtomicBool::new(settings.camera_lids));
    if let Some(dir) = camera_path.parent() {
        eyecam_ctl::LiveControl::new(dir.to_owned()).spawn(live_wanted.clone())?;
    }
    let pupil_output = Output::new(pupil_target(&settings));
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
        feo_session: feo::new_session(),
        feo_sequence: 0,
        feo_idle: None,
        pace: Pace::default(),
        camera: LiveReader::new(camera_path, Instant::now()),
        camera_live: None,
        squint_out: [false; 2],
        pupil_output,
        pupil_bits: PupilBits::default(),
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
            live_wanted.store(bridge.settings.camera_lids, Ordering::Relaxed);
        }
        bridge.output.refresh();
        if pupil_stream(&bridge.settings).is_some() {
            bridge.pupil_output.refresh();
        }
        if let Some(line) = eye.open_if_due(Instant::now()) {
            // ("Can't read eye data" is kept for the panel)
            if eye.error.is_some() {
                status::log_error(&line);
            } else {
                eprintln!("{line}");
            }
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
        bridge.keep_feo_alive();
        if status_file.due() {
            if let Some(line) = eye.refresh_dominant_eye() {
                eprintln!("{line}");
            }
            if let Some(line) = bridge.check_low_rate(Instant::now()) {
                eprintln!("{line}");
            }
            // Also while there are no eye samples to read it with
            bridge.read_camera();
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
            .map(|data| process(settings, &mut smoother, [1.0; 2], data, None))
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
        assert_eq!(choose_gaze(angles, [true; 2], false, true), [5.0, 6.0, 5.0, 6.0, 5.0, 6.0]);
        assert_eq!(choose_gaze(angles, [true, false], false, true), [1.0, 2.0, 1.0, 2.0, 1.0, 2.0]);
        assert_eq!(choose_gaze(angles, [false; 2], false, true), [5.0, 6.0, 5.0, 6.0, 5.0, 6.0]);
        // Each eye on its own: the unreliable left eye takes the right eye's up / down, and both turn in from the
        // right eye's sideways gaze
        let half = VERGENCE_DEG / 2.0 / 45.0;
        let angles = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6];
        assert_eq!(choose_gaze(angles, [false, true], true, true), [0.3 + half, 0.4, 0.3 - half, 0.4, 0.3, 0.4]);
    }

    /// Both eyes open, the combined gaze straight ahead, and Valve's per-eye x `vergence` degrees apart at sample `index`.
    fn crossed(index: usize, vergence: f32, openness: [f32; 2]) -> EyeData {
        crossed_down(index, vergence, 0.0, openness)
    }

    /// The same, with the eyes `down` degrees down.
    fn crossed_down(index: usize, vergence: f32, down: f32, openness: [f32; 2]) -> EyeData {
        let half = (vergence / 2.0).to_radians().tan();
        let y = -down.to_radians().tan();
        let mut data = reading(index, [0.0, y], openness);
        data.gaze = [[half, y, -1.0], [-half, y, -1.0]];
        data
    }

    /// Whether any sample let cross-eye through.
    fn any_cross_eye(sent: &[Sample]) -> bool {
        sent.iter().any(|sample| sample.cross_eye)
    }

    /// The left - right sent, in degrees.
    fn sent_vergence(sample: &Sample) -> f32 {
        (sample.gaze[0] - sample.gaze[2]) * 45.0
    }

    fn independent() -> Settings {
        Settings {
            independent_eyes: true,
            gaze_deadzone: 0.0,
            ..settings()
        }
    }

    #[test]
    fn intentional_cross_eye_shows_once_it_lasts() {
        // 1 s straight ahead, 1 s turning in to 30°, 3 s held there, then straight ahead again (90 samples a second)
        let vergence = |i: usize| match i {
            0..90 => 0.0,
            90..180 => (i - 90) as f32 / 90.0 * 30.0,
            180..450 => 30.0,
            _ => 0.0,
        };
        let readings: Vec<EyeData> = (0..600).map(|i| crossed(i, vergence(i), [0.9; 2])).collect();
        let sent = run(&independent(), &readings);
        // 20° is reached at sample 150 and 25° at 165: it shows 0.25 s (23 samples) after 25°, before 0.5 s at 20°
        assert!(sent[..187].iter().all(|sample| !sample.cross_eye && (sent_vergence(sample) - VERGENCE_DEG).abs() < 1e-3));
        assert!(sent[189].cross_eye);
        // Blended in over 0.2 s, then Valve's 30°, split evenly around the combined gaze
        assert!((sent_vergence(&sent[240]) - 30.0).abs() < 0.2, "{}", sent_vergence(&sent[240]));
        assert!((sent[300].gaze[0] + sent[300].gaze[2]).abs() < 1e-4);
        // Back below 15° at sample 450: gone 0.15 s later, and back to the fixed vergence 0.2 s after that
        assert!(sent[455].cross_eye && !sent[466].cross_eye);
        assert!((sent_vergence(&sent[500]) - VERGENCE_DEG).abs() < 1e-3);
        // Without --independent-eyes nothing of it shows
        assert!(run(&settings(), &readings).iter().all(|sample| !sample.cross_eye && sample.gaze[0] == sample.gaze[2]));
    }

    #[test]
    fn a_shallow_cross_eye_shows_after_half_a_second() {
        // 0.5 s straight ahead, then 22° (the shallowest tries): 0.5 s (45 samples) at or above 20°
        let readings: Vec<EyeData> =
            (0..165).map(|i| crossed(i, if i < 45 { 0.0 } else { 22.0 }, [1.0; 2])).collect();
        let sent = run(&independent(), &readings);
        assert!(!sent[45 + 43].cross_eye && sent[45 + 46].cross_eye);
        assert!((sent_vergence(&sent[145]) - 22.0).abs() < 0.2);
    }

    #[test]
    fn noise_does_not_look_like_cross_eye() {
        // A 0.2 s spike to 30° (everyday stretches at or above 25° lasted up to 0.16 s), 19° held for 3 s, and 22° held
        // for 3 s with the eyes only 0.6 open (as at the end of one everyday recording)
        let spike: Vec<EyeData> = (0..180).map(|i| crossed(i, if (60..78).contains(&i) { 30.0 } else { 0.0 }, [0.9; 2])).collect();
        let steady: Vec<EyeData> = (0..360).map(|i| crossed(i, 19.0, [0.9; 2])).collect();
        let narrowed: Vec<EyeData> = (0..360).map(|i| crossed(i, 22.0, [0.6; 2])).collect();
        // 0.6 s at 22° (after 0.5 s straight ahead) with a blink in the middle starts over after it
        let broken: Vec<EyeData> = (0..105)
            .map(|i| {
                let vergence = if i < 45 { 0.0 } else { 22.0 };
                crossed(i, vergence, if (65..69).contains(&i) { [0.2; 2] } else { [0.9; 2] })
            })
            .collect();
        for readings in [spike, steady, narrowed, broken] {
            for sample in run(&independent(), &readings) {
                assert!(!sample.cross_eye && (sent_vergence(&sample) - VERGENCE_DEG).abs() < 1e-3);
            }
        }
    }

    #[test]
    fn a_blink_while_cross_eyed_holds_it_and_a_long_one_ends_it() {
        // 0.5 s straight ahead, cross-eyed (it shows from sample 68), a 0.2 s blink (Valve's per-eye x wild
        // meanwhile), 1 s cross-eyed, then a 0.6 s blink and cross-eyed again
        let blinking = |i: usize| (150..168).contains(&i) || (258..312).contains(&i);
        let readings: Vec<EyeData> = (0..400)
            .map(|i| match i {
                _ if blinking(i) => crossed(i, 0.0, [0.1; 2]),
                0..45 => crossed(i, 0.0, [0.9; 2]),
                _ => crossed(i, 30.0, [0.9; 2]),
            })
            .collect();
        let sent = run(&independent(), &readings);
        assert!(sent[100].cross_eye && (sent_vergence(&sent[100]) - 30.0).abs() < 0.2);
        // The short blink holds it
        assert!(sent[150..200].iter().all(|sample| sample.cross_eye && (sent_vergence(sample) - 30.0).abs() < 0.2));
        // The long one ends it after 0.3 s and the vergence goes back to the fixed one; it shows again 0.25 s after
        // the eyes have been open for 0.3 s
        assert!(sent[280].cross_eye && !sent[290].cross_eye);
        assert!((sent_vergence(&sent[310]) - VERGENCE_DEG).abs() < 1e-3);
        assert!(!sent[360].cross_eye && sent[363].cross_eye);
    }

    #[test]
    fn a_wear_that_reads_turned_in_raises_the_lines() {
        // 15 s of Valve reading 8°, 17° and 19° in turn (0.5 s each, a median of 17°), so the lines are 7° higher;
        // then 26° for 2 s after an ordinary reading (shown with the lines as they were), 1 s more of the same, and 40°
        let wear = |i: usize| [8.0, 17.0, 19.0][i / 45 % 3];
        let vergence = |i: usize| match i {
            0..1350 => wear(i),
            1350..1395 => 8.0,
            1395..1575 => 26.0,
            1575..1665 => wear(i),
            1665..1710 => 8.0,
            1710..1800 => 40.0,
            _ => 8.0,
        };
        let readings: Vec<EyeData> = (0..1890).map(|i| crossed(i, vergence(i), [0.9; 2])).collect();
        let sent = run(&independent(), &readings);
        assert!(!any_cross_eye(&sent[..1710]));
        assert!(sent[1760].cross_eye && (sent_vergence(&sent[1790]) - 40.0).abs() < 0.2);
        // Below the raised exit line (22°) for 0.15 s, it is gone again
        assert!(!sent[1880].cross_eye);
        // The same 26° at the start of a wear (no level yet) shows
        let fresh: Vec<EyeData> = (1350..1575).map(|i| crossed(i - 1350, vergence(i), [0.9; 2])).collect();
        assert!(any_cross_eye(&run(&independent(), &fresh)));
        // What the wear reads is kept over a gap in the samples, which starts the rest over
        let gapped: Vec<EyeData> = readings
            .iter()
            .enumerate()
            .map(|(i, data)| {
                let mut data = *data;
                if i >= 1340 {
                    data.sample_time += 2.0;
                }
                data
            })
            .collect();
        assert!(!any_cross_eye(&run(&independent(), &gapped)[..1710]));
    }

    #[test]
    fn the_wear_level_goes_on_after_the_sample_clock_goes_back() {
        let mut level = CrossEyeLevel::default();
        let feed = |level: &mut CrossEyeLevel, start: f64, value: f32| {
            (0..CROSS_EYE_LEVEL_MIN + 90).map(|i| level.add(start + i as f64 / 90.0, value, true)).last().unwrap()
        };
        assert_eq!(feed(&mut level, 1000.0, 17.0), Some(17.0));
        // The eye server restarted and its clock with it: the samples from "after" it go, the last level holds until
        // there are enough new ones, and then it follows them (instead of waiting until the clock is past 1011 s)
        assert_eq!(level.add(5.0, 3.0, true), Some(17.0));
        assert!(level.recent.len() == 1 && level.next == Some(5.0 + CROSS_EYE_LEVEL_EVERY_S));
        assert_eq!(feed(&mut level, 5.0 + 1.0 / 90.0, 3.0), Some(3.0));
    }

    #[test]
    fn the_wear_level_is_kept_over_a_gap_but_not_after_tracking_stops() {
        let mut bridge = test_bridge(independent());
        let mark = |bridge: &mut Bridge| {
            bridge.smoother.cross_eye.level.recent.push_back((5.0, 17.0));
            bridge.smoother.cross_eye.level.level = Some(17.0);
        };
        mark(&mut bridge);
        bridge.smoother.reset();
        assert_eq!(bridge.smoother.cross_eye.level.level, Some(17.0));
        bridge.on_lost("test").unwrap();
        let level = &bridge.smoother.cross_eye.level;
        assert!(level.level.is_none() && level.recent.is_empty() && level.next.is_none());
    }

    #[test]
    fn far_down_or_slowly_crept_up_is_not_cross_eye() {
        // 0.5 s straight ahead, then 30° for 2 s: 20° down it does not show, 10° down it does
        let looking = |down: f32| -> Vec<EyeData> {
            (0..225).map(|i| crossed_down(i, if i < 45 { 0.0 } else { 30.0 }, down, [0.9; 2])).collect()
        };
        assert!(!any_cross_eye(&run(&independent(), &looking(20.0))));
        assert!(any_cross_eye(&run(&independent(), &looking(10.0))));
        // From 10° up to 26° over 10 s (20° 3.1 s after it was last below 15°), then 26° for 2 s
        let creeping: Vec<EyeData> =
            (0..1080).map(|i| crossed(i, 10.0 + (i.min(900) as f32 / 900.0) * 16.0, [0.9; 2])).collect();
        assert!(!any_cross_eye(&run(&independent(), &creeping)));
    }

    #[test]
    fn right_after_a_blink_is_not_cross_eye() {
        // Straight ahead, 0.3 s shut, then 34° for 0.3 s from the moment the eyes open (Valve still reading the shut
        // eyes' turn-in) and straight ahead again
        let readings: Vec<EyeData> = (0..180)
            .map(|i| match i {
                45..72 => crossed(i, 25.0, [0.1; 2]),
                72..99 => crossed(i, 34.0, [0.9; 2]),
                _ => crossed(i, 0.0, [0.9; 2]),
            })
            .collect();
        assert!(!any_cross_eye(&run(&independent(), &readings)));
    }

    #[test]
    fn far_down_slope_leaves_an_eye_that_can_widen_alone() {
        // Room to widen (0.85 + 0.07 <= 0.97): looking 24° down at a reading of 0.80, the eye is normally open, not
        // widened (the slope would have moved where widening starts down to 0.66)
        let room = Settings {
            lid_fit_closed_left: Some(0.2),
            lid_fit_up_left: Some(0.85),
            lid_fit_open_left: Some(0.85),
            lid_fit_down_left: Some(0.8),
            ..settings()
        };
        let down = -24.0 / 45.0;
        let lid = |settings: &Settings| {
            let fits = fits_at(settings, down, [true; 2]);
            lid_to_vrcft(lid_inputs_with([0.8; 2], down, [1.0; 2], settings, fits)[0], settings)
        };
        assert!(widen_room(&room.lid_fit()[0].unwrap(), room.lid_widen));
        assert!((lid(&room) - LID_RELAXED).abs() < 1e-3, "{}", lid(&room));
        assert_eq!(fits_at(&room, down, [true; 2])[0], room.lid_fit()[0]);
        // No room (--lid-widen off, or an open reading at the ceiling): the slope applies as before
        let off = Settings {
            lid_widen: Widen::Off,
            ..room.clone()
        };
        assert!(fits_at(&off, down, [true; 2])[0].unwrap().down < 0.8 - 0.1);
        let saturated = Settings {
            lid_fit_open_left: Some(1.0),
            lid_fit_up_left: Some(1.0),
            ..room
        };
        assert!(fits_at(&saturated, down, [true; 2])[0].unwrap().down < 0.8 - 0.1);
    }

    #[test]
    fn the_ceiling_rule_is_for_eyes_whose_open_reading_is_at_the_ceiling() {
        let fitted_at = |open: f32| Settings {
            lid_fit_closed_left: Some(0.0),
            lid_fit_up_left: Some(open),
            lid_fit_open_left: Some(open),
            lid_fit_down_left: Some(open),
            ..settings()
        };
        // A fitted eye that reads 0.85 open: 0.90 and up is widening there, never a jump to the ceiling
        assert_eq!(ceiling_eyes(&fitted_at(0.85), true), [false, true]);
        assert_eq!(ceiling_eyes(&fitted_at(1.0), false), [true, false]);
        assert_eq!(ceiling_eyes(&fitted_at(0.9), false)[0], true);
        // Without a fit it goes by the saturation
        assert_eq!(ceiling_eyes(&settings(), false), [false; 2]);
        assert_eq!(ceiling_eyes(&settings(), true), [true; 2]);
    }

    #[test]
    fn a_camera_eyelid_is_held_through_short_gaps() {
        let lid = |lid: f32| Some(CameraLid { lid, closed: false, frame: 0.011 });
        let mut gap = CameraGap::default();
        // The first sample says which it is
        assert_eq!(gap.steady(0.0, lid(0.6)), (lid(0.6), None));
        // A gap of up to 0.2 s keeps the last eyelid to raise to; longer, and the eye is off the camera
        assert_eq!(gap.steady(0.1, None), (None, Some(0.6)));
        assert_eq!(gap.steady(0.2, None), (None, Some(0.6)));
        assert_eq!(gap.steady(0.21, None), (None, None));
        // Back: taken once it has kept coming for 0.2 s
        assert_eq!(gap.steady(0.3, lid(0.5)), (None, None));
        assert_eq!(gap.steady(0.45, lid(0.5)), (None, None));
        assert_eq!(gap.steady(0.51, lid(0.55)), (lid(0.55), None));
        // A blip back while off starts that over
        let mut off = CameraGap::default();
        assert_eq!(off.steady(0.0, None), (None, None));
        assert_eq!(off.steady(0.1, lid(0.5)), (None, None));
        assert_eq!(off.steady(0.15, None), (None, None));
        assert_eq!(off.steady(0.2, lid(0.5)), (None, None));
        assert_eq!(off.steady(0.35, lid(0.5)), (None, None));
        assert_eq!(off.steady(0.41, lid(0.5)), (lid(0.5), None));
        // Through the pipeline: an eye the eye server reads low (0.85 through a fit that reads 1.000 open) and the
        // camera sees open keeps the camera's raise through a 0.1 s gap, instead of stepping down to the eye server's
        let fitted = Settings {
            lid_fit_closed_left: Some(0.0),
            lid_fit_up_left: Some(1.0),
            lid_fit_open_left: Some(1.0),
            lid_fit_down_left: Some(1.0),
            lid_fit_closed_right: Some(0.0),
            lid_fit_up_right: Some(1.0),
            lid_fit_open_right: Some(1.0),
            lid_fit_down_right: Some(1.0),
            lid_open_snap: LID_RELAXED,
            lid_calibration: false,
            ..settings()
        };
        let mut smoother = Smoother::new(&fitted);
        let seen = camera_samples(&fitted, &mut smoother, 10.0, 1.0, |_| [0.85; 2], |_| Some(camera(0.75, 0.0)));
        assert!(seen.last().unwrap().lids.iter().all(|lid| (lid - LID_RELAXED).abs() < 0.01));
        let gap = camera_samples(&fitted, &mut smoother, 11.0, 0.1, |_| [0.85; 2], |_| None);
        assert!(gap.iter().all(|sample| sample.lids.iter().all(|lid| (lid - LID_RELAXED).abs() < 0.01)), "{:?}", gap[8].lids);
    }

    #[test]
    fn switching_eye_behavior_starts_over_what_only_v2_keeps() {
        let mut bridge = test_bridge(settings());
        let mark = |bridge: &mut Bridge| {
            bridge.smoother.lid_doubt[0].camera = true;
            bridge.smoother.ceiling_since[1] = Some(1.0);
            bridge.smoother.cross_eye.engaged = true;
            bridge.smoother.camera_gap[0].on = Some(true);
            bridge.smoother.last_time = Some(5.0);
        };
        let cleared = |bridge: &Bridge| {
            !bridge.smoother.lid_doubt[0].camera
                && bridge.smoother.ceiling_since == [None; 2]
                && !bridge.smoother.cross_eye.engaged
                && bridge.smoother.camera_gap[0].on.is_none()
                // The filters go on
                && bridge.smoother.last_time == Some(5.0)
        };
        let v1 = Settings {
            eye_behavior: 1,
            ..settings()
        };
        for next in [v1.clone(), settings()] {
            mark(&mut bridge);
            bridge.apply(reload(&next)).unwrap();
            assert!(cleared(&bridge));
        }
        // Another change keeps them
        mark(&mut bridge);
        bridge.apply(reload(&Settings { lid_sync: 0.3, ..settings() })).unwrap();
        assert!(bridge.smoother.cross_eye.engaged && bridge.smoother.lid_doubt[0].camera);
    }

    #[test]
    fn eye_behavior_1_sends_each_eyes_own_gaze_and_0_7_5s_eyelids() {
        let v1 = Settings {
            eye_behavior: 1,
            ..independent()
        };
        // Each eye's own gaze as the Frame reads it, an unreliable one held (choose_gaze leaves it as it is)
        let angles = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6];
        assert_eq!(choose_gaze(angles, [true; 2], true, false), angles);
        assert_eq!(choose_gaze(angles, [false, true], true, false), [0.1, 0.2, 0.3, 0.4, 0.3, 0.4]);
        // Valve's 10° apart goes out 10° apart (not the fixed vergence), and 30° held is no cross-eye gate
        let apart: Vec<EyeData> = (0..90).map(|i| crossed(i, 10.0, [0.9; 2])).collect();
        let sent = run(&v1, &apart);
        assert!((sent_vergence(&sent[89]) - 10.0).abs() < 0.05, "{}", sent_vergence(&sent[89]));
        let crossed_long: Vec<EyeData> = (0..180).map(|i| crossed(i, 30.0, [0.9; 2])).collect();
        assert!(run(&v1, &crossed_long).iter().all(|sample| !sample.cross_eye));
        // A nearly open eyelid (VRCFT 0.6 at the default lid marks) is not eased up to relaxed open
        let nearly: Vec<EyeData> = (0..120).map(|i| reading(i, [0.0, 0.0], [0.7; 2])).collect();
        let lid = |settings: &Settings| run(settings, &nearly)[119].lids[0];
        assert!((lid(&v1) - 0.6).abs() < 1e-3, "{}", lid(&v1));
        assert!(lid(&independent()) > 0.65);
        // From relaxed open up the camera's eyelid, kept at relaxed open or above; below it the eye server's
        assert_eq!(mix_lid_v1(0.8, Some(0.7)), LID_RELAXED);
        assert_eq!(mix_lid_v1(0.8, Some(0.9)), 0.9);
        assert_eq!(mix_lid_v1(0.6, Some(0.9)), 0.6);
        assert_eq!(mix_lid_v1(0.8, None), 0.8);
    }

    #[test]
    fn independent_eyes_turn_in_by_the_fixed_vergence_from_the_combined_gaze() {
        let half = VERGENCE_DEG / 2.0 / 45.0;
        // The Frame's own per-eye x 17° apart (as at 22:44 on 2026-10-08): only the combined x counts
        let angles = [0.11, -0.1, -0.276, -0.1, -0.083, -0.1];
        assert_eq!(choose_gaze(angles, [true; 2], true, true), [-0.083 + half, -0.1, -0.083 - half, -0.1, -0.083, -0.1]);
        // Each eye keeps its own up / down
        let angles = [0.0, 0.2, 0.0, 0.25, 0.0, 0.22];
        assert_eq!(choose_gaze(angles, [true; 2], true, true)[1..4], [0.2, -half, 0.25]);
        // Never past the end of the range
        assert_eq!(choose_gaze([1.0, 0.0, 1.0, 0.0, 1.0, 0.0], [true; 2], true, true)[0], 1.0);
        // Through the filters: the left - right sent stays the vergence whatever the per-eye readings do
        let settings = Settings {
            independent_eyes: true,
            gaze_deadzone: 0.0,
            ..settings()
        };
        let readings: Vec<EyeData> = (0..60)
            .map(|i| {
                let mut data = reading(i, [0.1, -0.05], [0.8; 2]);
                let wobble = if i % 20 < 10 { 0.3 } else { 0.0 };
                data.gaze = [[0.1 + wobble, -0.05, -1.0], [0.1 - wobble, -0.05, -1.0]];
                data
            })
            .collect();
        for sample in run(&settings, &readings) {
            let vergence = (sample.gaze[0] - sample.gaze[2]) * 45.0;
            assert!((vergence - VERGENCE_DEG).abs() < 1e-3, "{vergence}");
        }
    }

    fn scaled(left: f32, right: f32, up: f32, down: f32) -> Settings {
        Settings {
            gaze_scale_left: left,
            gaze_scale_right: right,
            gaze_scale_up: up,
            gaze_scale_down: down,
            ..settings()
        }
    }

    #[test]
    fn gaze_scale_at_one_changes_nothing() {
        // Bit for bit, whatever the values (a negative zero, past the range, NaN)
        let bits = |gaze: [f32; 6]| gaze.map(f32::to_bits);
        for gaze in [
            [0.1, -0.2, 0.3, -0.4, 0.2, -0.3],
            [-0.0, 0.0, -1.0, 1.0, -0.0, 0.0],
            [f32::NAN, 0.5, 2.0, -3.0, f32::NAN, -0.7],
        ] {
            assert_eq!(bits(scale_gaze(gaze, &settings())), bits(gaze));
            assert_eq!(bits(scale_gaze(gaze, &scaled(1.0, 1.0, 1.0, 1.0))), bits(gaze));
        }
    }

    #[test]
    fn each_gaze_scale_moves_only_its_own_half() {
        let close = |a: [f32; 6], b: [f32; 6]| a.iter().zip(&b).all(|(a, b)| (a - b).abs() < 1e-6);
        let gaze = |x: f32, y: f32| [x, y, x, y, x, y];
        // Left: only x below 0
        let left = scaled(0.5, 1.0, 1.0, 1.0);
        assert!(close(scale_gaze(gaze(-0.4, 0.2), &left), gaze(-0.2, 0.2)));
        assert_eq!(scale_gaze(gaze(0.4, -0.2), &left), gaze(0.4, -0.2));
        // Right: only x above 0
        let right = scaled(1.0, 1.5, 1.0, 1.0);
        assert!(close(scale_gaze(gaze(0.4, 0.2), &right), gaze(0.6, 0.2)));
        assert_eq!(scale_gaze(gaze(-0.4, 0.2), &right), gaze(-0.4, 0.2));
        // Up: only y above 0
        let up = scaled(1.0, 1.0, 0.25, 1.0);
        assert!(close(scale_gaze(gaze(0.1, 0.4), &up), gaze(0.1, 0.1)));
        assert_eq!(scale_gaze(gaze(0.1, -0.4), &up), gaze(0.1, -0.4));
        // Down: only y below 0
        let down = scaled(1.0, 1.0, 1.0, 1.2);
        assert!(close(scale_gaze(gaze(0.1, -0.5), &down), gaze(0.1, -0.6)));
        assert_eq!(scale_gaze(gaze(0.1, 0.5), &down), gaze(0.1, 0.5));
        // Straight ahead stays; never past the end of the range
        assert_eq!(scale_gaze(gaze(0.0, 0.0), &scaled(0.25, 1.5, 0.25, 1.5)), gaze(0.0, 0.0));
        assert_eq!(scale_gaze(gaze(0.8, -0.9), &scaled(1.0, 1.5, 1.0, 1.5)), gaze(1.0, -1.0));
        // The ends of the range: 5 % and 200 %
        assert!(close(scale_gaze(gaze(-0.4, 0.4), &scaled(0.05, 1.0, 2.0, 1.0)), gaze(-0.02, 0.8)));
        assert!(close(scale_gaze(gaze(0.4, -0.3), &scaled(1.0, 0.05, 1.0, 2.0)), gaze(0.02, -0.6)));
        assert_eq!(scale_gaze(gaze(0.6, -0.7), &scaled(1.0, 2.0, 1.0, 2.0)), gaze(1.0, -1.0));
        // Through the processing: settled on (-0.2, 0.1) on the -1..1 scale, left goes 1.5 times as far and up half
        // as far, in both eye behaviours, smoothed or raw
        let tan = |angle: f32| (angle * 45.0).to_radians().tan();
        let readings: Vec<EyeData> = (0..180).map(|i| reading(i, [tan(-0.2), tan(0.1)], [0.85; 2])).collect();
        for eye_behavior in [1, 2] {
            for raw in [false, true] {
                let base = Settings { eye_behavior, raw, ..settings() };
                let plain_sample = &run(&base, &readings)[179];
                let wider_sample = &run(&Settings { gaze_scale_left: 1.5, gaze_scale_up: 0.5, ..base }, &readings)[179];
                let [plain, wider] = [plain_sample.gaze, wider_sample.gaze];
                // The gaze before the scale (for the panel's dots) is what goes out without it
                assert_eq!(plain_sample.unscaled_gaze, plain);
                assert_eq!(wider_sample.unscaled_gaze, plain);
                assert!((wider[4] - plain[4] * 1.5).abs() < 1e-5, "{plain:?} {wider:?}");
                assert!((wider[5] - plain[5] * 0.5).abs() < 1e-6, "{plain:?} {wider:?}");
                assert!((plain[4] + 0.2).abs() < 1e-3 && (plain[5] - 0.1).abs() < 1e-3, "{plain:?}");
                assert_eq!(wider, gaze(wider[4], wider[5]));
            }
        }
    }

    #[test]
    fn gaze_scale_keeps_the_eyes_apart_and_the_eyelids_as_they_were() {
        let scale = |base: Settings| Settings {
            gaze_scale_left: 0.5,
            gaze_scale_right: 1.5,
            gaze_scale_up: 0.5,
            gaze_scale_down: 1.5,
            ..base
        };
        // v2 with --independent-eyes: the fixed vergence, looking left and up or right and down
        for [x, y] in [[-0.3f32, 0.2f32], [0.3, -0.2]] {
            let readings: Vec<EyeData> = (0..120).map(|i| reading(i, [x, y], [0.85; 2])).collect();
            let plain = run(&independent(), &readings);
            let sent = run(&scale(independent()), &readings);
            for (plain, sample) in plain.iter().zip(&sent) {
                assert!((sent_vergence(sample) - VERGENCE_DEG).abs() < 1e-3, "{}", sent_vergence(sample));
                assert_eq!(sample.lids, plain.lids);
            }
            assert!((sent[119].gaze[4] - plain[119].gaze[4]).abs() > 0.05);
        }
        // ...and an intentional cross-eye still shows as Valve's 30°, while looking a little left
        let vergence = |i: usize| if i < 90 { 0.0 } else { 30.0 };
        let readings: Vec<EyeData> = (0..300)
            .map(|i| {
                let mut data = crossed(i, vergence(i), [0.9; 2]);
                data.fixation_point[0] = -0.1;
                data
            })
            .collect();
        let sent = run(&scale(independent()), &readings);
        let last = &sent[299];
        assert!(last.cross_eye && (sent_vergence(last) - 30.0).abs() < 0.2, "{}", sent_vergence(last));
        // v1 with --independent-eyes: Valve's 10° apart stays 10° apart
        let v1 = Settings {
            eye_behavior: 1,
            ..independent()
        };
        let apart: Vec<EyeData> = (0..90)
            .map(|i| {
                let mut data = crossed(i, 10.0, [0.9; 2]);
                data.fixation_point[0] = 0.2;
                data
            })
            .collect();
        let plain = run(&v1, &apart);
        let sent = run(&scale(v1), &apart);
        assert!((sent_vergence(&sent[89]) - sent_vergence(&plain[89])).abs() < 1e-3);
        assert!((sent_vergence(&sent[89]) - 10.0).abs() < 0.05, "{}", sent_vergence(&sent[89]));
        assert!(sent[89].gaze[4] > plain[89].gaze[4] + 0.05);
        // Looking far down with an eye fit: the eyelids go by where the eyes really look, not the scaled gaze
        let fit = |settings: &mut Settings| {
            (settings.lid_fit_closed_left, settings.lid_fit_up_left) = (Some(0.15), Some(0.93));
            (settings.lid_fit_open_left, settings.lid_fit_down_left) = (Some(0.92), Some(0.6));
            (settings.lid_fit_closed_right, settings.lid_fit_up_right) = (Some(0.15), Some(0.93));
            (settings.lid_fit_open_right, settings.lid_fit_down_right) = (Some(0.92), Some(0.6));
        };
        let mut unscaled = settings();
        fit(&mut unscaled);
        let fitted = scale(unscaled.clone());
        let down: Vec<EyeData> = (0..120).map(|i| reading(i, [0.0, -0.4], [0.62; 2])).collect();
        let (a, b) = (run(&fitted, &down), run(&unscaled, &down));
        for (a, b) in a.iter().zip(&b) {
            assert_eq!(a.lids, b.lids);
        }
        assert!(a[119].gaze[5] < b[119].gaze[5] - 0.05);
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

        // With each eye on its own, the reliable eye moves both: the unreliable one follows it, still turned in by
        // the vergence, rather than being held where it was.
        let independent = Settings {
            independent_eyes: true,
            gaze_deadzone: 0.0,
            ..settings
        };
        readings[10].pre_fusion_covariance = [[1.0; 3], [0.0; 3]];
        let sent = run(&independent, &readings);
        assert!(sent[10].gaze.iter().step_by(2).zip(sent[9].gaze.iter().step_by(2)).all(|(now, before)| now < before));
        assert!(((sent[10].gaze[0] - sent[10].gaze[2]) * 45.0 - VERGENCE_DEG).abs() < 1e-3);
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
            step(&fitted(), &mut smoother, &mut calibration, &reading(i, [0.0, 0.0], [0.6, 0.6]), true, None);
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
    fn a_new_source_address_reconnects_and_no_route_keeps_the_socket() {
        let ip = |text: &str| text.parse::<IpAddr>().unwrap();
        let unreachable = || Err(io::Error::from(io::ErrorKind::NetworkUnreachable));
        // Wi-Fi to the Steam Link adapter: reconnect from the new address
        assert_eq!(moved_source(ip("192.168.1.23"), &Ok(ip("10.35.78.1"))), Some(ip("10.35.78.1")));
        // Same route, or none (the network is switching): keep the socket
        assert_eq!(moved_source(ip("192.168.1.23"), &Ok(ip("192.168.1.23"))), None);
        assert_eq!(moved_source(ip("192.168.1.23"), &unreachable()), None);
        assert_eq!(moved_source(ip("192.168.1.23"), &Ok(ip("0.0.0.0"))), None);
        assert_eq!(moved_source(ip("fe80::1"), &Ok(ip("fd00::2"))), Some(ip("fd00::2")));

        // The real route to the loopback starts from the loopback
        assert_eq!(route_source("127.0.0.1:9".parse().unwrap()).unwrap(), ip("127.0.0.1"));

        // Sending starting to fail or working again asks for a check at once; failing on, "refused" and drops don't
        let addr: SocketAddr = "192.0.2.1:9000".parse().unwrap();
        let mut output = Output::new(Target::Fixed { host: "192.0.2.1".into(), port: 9000 });
        let t0 = Instant::now();
        let error = |kind| Err(io::Error::from(kind));
        output.note(t0, addr, &Ok(20));
        output.note(t0, addr, &error(io::ErrorKind::ConnectionRefused));
        output.note(t0, addr, &error(io::ErrorKind::WouldBlock));
        assert!(!output.route_check_due);
        output.note(t0, addr, &error(io::ErrorKind::NetworkUnreachable));
        assert!(output.route_check_due);
        output.route_check_due = false;
        output.note(t0, addr, &error(io::ErrorKind::NetworkUnreachable));
        output.note(t0, addr, &error(io::ErrorKind::HostUnreachable));
        assert!(!output.route_check_due);
        output.note(t0, addr, &Ok(20));
        assert!(output.route_check_due);
    }

    // Linux routes all of 127/8 to the loopback, so a socket sending from 127.0.0.2 stands in for one left on an old
    // network's address.
    #[cfg(target_os = "linux")]
    #[test]
    fn a_settled_fixed_target_follows_a_new_source_address_without_another_look_up() {
        let listener = UdpSocket::bind("127.0.0.1:0").unwrap();
        listener.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
        let addr = listener.local_addr().unwrap();
        let loopback = |last: u8| IpAddr::from([127, 0, 0, last]);
        let local = |output: &Output| output.socket.as_ref().unwrap().0.local_addr().unwrap().ip();
        let mut output = Output::new(Target::Fixed { host: "127.0.0.1".into(), port: addr.port() });
        output.refresh();
        let resolved = output.last_resolve;
        assert!(resolved.is_some() && output.addr() == Some(addr));
        // The route is unchanged: the same socket stays. Asked for within ROUTE_CHECK_MIN of the last check, the check
        // waits
        output.route_check_due = true;
        output.refresh();
        assert_eq!(local(&output), loopback(1));
        assert!(output.route_check_due);

        let stale = UdpSocket::bind("127.0.0.2:0").unwrap();
        stale.connect(addr).unwrap();
        output.socket = Some((stale, addr));
        output.health.failing = Some("stale".into());
        // Not due yet: nothing happens
        output.refresh();
        assert_eq!(local(&output), loopback(2));
        // Due (RESOLVE_INTERVAL later, or a send failed): a new socket from the route's address, health starts over,
        // and the fixed host is not looked up again
        output.last_route_check = Some(Instant::now() - RESOLVE_INTERVAL);
        output.refresh();
        assert_eq!(local(&output), loopback(1));
        assert_eq!(output.addr(), Some(addr));
        assert_eq!(output.health.failing, None);
        assert_eq!(output.last_resolve, resolved);
        assert!(output.send("/x".into(), vec![OscType::Bool(true)]).is_ok());
        let mut buf = [0u8; 64];
        let (_, from) = listener.recv_from(&mut buf).unwrap();
        assert_eq!(from.ip(), loopback(1));
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
            unscaled_gaze: [0.1, 0.2, 0.3, 0.4, 0.5, 0.6],
            lids: [0.375, 0.75],
            reliable: [true; 2],
            gaze_held: false,
            camera: CameraValues::default(),
            lid_from: [LidFrom::EyeServer; 2],
            closed_overruled: [false; 2],
            cross_eye: false,
            close_held: [false; 2],
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
            feo_session: 7,
            feo_sequence: 0,
            feo_idle: None,
            pace: Pace::default(),
            camera: LiveReader::new(PathBuf::from("/nonexistent"), Instant::now()),
            camera_live: None,
            squint_out: [false; 2],
            // Never VRChat's port on this machine in a test: a test that wants the pupils points it at its own listener
            pupil_output: Output::new(Target::Fixed { host: "192.0.2.1".into(), port: 9 }),
            pupil_bits: PupilBits::default(),
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

    fn close(a: &[f32], b: &[f32]) -> bool {
        a.len() == b.len() && a.iter().zip(b).all(|(a, b)| (a - b).abs() < 1e-5)
    }

    fn native(settings: &Settings, sample: &Sample) -> Vec<(String, Vec<f32>)> {
        native_messages(settings, sample.gaze, sample.lids)
            .into_iter()
            .map(|(addr, args)| {
                let values = args.into_iter().map(|arg| match arg {
                    OscType::Float(value) => value,
                    other => panic!("not a float: {other:?}"),
                });
                (addr, values.collect())
            })
            .collect()
    }

    #[test]
    fn gaze_vector_undoes_gaze_angles() {
        // Frame space: -Z forward. Looking right and up.
        let frame = [0.3, 0.2, -0.9];
        let length = (0.3f32 * 0.3 + 0.2 * 0.2 + 0.9 * 0.9).sqrt();
        let unity = [0.3 / length, 0.2 / length, 0.9 / length];
        assert!(close(&gaze_vector(gaze_angles(frame)), &unity));
        assert!(close(&gaze_vector([0.0, 0.0]), &[0.0, 0.0, 1.0]));
        // 1.0 is 45° to the right.
        let half = std::f32::consts::FRAC_1_SQRT_2;
        assert!(close(&gaze_vector([1.0, 0.0]), &[half, 0.0, half]));
    }

    #[test]
    fn native_eyes_send_the_combined_gaze_and_one_eyelid() {
        let messages = native(&settings(), &sample());
        assert_eq!(messages.len(), 2);
        assert_eq!(messages[0].0, "/tracking/eye/CenterVec");
        assert!(close(&messages[0].1, &gaze_vector([0.5, 0.6])));
        assert_eq!(messages[1].0, "/tracking/eye/EyesClosedAmount");
        // Lids 0.375 and 0.75 (VRCFT) average to 0.5625, three quarters of relaxed open.
        assert!(close(&messages[1].1, &[0.25]));
    }

    #[test]
    fn native_eyes_send_each_eye_when_independent() {
        let independent = Settings {
            independent_eyes: true,
            ..settings()
        };
        let messages = native(&independent, &sample());
        assert_eq!(messages[0].0, "/tracking/eye/LeftRightVec");
        assert!(close(&messages[0].1, &[gaze_vector([0.1, 0.2]), gaze_vector([0.3, 0.4])].concat()));
    }

    #[test]
    fn native_eyelids_ignore_widening() {
        let with_lids = |lids| native(&settings(), &Sample { lids, ..sample() })[1].1.clone();
        assert!(close(&with_lids([1.0; 2]), &[0.0]));
        assert!(close(&with_lids([0.75; 2]), &[0.0]));
        assert!(close(&with_lids([0.0; 2]), &[1.0]));
    }

    #[test]
    fn native_eyes_go_out_only_in_vrchat_mode_when_turned_on() {
        let on = Settings {
            native_eyes: true,
            ..settings()
        };
        // Off by default: nothing changes for avatars that follow the VRCFT parameters
        assert!(native_stream(&settings()).is_none());
        assert_eq!(native_stream(&on), Some(("auto", 9000)));
        assert!(native_stream(&Settings { sending: false, ..on.clone() }).is_none());
        for output in [OutputKind::Etvr, OutputKind::LiveLink] {
            assert!(native_stream(&Settings { output, ..on.clone() }).is_none());
        }
        // Moving the stream ends the old one (it gets its own last neutral eyes)
        assert_ne!(native_stream(&on), native_stream(&Settings { port: Some(9001), ..on.clone() }));
        assert_eq!(native_stream(&on), native_stream(&Settings { lid_open: 0.85, ..on.clone() }));
    }

    #[test]
    fn losing_tracking_leaves_the_native_eyes_open_and_ahead() {
        let listener = UdpSocket::bind("127.0.0.1:0").unwrap();
        listener.set_read_timeout(Some(Duration::from_millis(500))).unwrap();
        let port = listener.local_addr().unwrap().port();
        let mut output = Output::new(Target::Fixed { host: "127.0.0.1".into(), port });
        output.refresh();
        send_native_neutral(&mut output, &settings()).unwrap();
        let receive = || {
            let mut buffer = [0u8; 256];
            let size = listener.recv(&mut buffer).unwrap();
            let (_, packet) = rosc::decoder::decode_udp(&buffer[..size]).unwrap();
            let OscPacket::Message(message) = packet else { panic!("not a message") };
            message
        };
        let look = receive();
        assert_eq!(look.addr, "/tracking/eye/CenterVec");
        assert_eq!(look.args, [OscType::Float(0.0), OscType::Float(0.0), OscType::Float(1.0)]);
        let lids = receive();
        assert_eq!(lids.addr, "/tracking/eye/EyesClosedAmount");
        assert_eq!(lids.args, [OscType::Float(0.0)]);
    }

    #[test]
    fn steamlink_names_go_out_only_in_vrchat_mode_when_turned_on() {
        let on = Settings {
            steamlink_params: true,
            ..settings()
        };
        assert!(steamlink_stream(&settings()).is_none());
        assert_eq!(steamlink_stream(&on), Some(("auto", 9000)));
        assert!(steamlink_stream(&Settings { sending: false, ..on.clone() }).is_none());
        for output in [OutputKind::Etvr, OutputKind::LiveLink] {
            assert!(steamlink_stream(&Settings { output, ..on.clone() }).is_none());
        }
        assert_ne!(steamlink_stream(&on), steamlink_stream(&Settings { port: Some(9001), ..on.clone() }));
    }

    #[test]
    fn losing_tracking_leaves_the_steamlink_eyes_open_and_ahead() {
        let listener = UdpSocket::bind("127.0.0.1:0").unwrap();
        listener.set_read_timeout(Some(Duration::from_millis(500))).unwrap();
        let port = listener.local_addr().unwrap().port();
        let mut output = Output::new(Target::Fixed { host: "127.0.0.1".into(), port });
        output.refresh();
        send_steamlink_neutral(&mut output).unwrap();
        let mut received = Vec::new();
        for _ in 0..12 {
            let mut buffer = [0u8; 256];
            let size = listener.recv(&mut buffer).unwrap();
            let (_, packet) = rosc::decoder::decode_udp(&buffer[..size]).unwrap();
            let OscPacket::Message(message) = packet else { panic!("not a message") };
            received.push((message.addr, message.args));
        }
        let value = |name: &str| {
            received
                .iter()
                .find(|(addr, _)| addr == &format!("/avatar/parameters/{name}"))
                .map(|(_, args)| args.clone())
                .unwrap()
        };
        for name in ["LeftEyeX", "LeftEyeY", "RightEyeX", "RightEyeY", "LeftEyeLid", "RightEyeLid"] {
            assert_eq!(value(name), [OscType::Float(0.0)], "{name}");
        }
        assert_eq!(value("LeftEyeSqueezeToggle"), [OscType::Int(0)]);
        assert_eq!(value("RightEyeLidExpandedSqueeze"), [OscType::Float(0.8)]);
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

    fn steamlink() -> Settings {
        Settings {
            steamlink_params: true,
            ..settings()
        }
    }

    #[test]
    fn steamlink_names_are_off_by_default() {
        assert!(!Settings::default().steamlink_params);
        let messages = sent(&settings());
        assert_eq!(messages.len(), 9);
        assert!(messages.iter().all(|(addr, _)| addr.starts_with("/avatar/parameters/FT/")));
    }

    #[test]
    fn steamlink_names_come_after_the_vrcft_ones_as_steam_link_sends_them() {
        let messages = sent(&steamlink());
        // The VRCFT ones are unchanged
        assert_eq!(messages[..9], sent(&settings())[..]);
        // sample(): gaze left (0.1, 0.2), right (0.3, 0.4); eyelids 0.375 (half closed) and 0.75 (relaxed open)
        let f = OscType::Float;
        let i = OscType::Int;
        let expected = [
            ("LeftEyeX", f(0.1)),
            ("LeftEyeY", f(-0.2)),
            ("RightEyeX", f(0.3)),
            ("RightEyeY", f(-0.4)),
            ("RightEyeLid", f(0.0)),
            ("RightEyeLidExpandedSqueeze", f(0.8)),
            ("RightEyeSqueezeToggle", i(0)),
            ("RightEyeWidenToggle", i(1)),
            ("LeftEyeLid", f(0.5)),
            ("LeftEyeLidExpandedSqueeze", f(0.8)),
            ("LeftEyeSqueezeToggle", i(0)),
            ("LeftEyeWidenToggle", i(1)),
        ]
        .map(|(name, value)| (format!("/avatar/parameters/{name}"), value));
        assert_eq!(messages[9..], expected);
        // On the wire: the toggles are int32, the rest float32, one value each
        for (addr, value) in &messages[9..] {
            let bytes = encoder::encode(&OscPacket::Message(OscMessage {
                addr: addr.clone(),
                args: vec![value.clone()],
            }))
            .unwrap();
            let tag = if addr.ends_with("Toggle") { b",i\0\0" } else { b",f\0\0" };
            assert!(bytes.windows(4).any(|window| window == tag), "{addr}");
        }
    }

    #[test]
    fn steamlink_eyelids_are_how_closed_the_eye_is() {
        // Relaxed open and widened: open; shut: 1; in between, evenly
        assert_eq!(steamlink_closed(0.75), 0.0);
        assert_eq!(steamlink_closed(1.0), 0.0);
        assert_eq!(steamlink_closed(0.0), 1.0);
        assert_eq!(steamlink_closed(0.375), 0.5);
        assert!((steamlink_closed(0.15) - 0.8).abs() < 1e-6);
        // Over half closed: squeezed, as Steam Link sends it
        let shut = Sample {
            lids: [0.15, 0.0],
            ..sample()
        };
        let messages = steamlink_messages(shut.gaze, shut.lids);
        let value = |name: &str| {
            messages
                .iter()
                .find(|(addr, _)| addr == &format!("/avatar/parameters/{name}"))
                .map(|(_, value)| value.clone())
                .unwrap()
        };
        assert_eq!(value("RightEyeLid"), OscType::Float(1.0));
        assert_eq!(value("RightEyeLidExpandedSqueeze"), OscType::Float(0.0));
        assert_eq!(value("RightEyeSqueezeToggle"), OscType::Int(1));
        assert_eq!(value("LeftEyeSqueezeToggle"), OscType::Int(1));
        assert_eq!(value("LeftEyeLidExpandedSqueeze"), OscType::Float(0.0));
        assert_eq!(value("LeftEyeWidenToggle"), OscType::Int(1));
    }

    #[test]
    fn steamlink_names_ignore_the_prefix_and_other_outputs() {
        for prefix in ["", "/Custom"] {
            let messages = sent(&Settings {
                prefix: prefix.into(),
                ..steamlink()
            });
            assert_eq!(messages[9].0, "/avatar/parameters/LeftEyeX");
            assert_eq!(messages[20].0, "/avatar/parameters/LeftEyeWidenToggle");
        }
        let etvr = Settings {
            output: OutputKind::Etvr,
            ..steamlink()
        };
        assert_eq!(sent(&etvr), sent(&Settings { output: OutputKind::Etvr, ..settings() }));
    }

    #[test]
    fn steamlink_names_go_out_with_each_sample() {
        // A real socket: one datagram per message, the Steam Link names among them
        let listener = UdpSocket::bind("127.0.0.1:0").unwrap();
        listener.set_read_timeout(Some(Duration::from_millis(500))).unwrap();
        let port = listener.local_addr().unwrap().port();
        let mut output = Output::new(Target::Fixed { host: "127.0.0.1".into(), port });
        output.refresh();
        for (addr, arg) in osc_messages(&steamlink(), &sample()) {
            output.send(addr, vec![arg]).unwrap();
        }
        let mut buffer = [0u8; 256];
        let mut received = Vec::new();
        while let Ok(size) = listener.recv(&mut buffer) {
            let (_, packet) = rosc::decoder::decode_udp(&buffer[..size]).unwrap();
            let OscPacket::Message(message) = packet else { panic!("not a message") };
            received.push((message.addr, message.args));
            if received.len() == 21 {
                break;
            }
        }
        assert_eq!(received.len(), 21);
        assert!(received.contains(&("/avatar/parameters/RightEyeY".into(), vec![OscType::Float(-0.4)])));
        assert!(received.contains(&("/avatar/parameters/LeftEyeSqueezeToggle".into(), vec![OscType::Int(0)])));
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
    fn status_keeps_the_last_error() {
        // The last problem logged, with its time, until another one replaces it
        let log = status::ErrorLog::new();
        assert_eq!(log.last(), None);
        let at = std::time::UNIX_EPOCH + Duration::from_millis(1_791_200_000_250);
        log.note("Can't send OSC to 192.168.0.60:9000 yet (Network is unreachable (os error 101))", at);
        log.note("No Steam Link connection found; waiting for one", at + Duration::from_secs(5));
        let last = log.last().unwrap();
        assert_eq!(last.text, "No Steam Link connection found; waiting for one");
        assert_eq!(last.time, 1_791_200_005.25);
        assert_eq!(
            serde_json::to_value(&last).unwrap(),
            serde_json::json!({"text": "No Steam Link connection found; waiting for one", "time": 1_791_200_005.25})
        );
        // status.json carries frameeyeosc's (null before the first)
        let bridge = test_bridge(settings());
        let json = serde_json::to_value(bridge.status(None, None)).unwrap();
        assert!(json.as_object().unwrap().contains_key("last_error"));
        status::log_error("Could not save /nowhere/calibration: No such file or directory (os error 2)");
        let json = serde_json::to_value(bridge.status(None, None)).unwrap();
        assert!(json["last_error"]["text"].is_string() && json["last_error"]["time"].as_f64().unwrap() > 1.7e9, "{json}");
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
                (process(settings, smoother, [1.0; 2], &data, None).lids, smoother.saturation.on)
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
    fn widening_is_capped_once_saturated() {
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
        // And it stays capped once the minute reads below 1.000 again: the share drifts with where you look
        let back = lids_over(&settings, &mut smoother, 120.0, 70.0, |_| [0.995, 0.995]);
        assert!(back.iter().all(|(lids, saturated)| *saturated && lids.iter().all(|lid| *lid <= LID_RELAXED)));
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
        // Once on it stays on, however low the share drifts as the old samples leave the minute (looking down a while)
        feed(&mut one_eye, 20.0, 120.0, |i| if i % 20 < 9 { [1.0, 1.0] } else { [0.8, 0.8] });
        assert!(one_eye.on, "45% saturated");
        feed(&mut one_eye, 140.0, 61.0, |i| if i % 10 < 1 { [1.0, 1.0] } else { [0.8, 0.8] });
        assert!(one_eye.on, "10% saturated");
        // A clock that starts over starts the count over, and it stays on through that too
        feed(&mut one_eye, 5.0, 5.0, |_| [0.8, 0.8]);
        assert!(one_eye.on && one_eye.open == 450, "{}", one_eye.open);
        // Off until more than half the minute reads 1.000
        let mut fresh = Saturation::default();
        feed(&mut fresh, 0.0, 61.0, |i| if i % 20 < 9 { [1.0, 1.0] } else { [0.8, 0.8] });
        assert!(!fresh.on, "45% saturated");
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

    /// eyecam-rec's values as read: both eyes fresh and calibrated for this wear, at eyelid `lid` with `squint`.
    pub(crate) fn camera(lid: f32, squint: f32) -> Live {
        let eye = eyecam_live::Eye {
            valid: true,
            lid,
            squint,
            pupil_mm: 4.0,
            pupil_dilation: 0.5,
            ..Default::default()
        };
        Live {
            calib_state: 1,
            recalib_suggested: false,
            live: true,
            eyes: [eye; 2],
            fresh: [true; 2],
            frame_ns: [0; 2],
        }
    }

    /// The samples for `seconds` of 90 Hz eye-server samples from `start` looking straight ahead, with each eye's
    /// openness from `openness(sample index)` and the eye camera's values from `camera(sample index)`.
    fn camera_samples(
        settings: &Settings,
        smoother: &mut Smoother,
        start: f64,
        seconds: f64,
        openness: impl Fn(usize) -> [f32; 2],
        camera: impl Fn(usize) -> Option<Live>,
    ) -> Vec<Sample> {
        (0..(seconds * 90.0) as usize)
            .map(|i| {
                let data = EyeData {
                    sample_time: start + i as f64 / 90.0,
                    gaze: [[0.0, 0.0, -1.0]; 2],
                    fixation_point: [0.0, 0.0, -1.0],
                    openness: openness(i),
                    ..EyeData::default()
                };
                process(settings, smoother, [1.0; 2], &data, camera(i).as_ref())
            })
            .collect()
    }

    /// A smoother whose openness already counts as saturated, as on SteamOS 0.4.3.
    fn saturated_smoother(settings: &Settings) -> Smoother {
        let mut smoother = Smoother::new(settings);
        smoother.saturation.on = true;
        smoother
    }

    fn all_relaxed_or_below(samples: &[Sample]) -> bool {
        samples.iter().all(|sample| sample.lids.iter().all(|lid| *lid <= LID_RELAXED))
    }

    #[test]
    fn the_cameras_widening_passes_the_saturation_cap() {
        let settings = Settings {
            lid_calibration: false,
            ..settings()
        };
        // The eye server reads relaxed open as 1.000; the camera sees the eyes widened
        let mut smoother = saturated_smoother(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 2.0, |_| [1.0; 2], |_| Some(camera(0.95, 0.0)));
        // Once it has lasted WIDEN_SUSTAIN (22.5 samples), like any widening
        assert!(all_relaxed_or_below(&sent[..23]), "{:?}", sent[22].lids);
        assert!(sent[24].lids.iter().all(|lid| *lid > LID_RELAXED), "{:?}", sent[24].lids);
        let last = sent.last().unwrap().lids;
        assert!(last.iter().all(|lid| (lid - 0.95).abs() < 0.005), "{last:?}");
        // Without the camera, without its wear calibration, or with camera_lids off: capped as before
        let uncalibrated = Live {
            calib_state: 2,
            ..camera(0.95, 0.0)
        };
        let off = Settings {
            camera_lids: false,
            ..settings.clone()
        };
        for (settings, camera) in [(&settings, None), (&settings, Some(uncalibrated)), (&off, Some(camera(0.95, 0.0)))] {
            let mut smoother = saturated_smoother(settings);
            let sent = camera_samples(settings, &mut smoother, 100.0, 2.0, |_| [1.0; 2], |_| camera);
            assert!(all_relaxed_or_below(&sent), "{camera:?}");
            assert_eq!(sent.last().unwrap().lids, [LID_RELAXED; 2]);
        }
        // Not saturated (before 0.4.3), the camera's eyelid wins over the eye server's from relaxed open up too
        let mut smoother = Smoother::new(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 2.0, |_| [1.0; 2], |_| Some(camera(0.75, 0.0)));
        assert!(!smoother.saturation.on);
        assert_eq!(sent.last().unwrap().lids, [LID_RELAXED; 2]);
        // The camera's eyelid stays between relaxed open and fully widened
        let mut smoother = saturated_smoother(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 2.0, |_| [1.0; 2], |_| Some(camera(1.4, 0.0)));
        assert!(sent.iter().all(|sample| sample.lids.iter().all(|lid| *lid <= 1.0)));
        // Held at 1.000 for long (no jump there), the camera's narrowed eyelid doesn't lower it either
        let sent = camera_samples(&settings, &mut smoother, 102.0, 1.0, |_| [1.0; 2], |_| Some(camera(0.3, 0.0)));
        assert!(sent.last().unwrap().lids.iter().all(|lid| (lid - LID_RELAXED).abs() < 1e-4));
        assert_eq!(sent.last().unwrap().lid_from, [LidFrom::EyeServer; 2]);
    }

    #[test]
    fn closing_stays_the_eye_servers() {
        let settings = Settings {
            lid_calibration: false,
            ..settings()
        };
        let mut smoother = saturated_smoother(&settings);
        // The camera says widened: a blink still comes from the eye server, and so does a half-closed eye the camera
        // sees narrowed further (its eyelid only ever raises the eye server's; see raised_to_camera)
        let sent = camera_samples(
            &settings,
            &mut smoother,
            100.0,
            3.0,
            |i| match i {
                90..=94 => [0.1, 0.1],
                180.. => [0.55, 0.55],
                _ => [1.0, 1.0],
            },
            |i| Some(camera(if i >= 180 { 0.3 } else { 1.0 }, 0.0)),
        );
        assert!(sent[89].lids.iter().all(|lid| *lid > 0.95), "{:?}", sent[89].lids);
        // (one sample late, through the 3-sample median)
        assert_eq!(sent[91].lids, [0.0; 2]);
        let half = lid_to_vrcft(0.55, &settings);
        assert!((sent.last().unwrap().lids[0] - half).abs() < 0.01, "{:?} {half}", sent.last().unwrap().lids);
        // --raw: the same mix, unsmoothed
        let raw = Settings { raw: true, ..settings.clone() };
        let mut smoother = Smoother::new(&raw);
        let openness = |i: usize| if i < 4 { [1.0; 2] } else { [0.55, 1.0] };
        let cameras = |i: usize| Some(if i < 4 { camera(0.9, 0.0) } else { camera_eyes([(0.3, 0.0, false), (0.9, 0.0, false)]) });
        let sent = camera_samples(&raw, &mut smoother, 100.0, 0.1, openness, cameras);
        assert_eq!(sent[0].lids, [0.9; 2]);
        // A wink-sized difference, so lid_sync leaves both as they are (the right's widening halved, the left's camera
        // seeing none; see share_widening)
        assert_eq!(sent[5].lids, [half, 0.825]);
    }

    /// An eye fit on SteamOS 0.4.3, where an open eye reads 1.000 (a little less looking down).
    fn saturated_fit() -> Settings {
        Settings {
            lid_calibration: false,
            lid_fit_closed_left: Some(0.2),
            lid_fit_up_left: Some(1.0),
            lid_fit_open_left: Some(1.0),
            lid_fit_down_left: Some(0.9),
            lid_fit_closed_right: Some(0.2),
            lid_fit_up_right: Some(1.0),
            lid_fit_open_right: Some(1.0),
            lid_fit_down_right: Some(0.9),
            ..settings()
        }
    }

    /// Like camera_samples, with the eyes looking `down_deg` down.
    fn camera_samples_down(
        settings: &Settings,
        down_deg: f32,
        seconds: f64,
        openness: impl Fn(usize) -> [f32; 2],
        camera: impl Fn(usize) -> Option<Live>,
    ) -> Vec<Sample> {
        let mut smoother = Smoother::new(settings);
        let direction = [0.0, -down_deg.to_radians().tan(), -1.0];
        (0..(seconds * 90.0) as usize)
            .map(|i| {
                let data = EyeData {
                    sample_time: 100.0 + i as f64 / 90.0,
                    gaze: [direction; 2],
                    fixation_point: direction,
                    openness: openness(i),
                    ..EyeData::default()
                };
                process(settings, &mut smoother, [1.0; 2], &data, camera(i).as_ref())
            })
            .collect()
    }

    #[test]
    fn an_open_eye_the_eye_server_reads_low_goes_out_as_the_camera_sees_it() {
        let settings = saturated_fit();
        let near = |a: f32, b: f32| (a - b).abs() < 1e-3;
        // Straight ahead, read 0.85 (VRCFT 0.55 through the fit) while the camera sees it relaxed open: relaxed open
        let low = lid_to_vrcft(lid_inputs([0.85; 2], 0.0, [1.0; 2], &settings)[0], &settings);
        assert!(low < 0.6, "{low}");
        let sent = camera_samples_down(&settings, 0.0, 2.0, |_| [0.85; 2], |_| Some(camera(0.75, 0.0)));
        let last = sent.last().unwrap();
        assert!(last.lids.iter().all(|lid| near(*lid, 0.75)), "{:?}", last.lids);
        assert_eq!(last.lid_from, [LidFrom::CameraRaised; 2]);
        // 24° down, read 0.66 (in either eye or both): relaxed open too
        for openness in [[0.66, 0.66], [0.66, 1.0], [1.0, 0.66]] {
            let sent = camera_samples_down(&settings, 24.0, 2.0, |_| openness, |_| Some(camera(0.75, 0.0)));
            let last = sent.last().unwrap();
            assert!(last.lids.iter().all(|lid| near(*lid, 0.75)), "{openness:?}: {:?}", last.lids);
        }
        // The camera sees it less open than that: no higher than the camera
        let sent = camera_samples_down(&settings, 0.0, 2.0, |_| [0.85; 2], |_| Some(camera(0.65, 0.0)));
        assert!(sent.last().unwrap().lids.iter().all(|lid| near(*lid, 0.65)), "{:?}", sent.last().unwrap().lids);
        // Widened on the camera: relaxed open, and of the camera's widening as much as the eye server's own eyelid
        // (0.55, halfway through VALVE_WIDEN_FADE) lets through, as without the raising
        let sent = camera_samples_down(&settings, 0.0, 2.0, |_| [0.85; 2], |_| Some(camera(1.0, 0.0)));
        let half = mix_lid_by(0.75, low, Some(1.0));
        assert!((half - 0.75 - 0.25 * camera_widen_share(low, 1.0)).abs() < 1e-6 && half > 0.85 && half < 0.9);
        assert!(sent.last().unwrap().lids.iter().all(|lid| (lid - half).abs() < 1e-3), "{:?} {half}", sent.last().unwrap().lids);
        assert_eq!(sent.last().unwrap().lid_from, [LidFrom::CameraRaised; 2]);
        // --raw: the same, unsmoothed
        let raw = Settings { raw: true, ..settings.clone() };
        let sent = camera_samples_down(&raw, 0.0, 0.1, |_| [0.85; 2], |_| Some(camera(0.75, 0.0)));
        assert_eq!((sent[0].lids, sent[0].lid_from), ([0.75; 2], [LidFrom::CameraRaised; 2]));
    }

    #[test]
    fn the_camera_only_ever_raises_an_open_eyelid() {
        let line = doubt_line(&settings());
        // Only ever up, to the camera's eyelid and no further than relaxed open
        assert_eq!(raised_to_camera(0.6, 0.45, line), (0.6, false));
        assert_eq!(raised_to_camera(0.4, 0.45, line), (0.45, true));
        assert_eq!(raised_to_camera(0.6, 0.9, line), (LID_RELAXED, true));
        // Below the doubt line (closing, a blink) and from relaxed open up: untouched
        assert_eq!(raised_to_camera(line - 0.01, 0.75, line), (line - 0.01, false));
        assert_eq!(raised_to_camera(0.0, 0.75, line), (0.0, false));
        assert_eq!(raised_to_camera(0.8, 0.75, line), (0.8, false));
        // A squint the camera sees (0.45) where the eye server reads VRCFT 0.6 (0.7 with the default marks): 0.6
        let settings = Settings {
            lid_sync: 0.0,
            ..camera_settings()
        };
        assert!((lid_to_vrcft(0.7, &settings) - 0.6).abs() < 1e-6);
        let mut smoother = Smoother::new(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 2.0, |_| [0.7, 0.8], |_| {
            Some(camera_eyes([(0.45, 0.6, false), (0.75, 0.0, false)]))
        });
        let last = sent.last().unwrap();
        assert!((last.lids[0] - 0.6).abs() < 1e-3 && (last.lids[1] - 0.75).abs() < 1e-3, "{:?}", last.lids);
        assert_eq!(last.lid_from, [LidFrom::EyeServer; 2]);
    }

    #[test]
    fn blinks_and_winks_still_close_with_the_camera_raising_open_eyelids() {
        let settings = saturated_fit();
        // Read 0.85 (raised to relaxed open), and a 200 ms blink in both eyes the camera does not see
        let sent = camera_samples_down(
            &settings,
            0.0,
            2.0,
            |i| if (90..108).contains(&i) { [0.2; 2] } else { [0.85; 2] },
            |_| Some(camera(0.75, 0.0)),
        );
        assert!((sent[89].lids[0] - 0.75).abs() < 1e-3, "{:?}", sent[89].lids);
        assert!(sent[91..109].iter().all(|sample| sample.lids == [0.0; 2]), "{:?}", sent[100].lids);
        assert!((sent.last().unwrap().lids[0] - 0.75).abs() < 1e-3);
        // A second's wink of the left eye, the camera seeing it closed: the left closed, the right relaxed open
        let sent = camera_samples_down(
            &settings,
            0.0,
            3.0,
            |i| if (90..180).contains(&i) { [0.2, 0.85] } else { [0.85; 2] },
            |i| {
                let left = if (90..180).contains(&i) { (0.0, 0.0, true) } else { (0.75, 0.0, false) };
                Some(camera_eyes([left, (0.75, 0.0, false)]))
            },
        );
        assert!(sent[95..180].iter().all(|sample| sample.lids[0] == 0.0), "{:?}", sent[95].lids);
        assert!(sent[130..180].iter().all(|sample| (sample.lids[1] - 0.75).abs() < 1e-3), "{:?}", sent[130].lids);
    }

    #[test]
    fn without_the_camera_eyelids_near_relaxed_open_count_as_open() {
        let settings = saturated_fit();
        let all = [true; 2];
        let snapped = |lid: f32| snapped_open([lid; 2], all, &settings)[0];
        // 0.53 by default: below it unchanged, from halfway to 0.75 relaxed open, smooth and never falling between
        assert_eq!(settings.lid_open_snap, 0.53);
        for lid in [0.0, 0.25, 0.5, 0.53] {
            assert_eq!(snapped(lid), lid);
        }
        for lid in [0.645, 0.7, 0.75] {
            assert_eq!(snapped(lid), 0.75);
        }
        assert_eq!(snapped(0.9), 0.9);
        let mut last = snapped(0.52);
        for step in 0..=200 {
            let lid = 0.52 + step as f32 * 0.001;
            let value = snapped(lid);
            assert!(value >= last - 1e-6 && value >= lid - 1e-6 && value - last < 0.01, "{lid}: {last} -> {value}");
            last = value;
        }
        // The same slope as below at the start, flat at the end
        assert!(((snapped(0.531) - snapped(0.53)) / 0.001 - 1.0).abs() < 0.06);
        assert!((snapped(0.64) - snapped(0.639)) / 0.001 < 0.06);
        // For a fitted eye straight ahead it is the zone it started as, 0.80 to 0.90 of the open reading (a fit 1.000
        // open and 0.000 shut, so it starts at 0.536)
        let zero = Settings { lid_fit_closed_left: Some(0.0), lid_fit_closed_right: Some(0.0), ..settings.clone() };
        let vrcft = |reading: f32| lid_to_vrcft(lid_inputs([reading; 2], 0.0, [1.0; 2], &zero)[0], &zero);
        let exact = Settings { lid_open_snap: vrcft(0.8), ..zero.clone() };
        for step in 0..=100 {
            let reading = 0.75 + step as f32 * 0.0025;
            let from = 0.8;
            let zone = if reading <= from {
                reading
            } else if reading >= 0.9 {
                1.0
            } else {
                let t = (reading - from) / 0.1;
                let (t2, t3) = (t * t, t * t * t);
                (2.0 * t3 - 3.0 * t2 + 1.0) * from + (t3 - 2.0 * t2 + t) * 0.1 + (3.0 * t2 - 2.0 * t3) * 1.0
            };
            let now = snapped_open([vrcft(reading); 2], all, &exact)[0];
            assert!((now - vrcft(zone)).abs() < 1e-4, "{reading}: {now} vs {}", vrcft(zone));
        }
        // Read 0.85 (VRCFT 0.55): between that and relaxed open; 0.7 (0.39) as before
        assert!(vrcft(0.85) < 0.6 && snapped(vrcft(0.85)) > vrcft(0.85) + 0.05 && snapped(vrcft(0.85)) < 0.75);
        assert_eq!(snapped(vrcft(0.7)), vrcft(0.7));
        // Eyes with the camera's eyelid are left alone, each eye by itself
        assert_eq!(snapped_open([0.6; 2], [false, true], &settings), [0.6, snapped(0.6)]);
        // Where it starts follows --lid-open-snap; 0.75 turns it off
        for snap in [0.0, 0.3, 0.6, 0.7] {
            let moved = Settings { lid_open_snap: snap, ..settings.clone() };
            let at = |lid: f32| snapped_open([lid; 2], all, &moved)[0];
            let end = snap + (0.75 - snap) / 2.0;
            assert_eq!((at(snap), at(end), at(0.75)), (snap, 0.75, 0.75), "{snap}");
            let mid = (snap + end) / 2.0;
            assert!(at(mid) > mid && at(mid) < 0.75, "{snap}");
        }
        let off = Settings { lid_open_snap: 0.75, ..settings.clone() };
        for lid in [0.0, 0.5, 0.6, 0.7, 0.74, 0.75, 0.9] {
            assert_eq!(snapped_open([lid; 2], all, &off), [lid; 2]);
        }
        // Through process: no camera, read 0.85 straight ahead
        let mut smoother = Smoother::new(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 2.0, |_| [0.85; 2], |_| None);
        let expected = snapped(lid_to_vrcft(lid_inputs([0.85; 2], 0.0, [1.0; 2], &settings)[0], &settings));
        assert!((sent.last().unwrap().lids[0] - expected).abs() < 1e-3, "{:?} {expected}", sent.last().unwrap().lids);
        // A fitted eye's scale applies after it: an eye scaled to 0.9 that reads relaxed open goes out as before
        // (less open), and one read low eases up and is then scaled
        let fits = settings.lid_fit();
        let scaled = |reading: f32, scale: f32, zone: bool| {
            snapped_lids([reading; 2], 0.0, [1.0, scale], &settings, fits, [zone; 2])[1]
        };
        let plain = |reading: f32, scale: f32| lid_to_vrcft(lid_inputs([reading; 2], 0.0, [1.0, scale], &settings)[1], &settings);
        for reading in [0.3, 0.5, 0.7, 1.0, 1.1] {
            for scale in [0.8, 0.9, 1.0, 1.1] {
                if lid_to_vrcft(lid_inputs([reading; 2], 0.0, [1.0; 2], &settings)[1], &settings) <= 0.53 || reading >= 1.0 {
                    assert_eq!(scaled(reading, scale, true), plain(reading, scale), "{reading} {scale}");
                }
                assert_eq!(scaled(reading, scale, false), plain(reading, scale), "{reading} {scale}");
            }
        }
        assert!((scaled(1.0, 0.9, true) - plain(1.0, 0.9)).abs() < 1e-6 && plain(1.0, 0.9) < 0.7);
        assert!(scaled(0.88, 0.9, true) > plain(0.88, 0.9) + 0.05 && scaled(0.88, 0.9, true) <= scaled(1.0, 0.9, true));
        assert!((scaled(0.88, 1.0, true) - snapped(plain(0.88, 1.0))).abs() < 1e-5);
        // Never falling as the reading rises, with any scale
        for scale in [0.8, 0.9, 1.1] {
            let mut last = 0.0;
            for step in 0..=300 {
                let value = scaled(0.5 + step as f32 * 0.002, scale, true);
                assert!(value >= last - 1e-6, "{scale} {step}: {last} -> {value}");
                last = value;
            }
        }
        // Without a fit too, on the lid marks
        let unfitted = camera_settings();
        let mut smoother = Smoother::new(&unfitted);
        let sent = camera_samples(&unfitted, &mut smoother, 100.0, 2.0, |_| [0.75; 2], |_| None);
        assert!((sent.last().unwrap().lids[0] - 0.75).abs() < 1e-3, "{:?}", sent.last().unwrap().lids);
    }

    #[test]
    fn far_down_without_the_camera_less_open_is_expected() {
        let settings = saturated_fit();
        let degrees = |deg: f32| -deg / 45.0;
        // Up to 17° down the fit as measured; from there, for eyes without the camera's eyelid, the expected open
        // reading falls 0.02 a degree from the reading 15° down (0.9 here), no lower than half the straight-ahead one
        let fits = settings.lid_fit();
        for deg in [-20.0, 0.0, 10.0, 15.0, 16.9] {
            assert_eq!(fits_at(&settings, degrees(deg), [true; 2]), fits, "{deg}");
        }
        let expected = |deg: f32, extended: bool| expected_open(&fits_at(&settings, degrees(deg), [extended; 2])[0].unwrap(), degrees(deg));
        assert!((expected(24.0, true) - 0.76).abs() < 1e-5, "{}", expected(24.0, true));
        assert!((expected(40.0, true) - 0.5).abs() < 1e-5, "{}", expected(40.0, true));
        assert_eq!(expected(24.0, false), 0.9);
        assert_eq!(fits_at(&settings, degrees(24.0), [false, true])[0], fits[0]);
        // 24° down, read 0.82 without the camera: relaxed open (was half closed)
        let sent = camera_samples_down(&settings, 24.0, 2.0, |_| [0.82; 2], |_| None);
        assert!(sent.last().unwrap().lids.iter().all(|lid| (lid - 0.75).abs() < 1e-3), "{:?}", sent.last().unwrap().lids);
        let before = lid_to_vrcft(lid_inputs([0.82; 2], degrees(24.0), [1.0; 2], &settings)[0], &settings);
        assert!(before < 0.65, "{before}");
        // The author's fit (1.000 open looking up, ahead and down): 0.86 is expected 24° down, so 0.82 is nearly open
        let author = Settings {
            lid_fit_closed_left: Some(0.003),
            lid_fit_down_left: Some(1.0),
            lid_fit_closed_right: Some(0.046),
            lid_fit_down_right: Some(1.0),
            ..settings.clone()
        };
        let sent = camera_samples_down(&author, 24.0, 2.0, |_| [0.82; 2], |_| None);
        let before = lid_to_vrcft(lid_inputs([0.82; 2], degrees(24.0), [1.0; 2], &author)[0], &author);
        assert!(sent.last().unwrap().lids.iter().all(|lid| *lid > 0.7) && before < 0.6, "{:?} {before}", sent.last().unwrap().lids);
        // Up to 15° down nothing changes: the same eyelids with the snap off as without this
        let off = Settings { lid_open_snap: LID_RELAXED, ..settings.clone() };
        for deg in [0.0, 10.0, 15.0] {
            let sent = camera_samples_down(&off, deg, 1.0, |_| [0.7; 2], |_| None);
            let plain = lid_inputs([0.7; 2], degrees(deg), [1.0; 2], &off).map(|value| lid_to_vrcft(value, &off));
            let last = sent.last().unwrap().lids;
            assert!((last[0] - plain[0]).abs() < 1e-4 && (last[1] - plain[1]).abs() < 1e-4, "{deg}: {last:?} {plain:?}");
        }
    }

    /// The camera's values with each eye's eyelid, squint and closed flag as given.
    fn camera_eyes(eyes: [(f32, f32, bool); 2]) -> Live {
        let mut live = camera(0.75, 0.0);
        for (eye, (lid, squint, closed)) in live.eyes.iter_mut().zip(eyes) {
            (eye.lid, eye.squint, eye.closed) = (lid, squint, closed);
        }
        live
    }

    fn camera_settings() -> Settings {
        Settings {
            lid_calibration: false,
            ..settings()
        }
    }

    /// Two seconds of samples with the camera's "closed" per eye from `closed(sample index)` (eyelid 0 then, else
    /// relaxed open) and the eye tracker's openness from `openness`.
    fn closed_run(
        settings: &Settings,
        closed: impl Fn(usize) -> [bool; 2],
        openness: impl Fn(usize) -> [f32; 2],
    ) -> Vec<Sample> {
        let mut smoother = Smoother::new(settings);
        camera_samples(settings, &mut smoother, 100.0, 2.0, openness, |i| {
            let [left, right] = closed(i).map(|closed| (if closed { 0.0 } else { 0.75 }, 0.0, closed));
            Some(camera_eyes([left, right]))
        })
    }

    #[test]
    fn the_cameras_closed_in_one_eye_waits_until_it_is_a_blink_or_a_wink() {
        let settings = camera_settings();
        let open = |_: usize| [1.0; 2];
        // The right eye's camera says closed for 5 samples while the left one sees its eye open and the eye tracker
        // reads both open: held back, the right eye stays open
        let flicker = |i: usize| [false, (40..45).contains(&i)];
        let sent = closed_run(&settings, flicker, open);
        assert!(sent.iter().all(|sample| sample.lids[1] > 0.7), "{:?}", sent[43].lids);
        assert!(sent[40..45].iter().all(|sample| sample.close_held == [false, true]));
        assert!(!sent[45].close_held[1] && !sent[39].close_held[1]);
        // As before without the gate: closed at once
        let ungated = Settings { camera_close_gate: false, ..settings.clone() };
        let sent = closed_run(&ungated, flicker, open);
        assert_eq!(sent[41].lids[1], 0.0);
        assert!(sent.iter().all(|sample| sample.close_held == [false; 2]));
        // Both cameras closed (a blink), also two samples apart: both eyes close
        let sent = closed_run(&settings, |i| [(40..55).contains(&i), (40..55).contains(&i)], open);
        assert_eq!(sent[41].lids, [0.0; 2]);
        let sent = closed_run(&settings, |i| [(40..55).contains(&i), (42..57).contains(&i)], open);
        assert_eq!(sent[40].close_held, [true, false]);
        assert_eq!(sent[43].lids, [0.0; 2]);
        // ...and once let through, a closing stays through to its end, also after the other eye has opened
        assert_eq!(sent[56].lids[1], 0.0);
        // The eye tracker reads the other eye closing (it caught that eye of the blink, the camera this one): a blink
        let sent = closed_run(&settings, flicker, |i| if (38..47).contains(&i) { [0.4, 1.0] } else { [1.0; 2] });
        let closed = |sample: &Sample| sample.lids[1] == 0.0 && !sample.close_held[1];
        assert!(sent[40..45].iter().all(closed), "{:?}", sent[41].lids);
        // A wink, which the eye tracker reads open all along (SteamOS 0.4.3): closed from CLOSE_WINK_S (18 samples) on,
        // the other eye open (shorter than CAMERA_CLOSED_DOUBT_S: these samples have the eye server sure of the eye,
        // and a longer one is SideCheck's to doubt)
        let wink = |i: usize| [false, (40..84).contains(&i)];
        let sent = closed_run(&settings, wink, open);
        assert!(sent[40..57].iter().all(|sample| sample.lids[1] > 0.1 && sample.close_held[1]), "{:?}", sent[50].lids);
        let winked = |sample: &Sample| sample.lids[1] == 0.0 && sample.lids[0] > 0.7;
        assert!(sent[59..84].iter().all(winked), "{:?}", sent[60].lids);
        // Looking 25 degrees down both cameras read closed: held back, the eyes stay open...
        let both = |i: usize| [(40..130).contains(&i); 2];
        let down = camera_samples_down(&settings, 25.0, 2.0, open, |i| {
            let [left, right] = both(i).map(|closed| (if closed { 0.0 } else { 0.75 }, 0.0, closed));
            Some(camera_eyes([left, right]))
        });
        // (from CAMERA_CLOSED_DOUBT_S on, with the eye server sure of open eyes, SideCheck takes them as open first)
        let held_open = |sample: &Sample| {
            (sample.close_held == [true; 2] || sample.closed_overruled == [true; 2])
                && sample.lids.iter().all(|lid| *lid > 0.5)
        };
        assert!(down[40..130].iter().all(held_open), "{:?}", down[60].lids);
        assert!(down[40..80].iter().all(|sample| sample.close_held == [true; 2]));
        // ...unless the eye tracker reads an eye closing: a blink while looking down closes
        let shut = |i: usize| if (40..55).contains(&i) { [0.3; 2] } else { [1.0; 2] };
        let blink_down = camera_samples_down(&settings, 25.0, 2.0, shut, |i| {
            let closed = (40..55).contains(&i);
            Some(camera_eyes([(if closed { 0.0 } else { 0.75 }, 0.0, closed); 2]))
        });
        assert!(blink_down[42..55].iter().all(|sample| sample.lids == [0.0; 2]), "{:?}", blink_down[45].lids);
        // 10 degrees down is not far enough for that: a blink of both cameras closes
        let blink_10 = camera_samples_down(&settings, 10.0, 2.0, open, |i| {
            let closed = (40..55).contains(&i);
            Some(camera_eyes([(if closed { 0.0 } else { 0.75 }, 0.0, closed); 2]))
        });
        assert!(blink_10[41..55].iter().all(|sample| sample.lids == [0.0; 2]), "{:?}", blink_10[45].lids);
        // Without the other eye's camera values there is nothing to pair with: closed at once, as before
        let mut smoother = Smoother::new(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 1.0, open, |i| {
            let closed = (40..45).contains(&i);
            let mut live = camera_eyes([(0.75, 0.0, false), (if closed { 0.0 } else { 0.75 }, 0.0, closed)]);
            live.fresh[0] = false;
            Some(live)
        });
        assert_eq!(sent[41].lids[1], 0.0);
    }

    #[test]
    fn a_blink_the_camera_sees_open_still_closes() {
        let settings = camera_settings();
        let mut smoother = Smoother::new(&settings);
        // 200 ms shut on the eye server, while the camera sees both eyes open and not closed all along
        let sent = camera_samples(
            &settings,
            &mut smoother,
            100.0,
            1.0,
            |i| if (45..63).contains(&i) { [0.2; 2] } else { [0.8; 2] },
            |_| Some(camera(0.75, 0.0)),
        );
        // (one sample late, through the 3-sample median)
        assert!(sent[46..64].iter().all(|sample| sample.lids == [0.0; 2]), "{:?}", sent[60].lids);
        assert!(sent.iter().all(|sample| sample.lid_from == [LidFrom::EyeServer; 2]));
        // One eye caught by the eye server, the other only half closing: the blink sync still closes both
        let mut smoother = Smoother::new(&settings);
        let sent = camera_samples(
            &settings,
            &mut smoother,
            100.0,
            1.0,
            |i| if (45..63).contains(&i) { [0.2, 0.4] } else { [0.8; 2] },
            |_| Some(camera(0.75, 0.0)),
        );
        assert!(sent[46..64].iter().all(|sample| sample.lids == [0.0; 2]), "{:?}", sent[60].lids);
    }

    #[test]
    fn an_eye_the_camera_sees_open_for_longer_than_a_blink_takes_its_eyelid() {
        let settings = camera_settings();
        let mut smoother = Smoother::new(&settings);
        // The left eye squints for a second: the eye server reads it closed, the camera narrowed (0.3, not closed)
        let squinting = |i: usize| (90..180).contains(&i);
        let sent = camera_samples(
            &settings,
            &mut smoother,
            100.0,
            3.0,
            |i| if squinting(i) { [0.2, 0.8] } else { [0.8; 2] },
            |i| Some(camera_eyes([if squinting(i) { (0.3, 0.8, false) } else { (0.75, 0.0, false) }, (0.75, 0.0, false)])),
        );
        // Closed for CAMERA_DOUBT_GRACE (27 samples), as a blink would be
        assert!(sent[91..117].iter().all(|sample| sample.lids[0] == 0.0), "{:?}", sent[116].lids);
        assert_eq!(sent[116].lid_from[0], LidFrom::EyeServer);
        // Then the camera's eyelid, not closed
        assert_eq!(sent[119].lid_from, [LidFrom::CameraOpen, LidFrom::EyeServer]);
        assert!(sent[119].lids[0] > 0.0);
        assert!(sent[140..180].iter().all(|sample| (sample.lids[0] - 0.3).abs() < 0.01), "{:?}", sent[140].lids);
        // Back to the eye server's once the two have agreed for CAMERA_AGREE_HOLD (18 samples)
        assert_eq!(sent[195].lid_from[0], LidFrom::CameraOpen);
        assert_eq!(sent[201].lid_from[0], LidFrom::EyeServer);
        assert!((sent.last().unwrap().lids[0] - LID_RELAXED).abs() < 0.01);
        // Without the camera's values, as before: closed all along
        let mut smoother = Smoother::new(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 3.0, |i| if squinting(i) { [0.2, 0.8] } else { [0.8; 2] }, |_| None);
        assert!(sent[91..180].iter().all(|sample| sample.lids[0] == 0.0));
    }

    #[test]
    fn an_eye_stuck_shut_on_the_eye_server_counts_in_a_gaze_capture_while_the_camera_sees_it_open() {
        // 2026-10-11: the eye server read the right eye 0.000 for a whole wear, the camera saw it open (about 0.75)
        let settings = camera_settings();
        let mut smoother = Smoother::new(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 3.0, |_| [0.8, 0.0], |_| Some(camera(0.75, 0.0)));
        // Not before the camera's eyelid takes over (CAMERA_DOUBT_GRACE, as for a blink); from then on, all along
        assert!(sent[..20].iter().all(|sample| !sample.capture_usable() && sample.camera_open() == [false; 2]));
        assert!(sent[40..].iter().all(|sample| sample.camera_open() == [false, true] && sample.capture_usable()));
        // Without the camera's values, as before: none
        let mut smoother = Smoother::new(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 3.0, |_| [0.8, 0.0], |_| None);
        assert!(sent.iter().all(|sample| !sample.capture_usable()));
    }

    #[test]
    fn short_jumps_up_do_not_hand_a_squinting_eye_back() {
        let settings = camera_settings();
        let mut smoother = Smoother::new(&settings);
        // Squinting for two seconds, the eye server's reading jumping to open for 120 ms (11 samples) twice
        let reading = |i: usize| match i {
            90..=269 if !(150..161).contains(&i) && !(200..211).contains(&i) => [0.2, 0.8],
            _ => [0.8; 2],
        };
        let sent = camera_samples(&settings, &mut smoother, 100.0, 3.0, reading, |i| {
            Some(camera_eyes([if (90..270).contains(&i) { (0.3, 0.8, false) } else { (0.75, 0.0, false) }, (0.75, 0.0, false)]))
        });
        assert!(sent[120..270].iter().all(|sample| sample.lid_from[0] == LidFrom::CameraOpen));
        assert!(sent[140..270].iter().all(|sample| sample.lids[0] > 0.25), "never closed again");
    }

    #[test]
    fn an_eye_squinting_along_with_a_closed_one_is_not_pulled_shut_once_the_camera_takes_over() {
        let settings = camera_settings();
        // The right eye held shut for 1.5 s; the left squints along: the eye server reads it at VRCFT 0.15, below
        // --blink-sync-below (0.35), the camera at 0.35 and not closed
        let shut = |i: usize| (90..225).contains(&i);
        let openness = |i: usize| if shut(i) { [0.4, 0.2] } else { [0.8; 2] };
        let cameras = |i: usize| {
            Some(if shut(i) {
                camera_eyes([(0.35, 0.8, false), (0.0, 0.0, true)])
            } else {
                camera(0.75, 0.0)
            })
        };
        let mut smoother = Smoother::new(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 3.0, openness, cameras);
        // Pulled shut with the right eye for the grace period, as a blink caught in one eye would be
        assert!(sent[92..116].iter().all(|sample| sample.lids == [0.0; 2]), "{:?}", sent[110].lids);
        // Then its own eyelid, from the camera, while the right eye stays shut
        assert!(sent[150..225].iter().all(|sample| sample.lids[0] > 0.3 && sample.lids[1] == 0.0), "{:?}", sent[150].lids);
        // Without the camera: shut all along (the behaviour this replaces)
        let mut smoother = Smoother::new(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 3.0, openness, |_| None);
        assert!(sent[92..225].iter().all(|sample| sample.lids == [0.0; 2]));
    }

    #[test]
    fn a_slow_camera_waits_longer_before_taking_over() {
        let settings = camera_settings();
        // The eye server reads the left eye closed for 0.4 s while the camera sees it open
        let openness = |i: usize| if (90..126).contains(&i) { [0.2, 0.8] } else { [0.8; 2] };
        let narrowed = |frame_ns: u64| {
            move |_| {
                let mut live = camera_eyes([(0.3, 0.8, false), (0.75, 0.0, false)]);
                live.frame_ns = [frame_ns; 2];
                Some(live)
            }
        };
        // 90 frames a second: CAMERA_DOUBT_GRACE (0.3 s) it is
        let mut smoother = Smoother::new(&settings);
        let fast = camera_samples(&settings, &mut smoother, 100.0, 2.0, openness, narrowed(11_111_111));
        assert_eq!(fast[122].lid_from[0], LidFrom::CameraOpen);
        // 15 a second: CAMERA_DOUBT_FRAMES of them (0.53 s), longer than this
        let mut smoother = Smoother::new(&settings);
        let slow = camera_samples(&settings, &mut smoother, 100.0, 2.0, openness, narrowed(66_666_667));
        assert!(slow[91..127].iter().all(|sample| sample.lids[0] == 0.0 && sample.lid_from[0] == LidFrom::EyeServer));
    }

    /// 4 s of samples with both eyes looking `left_deg` to the left (the left eye outward), the eye server reading each
    /// eye shut over `shut[eye]` (else 0.9) and unsure of both all along (so only looking to the side doubts the camera,
    /// not CAMERA_CLOSED_DOUBT_S), and the camera seeing the left eye closed over `cam` (else at 0.6).
    fn side_samples(
        settings: &Settings,
        left_deg: f32,
        shut: [std::ops::Range<usize>; 2],
        cam: std::ops::Range<usize>,
    ) -> Vec<Sample> {
        let mut smoother = Smoother::new(settings);
        let direction = [-left_deg.to_radians().tan(), 0.0, -1.0];
        (0..360)
            .map(|i| {
                let data = EyeData {
                    sample_time: 100.0 + i as f64 / 90.0,
                    gaze: [direction; 2],
                    fixation_point: direction,
                    openness: [0, 1].map(|eye| if shut[eye].contains(&i) { 0.1 } else { 0.9 }),
                    pre_fusion_covariance: [[2.0 * CAMERA_CLOSED_DOUBT_VARIANCE; 3]; 2],
                    ..EyeData::default()
                };
                let closed = cam.contains(&i);
                let camera = camera_eyes([(if closed { 0.0 } else { 0.6 }, 0.0, closed), (0.75, 0.0, false)]);
                process(settings, &mut smoother, [1.0; 2], &data, Some(&camera))
            })
            .collect()
    }

    #[test]
    fn the_cameras_closed_is_doubted_while_looking_to_the_side() {
        // One eye's camera only: without the close gate, which holds such a "closed" back until it is a wink
        let settings = Settings { camera_close_gate: false, ..camera_settings() };
        let run = |deg: f32, shut: [std::ops::Range<usize>; 2]| side_samples(&settings, deg, shut, 90..270);
        // gaze_angles' x is positive to the right: looking left is the left eye outward, the right eye inward
        let left = EyeData { gaze: [[-(25f32.to_radians().tan()), 0.0, -1.0]; 2], ..EyeData::default() };
        assert!(gaze_angles(left.gaze[0])[0] < 0.0);
        assert_eq!(outward_degrees(&left).map(f32::round), [25.0, -25.0]);
        // 25° outward or inward: the camera's last open eyelid instead
        for deg in [25.0, -25.0] {
            let sent = run(deg, [0..0, 0..0]);
            assert!(sent[90..270].iter().all(|sample| sample.lids[0] > 0.5 && sample.lid_from[0] != LidFrom::CameraClosed), "{deg}: {:?}", sent[100].lids);
        }
        // Straight ahead or 10° to the side: closed, as before
        for deg in [0.0, 10.0, -10.0] {
            let sent = run(deg, [0..0, 0..0]);
            assert!(sent[90..270].iter().all(|sample| sample.lids[0] == 0.0), "{deg}: {:?}", sent[100].lids);
            assert_eq!(sent[90].lid_from[0], LidFrom::CameraClosed, "{deg}");
        }
        // A blink the eye server reads in that eye still closes, and for CAMERA_SIDE_SHUT_RECENT after it the camera's
        // closed stands
        let sent = run(25.0, [150..160, 150..160]);
        assert!(sent[151..161].iter().all(|sample| sample.lids == [0.0; 2]), "{:?}", sent[155].lids);
        assert!(sent[161..173].iter().all(|sample| sample.lids[0] == 0.0), "{:?}", sent[170].lids);
        assert!(sent[200..270].iter().all(|sample| sample.lids[0] > 0.5), "{:?}", sent[200].lids);
        // The other eye closing does not let a closed that began long before through
        let sent = run(25.0, [0..0, 150..170]);
        assert!(sent[152..170].iter().all(|sample| sample.lids[1] == 0.0), "{:?}", sent[160].lids);
        assert!(sent[90..270].iter().all(|sample| sample.lids[0] > 0.5), "{:?}", sent[160].lids);
        // --eye-behavior 1 never closed an eye on the camera's word, and still does not
        let v1 = Settings { eye_behavior: 1, ..settings.clone() };
        let mut smoother = Smoother::new(&v1);
        let sent = camera_samples(&v1, &mut smoother, 100.0, 2.0, |_| [0.9; 2], |_| Some(camera_eyes([(0.0, 0.0, true), (0.75, 0.0, false)])));
        assert!(sent[90..].iter().all(|sample| sample.lids[0] > 0.5));
        assert!(smoother.side.open_lid == [None; 2] && smoother.side.shut_at == [None; 2], "not even kept with 1");
    }

    #[test]
    fn a_blink_the_eye_server_reads_in_the_other_eye_only_still_closes_a_side_looking_eye() {
        let settings = camera_settings();
        // 2026-10-09 18:17: the camera sees the left eye shut for 0.1 s with the right eye's blink; the eye server reads
        // the left eye open
        let sent = side_samples(&settings, 25.0, [0..0, 150..161], 150..159);
        assert!(sent[150..159].iter().all(|sample| sample.lids[0] == 0.0), "{:?}", sent[152].lids);
        // The camera's closed starting 0.1 s after the other eye's closing: still the blink
        let sent = side_samples(&settings, 25.0, [0..0, 150..161], 159..168);
        assert!(sent[159..168].iter().all(|sample| sample.lids[0] == 0.0), "{:?}", sent[160].lids);
        // ... 0.3 s after it: not
        let sent = side_samples(&settings, 25.0, [0..0, 150..161], 177..186);
        assert!(sent[177..186].iter().all(|sample| sample.lids[0] > 0.5), "{:?}", sent[180].lids);
        // The other eye winking (0.55 s): through for CAMERA_SIDE_BLINK_MAX at most, then the last open eyelid
        let sent = side_samples(&settings, 25.0, [0..0, 150..200], 150..200);
        assert!(sent[150..170].iter().all(|sample| sample.lids[0] == 0.0), "{:?}", sent[160].lids);
        assert!(sent[190..200].iter().all(|sample| sample.lids[0] > 0.45), "{:?}", sent[195].lids);
        // A closed of the camera's longer than CAMERA_SIDE_BLINK_MAX with a blink: the rest held open
        let sent = side_samples(&settings, 25.0, [0..0, 150..161], 150..230);
        assert!(sent[205..230].iter().all(|sample| sample.lids[0] > 0.5), "{:?}", sent[210].lids);
    }

    #[test]
    fn the_side_check_takes_the_last_open_camera_eyelid() {
        let lid = |lid: f32, closed: bool| Some(CameraLid { lid, closed, frame: 0.011 });
        let mut check = SideCheck::default();
        // (The eye server unsure of both eyes all along, so only looking to the side counts)
        // No open one yet: relaxed open
        assert_eq!(check.step(1.0, [0.9; 2], [1.0; 2], [20.0; 2], [lid(0.0, true), None]), [lid(LID_RELAXED, false), None]);
        assert_eq!(check.step(1.01, [0.9; 2], [1.0; 2], [20.0; 2], [lid(0.62, false), lid(0.7, false)]), [lid(0.62, false), lid(0.7, false)]);
        // Outward and inward alike
        assert_eq!(check.step(1.02, [0.9; 2], [1.0; 2], [20.0, -20.0], [lid(0.0, true); 2]), [lid(0.62, false), lid(0.7, false)]);
        // Not at the line itself, nor without a gaze
        assert_eq!(check.step(1.03, [0.9; 2], [1.0; 2], [CAMERA_SIDE_DEG, f32::NAN], [lid(0.0, true); 2]), [lid(0.0, true); 2]);
        // Not within CAMERA_SIDE_SHUT_RECENT of that eye reading shut
        let mut check = SideCheck { open_lid: [Some(0.62), Some(0.7)], ..SideCheck::default() };
        check.step(2.0, [0.3, 0.9], [1.0; 2], [20.0; 2], [None; 2]);
        assert_eq!(check.step(2.1, [0.9; 2], [1.0; 2], [20.0; 2], [lid(0.0, true), None]), [lid(0.0, true), None]);
        assert_eq!(check.step(2.2, [0.9; 2], [1.0; 2], [20.0; 2], [lid(0.0, true), None]), [lid(0.62, false), None]);
        // The other eye's closing: a closed of the camera's that began with it goes through, one from long before not
        let mut check = SideCheck { open_lid: [Some(0.62), Some(0.7)], ..SideCheck::default() };
        assert_eq!(check.step(3.0, [0.9; 2], [1.0; 2], [20.0; 2], [lid(0.0, true), None]), [lid(0.62, false), None]);
        assert_eq!(check.step(3.1, [0.9, 0.2], [1.0; 2], [20.0; 2], [lid(0.0, true), None]), [lid(0.0, true), None]);
        assert_eq!(check.step(3.31, [0.9, 0.2], [1.0; 2], [20.0; 2], [lid(0.0, true), None]), [lid(0.62, false), None], "0.31 s");
        let mut check = SideCheck { open_lid: [Some(0.62), Some(0.7)], ..SideCheck::default() };
        assert_eq!(check.step(4.0, [0.9; 2], [1.0; 2], [20.0; 2], [lid(0.0, true), None]), [lid(0.62, false), None]);
        assert_eq!(check.step(4.2, [0.9, 0.2], [1.0; 2], [20.0; 2], [lid(0.0, true), None]), [lid(0.62, false), None], "0.2 s apart");
        // A last open eyelid that was widened is held at relaxed open
        let mut check = SideCheck::default();
        check.step(5.0, [0.9; 2], [1.0; 2], [20.0; 2], [lid(0.9, false), lid(0.7, false)]);
        let held = check.step(5.01, [0.9; 2], [1.0; 2], [20.0; 2], [lid(0.0, true); 2]);
        assert_eq!(held, [lid(LID_RELAXED, false), lid(0.7, false)]);
    }

    /// 5 s of samples looking straight ahead, the eye server reading the eyes at `openness(sample index)`, the larger of
    /// the left eye's pre-fusion gaze variances at `variance(sample index)` (the right eye's 0.001), and the camera
    /// seeing the left eye closed over `cam` (else at relaxed open).
    fn doubt_samples(
        settings: &Settings,
        openness: impl Fn(usize) -> [f32; 2],
        variance: impl Fn(usize) -> f32,
        cam: std::ops::Range<usize>,
    ) -> Vec<Sample> {
        doubt_samples_with_gaps(settings, openness, variance, cam, 0..0)
    }

    /// As doubt_samples, with the camera's values of the left eye stale (none to use) over `gap`.
    fn doubt_samples_with_gaps(
        settings: &Settings,
        openness: impl Fn(usize) -> [f32; 2],
        variance: impl Fn(usize) -> f32,
        cam: std::ops::Range<usize>,
        gap: std::ops::Range<usize>,
    ) -> Vec<Sample> {
        let mut smoother = Smoother::new(settings);
        (0..450)
            .map(|i| {
                let data = EyeData {
                    sample_time: 100.0 + i as f64 / 90.0,
                    gaze: [[0.0, 0.0, -1.0]; 2],
                    fixation_point: [0.0, 0.0, -1.0],
                    openness: openness(i),
                    pre_fusion_covariance: [[variance(i), variance(i) / 2.0, 0.0], [0.001, 0.0005, 0.0]],
                    ..EyeData::default()
                };
                let closed = cam.contains(&i);
                let mut camera = camera_eyes([(if closed { 0.0 } else { 0.75 }, 0.0, closed), (0.75, 0.0, false)]);
                camera.fresh[0] = !gap.contains(&i);
                process(settings, &mut smoother, [1.0; 2], &data, Some(&camera))
            })
            .collect()
    }

    #[test]
    fn a_short_gap_in_the_cameras_values_keeps_the_doubt_and_a_long_one_starts_it_over() {
        let settings = camera_settings();
        let sure = |_| 0.001;
        // The camera's left eye values stale for 33 ms (3 samples) 1.2 s into its 3.3 s closed, after it was taken as
        // open: open through the gap (the eye server's eyelid) and after it (still doubted), to the closed's end
        let sent = doubt_samples_with_gaps(&settings, |_| [0.9; 2], sure, 90..390, 198..201);
        assert!(sent[170..390].iter().all(|sample| sample.lids[0] > 0.7), "{:?}", sent[205].lids);
        assert!(sent[170..198].iter().chain(&sent[201..390]).all(|sample| sample.closed_overruled[0]), "{:?}", sent[201].closed_overruled);
        assert!(sent[198..201].iter().all(|sample| !sample.closed_overruled[0] && sample.lid_from[0] != LidFrom::CameraClosed));
        // ...also one of nearly CAMERA_GAP_HOLD (16 samples, 0.19 s since its last "closed")
        let sent = doubt_samples_with_gaps(&settings, |_| [0.9; 2], sure, 90..390, 198..214);
        assert!(sent[170..390].iter().all(|sample| sample.lids[0] > 0.7), "{:?}", sent[216].lids);
        // A gap of 0.3 s: its "closed" after the gap is a new stretch (from 225, where the values came back), closed once
        // the values are taken again (CAMERA_GAP_HOLD after they came back: 243) until CAMERA_CLOSED_DOUBT_S into it
        let sent = doubt_samples_with_gaps(&settings, |_| [0.9; 2], sure, 90..390, 198..225);
        assert!(sent[198..243].iter().all(|sample| sample.lids[0] > 0.7 && !sample.closed_overruled[0]), "{:?}", sent[230].lids);
        assert!(sent[243..271].iter().all(|sample| sample.lids[0] == 0.0 && !sample.closed_overruled[0]), "{:?}", sent[260].lids);
        assert_eq!(sent[243].lid_from[0], LidFrom::CameraClosed);
        assert!(sent[271..390].iter().all(|sample| sample.closed_overruled[0]), "{:?}", sent[271].closed_overruled);
        assert!(sent[274..390].iter().all(|sample| sample.lids[0] > 0.7), "{:?}", sent[274].lids);
    }

    #[test]
    fn the_other_eyes_blink_does_not_close_an_eye_whose_closed_is_doubted() {
        let settings = camera_settings();
        let sure = |_| 0.001;
        // The eye server reads a 0.1 s blink of the right eye only, 2 s into the camera's 3.3 s closed of the left eye
        // (taken as open by then): the right eye closes for it, the left one stays open (its "closed" began long
        // before the blink, see blink_with_other)
        let wink = |i: usize| if (270..279).contains(&i) { [0.9, 0.05] } else { [0.9; 2] };
        let sent = doubt_samples(&settings, wink, sure, 90..390);
        assert!(sent[272..279].iter().all(|sample| sample.lids[1] == 0.0), "{:?}", sent[275].lids);
        assert!(sent[170..390].iter().all(|sample| sample.lids[0] > 0.7 && sample.closed_overruled[0]), "{:?}", sent[275].lids);
        // Both eyes' blink, read by the eye server: closes the left eye too, and its "closed" stands to its end (the eye
        // server read it closing in it)
        let blink = |i: usize| if (270..279).contains(&i) { [0.05; 2] } else { [0.9; 2] };
        let sent = doubt_samples(&settings, blink, sure, 90..390);
        assert!(sent[272..279].iter().all(|sample| sample.lids == [0.0; 2]), "{:?}", sent[275].lids);
        assert!(sent[272..390].iter().all(|sample| sample.lids[0] == 0.0), "{:?}", sent[300].lids);
    }

    #[test]
    fn a_long_closed_of_the_cameras_is_doubted_while_the_eye_server_reads_the_eye_open_and_is_sure() {
        // One eye's camera only: without the close gate, which holds such a "closed" back until it is a wink
        let settings = Settings { camera_close_gate: false, ..camera_settings() };
        let sure = |_| 0.001;
        // 2026-10-12 01:01, lying: the camera reads the open left eye closed for 3.3 s; the eye server reads it open
        let sent = doubt_samples(&settings, |_| [0.9; 2], sure, 90..390);
        // Closed for its first CAMERA_CLOSED_DOUBT_S (45 samples), then its last open eyelid
        assert!(sent[90..135].iter().all(|sample| sample.lids[0] == 0.0 && sample.closed_overruled == [false; 2]));
        assert_eq!(sent[90].lid_from[0], LidFrom::CameraClosed);
        assert!(sent[137..390].iter().all(|sample| sample.closed_overruled == [true, false]), "{:?}", sent[137].closed_overruled);
        assert!(sent[170..390].iter().all(|sample| sample.lids[0] > 0.7 && sample.lid_from[0] != LidFrom::CameraClosed), "{:?}", sent[170].lids);
        assert!(sent[390..].iter().all(|sample| sample.closed_overruled == [false; 2]));
        // --eye-behavior 1 never closed an eye on the camera's word, and is as before
        let v1 = Settings { eye_behavior: 1, ..settings.clone() };
        let sent = doubt_samples(&v1, |_| [0.9; 2], sure, 90..390);
        assert!(sent.iter().all(|sample| sample.closed_overruled == [false; 2] && sample.lids[0] > 0.7));
    }

    #[test]
    fn a_blink_the_eye_server_misses_still_closes_however_sure_it_is() {
        // One eye's camera only: without the close gate, which holds such a "closed" back until it is a wink
        let settings = Settings { camera_close_gate: false, ..camera_settings() };
        // 0.1, 0.3 and 0.45 s (blinks are 70-400 ms): closed all along
        for samples in [9, 27, 40] {
            let sent = doubt_samples(&settings, |_| [0.9; 2], |_| 0.001, 90..90 + samples);
            assert!(sent[90..90 + samples].iter().all(|sample| sample.lids[0] == 0.0), "{samples}");
            assert!(sent.iter().all(|sample| sample.closed_overruled == [false; 2]), "{samples}");
        }
    }

    #[test]
    fn the_cameras_closed_stands_where_the_eye_server_read_the_eye_closing_in_it() {
        // One eye's camera only: without the close gate, which holds such a "closed" back until it is a wink
        let settings = Settings { camera_close_gate: false, ..camera_settings() };
        let sure = |_| 0.001;
        // The eye server reads the left eye closing for a moment 1.2 s into the camera's 3.3 s closed: closed to its end
        let dip = |i: usize| if (198..203).contains(&i) { [0.4, 0.9] } else { [0.9; 2] };
        let sent = doubt_samples(&settings, dip, sure, 90..390);
        assert!(sent[170..198].iter().all(|sample| sample.lids[0] > 0.7), "{:?}", sent[190].lids);
        assert!(sent[199..390].iter().all(|sample| sample.lids[0] == 0.0 && !sample.closed_overruled[0]), "{:?}", sent[300].lids);
        // ... or just after its first CAMERA_CLOSED_DOUBT_BLINK_S (0.41 s in, just below CAMERA_CLOSED_DOUBT_OPEN): closed
        // all along
        let late = |i: usize| if i == 127 { [CAMERA_CLOSED_DOUBT_OPEN - 0.01, 0.9] } else { [0.9; 2] };
        let sent = doubt_samples(&settings, late, sure, 90..390);
        assert!(sent[90..390].iter().all(|sample| sample.lids[0] == 0.0), "{:?}", sent[300].lids);
        // At CAMERA_CLOSED_DOUBT_OPEN itself it counts as open
        let line = |_| [CAMERA_CLOSED_DOUBT_OPEN, 0.9];
        let sent = doubt_samples(&settings, line, sure, 90..390);
        assert!(sent[137..390].iter().all(|sample| sample.closed_overruled[0]));
    }

    #[test]
    fn a_long_closed_of_the_cameras_that_began_with_a_blink_is_doubted_from_the_blinks_end() {
        // One eye's camera only: without the close gate, which holds such a "closed" back until it is a wink
        let settings = Settings { camera_close_gate: false, ..camera_settings() };
        let sure = |_| 0.001;
        // The eye server reads a blink over the first 0.1 s of the camera's closed (which goes on for 3.3 s): closed until
        // CAMERA_CLOSED_DOUBT_S after its last sample below CAMERA_CLOSED_DOUBT_OPEN (99), then its last open eyelid
        let blink = |i: usize| if (90..100).contains(&i) { [0.1; 2] } else { [0.9; 2] };
        let sent = doubt_samples(&settings, blink, sure, 90..390);
        assert!(sent[90..144].iter().all(|sample| sample.lids[0] == 0.0 && !sample.closed_overruled[0]), "{:?}", sent[140].lids);
        assert!(sent[146..390].iter().all(|sample| sample.closed_overruled[0]), "{:?}", sent[146].closed_overruled);
        assert!(sent[180..390].iter().all(|sample| sample.lids[0] > 0.7), "{:?}", sent[180].lids);
        // A reading below it up to CAMERA_CLOSED_DOUBT_BLINK_S in (0.39 s) counts from there
        let edge = |i: usize| if i == 125 { [0.3, 0.9] } else { [0.9; 2] };
        let sent = doubt_samples(&settings, edge, sure, 90..390);
        assert!(sent[90..170].iter().all(|sample| sample.lids[0] == 0.0), "{:?}", sent[165].lids);
        assert!(sent[172..390].iter().all(|sample| sample.closed_overruled[0]), "{:?}", sent[172].closed_overruled);
    }

    #[test]
    fn camera_closed_doubt_off_leaves_a_long_closed_of_the_cameras_closed() {
        // As in a_long_closed_of_the_cameras_is_doubted_..., without the doubt: closed all along (a wink or shut eyes
        // the eye server reads open and is sure of)
        let settings = Settings { camera_close_gate: false, camera_closed_doubt: false, ..camera_settings() };
        let sent = doubt_samples(&settings, |_| [0.9; 2], |_| 0.001, 90..390);
        assert!(sent[90..390].iter().all(|sample| sample.lids[0] == 0.0 && !sample.closed_overruled[0]));
        // Looking to the side is still checked
        let side = Settings { camera_closed_doubt: false, ..settings };
        let run = side_samples(&side, 25.0, [0..0, 0..0], 90..270);
        assert!(run[90..270].iter().all(|sample| sample.lids[0] > 0.5), "{:?}", run[150].lids);
    }

    #[test]
    fn the_cameras_closed_stands_while_the_eye_server_is_unsure_of_the_eye() {
        // One eye's camera only: without the close gate, which holds such a "closed" back until it is a wink
        let settings = Settings { camera_close_gate: false, ..camera_settings() };
        // Once taken as open, the variance's spikes shorter than CAMERA_CLOSED_DOUBT_UNSURE_S leave it open
        let spikes = |i: usize| if (200..202).contains(&i) || (250..270).contains(&i) { 0.05 } else { 0.001 };
        let sent = doubt_samples(&settings, |_| [0.9; 2], spikes, 90..390);
        assert!(sent[137..390].iter().all(|sample| sample.closed_overruled[0]), "{:?}", sent[260].closed_overruled);
        assert!(sent[170..390].iter().all(|sample| sample.lids[0] > 0.7), "{:?}", sent[260].lids);
        // Unsure from 2 s into the camera's 3.3 s closed for 0.5 s: closed once that has lasted
        // CAMERA_CLOSED_DOUBT_UNSURE_S, open again once it is sure again
        let unsure = |i: usize| if (270..315).contains(&i) { 0.05 } else { 0.001 };
        let sent = doubt_samples(&settings, |_| [0.9; 2], unsure, 90..390);
        assert!(sent[170..292].iter().all(|sample| sample.lids[0] > 0.7), "{:?}", sent[290].lids);
        assert!(sent[293..315].iter().all(|sample| sample.lids[0] == 0.0 && !sample.closed_overruled[0]), "{:?}", sent[300].lids);
        assert!(sent[315..390].iter().all(|sample| sample.closed_overruled[0]));
        assert!(sent[360..390].iter().all(|sample| sample.lids[0] > 0.7), "{:?}", sent[360].lids);
        // Unsure as CAMERA_CLOSED_DOUBT_S runs out: not taken as open before it is sure
        let first = |i: usize| if (120..150).contains(&i) { 0.05 } else { 0.001 };
        let sent = doubt_samples(&settings, |_| [0.9; 2], first, 90..390);
        assert!(sent[90..150].iter().all(|sample| sample.lids[0] == 0.0), "{:?}", sent[145].lids);
        assert!(sent[150..390].iter().all(|sample| sample.closed_overruled[0]));
        // The eye server reading the eye below CAMERA_CLOSED_DOUBT_OPEN closes it at once, also through a spike
        let dip = |i: usize| if i == 250 { [0.4, 0.9] } else { [0.9; 2] };
        let sent = doubt_samples(&settings, dip, spikes, 90..390);
        assert!(sent[250..390].iter().all(|sample| sample.lids[0] == 0.0 && !sample.closed_overruled[0]), "{:?}", sent[300].lids);
        // At the line itself unsure; unsure all along (or no variance read), closed all along
        for variance in [CAMERA_CLOSED_DOUBT_VARIANCE, 0.05, f32::NAN] {
            let sent = doubt_samples(&settings, |_| [0.9; 2], |_| variance, 90..390);
            assert!(sent[90..390].iter().all(|sample| sample.lids[0] == 0.0), "{variance}: {:?}", sent[300].lids);
        }
        // Either variance counts, and one that is not a number is unsure
        let data = EyeData { pre_fusion_covariance: [[0.001, 0.02, 0.5], [f32::NAN, 0.001, 0.0]], ..EyeData::default() };
        assert_eq!(gaze_variance(&data), [0.02, f32::INFINITY]);
    }

    #[test]
    fn the_raise_peak_keeps_the_cameras_highest_open_eyelid_of_the_last_0_3_s() {
        let lid = |lid: f32, closed: bool| Some(CameraLid { lid, closed, frame: 0.011 });
        let mut peak = RaisePeak::default();
        assert_eq!(peak.step(1.0, [0.9; 2], [lid(0.7, false), None]), [Some(0.7), None]);
        assert_eq!(peak.step(1.1, [0.9; 2], [lid(0.4, false), None]), [Some(0.7), None]);
        assert_eq!(peak.step(1.29, [0.9; 2], [lid(0.5, false), None]), [Some(0.7), None]);
        // 0.7 is 0.31 s old: gone
        assert_eq!(peak.step(1.31, [0.9; 2], [lid(0.45, false), None]), [Some(0.5), None]);
        // Through a gap, what is left
        assert_eq!(peak.step(1.35, [0.9; 2], [None; 2]), [Some(0.5), None]);
        assert_eq!(peak.step(1.65, [0.9; 2], [None; 2]), [None; 2]);
        // Forgotten when the camera sees the eye closed, or the eye server reads it shut (that eye only)
        peak.step(2.0, [0.9; 2], [lid(0.7, false); 2]);
        assert_eq!(peak.step(2.01, [0.9; 2], [lid(0.0, true), lid(0.3, false)]), [None, Some(0.7)]);
        assert_eq!(peak.step(2.02, [0.9, 0.4], [lid(0.3, false); 2]), [Some(0.3), None]);
    }

    #[test]
    fn an_eyelid_raised_to_a_wobbling_camera_does_not_tremble_and_still_blinks() {
        let settings = camera_settings();
        // The eye server reads the left eye open but low; the camera's eyelid wobbles between 0.72 and 0.42 three times
        // a second; a blink at samples 200-209
        let openness = |i: usize| if (200..209).contains(&i) { [0.05; 2] } else { [0.6, 0.9] };
        let cameras = |i: usize| {
            let wobble = if (i / 15) % 2 == 0 { 0.72 } else { 0.42 };
            Some(camera_eyes([(wobble, 0.0, false), (0.75, 0.0, false)]))
        };
        let mut smoother = Smoother::new(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 3.0, openness, cameras);
        let steady: Vec<f32> = sent[100..195].iter().map(|sample| sample.lids[0]).collect();
        let (lo, hi) = steady.iter().fold((1.0f32, 0.0f32), |(lo, hi), lid| (lo.min(*lid), hi.max(*lid)));
        assert!(hi - lo < 0.02 && lo > 0.68, "{lo} - {hi}");
        assert!(sent[100..195].iter().all(|sample| sample.lid_from[0] == LidFrom::CameraRaised));
        // The blink closes from its first sample through the 3-sample median, as without the camera
        assert!(sent[201..210].iter().all(|sample| sample.lids == [0.0; 2]), "{:?}", sent[201].lids);
        let mut alone = Smoother::new(&settings);
        let without = camera_samples(&settings, &mut alone, 100.0, 3.0, openness, |_| None);
        assert!(without[201..210].iter().all(|sample| sample.lids == [0.0; 2]));
    }

    #[test]
    fn an_eye_the_camera_sees_closed_is_closed() {
        // Without the close gate: in one eye only, which the gate holds back unless it is a blink or a wink (see
        // the_cameras_closed_in_one_eye_waits_until_it_is_a_blink_or_a_wink)
        let settings = Settings { camera_close_gate: false, ..camera_settings() };
        // The eye server misses a 100 ms blink of the left eye (9 samples) that the camera sees
        let cameras = |i: usize| {
            Some(if (90..99).contains(&i) { camera_eyes([(0.0, 0.0, true), (0.75, 0.0, false)]) } else { camera(0.75, 0.0) })
        };
        let mut smoother = Smoother::new(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 2.0, |_| [0.8; 2], cameras);
        assert!(sent[90..99].iter().all(|sample| sample.lids[0] == 0.0 && sample.lids[1] > 0.7), "{:?}", sent[90].lids);
        assert_eq!(sent[90].lid_from, [LidFrom::CameraClosed, LidFrom::EyeServer]);
        // Held for --blink-hold-ms like any closing (80 ms from its start, so through the 9 samples), then opening
        assert!(sent[100].lids[0] > 0.0 && sent[100].lid_from[0] == LidFrom::EyeServer);
        // The blink sync closes the other eye too when that one is half closing
        let mut smoother = Smoother::new(&settings);
        let half = |i: usize| if (85..105).contains(&i) { [0.8, 0.4] } else { [0.8; 2] };
        let sent = camera_samples(&settings, &mut smoother, 100.0, 2.0, half, cameras);
        assert!(sent[90..99].iter().all(|sample| sample.lids == [0.0; 2]), "{:?}", sent[90].lids);
        // A one-frame closed flag is held as a short blink
        let one = |i: usize| Some(if i == 90 { camera_eyes([(0.0, 0.0, true), (0.75, 0.0, false)]) } else { camera(0.75, 0.0) });
        let mut smoother = Smoother::new(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 2.0, |_| [0.8; 2], one);
        assert!(sent[90..97].iter().all(|sample| sample.lids[0] == 0.0), "{:?}", sent[96].lids);
        // While the openness is saturated, too (the cap is for eyes without the camera)
        let mut smoother = saturated_smoother(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 2.0, |_| [1.0; 2], cameras);
        assert!(sent[90..99].iter().all(|sample| sample.lids[0] == 0.0));
        // --raw: closed while the camera says so, nothing held
        let raw = Settings { raw: true, ..settings.clone() };
        let mut smoother = Smoother::new(&raw);
        let sent = camera_samples(&raw, &mut smoother, 100.0, 2.0, |_| [0.8; 2], cameras);
        assert_eq!((sent[90].lids, sent[90].lid_from[0]), ([0.0, LID_RELAXED], LidFrom::CameraClosed));
        assert_eq!(sent[99].lids, [LID_RELAXED; 2]);
    }

    #[test]
    fn the_cameras_widening_shows_unless_the_eye_server_reads_the_eye_closing() {
        // Without lid_sync, so each eye's own eyelid shows
        let settings = Settings {
            lid_sync: 0.0,
            ..camera_settings()
        };
        // Both eyes widened on the camera; the eye server reads the right one lower (VRCFT 0.6, at least
        // WIDEN_RESET_BELOW + VALVE_WIDEN_FADE): both widened, once it has lasted WIDEN_SUSTAIN
        let mut smoother = Smoother::new(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 2.0, |_| [0.8, 0.7], |_| Some(camera(1.0, 0.0)));
        assert!(sent[..22].iter().all(|sample| sample.lids.iter().all(|lid| *lid <= LID_RELAXED)));
        assert!(sent.last().unwrap().lids.iter().all(|lid| *lid > 0.99), "{:?}", sent.last().unwrap().lids);
        assert_eq!(sent.last().unwrap().lid_from, [LidFrom::EyeServer, LidFrom::CameraWidened]);
        // Read below that (0.48): raised to relaxed open (see raised_to_camera), but the widening still goes by the
        // eye server's 0.48, so none of it comes through
        let mut smoother = Smoother::new(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 2.0, |_| [0.8, 0.62], |_| Some(camera(1.0, 0.0)));
        let last = sent.last().unwrap();
        assert!(last.lids[0] > 0.99 && (last.lids[1] - LID_RELAXED).abs() < 1e-3, "{:?}", last.lids);
        assert_eq!(last.lid_from, [LidFrom::EyeServer, LidFrom::CameraRaised]);
        assert_eq!(widen_basis([0.75, 0.75], [0.48, 0.6], [LidFrom::CameraRaised, LidFrom::EyeServer]), [0.48, 0.75]);
        assert_eq!(mix_lid_by(0.75, 0.48, Some(1.0)), 0.75);
        assert_eq!(mix_lid_by(0.75, 0.75, Some(1.0)), mix_lid(0.75, Some(1.0)));
        // Read closing (below the doubt line) for a blink's length, the eye server's stands: no widening
        assert!((mix_lid(0.2, Some(1.0)) - 0.2).abs() < 1e-6);
        // While saturated too, and still held back until it lasts
        let mut smoother = saturated_smoother(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 2.0, |_| [1.0, 0.7], |_| Some(camera(1.0, 0.0)));
        assert!(sent[..22].iter().all(|sample| sample.lids.iter().all(|lid| *lid <= LID_RELAXED)));
        assert!(sent.last().unwrap().lids.iter().all(|lid| *lid > 0.99));
        // The camera's eyelid at relaxed open or below: no widening, the eye server's raised to the camera's
        let mut smoother = Smoother::new(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 2.0, |_| [0.8, 0.65], |_| Some(camera(0.7, 0.0)));
        assert!((sent.last().unwrap().lids[1] - 0.7).abs() < 0.01, "{:?}", sent.last().unwrap().lids);
        // In between, part of the way: from WIDEN_RESET_BELOW over VALVE_WIDEN_FADE, and from relaxed open over
        // CAMERA_WIDEN_FADE
        assert!((mix_lid(0.525, Some(1.0)) - (0.525 + 0.25 * 0.475)).abs() < 1e-5);
        assert!((mix_lid(0.6, Some(0.775)) - (0.6 + 0.5 * 0.175)).abs() < 1e-5);
        assert_eq!((mix_lid(0.6, Some(0.75)), mix_lid(0.6, Some(0.8)), mix_lid(0.8, Some(0.775))), (0.6, 0.8, 0.7625));
        // Continuous: a small change in either value moves the eyelid a little (the camera's wobble around relaxed open
        // used to flip a VRCFT 0.6 eyelid to 0.75 and back)
        for valve in (0..=100).map(|i| i as f32 / 100.0) {
            for camera in (0..=100).map(|i| 0.6 + i as f32 * 0.004) {
                let lid = mix_lid(valve, Some(camera));
                assert!((mix_lid(valve + 0.002, Some(camera)) - lid).abs() < 0.02, "{valve} {camera}");
                assert!((mix_lid(valve, Some(camera + 0.002)) - lid).abs() < 0.02, "{valve} {camera}");
            }
        }
    }

    #[test]
    fn a_few_low_camera_readings_at_a_jump_to_the_ceiling_make_no_dip() {
        let settings = Settings {
            lid_sync: 0.0,
            ..camera_settings()
        };
        // The right eye's reading goes between 0.9 and 1.000 every 5 samples; the camera sees it relaxed open but for
        // three low readings (33 ms) at the jump at sample 95 (it went down to 0.37 when the rule took the camera's own),
        // and narrowed from sample 130 on
        let openness = |i: usize| if i % 10 < 5 { [1.0, 0.9] } else { [1.0; 2] };
        let cameras = |i: usize| {
            let right = if (95..98).contains(&i) || i >= 130 { 0.3 } else { LID_RELAXED };
            Some(camera_eyes([(LID_RELAXED, 0.0, false), (right, 0.0, false)]))
        };
        let mut smoother = saturated_smoother(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 2.0, openness, cameras);
        let low = sent[90..125].iter().map(|sample| sample.lids[1]).fold(1.0f32, f32::min);
        assert!(low > 0.65, "{low}");
        // Held narrowed past CAMERA_RAISE_PEAK_S, a jump is lowered to it as before
        assert!(sent[165..180].iter().any(|sample| sample.lid_from[1] == LidFrom::CameraAtCeiling && sample.lids[1] < 0.5), "{:?}", sent[170].lids);
    }

    #[test]
    fn a_jump_to_the_ceiling_keeps_the_cameras_half_closed_eyelid() {
        let settings = Settings {
            lid_sync: 0.0,
            ..camera_settings()
        };
        // SteamOS 0.4.3: a relaxed left eye reads 1.000; the right is held half closed (0.55, the camera agreeing)
        // and its reading jumps to 1.000 for 3 samples, as on 2026-10-07
        let half = lid_to_vrcft(0.55, &settings);
        let openness = |i: usize| if (90..93).contains(&i) { [1.0; 2] } else { [1.0, 0.55] };
        let cameras = |_| Some(camera_eyes([(LID_RELAXED, 0.0, false), (half, 0.3, false)]));
        let mut smoother = saturated_smoother(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 2.0, openness, cameras);
        assert!(sent[60..].iter().all(|sample| (sample.lids[1] - half).abs() < 1e-3), "{:?}", sent[92].lids);
        assert_eq!(sent[92].lid_from, [LidFrom::EyeServer, LidFrom::CameraAtCeiling]);
        assert!(sent.iter().all(|sample| (sample.lids[0] - LID_RELAXED).abs() < 1e-5));
        // Not in --raw, which has none of the timed stages
        let raw = Settings { raw: true, ..settings.clone() };
        let mut smoother = Smoother::new(&raw);
        let sent = camera_samples(&raw, &mut smoother, 100.0, 2.0, openness, cameras);
        assert!((sent[90].lids[1] - LID_RELAXED).abs() < 1e-6, "{:?}", sent[90].lids);
        // A reading on its way there takes the camera's part of the way: none at CEILING_FROM, half way between...
        assert_eq!((ceiling_share(CEILING_FROM), ceiling_share(1.0)), (0.0, 1.0));
        assert!((ceiling_share((CEILING_FROM + SATURATED_READING) / 2.0) - 0.5).abs() < 1e-4);
        let lowered = |valve: f32, share: f32, camera: f32| lowered_at_ceiling(valve, share, camera);
        assert_eq!(lowered(0.75, 0.5, 0.35), (0.55, 0.5));
        // ...all of it while the camera sees the eye clearly narrowed, none while it sees it about open, never up
        assert_eq!(lowered(0.75, 1.0, CEILING_NARROWED.0), (CEILING_NARROWED.0, 1.0));
        let (part, share) = lowered(0.75, 1.0, (CEILING_NARROWED.0 + CEILING_NARROWED.1) / 2.0);
        assert!((share - 0.5).abs() < 1e-4 && part > 0.65 && part < 0.75, "{part} {share}");
        assert_eq!(lowered(0.75, 1.0, CEILING_NARROWED.1), (0.75, 0.0));
        assert_eq!(lowered(0.75, 1.0, 0.9), (0.75, 0.0));
        assert_eq!(lowered(0.2, 1.0, 0.3), (0.2, 1.0));
        // Held at 1.000 past CEILING_JUMP, it is the eye server's again (a relaxed eye looking down reads so while the
        // camera sees its eyelid lower)
        let held = |i: usize| if i >= 90 { [1.0; 2] } else { [1.0, 0.55] };
        let mut smoother = saturated_smoother(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 2.0, held, cameras);
        assert!((sent[93].lids[1] - half).abs() < 1e-3, "{:?}", sent[93].lids);
        assert!((sent.last().unwrap().lids[1] - LID_RELAXED).abs() < 1e-3, "{:?}", sent.last().unwrap().lids);
        assert_eq!(sent.last().unwrap().lid_from[1], LidFrom::EyeServer);
        // Without the camera's values, the jump goes out as before
        let mut smoother = saturated_smoother(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 2.0, openness, |_| None);
        assert!(sent[93].lids[1] > half + 0.1, "{:?}", sent[93].lids);
        // An eye the camera sees nearly shut (a hard squint) while the eye server reads it at 1.000 is not a blink: the
        // other eye, half closing, is not pulled shut with it
        let squint = |_| Some(camera_eyes([(0.0, 0.9, false), (half, 0.3, false)]));
        let mut smoother = saturated_smoother(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 1.0, |_| [1.0, 0.4], squint);
        assert!(sent[..9].iter().all(|sample| sample.lids[0] < 0.03), "{:?}", sent[5].lids);
        assert!(sent.iter().all(|sample| sample.lids[1] > 0.1), "{:?}", sent[5].lids);
    }

    #[test]
    fn an_eye_reopening_at_the_ceiling_follows_the_camera() {
        let settings = camera_settings();
        // A 200 ms blink, read by both; the camera says closed 30 ms later and for 60 ms longer, then its eyelid
        // opens over 200 ms while the eye server reads 1.000 at once
        let openness = |i: usize| if (90..108).contains(&i) { [0.0; 2] } else { [1.0; 2] };
        let cameras = |i: usize| {
            Some(match i {
                93..=113 => camera_eyes([(0.0, 0.0, true); 2]),
                114..=131 => camera(LID_RELAXED * (i - 113) as f32 / 18.0, 0.0),
                _ => camera(LID_RELAXED, 0.0),
            })
        };
        let mut smoother = saturated_smoother(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 2.0, openness, cameras);
        assert!(sent[92..114].iter().all(|sample| sample.lids == [0.0; 2]), "{:?}", sent[110].lids);
        // Opening with the camera, not jumping to relaxed open with the eye server, then going over to it
        let between = |lid: f32| lid > 0.0 && lid < LID_RELAXED - 0.05;
        assert!(sent[114..122].iter().all(|sample| between(sample.lids[0])), "{:?}", sent[121].lids);
        assert!(sent[114..150].windows(2).all(|pair| pair[1].lids[0] >= pair[0].lids[0]));
        assert!(sent[150..].iter().all(|sample| (sample.lids[0] - LID_RELAXED).abs() < 0.01), "{:?}", sent[150].lids);
    }

    #[test]
    fn widening_in_one_eye_is_halved() {
        let settings = Settings {
            lid_sync: 0.0,
            ..camera_settings()
        };
        let run = |cameras: Live| {
            let mut smoother = Smoother::new(&settings);
            camera_samples(&settings, &mut smoother, 100.0, 2.0, |_| [0.8; 2], |_| Some(cameras)).last().unwrap().lids
        };
        // The left eye's camera sees it fully widened, the right's not at all: half of it
        let one = run(camera_eyes([(1.0, 0.0, false), (LID_RELAXED, 0.0, false)]));
        assert!((one[0] - 0.875).abs() < 1e-3 && (one[1] - LID_RELAXED).abs() < 1e-5, "{one:?}");
        // Both widened: as they are
        let both = run(camera(1.0, 0.0));
        assert!(both.iter().all(|lid| (lid - 1.0).abs() < 1e-3), "{both:?}");
        // Unevenly: the wider one meets the other halfway, which keeps its own
        let uneven = run(camera_eyes([(1.0, 0.0, false), (0.9, 0.0, false)]));
        assert!((uneven[0] - 0.95).abs() < 1e-3 && (uneven[1] - 0.9).abs() < 1e-3, "{uneven:?}");
        // The other eye half closed on the camera counts as not widened
        let squinting = run(camera_eyes([(1.0, 0.0, false), (0.4, 0.5, false)]));
        assert!((squinting[0] - 0.875).abs() < 1e-3, "{squinting:?}");
        // Only the left eye's camera values used (the right's not fresh): unchanged
        let left_only = run(Live {
            fresh: [true, false],
            ..camera_eyes([(1.0, 0.0, false), (LID_RELAXED, 0.0, false)])
        });
        assert!((left_only[0] - 1.0).abs() < 1e-3 && (left_only[1] - LID_RELAXED).abs() < 1e-5, "{left_only:?}");
        assert_eq!(share_widening([1.0, 0.3], [Some(1.0), None]), [1.0, 0.3]);
        assert_eq!(share_widening([1.0, 0.3], [None; 2]), [1.0, 0.3]);
        // Below relaxed open nothing changes, and the limit moves with the values (no step)
        assert_eq!(share_widening([0.3, 0.5], [Some(1.0), Some(0.2)]), [0.3, 0.5]);
        let limited = |own: f32| share_widening([own, LID_RELAXED], [Some(own), Some(LID_RELAXED)])[0];
        assert!((limited(0.7501) - 0.7501).abs() < 1e-4 && (limited(0.8) - 0.775).abs() < 1e-6);
        // --raw too
        let raw = Settings { raw: true, ..settings.clone() };
        let mut smoother = Smoother::new(&raw);
        let cameras = |_| Some(camera_eyes([(1.0, 0.0, false), (LID_RELAXED, 0.0, false)]));
        let sent = camera_samples(&raw, &mut smoother, 100.0, 0.1, |_| [0.8; 2], cameras);
        assert_eq!(sent[0].lids, [0.875, LID_RELAXED]);
    }

    #[test]
    fn the_lid_floor_holds_a_squint_the_camera_sees_open() {
        // Off by default (it would surprise users who never set it); 0.25 here
        assert_eq!(camera_settings().camera_lid_floor, 0.0);
        let settings = Settings { camera_lid_floor: 0.25, ..camera_settings() };
        // A hard squint of the left eye for a second: the eye server reads it closed, the camera at 0.05, not closed
        let squinting = |i: usize| (90..180).contains(&i);
        let openness = |i: usize| if squinting(i) { [0.2, 0.8] } else { [0.8; 2] };
        let cameras = |i: usize| {
            Some(camera_eyes([if squinting(i) { (0.05, 1.0, false) } else { (0.75, 0.0, false) }, (0.75, 0.0, false)]))
        };
        let mut smoother = Smoother::new(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 3.0, openness, cameras);
        // Closed within the grace period, as a blink would be; then at the floor, not the camera's 0.05
        assert!(sent[91..117].iter().all(|sample| sample.lids[0] == 0.0));
        assert!(sent[140..180].iter().all(|sample| (sample.lids[0] - 0.25).abs() < 0.005), "{:?}", sent[140].lids);
        // 0 turns it off: the camera's eyelid as it is
        let off = Settings {
            camera_lid_floor: 0.0,
            ..settings.clone()
        };
        let mut smoother = Smoother::new(&off);
        let sent = camera_samples(&off, &mut smoother, 100.0, 3.0, openness, cameras);
        assert!(sent[140..180].iter().all(|sample| (sample.lids[0] - 0.05).abs() < 0.005), "{:?}", sent[140].lids);
        // A higher floor
        let high = Settings {
            camera_lid_floor: 0.4,
            lid_sync: 0.0,
            ..settings.clone()
        };
        let mut smoother = Smoother::new(&high);
        let sent = camera_samples(&high, &mut smoother, 100.0, 3.0, openness, cameras);
        assert!(sent[140..180].iter().all(|sample| (sample.lids[0] - 0.4).abs() < 0.005), "{:?}", sent[140].lids);
    }

    #[test]
    fn lid_sync_does_not_pull_a_floored_eye_below_the_floor() {
        // The right eye held shut (the camera sees it closed); the left squints along at the camera's 0.05. lid_sync
        // (0.4) would pull the floored 0.25 toward the shut eye's 0
        let settings = Settings { camera_lid_floor: 0.25, ..camera_settings() };
        let shut = |i: usize| (90..225).contains(&i);
        let openness = |i: usize| if shut(i) { [0.4, 0.2] } else { [0.8; 2] };
        let cameras = |i: usize| {
            Some(if shut(i) { camera_eyes([(0.05, 1.0, false), (0.0, 0.0, true)]) } else { camera(0.75, 0.0) })
        };
        let mut smoother = Smoother::new(&settings);
        let sent = camera_samples(&settings, &mut smoother, 100.0, 3.0, openness, cameras);
        assert!(sent[92..116].iter().all(|sample| sample.lids == [0.0; 2]));
        assert!(sent[150..225].iter().all(|sample| sample.lids[0] > 0.249 && sample.lids[1] == 0.0), "{:?}", sent[150].lids);
    }

    #[test]
    fn the_lid_floor_never_holds_a_blink_or_a_closed_eye_open() {
        let settings = camera_settings();
        // A 200 ms blink on the eye server while the camera, lagging, sees the eyes open at 0.05
        let mut smoother = Smoother::new(&settings);
        let sent = camera_samples(
            &settings,
            &mut smoother,
            100.0,
            1.0,
            |i| if (45..63).contains(&i) { [0.2; 2] } else { [0.8; 2] },
            |i| Some(if (45..63).contains(&i) { camera(0.05, 0.0) } else { camera(0.75, 0.0) }),
        );
        assert!(sent[46..64].iter().all(|sample| sample.lids == [0.0; 2]), "{:?}", sent[60].lids);
        // The camera sees the left eye closed while the eye server reads it open (without the close gate, which would
        // hold a "closed" in one eye this short back)
        let ungated = Settings { camera_close_gate: false, ..settings.clone() };
        let mut smoother = Smoother::new(&ungated);
        let sent = camera_samples(&ungated, &mut smoother, 100.0, 1.0, |_| [0.8; 2], |i| {
            Some(if (45..54).contains(&i) { camera_eyes([(0.0, 0.0, true), (0.75, 0.0, false)]) } else { camera(0.75, 0.0) })
        });
        assert!(sent[45..54].iter().all(|sample| sample.lids[0] == 0.0));
        // --raw: the same
        let raw = Settings { raw: true, ..settings.clone() };
        let mut smoother = Smoother::new(&raw);
        let sent = camera_samples(&raw, &mut smoother, 100.0, 0.2, |_| [0.2; 2], |_| Some(camera(0.05, 0.0)));
        assert!(sent.iter().all(|sample| sample.lids == [0.0; 2]));
        // ...where it floors an eye the eye server does not read closing (here 0.48, under a floor of 0.6)
        let floor = Settings { camera_lid_floor: 0.6, lid_sync: 0.0, ..raw.clone() };
        let mut smoother = Smoother::new(&floor);
        let sent = camera_samples(&floor, &mut smoother, 100.0, 0.1, |_| [0.8, 0.62], |_| Some(camera(0.3, 0.0)));
        assert_eq!(sent[0].lids, [LID_RELAXED, 0.6]);
    }

    #[test]
    fn an_eye_the_camera_stops_seeing_while_widened_goes_back_to_relaxed() {
        let settings = Settings {
            lid_calibration: false,
            ..settings()
        };
        let mut smoother = saturated_smoother(&settings);
        let wide = camera_samples(&settings, &mut smoother, 100.0, 1.0, |_| [1.0; 2], |_| Some(camera(1.0, 0.0)));
        assert!(wide.last().unwrap().lids.iter().all(|lid| *lid > 0.99));
        // The left eye's values go stale: it is capped again at once (a gap bridges only the raise to the camera's
        // eyelid, never its widening), though the right eye stays widened (and lid_sync would pull the left one toward
        // it)
        let left_stale = Live {
            fresh: [false, true],
            ..camera(1.0, 0.0)
        };
        let one = camera_samples(&settings, &mut smoother, 101.0, 0.5, |_| [1.0; 2], |_| Some(left_stale));
        assert!(one.iter().all(|sample| sample.lids[0] <= LID_RELAXED && sample.lids[1] > 0.9), "{:?}", one[0].lids);
        assert_eq!(one[0].camera.squint, [None, Some(0.0)]);
        // Both: capped, filters and all
        let none = camera_samples(&settings, &mut smoother, 101.5, 0.5, |_| [1.0; 2], |_| None);
        assert!(all_relaxed_or_below(&none));
        assert!(smoother.lids.iter().all(|filter| filter.value.unwrap() <= LID_RELAXED));
        // Back: the widening shows only once it has lasted again, not stuck from before
        let back = camera_samples(&settings, &mut smoother, 102.0, 1.0, |_| [1.0; 2], |_| Some(camera(1.0, 0.0)));
        assert!(all_relaxed_or_below(&back[..23]), "{:?}", back[22].lids);
        assert!(back.last().unwrap().lids.iter().all(|lid| *lid > 0.99));
    }

    #[test]
    fn camera_squint_and_pupils_are_smoothed_per_eye_and_start_over_after_a_gap() {
        let settings = settings();
        let mut cameras = CameraSmoother::default();
        let with = |squint: f32, pupil_mm: f32, dilation: f32| {
            let mut live = camera(0.9, squint);
            for eye in &mut live.eyes {
                (eye.pupil_mm, eye.pupil_dilation) = (pupil_mm, dilation);
            }
            live
        };
        // The first value as it is, then a share of each new one
        let (lids, values) = cameras.take(&settings, Some(&with(1.0, 4.0, 0.5)), true);
        assert_eq!(lids.map(|lid| lid.map(|lid| lid.lid)), [Some(0.9); 2]);
        assert_eq!(values.squint, [Some(1.0); 2]);
        assert_eq!((values.pupil_diameter, values.pupil_dilation), ([Some(4.0); 2], [Some(0.5); 2]));
        let (_, values) = cameras.take(&settings, Some(&with(0.0, 6.0, 1.0)), true);
        assert_eq!(values.squint, [Some(1.0 - CAMERA_SQUINT_ALPHA); 2]);
        assert_eq!(values.pupil_diameter, [Some(4.0 + 2.0 * CAMERA_PUPIL_ALPHA); 2]);
        assert_eq!(values.pupil_dilation, [Some(0.5 + 0.5 * CAMERA_PUPIL_ALPHA); 2]);
        // Unsmoothed with --raw
        let mut copy = cameras;
        let (_, raw) = copy.take(&settings, Some(&with(0.0, 6.0, 1.0)), false);
        assert_eq!((raw.squint, raw.pupil_diameter), ([Some(0.0); 2], [Some(6.0); 2]));
        // A gap in one eye: that eye starts over from its new value, the other carries on
        let left_gone = Live {
            fresh: [false, true],
            ..with(0.0, 6.0, 1.0)
        };
        let (lids, values) = cameras.take(&settings, Some(&left_gone), true);
        assert_eq!(lids.map(|lid| lid.map(|lid| lid.lid)), [None, Some(0.9)]);
        assert_eq!((values.squint[0], values.pupil_dilation[0]), (None, None));
        let (_, values) = cameras.take(&settings, Some(&with(0.0, 6.0, 1.0)), true);
        assert_eq!(values.squint[0], Some(0.0));
        assert!(values.squint[1].unwrap() > 0.0);
        // Without a wear calibration: no eyelid or squint, but the pupils
        let uncalibrated = Live {
            calib_state: 2,
            ..with(0.4, 6.0, 1.0)
        };
        let (lids, values) = cameras.take(&settings, Some(&uncalibrated), true);
        assert_eq!((lids, values.squint), ([None; 2], [None; 2]));
        assert!(values.pupil_dilation.iter().all(Option::is_some));
        // camera_lids off: nothing
        let off = Settings {
            camera_lids: false,
            ..settings.clone()
        };
        assert_eq!(cameras.take(&off, Some(&with(0.4, 6.0, 1.0)), true), ([None; 2], CameraValues::default()));
        // Squint and dilation are kept within 0..1
        let (_, values) = CameraSmoother::default().take(&settings, Some(&with(1.5, -1.0, -0.5)), true);
        assert_eq!((values.squint[0], values.pupil_diameter[0], values.pupil_dilation[0]), (Some(1.0), Some(0.0), Some(0.0)));
        // A gap in the eye samples starts the smoothing over too
        let mut smoother = Smoother::new(&settings);
        camera_samples(&settings, &mut smoother, 100.0, 0.1, |_| [0.8; 2], |_| Some(with(1.0, 4.0, 0.5)));
        let after = camera_samples(&settings, &mut smoother, 101.0, 0.05, |_| [0.8; 2], |_| Some(with(0.0, 4.0, 0.5)));
        assert_eq!(after[0].camera.squint, [Some(0.0); 2]);
    }

    /// `name`'s value among `messages`, if it is there.
    fn param(messages: &[(String, OscType)], name: &str) -> Option<f32> {
        messages.iter().find(|(addr, _)| addr == name).map(|(_, value)| match value {
            OscType::Float(value) => *value,
            other => panic!("{name}: {other:?}"),
        })
    }

    #[test]
    fn vrchat_gets_the_cameras_squint_and_pupils() {
        let camera = CameraValues {
            squint: [Some(0.2), Some(0.4)],
            pupil_dilation: [Some(0.5), Some(0.7)],
            pupil_diameter: [Some(4.0), Some(12.0)],
        };
        let messages = osc_messages(&settings(), &Sample { camera, ..sample() });
        let addrs: Vec<&str> = messages.iter().map(|(addr, _)| addr.as_str()).collect();
        // After the eye set as before
        assert_eq!(messages[..9], sent(&settings())[..]);
        assert_eq!(
            addrs[9..],
            [
                "/avatar/parameters/FT/v2/EyeSquintLeft",
                "/avatar/parameters/FT/v2/EyeSquintRight",
                "/avatar/parameters/FT/v2/EyeSquint",
                "/avatar/parameters/FT/v2/PupilDilation",
                "/avatar/parameters/FT/v2/PupilDiameterLeft",
                "/avatar/parameters/FT/v2/PupilDiameterRight",
                "/avatar/parameters/FT/v2/PupilDiameter",
            ]
        );
        let value = |name: &str| param(&messages, &format!("/avatar/parameters/FT/v2/{name}")).unwrap();
        let expected = [
            ("EyeSquintLeft", 0.2),
            ("EyeSquintRight", 0.4),
            ("EyeSquint", 0.3),
            ("PupilDilation", 0.6),
            // In cm, and at most 1
            ("PupilDiameterLeft", 0.4),
            ("PupilDiameterRight", 1.0),
            ("PupilDiameter", 0.7),
        ];
        for (name, expected) in expected {
            assert!((value(name) - expected).abs() < 1e-6, "{name}: {}", value(name));
        }
        // Only the eyes they are used for, and the combined ones from those
        let one_eye = CameraValues {
            squint: [None, Some(0.4)],
            pupil_dilation: [Some(0.5), None],
            pupil_diameter: [Some(4.0), None],
        };
        let messages = osc_messages(&settings(), &Sample { camera: one_eye, ..sample() });
        let value = |name: &str| param(&messages, &format!("/avatar/parameters/FT/v2/{name}"));
        assert_eq!((value("EyeSquintLeft"), value("EyeSquintRight"), value("EyeSquint")), (None, Some(0.4), Some(0.4)));
        assert_eq!((value("PupilDiameterLeft"), value("PupilDiameterRight")), (Some(0.4), None));
        assert_eq!((value("PupilDilation"), value("PupilDiameter")), (Some(0.5), Some(0.4)));
        // Not to the other outputs; the Steam Link names stay last; the prefix applies
        let etvr = Settings {
            output: OutputKind::Etvr,
            ..settings()
        };
        assert_eq!(osc_messages(&etvr, &Sample { camera, ..sample() }), sent(&etvr));
        let messages = osc_messages(&steamlink(), &Sample { camera, ..sample() });
        assert_eq!(messages.len(), 28);
        assert_eq!(messages[16..], steamlink_messages(sample().gaze, sample().lids)[..]);
        let bare = Settings {
            prefix: String::new(),
            ..settings()
        };
        assert_eq!(osc_messages(&bare, &Sample { camera, ..sample() })[9].0, "/avatar/parameters/v2/EyeSquintLeft");
    }

    #[test]
    fn livelink_mode_sends_the_pupils_alone_to_vrchat() {
        let camera = CameraValues {
            squint: [Some(0.3); 2],
            pupil_dilation: [Some(0.5), Some(0.7)],
            pupil_diameter: [Some(4.0), Some(12.0)],
        };
        let livelink = Settings {
            output: OutputKind::LiveLink,
            ..settings()
        };
        let messages = pupil_messages(&livelink, &camera);
        let addrs: Vec<&str> = messages.iter().map(|(addr, _)| addr.as_str()).collect();
        assert_eq!(
            addrs,
            [
                "/avatar/parameters/FT/v2/PupilDilation",
                "/avatar/parameters/FT/v2/PupilDiameterLeft",
                "/avatar/parameters/FT/v2/PupilDiameterRight",
                "/avatar/parameters/FT/v2/PupilDiameter",
            ]
        );
        // The same values the VRChat-direct path sends, nothing else
        let direct = osc_messages(&settings(), &Sample { camera, ..sample() });
        assert!(messages.iter().all(|message| direct.contains(message)));
        assert!(messages.iter().all(|(addr, _)| !addr.contains("EyeLid") && !addr.contains("EyeSquint")));
        assert!(pupil_messages(&livelink, &CameraValues { pupil_dilation: [None; 2], pupil_diameter: [None; 2], ..camera }).is_empty());
        let bare = Settings {
            prefix: String::new(),
            ..livelink.clone()
        };
        assert_eq!(pupil_messages(&bare, &camera)[0].0, "/avatar/parameters/v2/PupilDilation");
        // Only in LiveLink mode, while sending, with the setting on; to port 9000 of the LiveLink target's host
        assert_eq!(pupil_stream(&livelink), Some(("auto", 9000)));
        assert!(pupil_stream(&settings()).is_none());
        assert!(pupil_stream(&Settings { output: OutputKind::Etvr, ..livelink.clone() }).is_none());
        assert!(pupil_stream(&Settings { sending: false, ..livelink.clone() }).is_none());
        assert!(pupil_stream(&Settings { pupils_to_vrchat: false, ..livelink.clone() }).is_none());
        assert!(pupil_target(&livelink) == Target::SteamLink { port: 9000 });
        let fixed = Settings {
            host: "192.168.0.60".into(),
            port: Some(11112),
            ..livelink.clone()
        };
        assert!(pupil_target(&fixed) == Target::Fixed { host: "192.168.0.60".into(), port: 9000 });
        assert_eq!(pupil_stream(&fixed), Some(("192.168.0.60", 9000)));
    }

    #[test]
    fn livelink_mode_sends_the_pupils_next_to_each_packet() {
        use eyecam_live::tests::{TempDir, TestEye, live_bytes, write_live};
        let dir = TempDir::new("pupils");
        let path = dir.0.join("live");
        let write = || {
            let now = eyecam_live::monotonic_ns();
            let mut eyes = [TestEye::at(now, 0.9, 0.2); 2];
            eyes[1].pupil_mm = 5.0;
            write_live(&path, &live_bytes(7, 1, true, eyes));
        };
        let livelink_listener = UdpSocket::bind("127.0.0.1:0").unwrap();
        let vrchat_listener = UdpSocket::bind("127.0.0.1:0").unwrap();
        let live = Settings {
            output: OutputKind::LiveLink,
            host: "127.0.0.1".into(),
            port: Some(livelink_listener.local_addr().unwrap().port()),
            lid_calibration: false,
            ..settings()
        };
        let mut bridge = test_bridge(live.clone());
        bridge.camera = LiveReader::new(path.clone(), Instant::now());
        // Standing in for VRChat's 9000
        let vrchat_port = vrchat_listener.local_addr().unwrap().port();
        bridge.pupil_output = Output::new(Target::Fixed { host: "127.0.0.1".into(), port: vrchat_port });
        bridge.pupil_output.refresh();
        // No pupils yet: the Live Link packet alone
        bridge.on_sample(reading(0, [0.0, 0.0], [0.8; 2])).unwrap();
        assert!(receive(&livelink_listener).is_some());
        assert!(osc_received(&vrchat_listener).is_empty());
        let json = serde_json::to_value(bridge.status(None, None)).unwrap();
        assert_eq!(json["pupil_target"], format!("127.0.0.1:{vrchat_port}"));
        assert!(json["sent"]["pupil_dilation"].is_null());
        // With them: the four pupil parameters, and nothing of the eyelids or gaze
        write();
        // (looked for again at once, rather than a second later)
        bridge.camera = LiveReader::new(path.clone(), Instant::now());
        bridge.livelink_throttle = Throttle::default();
        bridge.on_sample(reading(1, [0.0, 0.0], [0.8; 2])).unwrap();
        assert!(livelink::tests::module_eyes(&receive(&livelink_listener).unwrap()).is_some());
        let received = osc_received(&vrchat_listener);
        let addrs: Vec<&str> = received.iter().map(|(addr, _)| addr.as_str()).collect();
        assert_eq!(addrs.len(), 4, "{addrs:?}");
        assert!(addrs.iter().all(|addr| addr.starts_with("/avatar/parameters/FT/v2/Pupil")), "{addrs:?}");
        assert_eq!(param(&received, "/avatar/parameters/FT/v2/PupilDiameterRight"), Some(0.5));
        assert!((param(&received, "/avatar/parameters/FT/v2/PupilDiameter").unwrap() - 0.45).abs() < 1e-6);
        let json = serde_json::to_value(bridge.status(None, None)).unwrap();
        assert_eq!(json["sent"]["pupil_dilation"], 0.5);
        assert!(json["sent"]["squint"].is_null());
        // Held back with the Live Link packet by the throttle: two samples at once, one packet and one set of pupils
        write();
        bridge.on_sample(reading(2, [0.0, 0.0], [0.8; 2])).unwrap();
        bridge.on_sample(reading(3, [0.0, 0.0], [0.8; 2])).unwrap();
        assert!(receive(&livelink_listener).is_some() && receive(&livelink_listener).is_none());
        assert_eq!(osc_received(&vrchat_listener).len(), 4);
        // Turned off: nothing more, and nothing to set back
        let off = Settings {
            pupils_to_vrchat: false,
            ..live.clone()
        };
        bridge.apply(reload(&off)).unwrap();
        bridge.pupil_output = Output::new(Target::Fixed { host: "127.0.0.1".into(), port: vrchat_port });
        bridge.pupil_output.refresh();
        receive(&livelink_listener);
        write();
        bridge.livelink_throttle = Throttle::default();
        bridge.on_sample(reading(4, [0.0, 0.0], [0.8; 2])).unwrap();
        assert!(receive(&livelink_listener).is_some());
        assert!(osc_received(&vrchat_listener).is_empty());
        let json = serde_json::to_value(bridge.status(None, None)).unwrap();
        assert!(json["pupil_target"].is_null() && json["sent"]["pupil_dilation"].is_null());
        bridge.on_lost("test").unwrap();
        assert!(osc_received(&vrchat_listener).is_empty());
    }

    #[test]
    fn frameeyeosc_packets_carry_the_sample_with_its_camera_values() {
        let camera = CameraValues {
            squint: [Some(0.3), None],
            pupil_dilation: [Some(0.25), Some(0.75)],
            pupil_diameter: [Some(4.0), Some(3.0)],
        };
        let seen = Sample { camera, ..sample() };
        let packet = feo_packet(&seen, 5, 41, 1_700_000_000_000_000);
        assert_eq!(packet[8..12], 41u32.to_le_bytes());
        assert_eq!(packet[72..76], 5u32.to_le_bytes());
        assert_eq!(packet[16..24], 1_700_000_000_000_000u64.to_le_bytes());
        let ([left, right], pupils) = feo::tests::module_eyes(&packet).unwrap();
        // The gaze and eyelids the Live Link packet carries, as VRCFT reads them back
        let [livelink_left, livelink_right] = livelink::tests::module_eyes(&livelink_packet(&seen, 0.0)).unwrap();
        for (eye, (openness, wide, x, y)) in [(left, livelink_left), (right, livelink_right)] {
            assert_eq!(eye.gaze, [x, y]);
            assert!((eye.openness - openness).abs() < 1e-6 && (eye.wide - wide).abs() < 1e-6, "{eye:?}");
        }
        assert!((left.vrcft_lid() - seen.lids[0]).abs() < 1e-6);
        // The squint of the eye whose eyelid is the camera's; the pupils as --output vrchat sends them
        assert_eq!((left.squint, right.squint), (0.3, 0.0));
        let direct = osc_messages(&settings(), &seen);
        let [diameter_left, diameter_right, dilation] = pupils.unwrap();
        let names = ["PupilDiameterLeft", "PupilDiameterRight", "PupilDilation"];
        for (name, value) in names.into_iter().zip([diameter_left, diameter_right, dilation]) {
            let expected = param(&direct, &format!("/avatar/parameters/FT/v2/{name}")).unwrap();
            assert!((value - expected).abs() < 1e-6, "{name}: {value} vs {expected}");
        }
        // Without the eye cameras: no squint or pupils
        let (_, pupils) = feo::tests::module_eyes(&feo_packet(&sample(), 1, 0, 0)).unwrap();
        assert!(pupils.is_none());
        assert_eq!(packet[12..16], (1u32 | 2 | 8 | 16).to_le_bytes());
        assert_eq!(feo_packet(&sample(), 1, 0, 0)[12..16], 1u32.to_le_bytes());
    }

    #[test]
    fn frameeyeosc_mode_sends_every_sample_and_says_when_the_eyes_stop() {
        let listener = UdpSocket::bind("127.0.0.1:0").unwrap();
        let vrchat_listener = UdpSocket::bind("127.0.0.1:0").unwrap();
        let module = Settings {
            output: OutputKind::FrameEyeOsc,
            host: "127.0.0.1".into(),
            port: Some(listener.local_addr().unwrap().port()),
            lid_calibration: false,
            ..settings()
        };
        let mut bridge = test_bridge(module.clone());
        // Nothing goes straight to VRChat in this mode
        assert!(pupil_stream(&module).is_none());
        let vrchat_port = vrchat_listener.local_addr().unwrap().port();
        bridge.pupil_output = Output::new(Target::Fixed { host: "127.0.0.1".into(), port: vrchat_port });
        bridge.pupil_output.refresh();
        let idle = |packet: &[u8]| packet[12..16] == [0; 4] && feo::tests::module_eyes(packet).is_some();
        let sequence = |packet: &[u8]| u32::from_le_bytes(packet[8..12].try_into().unwrap());
        // Before any eye data: a packet without TRACKING, twice a second
        bridge.keep_feo_alive();
        let first = receive(&listener).unwrap();
        assert!(idle(&first) && sequence(&first) == 0);
        bridge.keep_feo_alive();
        assert!(receive(&listener).is_none());
        // Every sample goes out at once (no rate limit), numbered on
        let look = [(std::f32::consts::PI / 8.0).tan(), -(std::f32::consts::PI / 8.0).tan()];
        for index in 0..5 {
            bridge.on_sample(reading(index, look, [1.0, 0.55])).unwrap();
        }
        for index in 0..5 {
            let packet = receive(&listener).unwrap();
            assert_eq!(packet.len(), feo::PACKET_LEN);
            assert_eq!(packet[72..76], 7u32.to_le_bytes());
            assert_eq!(sequence(&packet), index + 1);
            assert!(!idle(&packet));
            let ([left, right], _) = feo::tests::module_eyes(&packet).unwrap();
            assert!(left.gaze[0] > 0.0 && left.gaze[1] < 0.0 && right.openness < 1.0);
        }
        assert!(receive(&listener).is_none());
        assert_eq!((bridge.received.len(), bridge.sent.len()), (5, 5));
        // Tracking runs, so nothing in between
        bridge.keep_feo_alive();
        assert!(receive(&listener).is_none());
        // The packet is the sample's (time stamp aside)
        bridge.on_sample(reading(5, look, [1.0, 0.55])).unwrap();
        let packet = receive(&listener).unwrap();
        let sample = bridge.latest.as_ref().unwrap();
        assert_eq!(packet[..16], feo_packet(sample, 7, 6, 0)[..16]);
        assert_eq!(packet[24..], feo_packet(sample, 7, 6, 0)[24..]);
        // Losing the eye data: one idle packet at once, then again only after FEO_IDLE_INTERVAL
        bridge.on_lost("test").unwrap();
        assert!(idle(&receive(&listener).unwrap()));
        bridge.keep_feo_alive();
        assert!(receive(&listener).is_none());
        bridge.feo_idle = Some(Instant::now() - FEO_IDLE_INTERVAL);
        bridge.keep_feo_alive();
        assert!(idle(&receive(&listener).unwrap()));
        // Pausing while tracking: one idle packet, and nothing after it
        bridge.on_sample(reading(6, look, [1.0, 0.55])).unwrap();
        receive(&listener).unwrap();
        let paused = Settings {
            sending: false,
            ..module.clone()
        };
        bridge.apply(reload(&paused)).unwrap();
        assert!(idle(&receive(&listener).unwrap()));
        bridge.on_sample(reading(7, look, [1.0, 0.55])).unwrap();
        bridge.on_lost("test").unwrap();
        bridge.feo_idle = None;
        bridge.keep_feo_alive();
        assert!(receive(&listener).is_none());
        // Switching to LiveLink while tracking: a last idle packet, then Live Link packets
        bridge.apply(reload(&module)).unwrap();
        bridge.on_sample(reading(8, look, [1.0, 0.55])).unwrap();
        assert!(!idle(&receive(&listener).unwrap()));
        let livelink = Settings {
            output: OutputKind::LiveLink,
            pupils_to_vrchat: false,
            ..module.clone()
        };
        bridge.apply(reload(&livelink)).unwrap();
        assert!(idle(&receive(&listener).unwrap()));
        bridge.on_sample(reading(9, look, [1.0, 0.55])).unwrap();
        assert!(livelink::tests::module_eyes(&receive(&listener).unwrap()).is_some());
        bridge.feo_idle = None;
        bridge.keep_feo_alive();
        while receive(&listener).is_some_and(|packet| packet[..4] != *b"FEO1") {}
        assert!(receive(&listener).is_none());
        assert!(osc_received(&vrchat_listener).is_empty());
    }

    #[test]
    fn frameeyeosc_mode_sends_the_eye_cameras_pupils_in_the_packet() {
        use eyecam_live::tests::{TempDir, TestEye, live_bytes, write_live};
        let dir = TempDir::new("feo-pupils");
        let path = dir.0.join("live");
        let listener = UdpSocket::bind("127.0.0.1:0").unwrap();
        let module = Settings {
            output: OutputKind::FrameEyeOsc,
            host: "127.0.0.1".into(),
            port: Some(listener.local_addr().unwrap().port()),
            lid_calibration: false,
            ..settings()
        };
        let mut bridge = test_bridge(module);
        bridge.camera = LiveReader::new(path.clone(), Instant::now());
        bridge.on_sample(reading(0, [0.0, 0.0], [0.8; 2])).unwrap();
        let (_, pupils) = feo::tests::module_eyes(&receive(&listener).unwrap()).unwrap();
        assert!(pupils.is_none());
        let json = serde_json::to_value(bridge.status(None, None)).unwrap();
        assert!(json["sent"]["pupil_dilation"].is_null() && json["pupil_target"].is_null());
        let now = eyecam_live::monotonic_ns();
        let mut eyes = [TestEye::at(now, 0.9, 0.2); 2];
        eyes[1].pupil_mm = 5.0;
        write_live(&path, &live_bytes(7, 1, true, eyes));
        bridge.camera = LiveReader::new(path.clone(), Instant::now());
        bridge.on_sample(reading(1, [0.0, 0.0], [0.8; 2])).unwrap();
        let packet = receive(&listener).unwrap();
        let (_, pupils) = feo::tests::module_eyes(&packet).unwrap();
        let [diameter_left, diameter_right, dilation] = pupils.unwrap();
        assert!((diameter_left - 0.4).abs() < 1e-6 && (diameter_right - 0.5).abs() < 1e-6);
        assert!((dilation - 0.5).abs() < 1e-6);
        // What the panel shows as sent: the pupils, and the squint the packet carries
        let json = serde_json::to_value(bridge.status(None, None)).unwrap();
        assert_eq!(json["sent"]["pupil_dilation"], 0.5);
        assert_eq!(json["output"], "frameeyeosc");
        let squint = bridge.latest.as_ref().unwrap().camera.squint;
        assert_eq!(json["sent"]["squint"].is_null(), squint == [None; 2]);
        assert_eq!(packet[12] & 0b110 != 0, squint != [None; 2]);
    }

    #[test]
    fn a_squint_that_stops_is_set_back_to_zero_once() {
        let settings = settings();
        let zero = |names: &[&str]| -> Vec<(String, OscType)> {
            names.iter().map(|name| (format!("/avatar/parameters/FT/v2/{name}"), OscType::Float(0.0))).collect()
        };
        assert!(squint_release(&settings, [false; 2], [None; 2]).is_empty());
        assert!(squint_release(&settings, [true; 2], [Some(0.3); 2]).is_empty());
        assert!(squint_release(&settings, [false; 2], [Some(0.3), None]).is_empty());
        // One eye stops: that eye; the combined one still goes out with the other
        assert_eq!(squint_release(&settings, [true; 2], [None, Some(0.3)]), zero(&["EyeSquintLeft"]));
        // The last one stops: the combined one too
        assert_eq!(squint_release(&settings, [false, true], [None; 2]), zero(&["EyeSquintRight", "EyeSquint"]));
        assert_eq!(squint_release(&settings, [true; 2], [None; 2]), zero(&["EyeSquintLeft", "EyeSquintRight", "EyeSquint"]));
    }

    /// The OSC messages that arrive until none has for 100 ms.
    #[test]
    fn pupil_bits_match_vrcfts_binary_parameters() {
        // (int)(value * 2^bits), bit k as PupilDilation{2^k}; above 0.99999 all of them
        assert_eq!(binary_bits(0.5, 3), [false, false, true]);
        assert_eq!(binary_bits(1.0, 3), [true; 3]);
        assert_eq!(binary_bits(0.0, 3), [false; 3]);
        assert_eq!(binary_bits(0.99999, 4), [true; 4]);
        assert_eq!(binary_bits(1.5, 2), [true; 2]);
        // Never negative: no bits (and no Negative parameter)
        assert_eq!(binary_bits(-0.5, 2), [false; 2]);
        for bits in 1..=4u8 {
            let steps = f32::from(1u8 << bits);
            assert_eq!(binary_bits(0.0, bits), vec![false; usize::from(bits)]);
            assert_eq!(binary_bits(1.0, bits), vec![true; usize::from(bits)]);
            // Each step's lower edge is that number, the value just below it the one before
            for big in 1..(1u32 << bits) {
                let at = big as f32 / steps;
                let number = |bools: Vec<bool>| bools.iter().rev().fold(0, |n, &bit| n * 2 + u32::from(bit));
                assert_eq!(number(binary_bits(at, bits)), big, "{bits} bits at {at}");
                assert_eq!(number(binary_bits(at - 1e-4, bits)), big - 1, "{bits} bits below {at}");
            }
        }
        assert_eq!(binary_bits(0.25, 2), [true, false]);
        assert_eq!(binary_bits(0.9374, 4), [false, true, true, true]);
    }

    #[test]
    fn pupil_bits_go_out_when_they_change_and_all_again_each_second() {
        let three = Settings {
            pupil_bits: 3,
            ..settings()
        };
        let names = |messages: &[(String, OscType)]| -> Vec<(String, bool)> {
            messages
                .iter()
                .map(|(addr, value)| match value {
                    OscType::Bool(bit) => (addr.trim_start_matches("/avatar/parameters/FT/v2/").to_owned(), *bit),
                    other => panic!("{addr}: {other:?}"),
                })
                .collect()
        };
        let bit = |name: &str, on: bool| (name.to_owned(), on);
        let mut bits = PupilBits::default();
        let start = Instant::now();
        // All of them first
        assert_eq!(
            names(&bits.messages(&three, Some(0.5), start)),
            [bit("PupilDilation1", false), bit("PupilDilation2", false), bit("PupilDilation4", true)]
        );
        // The same: nothing; a change: only the bits that changed (4 -> 3)
        assert!(bits.messages(&three, Some(0.55), start + Duration::from_millis(10)).is_empty());
        assert_eq!(
            names(&bits.messages(&three, Some(0.4), start + Duration::from_millis(20))),
            [bit("PupilDilation1", true), bit("PupilDilation2", true), bit("PupilDilation4", false)]
        );
        assert_eq!(
            names(&bits.messages(&three, Some(0.5), start + Duration::from_millis(30))),
            [bit("PupilDilation1", false), bit("PupilDilation2", false), bit("PupilDilation4", true)]
        );
        assert_eq!(
            names(&bits.messages(&three, Some(0.25), start + Duration::from_millis(40))),
            [bit("PupilDilation2", true), bit("PupilDilation4", false)]
        );
        // No pupil: nothing, and nothing set back (the avatar holds them)
        assert!(bits.messages(&three, None, start + Duration::from_millis(50)).is_empty());
        assert!(bits.messages(&three, Some(0.25), start + Duration::from_millis(60)).is_empty());
        // A second after the last full set: all of them again, unchanged or not
        assert!(bits.messages(&three, Some(0.25), start + Duration::from_millis(999)).is_empty());
        assert_eq!(bits.messages(&three, Some(0.25), start + Duration::from_secs(1)).len(), 3);
        assert!(bits.messages(&three, Some(0.25), start + Duration::from_millis(1500)).is_empty());
        assert_eq!(bits.messages(&three, Some(0.25), start + Duration::from_secs(2)).len(), 3);
        // Another count, or a reset (settings or target changed): all of them at once
        let one = Settings {
            pupil_bits: 1,
            ..settings()
        };
        assert_eq!(
            names(&bits.messages(&one, Some(0.25), start + Duration::from_millis(2100))),
            [bit("PupilDilation1", false)]
        );
        bits.reset();
        assert_eq!(bits.messages(&one, Some(0.25), start + Duration::from_millis(2200)).len(), 1);
        // 0: the float only; the prefix applies as to the float
        assert!(PupilBits::default().messages(&settings(), Some(0.5), start).is_empty());
        let bare = Settings {
            prefix: String::new(),
            ..one
        };
        assert_eq!(PupilBits::default().messages(&bare, Some(1.0), start)[0].0, "/avatar/parameters/v2/PupilDilation1");
    }

    #[test]
    fn pupil_bits_go_wherever_the_pupils_go_to_vrchat() {
        use eyecam_live::tests::{TempDir, TestEye, live_bytes, write_live};
        let dir = TempDir::new("pupil-bits");
        let path = dir.0.join("live");
        let write = || {
            let now = eyecam_live::monotonic_ns();
            write_live(&path, &live_bytes(7, 1, true, [TestEye::at(now, 0.9, 0.2); 2]));
        };
        for output in [OutputKind::Vrchat, OutputKind::LiveLink] {
            // VRChat directly: on the output; LiveLink: next to the packet, on the pupils' own sender
            let target_listener = UdpSocket::bind("127.0.0.1:0").unwrap();
            let vrchat_listener = UdpSocket::bind("127.0.0.1:0").unwrap();
            let direct = output == OutputKind::Vrchat;
            let settings = Settings {
                output,
                host: "127.0.0.1".into(),
                port: Some(if direct { &vrchat_listener } else { &target_listener }.local_addr().unwrap().port()),
                lid_calibration: false,
                pupil_bits: 2,
                ..settings()
            };
            let mut bridge = test_bridge(settings);
            bridge.pupil_output = Output::new(Target::Fixed {
                host: "127.0.0.1".into(),
                port: vrchat_listener.local_addr().unwrap().port(),
            });
            bridge.pupil_output.refresh();
            write();
            bridge.camera = LiveReader::new(path.clone(), Instant::now());
            bridge.on_sample(reading(0, [0.0, 0.0], [0.8; 2])).unwrap();
            let received = osc_received(&vrchat_listener);
            let is_bit = |addr: &str| addr.ends_with("PupilDilation1") || addr.ends_with("PupilDilation2");
            let bits: Vec<_> = received.iter().filter(|(addr, _)| is_bit(addr)).collect();
            // The dilation is 0.5: 2 of 4 steps, the float next to the bits
            assert_eq!(
                bits,
                [
                    &("/avatar/parameters/FT/v2/PupilDilation1".to_owned(), OscType::Bool(false)),
                    &("/avatar/parameters/FT/v2/PupilDilation2".to_owned(), OscType::Bool(true)),
                ],
                "{output:?}"
            );
            assert_eq!(param(&received, "/avatar/parameters/FT/v2/PupilDilation"), Some(0.5), "{output:?}");
            // Unchanged within the second: the float again, the bits not
            write();
            bridge.livelink_throttle = Throttle::default();
            bridge.on_sample(reading(1, [0.0, 0.0], [0.8; 2])).unwrap();
            let received = osc_received(&vrchat_listener);
            assert!(param(&received, "/avatar/parameters/FT/v2/PupilDilation").is_some(), "{output:?}");
            assert!(!received.iter().any(|(addr, _)| is_bit(addr)), "{output:?}");
        }
    }

    fn osc_received(listener: &UdpSocket) -> Vec<(String, OscType)> {
        listener.set_read_timeout(Some(Duration::from_millis(100))).unwrap();
        let mut buffer = [0u8; 1024];
        let mut received = Vec::new();
        while let Ok(size) = listener.recv(&mut buffer) {
            let (_, packet) = rosc::decoder::decode_udp(&buffer[..size]).unwrap();
            let OscPacket::Message(message) = packet else { panic!("not a message") };
            received.push((message.addr, message.args[0].clone()));
        }
        received
    }

    #[test]
    fn the_cameras_values_go_out_and_their_squint_is_set_back_when_it_stops() {
        use eyecam_live::tests::{TempDir, TestEye, live_bytes, write_live};
        let dir = TempDir::new("bridge");
        let path = dir.0.join("live");
        let write = |calib_state: u32| {
            let now = eyecam_live::monotonic_ns();
            write_live(&path, &live_bytes(7, calib_state, true, [TestEye::at(now, 0.9, 0.6); 2]));
        };
        let listener = UdpSocket::bind("127.0.0.1:0").unwrap();
        let vrchat = Settings {
            host: "127.0.0.1".into(),
            port: Some(listener.local_addr().unwrap().port()),
            lid_calibration: false,
            ..settings()
        };
        let mut bridge = test_bridge(vrchat.clone());
        // No live file: nothing about the camera, in the messages or the status
        bridge.on_sample(reading(0, [0.0, 0.0], [0.8; 2])).unwrap();
        let received = osc_received(&listener);
        assert_eq!(received.len(), 9);
        let json = serde_json::to_value(bridge.status(None, None)).unwrap();
        assert!(json["camera"].is_null());
        assert!(json["sent"]["squint"].is_null() && json["sent"]["pupil_dilation"].is_null());

        bridge.camera = LiveReader::new(path.clone(), Instant::now());
        let squint = |name: &str| format!("/avatar/parameters/FT/v2/EyeSquint{name}");
        write(1);
        bridge.on_sample(reading(1, [0.0, 0.0], [0.8; 2])).unwrap();
        let received = osc_received(&listener);
        assert_eq!(param(&received, &squint("Left")), Some(0.6));
        assert_eq!(param(&received, &squint("")), Some(0.6));
        assert_eq!(param(&received, "/avatar/parameters/FT/v2/PupilDiameterRight"), Some(0.4));
        let json = serde_json::to_value(bridge.status(None, None)).unwrap();
        let camera = &json["camera"];
        assert_eq!((camera["present"].as_bool(), camera["calib_state"].as_u64()), (Some(true), Some(1)));
        assert_eq!(camera["used"], serde_json::json!([true, true]));
        assert_eq!(camera["pupil_used"], serde_json::json!([true, true]));
        assert!(camera["error"].is_null() && camera["recalib_suggested"] == false);
        assert_eq!(json["sent"]["squint"].as_array().unwrap().len(), 2);
        assert_eq!(json["sent"]["squint"][0].as_f64().unwrap() as f32, 0.6);
        assert_eq!(json["sent"]["pupil_dilation"], 0.5);

        // The wear calibration goes: the squint is set back to 0, once; the pupils go on
        write(0);
        bridge.on_sample(reading(2, [0.0, 0.0], [0.8; 2])).unwrap();
        let received = osc_received(&listener);
        for name in ["Left", "Right", ""] {
            assert_eq!(param(&received, &squint(name)), Some(0.0), "{name}");
        }
        assert!(param(&received, "/avatar/parameters/FT/v2/PupilDilation").is_some());
        bridge.on_sample(reading(3, [0.0, 0.0], [0.8; 2])).unwrap();
        let received = osc_received(&listener);
        assert!(received.iter().all(|(addr, _)| !addr.contains("EyeSquint")), "{received:?}");
        let json = serde_json::to_value(bridge.status(None, None)).unwrap();
        assert_eq!(json["camera"]["used"], serde_json::json!([false, false]));
        assert!(json["sent"]["squint"].is_null());

        // Losing the eye data sets it back too
        write(1);
        bridge.on_sample(reading(4, [0.0, 0.0], [0.8; 2])).unwrap();
        osc_received(&listener);
        bridge.on_lost("test").unwrap();
        let received = osc_received(&listener);
        assert_eq!(received[0], ("/avatar/parameters/FT/EyeTrackingActive".into(), OscType::Bool(false)));
        assert_eq!(received[1..], [squint("Left"), squint("Right"), squint("")].map(|addr| (addr, OscType::Float(0.0))));
        bridge.on_lost("test").unwrap();
        assert!(osc_received(&listener).iter().all(|(addr, _)| !addr.contains("EyeSquint")));

        // And so does pausing, or moving VRChat elsewhere: where it went
        for changed in [Settings { sending: false, ..vrchat.clone() }, Settings { prefix: "/Other".into(), ..vrchat.clone() }] {
            bridge.apply(reload(&vrchat)).unwrap();
            write(1);
            bridge.on_sample(reading(5, [0.0, 0.0], [0.8; 2])).unwrap();
            osc_received(&listener);
            bridge.apply(reload(&changed)).unwrap();
            let received = osc_received(&listener);
            assert_eq!(param(&received, &squint("")), Some(0.0), "{received:?}");
            assert!(bridge.squint_out == [false; 2]);
        }

        // A file that isn't right: there, with the reason
        write_live(&path, &[0u8; 200]);
        bridge.camera = LiveReader::new(path.clone(), Instant::now());
        bridge.read_camera();
        let json = serde_json::to_value(bridge.status(None, None)).unwrap();
        assert_eq!(json["camera"]["present"], false);
        assert!(json["camera"]["error"].as_str().unwrap().contains("magic"), "{}", json["camera"]);
    }
}
