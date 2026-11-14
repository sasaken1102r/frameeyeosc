//! The panel's live eye values: its debug gaze dots and its eye preview. While `gaze_debug_dots` is on, or while the
//! panel listens on the socket (it binds it only while its eye preview is open, and removes it when that closes), every
//! processed sample goes to the panel as one datagram on a Unix socket in the status folder. Nothing leaves the device,
//! nothing is sent otherwise, and nothing here changes what goes to VRChat or VRCFT: these are copies for display.
//!
//! A datagram is PACKET_SIZE bytes, little-endian: b"FEOD", version 3, flags (bit 0: per-eye gaze is sent; bit 1: the
//! user's gaze scale is in use, not 1 on all sides; bit 2: brows are sent, each eye's at 34), 2 reserved zero bytes,
//! the sample time (f64), the six gaze values as sent and the six before smoothing (f32 each: left x/y, right x/y,
//! combined x/y; 1.0 = 45°), each eye's values (EYE_SIZE bytes; the left eye first, anatomical; below), then at
//! UNSCALED_AT the six gaze values as sent but before the user's gaze scale (the same as the gaze as sent while bit 1
//! is clear). Each eye's values:
//!   0  f32  eyelid as sent (VRCFT scale: 0 closed, 0.75 relaxed open, 1 widened)
//!   4  f32  Valve's openness as read       8  f32  ...after the eye's scale (what the panel's raw bar shows)
//!   12 f32  the eye camera's eyelid (eyecam-rec's, VRCFT scale; NaN without fresh camera values for the eye)
//!   16 f32  squint as sent (NaN when none goes out)     20 f32  pupil diameter as sent (mm; NaN when none)
//!   24 f32  the eye camera's squint (NaN as at 12)       28 f32  the eye camera's pupil (mm; NaN as at 12)
//!   32 u8   where the sent eyelid came from (LidFrom: 0 the eye server's usual mix, 1.. the camera overrode it)
//!   33 u8   flags (bit 0: the eye camera reads the eye closed)
//!   34 u16  the brow as sent (brow_from_widen: 0 = 0.0 at rest .. 65535 = 1.0 raised all the way); 0 while flag bit 2
//!           is clear (no brows go out: brow_from_widen is off, or the output takes none)
//! Version 3 is version 2 (136 bytes) with flag bit 1 and the gaze before the scale added at the end. Flag bit 2 and
//! the brows came later (0.8.0) in what were reserved zero bytes, with the same version and size: a panel from before
//! reads such a datagram as it did, and this panel reads an older frameeyeosc's (bit 2 clear) as without brows. The
//! panel also reads version 2 (taking the gaze as sent for the gaze before the scale); other versions it ignores (and
//! frameeyeosc never reads any).
//!
//! While gaze_debug_dots_compare is on too (and both eyes have a pupil map), each sample's datagram above is preceded by
//! a second, smaller one with the other gaze source's combined gaze (OTHER_SIZE bytes, little-endian): b"FEOC", version
//! 1, flags (bit 0: there is a value for this sample; bit 1: it is Valve's gaze, the pupils' while clear), 2 reserved
//! zero bytes, the sample time (f64, the same as in the sample's own datagram), then x and y (f32, 1.0 = 45°; 0 while
//! bit 0 is clear). A datagram of its own, so the sample's datagram stays as it was: a panel from before reads its
//! magic and size as not its own and ignores it, and this panel shows no other dot with a frameeyeosc from before (none
//! comes). Without the switch nothing of it is sent.
//!
//! The panel's gaze error check ("Measure gaze error") binds a second socket, MEASURE_SOCKET_NAME, while it runs; while
//! that is there (looked at as above), each sample also goes to it as MEASURE_SIZE bytes, little-endian: b"FEOM",
//! version 1, flags (bit 0: both eyes have a pupil map; bit 1: the sample counts, both eyes open enough as the eye
//! fit's captures count them; bit 2: Valve's gaze is held; bit 3: the pupils' gaze is still easing from the source
//! before, pupil.rs's blend, so not yet its own; a later bit in what was 0, read as clear by a panel from before;
//! bit 4: the gazes are before any smoothing, as below, where a frameeyeosc before it sent them as sent, smoothed),
//! the pupils' state (0 none without a map, then pupil.rs's
//! InUse: 1 both pupils, 2 the left only, 3 the right only, 4 held, 5 Valve's standing in), a zero byte, the sample
//! time (f64), Valve's combined x and y as gaze_source "valve" works it out, the pupils' as "pupil" does (f32, 1.0 =
//! 45°, both before any smoothing and the user's gaze scale, held while the eyes are shut; the pupils' 0 without a
//! map), each eye's openness as read (f32). Only the
//! panel's check reads it; nothing sent elsewhere changes.
//!
//! With the gaze guard on for the Valve gaze worked out (gaze_guard.rs: gaze_source "valve" with gaze_guard, or the
//! other source's run while gaze_source is "pupil"), Valve's x and y above are Valve's own gaze without the guard, and
//! each such sample's datagram is preceded by one more on the same socket, GUARD_SIZE bytes, little-endian: b"FEOG",
//! version 1, flags (bit 0: the witness was trusted for this sample; bit 1: both pupils gave the witness's gaze), 2
//! reserved zero bytes, the sample time (f64, the same as in the sample's datagram), the guarded gaze (Valve's with
//! the offset: what gaze_guard sends) as x and y the same way as Valve's, the offset added to the left eye's and to the
//! right eye's gaze (x, y each) and the witness's gaze (the pupils' combined gaze; 0 while bit 1 is clear), all f32
//! with 1.0 = 45°. A datagram of its own, so the sample's datagram stays as it was: a panel from before reads its magic
//! and size as not its own and ignores it. Without the guard nothing of it is sent.

use std::io::ErrorKind;
use std::os::unix::fs::FileTypeExt;
use std::os::unix::net::UnixDatagram;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

pub const SOCKET_NAME: &str = "gaze-dots.sock";
pub const MEASURE_SOCKET_NAME: &str = "gaze-measure.sock";
const MEASURE_MAGIC: &[u8; 4] = b"FEOM";
const MEASURE_VERSION: u8 = 1;
pub const MEASURE_SIZE: usize = 40;
const MEASURE_MAPPED: u8 = 1;
const MEASURE_USABLE: u8 = 2;
const MEASURE_VALVE_HELD: u8 = 4;
const MEASURE_PUPIL_BLENDING: u8 = 8;
const MEASURE_BEFORE_SMOOTHING: u8 = 16;
// The gaze guard's datagram on the measure socket.
const GUARD_MAGIC: &[u8; 4] = b"FEOG";
const GUARD_VERSION: u8 = 1;
pub const GUARD_SIZE: usize = 48;
const GUARD_TRUSTED: u8 = 1;
const GUARD_WITNESS: u8 = 2;
const MAGIC: &[u8; 4] = b"FEOD";
const VERSION: u8 = 3;
const EYES: usize = 64;
const EYE_SIZE: usize = 36;
// Where each eye's brow is in its values (u16), and flag bit 2 that says brows are there.
const BROW_AT: usize = 34;
const BROWS_FLAG: u8 = 4;
const UNSCALED_AT: usize = EYES + 2 * EYE_SIZE;
pub const PACKET_SIZE: usize = UNSCALED_AT + 24;
// The other gaze's datagram (gaze_debug_dots_compare).
const OTHER_MAGIC: &[u8; 4] = b"FEOC";
const OTHER_VERSION: u8 = 1;
pub const OTHER_SIZE: usize = 24;
const OTHER_HAS: u8 = 1;
const OTHER_VALVE: u8 = 2;
// While gaze_debug_dots is off, whether the panel listens is looked at this often (one stat).
const LOOK_INTERVAL: Duration = Duration::from_secs(1);

/// One eye's values for the panel.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DotEye {
    pub lid: f32,
    pub openness: f32,
    pub openness_scaled: f32,
    pub camera_lid: f32,
    pub squint: f32,
    pub pupil_mm: f32,
    pub camera_squint: f32,
    pub camera_pupil_mm: f32,
    pub lid_from: u8,
    pub camera_closed: bool,
}

/// One sample for the panel.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DotSample {
    pub time: f64,
    pub gaze: [f32; 6],
    pub raw_gaze: [f32; 6],
    // As sent, but before the user's gaze scale.
    pub unscaled_gaze: [f32; 6],
    pub independent: bool,
    // The user's gaze scale is in use (Settings::gaze_scaled).
    pub scaled: bool,
    pub eyes: [DotEye; 2],
    // Each brow as sent (left, right; 0..1), or None while no brows go out.
    pub brows: Option<[f32; 2]>,
}

/// The gaze source not in use, for the extra debug dot (gaze_debug_dots_compare): its combined gaze as that source
/// would have sent it (None for a sample where it gives none), and whether it is Valve's (the pupils' otherwise).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct OtherSample {
    pub time: f64,
    pub gaze: Option<[f32; 2]>,
    pub valve: bool,
}

pub fn encode_other(other: &OtherSample) -> [u8; OTHER_SIZE] {
    let mut packet = [0u8; OTHER_SIZE];
    packet[..4].copy_from_slice(OTHER_MAGIC);
    packet[4] = OTHER_VERSION;
    packet[5] = if other.valve { OTHER_VALVE } else { 0 };
    packet[8..16].copy_from_slice(&other.time.to_le_bytes());
    if let Some([x, y]) = other.gaze {
        packet[5] |= OTHER_HAS;
        packet[16..20].copy_from_slice(&x.to_le_bytes());
        packet[20..24].copy_from_slice(&y.to_le_bytes());
    }
    packet
}

#[cfg(test)]
pub fn decode_other(packet: &[u8]) -> Option<OtherSample> {
    if packet.len() != OTHER_SIZE || &packet[..4] != OTHER_MAGIC || packet[4] != OTHER_VERSION {
        return None;
    }
    let f32_at = |at: usize| f32::from_le_bytes(packet[at..at + 4].try_into().unwrap());
    Some(OtherSample {
        time: f64::from_le_bytes(packet[8..16].try_into().ok()?),
        gaze: (packet[5] & OTHER_HAS != 0).then(|| [f32_at(16), f32_at(20)]),
        valve: packet[5] & OTHER_VALVE != 0,
    })
}

/// One sample for the panel's gaze error check: Valve's and the pupils' combined gaze, each as its gaze_source sends it
/// (before the user's gaze scale).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct MeasureSample {
    pub time: f64,
    pub mapped: bool,
    pub usable: bool,
    pub valve_held: bool,
    pub pupil_blending: bool,
    pub before_smoothing: bool,
    // 0 without a map; else 1 + pupil.rs's InUse in its order (Pupil, Left, Right, Hold, Valve).
    pub pupil_state: u8,
    pub valve: [f32; 2],
    pub pupil: [f32; 2],
    pub openness: [f32; 2],
}

pub fn encode_measure(sample: &MeasureSample) -> [u8; MEASURE_SIZE] {
    let mut packet = [0u8; MEASURE_SIZE];
    packet[..4].copy_from_slice(MEASURE_MAGIC);
    packet[4] = MEASURE_VERSION;
    packet[5] = [
        (sample.mapped, MEASURE_MAPPED),
        (sample.usable, MEASURE_USABLE),
        (sample.valve_held, MEASURE_VALVE_HELD),
        (sample.pupil_blending, MEASURE_PUPIL_BLENDING),
        (sample.before_smoothing, MEASURE_BEFORE_SMOOTHING),
    ]
    .iter()
        .filter(|(on, _)| *on)
        .fold(0, |flags, (_, bit)| flags | bit);
    packet[6] = sample.pupil_state;
    packet[8..16].copy_from_slice(&sample.time.to_le_bytes());
    for (i, value) in sample.valve.iter().chain(&sample.pupil).chain(&sample.openness).enumerate() {
        packet[16 + i * 4..20 + i * 4].copy_from_slice(&value.to_le_bytes());
    }
    packet
}

#[cfg(test)]
pub fn decode_measure(packet: &[u8]) -> Option<MeasureSample> {
    if packet.len() != MEASURE_SIZE || &packet[..4] != MEASURE_MAGIC || packet[4] != MEASURE_VERSION {
        return None;
    }
    let f32_at = |at: usize| f32::from_le_bytes(packet[at..at + 4].try_into().unwrap());
    Some(MeasureSample {
        time: f64::from_le_bytes(packet[8..16].try_into().ok()?),
        mapped: packet[5] & MEASURE_MAPPED != 0,
        usable: packet[5] & MEASURE_USABLE != 0,
        valve_held: packet[5] & MEASURE_VALVE_HELD != 0,
        pupil_blending: packet[5] & MEASURE_PUPIL_BLENDING != 0,
        before_smoothing: packet[5] & MEASURE_BEFORE_SMOOTHING != 0,
        pupil_state: packet[6],
        valve: [f32_at(16), f32_at(20)],
        pupil: [f32_at(24), f32_at(28)],
        openness: [f32_at(32), f32_at(36)],
    })
}

/// The gaze guard's values for a sample of the panel's checks (see the module comment): the guarded combined gaze,
/// each eye's offset and the witness's gaze (all 1.0 = 45°), and whether the witness was trusted.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct GuardMeasure {
    pub time: f64,
    pub trusted: bool,
    pub guarded: [f32; 2],
    pub offset: [[f32; 2]; 2],
    pub witness: Option<[f32; 2]>,
}

pub fn encode_guard(guard: &GuardMeasure) -> [u8; GUARD_SIZE] {
    let mut packet = [0u8; GUARD_SIZE];
    packet[..4].copy_from_slice(GUARD_MAGIC);
    packet[4] = GUARD_VERSION;
    packet[5] = u8::from(guard.trusted) * GUARD_TRUSTED | u8::from(guard.witness.is_some()) * GUARD_WITNESS;
    packet[8..16].copy_from_slice(&guard.time.to_le_bytes());
    let witness = guard.witness.unwrap_or([0.0; 2]);
    let values = guard.guarded.iter().chain(guard.offset.as_flattened()).chain(&witness);
    for (i, value) in values.enumerate() {
        packet[16 + i * 4..20 + i * 4].copy_from_slice(&value.to_le_bytes());
    }
    packet
}

#[cfg(test)]
pub fn decode_guard(packet: &[u8]) -> Option<GuardMeasure> {
    if packet.len() != GUARD_SIZE || &packet[..4] != GUARD_MAGIC || packet[4] != GUARD_VERSION {
        return None;
    }
    let f32_at = |at: usize| f32::from_le_bytes(packet[at..at + 4].try_into().unwrap());
    Some(GuardMeasure {
        time: f64::from_le_bytes(packet[8..16].try_into().ok()?),
        trusted: packet[5] & GUARD_TRUSTED != 0,
        guarded: [f32_at(16), f32_at(20)],
        offset: [[f32_at(24), f32_at(28)], [f32_at(32), f32_at(36)]],
        witness: (packet[5] & GUARD_WITNESS != 0).then(|| [f32_at(40), f32_at(44)]),
    })
}

/// A brow (0..1) as its u16: 0..=65535, rounded; NaN as 0.
fn brow_bits(brow: f32) -> u16 {
    if brow.is_finite() { (brow.clamp(0.0, 1.0) * 65535.0).round() as u16 } else { 0 }
}

pub fn encode(sample: &DotSample) -> [u8; PACKET_SIZE] {
    let mut packet = [0u8; PACKET_SIZE];
    packet[..4].copy_from_slice(MAGIC);
    packet[4] = VERSION;
    packet[5] = u8::from(sample.independent) | (u8::from(sample.scaled) << 1);
    if sample.brows.is_some() {
        packet[5] |= BROWS_FLAG;
    }
    packet[8..16].copy_from_slice(&sample.time.to_le_bytes());
    for (i, value) in sample.gaze.iter().chain(&sample.raw_gaze).enumerate() {
        packet[16 + i * 4..20 + i * 4].copy_from_slice(&value.to_le_bytes());
    }
    for (e, eye) in sample.eyes.iter().enumerate() {
        let at = EYES + e * EYE_SIZE;
        let values = [
            eye.lid,
            eye.openness,
            eye.openness_scaled,
            eye.camera_lid,
            eye.squint,
            eye.pupil_mm,
            eye.camera_squint,
            eye.camera_pupil_mm,
        ];
        for (i, value) in values.iter().enumerate() {
            packet[at + i * 4..at + 4 + i * 4].copy_from_slice(&value.to_le_bytes());
        }
        packet[at + 32] = eye.lid_from;
        packet[at + 33] = u8::from(eye.camera_closed);
        if let Some(brows) = sample.brows {
            packet[at + BROW_AT..at + BROW_AT + 2].copy_from_slice(&brow_bits(brows[e]).to_le_bytes());
        }
    }
    for (i, value) in sample.unscaled_gaze.iter().enumerate() {
        packet[UNSCALED_AT + i * 4..UNSCALED_AT + 4 + i * 4].copy_from_slice(&value.to_le_bytes());
    }
    packet
}

#[cfg(test)]
pub fn decode(packet: &[u8]) -> Option<DotSample> {
    if packet.len() != PACKET_SIZE || &packet[..4] != MAGIC || packet[4] != VERSION {
        return None;
    }
    let f32_at = |at: usize| f32::from_le_bytes(packet[at..at + 4].try_into().unwrap());
    let time = f64::from_le_bytes(packet[8..16].try_into().ok()?);
    Some(DotSample {
        time,
        gaze: std::array::from_fn(|i| f32_at(16 + i * 4)),
        raw_gaze: std::array::from_fn(|i| f32_at(40 + i * 4)),
        unscaled_gaze: std::array::from_fn(|i| f32_at(UNSCALED_AT + i * 4)),
        independent: packet[5] & 1 != 0,
        scaled: packet[5] & 2 != 0,
        brows: (packet[5] & BROWS_FLAG != 0).then(|| {
            std::array::from_fn(|e| {
                let at = EYES + e * EYE_SIZE + BROW_AT;
                f32::from(u16::from_le_bytes([packet[at], packet[at + 1]])) / 65535.0
            })
        }),
        eyes: std::array::from_fn(|e| {
            let at = EYES + e * EYE_SIZE;
            DotEye {
                lid: f32_at(at),
                openness: f32_at(at + 4),
                openness_scaled: f32_at(at + 8),
                camera_lid: f32_at(at + 12),
                squint: f32_at(at + 16),
                pupil_mm: f32_at(at + 20),
                camera_squint: f32_at(at + 24),
                camera_pupil_mm: f32_at(at + 28),
                lid_from: packet[at + 32],
                camera_closed: packet[at + 33] & 1 != 0,
            }
        }),
    })
}

/// Sends the samples while they are wanted; holds no socket otherwise.
pub struct DotStream {
    path: PathBuf,
    socket: Option<UnixDatagram>,
    // Whether the panel listened when last looked (its socket file was there), and when that was.
    listening: bool,
    looked: Option<Instant>,
}

impl DotStream {
    /// `dir` is the status folder ($XDG_RUNTIME_DIR/frameeyeosc), where the panel binds the socket.
    pub fn new(dir: &Path) -> Self {
        Self::named(dir, SOCKET_NAME)
    }

    /// The same for another of the panel's sockets (MEASURE_SOCKET_NAME).
    pub fn named(dir: &Path, name: &str) -> Self {
        Self {
            path: dir.join(name),
            socket: None,
            listening: false,
            looked: None,
        }
    }

    /// Whether to build and send this sample: always while `dots` (gaze_debug_dots) is on, as before; otherwise while
    /// the panel's socket is there (looked at once every LOOK_INTERVAL).
    pub fn wanted(&mut self, dots: bool, now: Instant) -> bool {
        if !dots && self.looked.is_none_or(|at| now.duration_since(at) >= LOOK_INTERVAL) {
            self.looked = Some(now);
            self.listening = std::fs::symlink_metadata(&self.path).is_ok_and(|meta| meta.file_type().is_socket());
        }
        let wanted = dots || self.listening;
        if !wanted {
            self.socket = None;
        }
        wanted
    }

    pub fn send(&mut self, sample: &DotSample) {
        self.send_bytes(&encode(sample));
    }

    /// The other gaze source's value for the next sample (gaze_debug_dots_compare): sent just before it.
    pub fn send_other(&mut self, other: &OtherSample) {
        self.send_bytes(&encode_other(other));
    }

    /// A sample for the panel's gaze error check (on its own socket: a DotStream::named(MEASURE_SOCKET_NAME)).
    pub fn send_measure(&mut self, sample: &MeasureSample) {
        self.send_bytes(&encode_measure(sample));
    }

    /// The gaze guard's values for the next sample of the panel's checks (on their socket): sent just before it.
    pub fn send_guard(&mut self, guard: &GuardMeasure) {
        self.send_bytes(&encode_guard(guard));
    }

    fn send_bytes(&mut self, bytes: &[u8]) {
        if self.socket.is_none() {
            self.socket = UnixDatagram::unbound()
                .and_then(|socket| socket.set_nonblocking(true).map(|()| socket))
                .ok();
        }
        // No panel listening (or it is busy) is normal: the sample is simply dropped. A socket file gone or left by a
        // panel that crashed waits for the next look
        if let Some(socket) = &self.socket
            && let Err(error) = socket.send_to(bytes, &self.path)
            && matches!(error.kind(), ErrorKind::NotFound | ErrorKind::ConnectionRefused)
        {
            self.listening = false;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn eye(lid: f32) -> DotEye {
        DotEye {
            lid,
            openness: 0.98,
            openness_scaled: 1.02,
            camera_lid: f32::NAN,
            squint: 0.25,
            pupil_mm: f32::NAN,
            camera_squint: 0.3,
            camera_pupil_mm: 6.1,
            lid_from: 4,
            camera_closed: true,
        }
    }

    fn sample() -> DotSample {
        DotSample {
            time: 19892.729813125,
            gaze: [0.1, -0.2, 0.3, -0.4, 0.5, -0.6],
            raw_gaze: [-0.1, 0.2, -0.3, 0.4, -0.5, 0.6],
            unscaled_gaze: [0.05, -0.1, 0.25, -0.3, 0.15, -0.2],
            independent: true,
            scaled: true,
            eyes: [eye(0.72), DotEye { camera_closed: false, lid_from: 0, ..eye(0.0) }],
            brows: None,
        }
    }

    /// Equal, NaN for NaN (the values without the eye camera are NaN).
    fn same(a: &DotSample, b: &DotSample) -> bool {
        let bits = |s: &DotSample| {
            let eyes: Vec<u32> = s
                .eyes
                .iter()
                .flat_map(|e| {
                    [e.lid, e.openness, e.openness_scaled, e.camera_lid, e.squint, e.pupil_mm, e.camera_squint, e.camera_pupil_mm]
                        .map(f32::to_bits)
                        .into_iter()
                        .chain([u32::from(e.lid_from), u32::from(e.camera_closed)])
                })
                .collect();
            let gazes = [s.gaze, s.raw_gaze, s.unscaled_gaze].map(|gaze| gaze.map(f32::to_bits));
            let brows = s.brows.map(|brows| brows.map(f32::to_bits));
            (s.time.to_bits(), gazes, s.independent, s.scaled, eyes, brows)
        };
        bits(a) == bits(b)
    }

    #[test]
    fn packets_read_back_exactly() {
        let sample = sample();
        let packet = encode(&sample);
        assert_eq!(PACKET_SIZE, 160);
        assert_eq!(&packet[..8], b"FEOD\x03\x03\x00\x00");
        assert!(same(&decode(&packet).unwrap(), &sample));
        // The left eye's lid first, the right eye's flags last
        assert_eq!(f32::from_le_bytes(packet[64..68].try_into().unwrap()), 0.72);
        assert_eq!(packet[64 + 33], 1);
        assert_eq!(packet[100 + 32..100 + 36], [0, 0, 0, 0]);
        // The gaze before the scale last: its combined x at 152
        assert_eq!(f32::from_le_bytes(packet[152..156].try_into().unwrap()), 0.15);
        // Without the scale in use, bit 1 is clear
        let plain = encode(&DotSample { scaled: false, independent: false, ..sample });
        assert_eq!(plain[5], 0);
        assert!(!decode(&plain).unwrap().scaled);
        let mut wrong = packet;
        wrong[0] = b'X';
        assert_eq!(decode(&wrong), None);
        // Versions 1 (40 bytes) and 2 (136), and anything else of the wrong size or version, are not this
        let mut old = packet;
        old[4] = 2;
        assert_eq!(decode(&old), None);
        assert_eq!(decode(&old[..136]), None);
        assert_eq!(decode(&packet[..40]), None);
        assert_eq!(decode(&packet[..136]), None);
        assert_eq!(decode(&packet[..159]), None);
    }

    #[test]
    fn brows_go_in_the_reserved_bytes_and_change_nothing_else() {
        let plain = sample();
        let with = DotSample { brows: Some([0.5, 1.0]), ..plain };
        let packet = encode(&with);
        // The same version and size, flag bit 2, and each eye's brow as a u16 at 34 (0.5 rounds to 32768)
        assert_eq!(packet.len(), 160);
        assert_eq!(&packet[..8], b"FEOD\x03\x07\x00\x00");
        assert_eq!(packet[64 + 34..64 + 36], 32768u16.to_le_bytes());
        assert_eq!(packet[100 + 34..100 + 36], [0xff, 0xff]);
        let read = decode(&packet).unwrap();
        let [left, right] = read.brows.unwrap();
        assert!((left - 0.5).abs() < 1e-4 && right == 1.0, "{left} {right}");
        // A reader from before (0.7.10's panel, which ignores flag bit 2 and the reserved bytes) reads the rest as it
        // did: only byte 5's bit 2 and the brow bytes differ from the packet without brows (0.5 is 0x8000: its low
        // byte stays 0)
        let without = encode(&plain);
        let differ: Vec<usize> = (0..PACKET_SIZE).filter(|&i| packet[i] != without[i]).collect();
        assert_eq!(differ, [5, 99, 134, 135]);
        assert_eq!(packet[5] & !BROWS_FLAG, without[5]);
        // Without brows, nothing of them: the bytes stay zero, as they were before (an older frameeyeosc's datagram
        // reads as without brows)
        assert_eq!(without[5] & BROWS_FLAG, 0);
        assert_eq!([without[98], without[99], without[134], without[135]], [0; 4]);
        assert_eq!(decode(&without).unwrap().brows, None);
        // At rest, out of range or not a number: 0, kept within 0..1
        let odd = decode(&encode(&DotSample { brows: Some([f32::NAN, 1.7]), ..plain })).unwrap();
        assert_eq!(odd.brows, Some([0.0, 1.0]));
        let rest = decode(&encode(&DotSample { brows: Some([0.0, -0.3]), ..plain })).unwrap();
        assert_eq!(rest.brows, Some([0.0, 0.0]));
        assert_eq!(packet[5] & BROWS_FLAG, BROWS_FLAG);
    }

    #[test]
    fn the_other_gaze_goes_in_a_datagram_of_its_own() {
        let other = OtherSample {
            time: 19892.729813125,
            gaze: Some([0.125, -0.25]),
            valve: true,
        };
        let packet = encode_other(&other);
        assert_eq!(packet.len(), OTHER_SIZE);
        assert_eq!(&packet[..8], b"FEOC\x01\x03\x00\x00");
        assert_eq!(decode_other(&packet), Some(other));
        assert_eq!(f32::from_le_bytes(packet[16..20].try_into().unwrap()), 0.125);
        // None for this sample (the pupils' gaze, both pupils not there): no value, zeros
        let none = OtherSample { gaze: None, valve: false, ..other };
        let empty = encode_other(&none);
        assert_eq!(&empty[..8], b"FEOC\x01\x00\x00\x00");
        assert_eq!(empty[16..], [0; 8]);
        assert_eq!(decode_other(&empty), Some(none));
        // Not a sample's datagram, nor the other way round: each reader keeps to its own
        assert_eq!(decode(&packet), None);
        assert_eq!(decode_other(&encode(&sample())), None);
        let mut wrong = packet;
        wrong[4] = 2;
        assert_eq!(decode_other(&wrong), None);
        assert_eq!(decode_other(&packet[..23]), None);
    }

    #[test]
    fn the_measure_datagram_reads_back_and_is_no_other() {
        let sample = MeasureSample {
            time: 19892.729813125,
            mapped: true,
            usable: true,
            valve_held: false,
            pupil_blending: false,
            before_smoothing: false,
            pupil_state: 1,
            valve: [0.125, -0.25],
            pupil: [0.5, 0.0625],
            openness: [0.75, 0.5],
        };
        let packet = encode_measure(&sample);
        assert_eq!(packet.len(), MEASURE_SIZE);
        assert_eq!(&packet[..8], b"FEOM\x01\x03\x01\x00");
        assert_eq!(decode_measure(&packet), Some(sample));
        assert_eq!(f32::from_le_bytes(packet[24..28].try_into().unwrap()), 0.5);
        assert_eq!(f32::from_le_bytes(packet[36..40].try_into().unwrap()), 0.5);
        let held = MeasureSample { mapped: false, usable: false, valve_held: true, pupil_state: 0, ..sample };
        assert_eq!(&encode_measure(&held)[..8], b"FEOM\x01\x04\x00\x00");
        assert_eq!(decode_measure(&encode_measure(&held)), Some(held));
        // Still easing: bit 3, nothing else moves
        let easing = MeasureSample { pupil_blending: true, ..sample };
        let eased = encode_measure(&easing);
        assert_eq!(&eased[..8], b"FEOM\x01\x0b\x01\x00");
        assert_eq!((0..MEASURE_SIZE).filter(|&i| eased[i] != packet[i]).collect::<Vec<_>>(), [5]);
        assert_eq!(decode_measure(&eased), Some(easing));
        // Before smoothing: bit 4
        let before = MeasureSample { before_smoothing: true, ..sample };
        assert_eq!(&encode_measure(&before)[..8], b"FEOM\x01\x13\x01\x00");
        assert_eq!(decode_measure(&encode_measure(&before)), Some(before));
        // None of the other datagrams, nor they this one
        assert_eq!(decode(&packet), None);
        assert_eq!(decode_other(&packet[..OTHER_SIZE]), None);
        assert_eq!(decode_measure(&encode(&sample_for_measure())[..MEASURE_SIZE]), None);
        let mut wrong = packet;
        wrong[4] = 2;
        assert_eq!(decode_measure(&wrong), None);
        assert_eq!(decode_measure(&packet[..39]), None);
    }

    #[test]
    fn the_guard_datagram_reads_back_and_is_no_other() {
        let guard = GuardMeasure {
            time: 19892.729813125,
            trusted: true,
            guarded: [0.125, -0.25],
            offset: [[-0.375, 0.0625], [-0.3125, 0.0]],
            witness: Some([0.1, -0.2]),
        };
        let packet = encode_guard(&guard);
        assert_eq!(packet.len(), GUARD_SIZE);
        assert_eq!(&packet[..8], b"FEOG\x01\x03\x00\x00");
        assert_eq!(decode_guard(&packet), Some(guard));
        // The time where the measure datagram has it, the right eye's offset x at 32, the witness last
        assert_eq!(f64::from_le_bytes(packet[8..16].try_into().unwrap()), guard.time);
        assert_eq!(f32::from_le_bytes(packet[32..36].try_into().unwrap()), -0.3125);
        assert_eq!(f32::from_le_bytes(packet[44..48].try_into().unwrap()), -0.2);
        // Not trusted, no witness: the flags clear, zeros for the witness
        let none = GuardMeasure { trusted: false, witness: None, ..guard };
        let empty = encode_guard(&none);
        assert_eq!(&empty[..8], b"FEOG\x01\x00\x00\x00");
        assert_eq!(empty[40..], [0; 8]);
        assert_eq!(decode_guard(&empty), Some(none));
        // None of the other datagrams, nor they this one: a panel from before reads FEOM only at its 40 bytes
        assert_eq!(decode_measure(&packet), None);
        assert_eq!(decode_measure(&packet[..MEASURE_SIZE]), None);
        assert_eq!(decode_other(&packet[..OTHER_SIZE]), None);
        assert_eq!(decode(&packet), None);
        let measure = encode_measure(&MeasureSample {
            time: 1.0,
            mapped: true,
            usable: true,
            valve_held: false,
            pupil_blending: false,
            before_smoothing: true,
            pupil_state: 1,
            valve: [0.0; 2],
            pupil: [0.0; 2],
            openness: [1.0; 2],
        });
        assert_eq!(decode_guard(&measure), None);
        let mut wrong = packet;
        wrong[4] = 2;
        assert_eq!(decode_guard(&wrong), None);
    }

    fn sample_for_measure() -> DotSample {
        sample()
    }

    #[test]
    fn the_panel_gets_each_sample_only_while_wanted() {
        let dir = std::env::temp_dir().join(format!("frameeyeosc-dots-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let now = Instant::now();
        let mut stream = DotStream::new(&dir);
        let sample = sample();
        // Nobody listening and the dots off: not wanted, no socket held
        assert!(!stream.wanted(false, now));
        assert!(stream.socket.is_none());
        let panel = UnixDatagram::bind(dir.join(SOCKET_NAME)).unwrap();
        panel.set_nonblocking(true).unwrap();
        // The panel's socket is noticed at the next look, a second later
        assert!(!stream.wanted(false, now + Duration::from_millis(500)));
        assert!(stream.wanted(false, now + LOOK_INTERVAL));
        stream.send(&sample);
        let mut buffer = [0u8; 256];
        let size = panel.recv(&mut buffer).unwrap();
        assert!(same(&decode(&buffer[..size]).unwrap(), &sample));
        // The panel closes its socket (the preview closed): the next send notices, and nothing more is sent
        drop(panel);
        std::fs::remove_file(dir.join(SOCKET_NAME)).unwrap();
        stream.send(&sample);
        assert!(!stream.wanted(false, now + LOOK_INTERVAL + Duration::from_millis(10)));
        assert!(stream.socket.is_none());
        // gaze_debug_dots on: wanted with nobody listening, as before (the sample is dropped)
        assert!(stream.wanted(true, now + LOOK_INTERVAL + Duration::from_millis(20)));
        stream.send(&sample);
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
