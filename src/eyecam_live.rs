//! eyecam-rec's live eye-camera values: each eye's eyelid, squint and pupil, worked out from the Frame's eye-camera
//! images at 90 Hz and published in a small shared-memory file. SteamOS 0.4.3's eye server reads a relaxed open eye as
//! 1.000, so widening can't come through from it (see SATURATION_WINDOW in main.rs); the camera still sees it.
//!
//! Only ever read: the file is opened read-only and mapped without write access, and nothing is sent to eyecam-rec.
//! Its layout (little-endian, the first STRUCT_SIZE bytes of a 4096-byte file):
//!   0   u32  magic (MAGIC, "EYCM")       4   u32  version (VERSION)    8   u32  struct size (at least STRUCT_SIZE)
//!   12  u32  writer pid                  16  u64  writer start (CLOCK_MONOTONIC ns)
//!   24  u32  seq (odd while being written; see read_consistent)
//!   28  u32  calibration: bit 0 calibrated for this wear (`calib wear`), bit 1 for this user (`calib user`),
//!            bit 2 a baseline learned for this wear from the relaxed face
//!   32  u64  time (CLOCK_MONOTONIC ns)    40  u64  camera time         48  u32  recalibration suggested
//!   52  u32  live (1 while processing)    56  the left eye             128 the right eye
//! Each eye (72 bytes):
//!   0   u64  capture time (CLOCK_MONOTONIC ns)   8   u64  camera time   16  u64  frame count
//!   24  u32  valid    28  u32  closed    32  f32  eyelid (VRCFT scale: 0 closed, 0.75 relaxed open as calibrated,
//!   1 fully widened)  36  f32  widen    40  f32  squint (0..1)    44  f32  pupil ratio    48  f32  pupil (mm)
//!   52  f32  pupil dilation (0..1)    56  f32  confidence    60  f32, 64  f32  debug    68  u32  reserved
//! The values are per frame (a 5-frame median, no smoothing). On a clean exit eyecam-rec writes live 0 and valid 0; a
//! crash writes nothing, so only the capture time tells the values are old.

use memmap2::{Mmap, MmapOptions};
use std::fs::{self, OpenOptions};
use std::os::unix::fs::{MetadataExt, OpenOptionsExt};
use std::path::{Path, PathBuf};
use std::ptr;
use std::sync::atomic::{AtomicU32, Ordering, fence};
use std::time::{Duration, Instant};

const MAGIC: u32 = 0x4D43_5945;
const VERSION: u32 = 1;
// The writer's side of the contract: the file is created at its full size and never shrunk or truncated in place (a
// mapped page past the end would crash the reader with SIGBUS); to replace it, eyecam-rec writes a new file and renames
// it over this one, which the reader notices by its inode (LiveReader::check). It is a regular file of this user's.
const STRUCT_SIZE: usize = 200;
const SEQ: usize = 24;
const EYES: [usize; 2] = [56, 128];
// The calibration bits that give this wear a baseline: calibrated (`calib wear`), or learned by itself.
const CALIB_BASELINE: u32 = 1 | 4;
// An eye's values are used while captured at most this long before they are read (9 frames at 90 Hz), and not more
// than CLOCK_AHEAD_NS after (the two clocks are the same; this allows for rounding). When the eye cameras run slower
// (Valve's eye tracker sets their rate, as low as 15 frames a second on some headsets, 67 ms apart), the limit is
// two of their frame intervals instead (FrameGaps).
const FRESH_NS: u64 = 100_000_000;
const CLOCK_AHEAD_NS: u64 = 5_000_000;
// A copy is tried this many times while eyecam-rec writes; then that sample goes without the camera.
const MAX_TRIES: usize = 8;
// While the file can't be read it is tried again this often; while it can, it is checked this often for having been
// replaced (one stat).
const CHECK_INTERVAL: Duration = Duration::from_secs(1);

/// `$XDG_RUNTIME_DIR/eyecam/live`, or under `/run/user/<uid>` without it.
pub fn live_path() -> PathBuf {
    std::env::var_os("XDG_RUNTIME_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(format!("/run/user/{}", unsafe { libc::getuid() })))
        .join("eyecam")
        .join("live")
}

/// CLOCK_MONOTONIC in ns, the clock eyecam-rec stamps its values with.
pub fn monotonic_ns() -> u64 {
    let mut time = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut time) };
    time.tv_sec as u64 * 1_000_000_000 + time.tv_nsec as u64
}

/// One eye's values, as far as frameeyeosc uses or records them.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Eye {
    pub time_ns: u64,
    pub valid: bool,
    pub closed: bool,
    pub lid: f32,
    pub wide: f32,
    pub squint: f32,
    pub pupil_mm: f32,
    pub pupil_dilation: f32,
    pub confidence: f32,
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct Record {
    // pid and start time
    writer: (u32, u64),
    calib_state: u32,
    recalib_suggested: bool,
    live: bool,
    eyes: [Eye; 2],
}

fn u32_at(bytes: &[u8; STRUCT_SIZE], offset: usize) -> u32 {
    u32::from_le_bytes(std::array::from_fn(|i| bytes[offset + i]))
}

fn u64_at(bytes: &[u8; STRUCT_SIZE], offset: usize) -> u64 {
    u64::from_le_bytes(std::array::from_fn(|i| bytes[offset + i]))
}

fn f32_at(bytes: &[u8; STRUCT_SIZE], offset: usize) -> f32 {
    f32::from_bits(u32_at(bytes, offset))
}

/// The values in a copy of the file's first STRUCT_SIZE bytes, or why they can't be used.
fn parse(bytes: &[u8; STRUCT_SIZE]) -> Result<Record, String> {
    let magic = u32_at(bytes, 0);
    if magic != MAGIC {
        return Err(format!("not eyecam-rec's live values (magic {magic:#010x})"));
    }
    let version = u32_at(bytes, 4);
    if version != VERSION {
        return Err(format!("unsupported version {version}; supported: {VERSION}"));
    }
    let size = u32_at(bytes, 8);
    if (size as usize) < STRUCT_SIZE {
        return Err(format!("too small a struct ({size} bytes)"));
    }
    Ok(Record {
        writer: (u32_at(bytes, 12), u64_at(bytes, 16)),
        calib_state: u32_at(bytes, 28),
        recalib_suggested: u32_at(bytes, 48) != 0,
        live: u32_at(bytes, 52) == 1,
        eyes: EYES.map(|eye| Eye {
            time_ns: u64_at(bytes, eye),
            valid: u32_at(bytes, eye + 24) == 1,
            closed: u32_at(bytes, eye + 28) == 1,
            lid: f32_at(bytes, eye + 32),
            wide: f32_at(bytes, eye + 36),
            squint: f32_at(bytes, eye + 40),
            pupil_mm: f32_at(bytes, eye + 48),
            pupil_dilation: f32_at(bytes, eye + 52),
            confidence: f32_at(bytes, eye + 56),
        }),
    })
}

/// A copy eyecam-rec did not write to meanwhile (a seqlock): `seq` is even before and the same after, else it is
/// tried again, at most MAX_TRIES times. `seq` must load with acquire ordering.
fn read_consistent<T>(seq: impl Fn() -> u32, copy: impl Fn() -> T) -> Option<T> {
    for _ in 0..MAX_TRIES {
        let before = seq();
        if before % 2 == 1 {
            std::hint::spin_loop();
            continue;
        }
        let values = copy();
        // The copy's reads may not move after the second look at seq
        fence(Ordering::Acquire);
        if seq() == before {
            return Some(values);
        }
    }
    None
}

/// eyecam-rec's values, as read at one moment.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Live {
    pub calib_state: u32,
    pub recalib_suggested: bool,
    /// eyecam-rec is processing.
    pub live: bool,
    pub eyes: [Eye; 2],
    /// Whether each eye's values are new enough to use: eyecam-rec processing, the eye valid, and captured at most
    /// FRESH_NS before they were read.
    pub fresh: [bool; 2],
}

impl Live {
    /// The values of `record` at `now_ns`, each eye fresh for `fresh_ns` after it was captured.
    fn new(record: &Record, now_ns: u64, fresh_ns: [u64; 2]) -> Self {
        let fresh = [0, 1].map(|e| {
            let eye = &record.eyes[e];
            record.live
                && eye.valid
                && eye.time_ns <= now_ns + CLOCK_AHEAD_NS
                && now_ns.saturating_sub(eye.time_ns) <= fresh_ns[e]
        });
        Self {
            calib_state: record.calib_state,
            recalib_suggested: record.recalib_suggested,
            live: record.live,
            eyes: record.eyes,
            fresh,
        }
    }

    /// eyecam-rec is processing, and at least one eye's values are fresh.
    pub fn present(&self) -> bool {
        self.fresh.contains(&true)
    }

    /// Whether each eye's eyelid and squint can be used: fresh, and with a baseline for this wear, either calibrated
    /// (calib_state bit 0) or learned by eyecam-rec from the relaxed face (bit 2, about 35 s after putting it on).
    /// Without either, up to 53% of the frames read widened when the eye is not.
    pub fn lids_usable(&self) -> [bool; 2] {
        [0, 1].map(|eye| {
            let Eye { lid, squint, .. } = self.eyes[eye];
            self.fresh[eye] && self.baseline() && lid.is_finite() && squint.is_finite()
        })
    }

    /// Whether this wear has a baseline: calibrated (calib_state bit 0) or learned by eyecam-rec (bit 2).
    pub fn baseline(&self) -> bool {
        self.calib_state & CALIB_BASELINE != 0
    }

    /// Whether each eye's pupil can be used: fresh (no calibration needed).
    pub fn pupil_usable(&self) -> [bool; 2] {
        [0, 1].map(|eye| {
            let Eye { pupil_mm, pupil_dilation, .. } = self.eyes[eye];
            self.fresh[eye] && pupil_mm.is_finite() && pupil_dilation.is_finite()
        })
    }
}

/// One eye's frame interval, from the capture times of the values read: the median of the last few gaps (under a
/// second) between new values. Values read less often than the camera delivers make the gaps longer, which only
/// makes the limit more lenient.
#[derive(Default)]
struct FrameGaps {
    last_ns: u64,
    gaps: std::collections::VecDeque<u64>,
}

impl FrameGaps {
    const KEEP: usize = 9;

    fn note(&mut self, time_ns: u64) {
        if time_ns == self.last_ns {
            return;
        }
        if self.last_ns != 0 && time_ns > self.last_ns && time_ns - self.last_ns < 1_000_000_000 {
            if self.gaps.len() == Self::KEEP {
                self.gaps.pop_front();
            }
            self.gaps.push_back(time_ns - self.last_ns);
        }
        self.last_ns = time_ns;
    }

    /// How long a value stays fresh: FRESH_NS, or two frame intervals when that is longer.
    fn fresh_ns(&self) -> u64 {
        let mut gaps: Vec<u64> = self.gaps.iter().copied().collect();
        gaps.sort_unstable();
        gaps.get(gaps.len() / 2).map_or(FRESH_NS, |gap| FRESH_NS.max(2 * gap))
    }
}

struct Mapped {
    map: Mmap,
    inode: u64,
    // The writer it was opened with (pid and start time)
    writer: (u32, u64),
}

impl Mapped {
    fn read(&self) -> Option<[u8; STRUCT_SIZE]> {
        let base = self.map.as_ptr();
        // The mapping is page-aligned and STRUCT_SIZE long, so seq is aligned and inside it; it is only ever loaded
        let seq = unsafe { &*base.add(SEQ).cast::<AtomicU32>() };
        read_consistent(
            || seq.load(Ordering::Acquire),
            || unsafe { ptr::read_volatile(base.cast::<[u8; STRUCT_SIZE]>()) },
        )
    }
}

/// Map the file read-only and check its header. Err says whether it could be opened at all, and why it can't be used.
/// Opened without blocking and without following a symlink, and only a regular file of this user's is used: a FIFO in
/// its place would otherwise hold up the eye-data loop on open, and someone else's file isn't eyecam-rec's. Anything
/// else is closed again (dropped) and tried again at the next check.
fn open(path: &Path) -> Result<(Mapped, Record), (bool, String)> {
    let file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NONBLOCK | libc::O_NOFOLLOW)
        .open(path)
        .map_err(|error| (false, error.to_string()))?;
    let metadata = file.metadata().map_err(|error| (true, error.to_string()))?;
    if !metadata.is_file() {
        return Err((true, "not a regular file".to_owned()));
    }
    let uid = unsafe { libc::getuid() };
    if metadata.uid() != uid {
        return Err((true, format!("owned by uid {}, not by this user ({uid})", metadata.uid())));
    }
    if metadata.len() < STRUCT_SIZE as u64 {
        return Err((true, format!("too small ({} bytes)", metadata.len())));
    }
    let map = unsafe { MmapOptions::new().len(STRUCT_SIZE).map(&file) }.map_err(|error| (true, error.to_string()))?;
    let mut mapped = Mapped {
        map,
        inode: metadata.ino(),
        writer: (0, 0),
    };
    let bytes = mapped.read().ok_or((true, "always being written".to_owned()))?;
    let record = parse(&bytes).map_err(|error| (true, error))?;
    mapped.writer = record.writer;
    Ok((mapped, record))
}

/// eyecam-rec's live file, kept mapped while it can be read. While it can't (eyecam-rec not running, a version this
/// does not know, ...), it is tried again every CHECK_INTERVAL; while it can, it is opened again when the file is
/// replaced or another eyecam-rec writes it.
pub struct LiveReader {
    path: PathBuf,
    mapped: Option<Mapped>,
    // Whether the file could be opened at the last try (it may still be unusable: see error).
    opened: bool,
    // Why it can't be used; None while it can.
    error: Option<String>,
    next_check: Instant,
    // Another writer was seen: open it again at the next check.
    reopen: bool,
    // Each eye's frame interval, for how long its values stay fresh.
    gaps: [FrameGaps; 2],
}

impl LiveReader {
    pub fn new(path: PathBuf, now: Instant) -> Self {
        Self {
            path,
            mapped: None,
            opened: false,
            error: None,
            next_check: now,
            reopen: false,
            gaps: Default::default(),
        }
    }

    /// Whether the file could be opened at the last try.
    pub fn opened(&self) -> bool {
        self.opened
    }

    /// Why the file can't be used, if it can't.
    pub fn error(&self) -> Option<&str> {
        self.error.as_deref()
    }

    /// Open the file if a try is due, or check that the mapped one is still the one at the path. Returns what to log:
    /// the file once it is opened, or why it can't be used, only when that reason is new (not every second).
    pub fn check(&mut self, now: Instant) -> Option<String> {
        if now < self.next_check {
            return None;
        }
        self.next_check = now + CHECK_INTERVAL;
        if let Some(mapped) = &self.mapped {
            // Shrunk below the values, reading them would crash (SIGBUS), so that lets go too
            let same = !self.reopen
                && fs::metadata(&self.path)
                    .is_ok_and(|metadata| metadata.ino() == mapped.inode && metadata.len() >= STRUCT_SIZE as u64);
            if same {
                return None;
            }
            self.mapped = None;
        }
        self.reopen = false;
        match open(&self.path) {
            Ok((mapped, _)) => {
                let line = format!(
                    "Reading eye-camera values from {} (eyecam-rec pid {})",
                    self.path.display(),
                    mapped.writer.0
                );
                self.mapped = Some(mapped);
                self.opened = true;
                self.error = None;
                Some(line)
            }
            Err((opened, error)) => {
                self.opened = opened;
                if self.error.as_ref() == Some(&error) {
                    return None;
                }
                let line = format!(
                    "No eye-camera values from {} ({error}); trying again every {} s",
                    self.path.display(),
                    CHECK_INTERVAL.as_secs()
                );
                self.error = Some(error);
                Some(line)
            }
        }
    }

    /// The newest values, judged fresh against `now_ns` (CLOCK_MONOTONIC); None while the file isn't mapped, or the
    /// values can't be used this time. No system calls: cheap enough for every eye sample.
    pub fn read(&mut self, now_ns: u64) -> Option<Live> {
        let mapped = self.mapped.as_ref()?;
        let bytes = mapped.read()?;
        match parse(&bytes) {
            Ok(record) => {
                if record.writer != mapped.writer {
                    // The same file, so the values still hold
                    self.reopen = true;
                }
                for (gaps, eye) in self.gaps.iter_mut().zip(&record.eyes) {
                    gaps.note(eye.time_ns);
                }
                Some(Live::new(&record, now_ns, self.gaps.each_ref().map(FrameGaps::fresh_ns)))
            }
            Err(_) => {
                // Rewritten as something else: let go, and say why at the next check
                self.mapped = None;
                None
            }
        }
    }
}

#[cfg(test)]
pub mod tests {
    use super::*;
    use std::cell::Cell;
    use std::os::unix::fs::FileExt;

    /// One eye's values for `write_live`.
    #[derive(Clone, Copy)]
    pub struct TestEye {
        pub time_ns: u64,
        pub valid: bool,
        pub closed: bool,
        pub lid: f32,
        pub wide: f32,
        pub squint: f32,
        pub pupil_mm: f32,
        pub pupil_dilation: f32,
        pub confidence: f32,
    }

    impl TestEye {
        /// Valid, captured `now_ns`, at eyelid `lid` (widened by as much as it is above relaxed open), with the given
        /// squint.
        pub fn at(now_ns: u64, lid: f32, squint: f32) -> Self {
            Self {
                time_ns: now_ns,
                valid: true,
                closed: false,
                lid,
                wide: ((lid - 0.75) / 0.25).max(0.0),
                squint,
                pupil_mm: 4.0,
                pupil_dilation: 0.5,
                confidence: 0.9,
            }
        }
    }

    /// The file's first STRUCT_SIZE bytes as eyecam-rec writes them (seq even).
    pub fn live_bytes(pid: u32, calib_state: u32, live: bool, eyes: [TestEye; 2]) -> [u8; STRUCT_SIZE] {
        let mut bytes = [0u8; STRUCT_SIZE];
        let mut put = |offset: usize, value: &[u8]| bytes[offset..offset + value.len()].copy_from_slice(value);
        put(0, &MAGIC.to_le_bytes());
        put(4, &VERSION.to_le_bytes());
        put(8, &(STRUCT_SIZE as u32).to_le_bytes());
        put(12, &pid.to_le_bytes());
        put(16, &(u64::from(pid) * 1000).to_le_bytes());
        put(24, &2u32.to_le_bytes());
        put(28, &calib_state.to_le_bytes());
        put(52, &u32::from(live).to_le_bytes());
        for (eye, base) in eyes.iter().zip(EYES) {
            put(base, &eye.time_ns.to_le_bytes());
            put(base + 24, &u32::from(eye.valid).to_le_bytes());
            put(base + 28, &u32::from(eye.closed).to_le_bytes());
            put(base + 32, &eye.lid.to_le_bytes());
            put(base + 36, &eye.wide.to_le_bytes());
            put(base + 40, &eye.squint.to_le_bytes());
            put(base + 48, &eye.pupil_mm.to_le_bytes());
            put(base + 52, &eye.pupil_dilation.to_le_bytes());
            put(base + 56, &eye.confidence.to_le_bytes());
        }
        bytes
    }

    /// Write a 4096-byte live file at `path` (in place, as eyecam-rec does).
    pub fn write_live(path: &Path, bytes: &[u8; STRUCT_SIZE]) {
        let file = fs::OpenOptions::new().create(true).write(true).truncate(false).open(path).unwrap();
        file.set_len(4096).unwrap();
        file.write_all_at(bytes, 0).unwrap();
    }

    /// A folder of its own under the temporary one, removed when dropped.
    pub struct TempDir(pub PathBuf);

    impl TempDir {
        pub fn new(name: &str) -> Self {
            let dir = std::env::temp_dir().join(format!("frameeyeosc-eyecam-{name}-{}", std::process::id()));
            let _ = fs::remove_dir_all(&dir);
            fs::create_dir_all(&dir).unwrap();
            Self(dir)
        }
    }

    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    const NOW: u64 = 1_000_000_000_000;

    fn eyes(lid: f32) -> [TestEye; 2] {
        [TestEye::at(NOW, lid, 0.2), TestEye::at(NOW - 20_000_000, lid, 0.4)]
    }

    #[test]
    fn values_are_read_where_eyecam_rec_writes_them() {
        let record = parse(&live_bytes(4242, 3, true, eyes(0.9))).unwrap();
        assert_eq!(record.writer, (4242, 4_242_000));
        assert_eq!((record.calib_state, record.recalib_suggested, record.live), (3, false, true));
        let expected = Eye {
            time_ns: NOW,
            valid: true,
            closed: false,
            lid: 0.9,
            wide: (0.9 - 0.75) / 0.25,
            squint: 0.2,
            pupil_mm: 4.0,
            pupil_dilation: 0.5,
            confidence: 0.9,
        };
        assert_eq!(record.eyes[0], expected);
        let mut shut = eyes(0.1);
        shut[1].closed = true;
        assert_eq!(parse(&live_bytes(1, 1, true, shut)).unwrap().eyes.map(|eye| eye.closed), [false, true]);
        assert_eq!((record.eyes[1].time_ns, record.eyes[1].squint), (NOW - 20_000_000, 0.4));
        let mut bytes = live_bytes(1, 1, true, eyes(0.9));
        bytes[48] = 1;
        assert!(parse(&bytes).unwrap().recalib_suggested);
        // A larger struct (a later version that only adds to it) is fine
        bytes[8..12].copy_from_slice(&256u32.to_le_bytes());
        assert!(parse(&bytes).is_ok());
    }

    #[test]
    fn a_header_it_does_not_know_is_refused() {
        let good = live_bytes(1, 1, true, eyes(0.9));
        let with = |offset: usize, value: u32| {
            let mut bytes = good;
            bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
            parse(&bytes)
        };
        assert_eq!(with(0, 0), Err("not eyecam-rec's live values (magic 0x00000000)".into()));
        assert_eq!(with(4, 2), Err("unsupported version 2; supported: 1".into()));
        assert_eq!(with(8, 199), Err("too small a struct (199 bytes)".into()));
        // "EYCM" in the file
        assert_eq!(&good[..4], b"EYCM");
    }

    #[test]
    fn a_copy_made_while_being_written_is_tried_again() {
        // Odd: being written. Retried, and given up after MAX_TRIES
        let looks = Cell::new(0);
        let copies = Cell::new(0);
        let seq = || {
            looks.set(looks.get() + 1);
            3
        };
        assert_eq!(read_consistent(seq, || copies.set(copies.get() + 1)), None);
        assert_eq!((looks.get(), copies.get()), (MAX_TRIES, 0));
        // Written to during the copy (seq moved on): copied again
        let values = [2, 4, 4, 4];
        let at = Cell::new(0);
        let copies = Cell::new(0);
        let seq = || {
            at.set(at.get() + 1);
            values[at.get() - 1]
        };
        let copy = || {
            copies.set(copies.get() + 1);
            copies.get()
        };
        assert_eq!(read_consistent(seq, copy), Some(2));
        // Odd first, then done
        let values = [5, 6, 6];
        let at = Cell::new(0);
        let seq = || {
            at.set(at.get() + 1);
            values[at.get() - 1]
        };
        assert_eq!(read_consistent(seq, || "copy"), Some("copy"));
        assert_eq!(at.get(), 3);
    }

    #[test]
    fn only_fresh_values_of_a_running_eyecam_rec_count() {
        let live = |live: bool, eyes: [TestEye; 2], now: u64| {
            Live::new(&parse(&live_bytes(1, 1, live, eyes)).unwrap(), now, [FRESH_NS; 2])
        };
        // The right eye 20 ms old
        let values = live(true, eyes(0.9), NOW);
        assert_eq!(values.fresh, [true, true]);
        assert!(values.present());
        // 100 ms is still fresh, a little more is not
        assert_eq!(live(true, eyes(0.9), NOW + 80_000_000).fresh, [true, true]);
        assert_eq!(live(true, eyes(0.9), NOW + 90_000_000).fresh, [true, false]);
        let stale = live(true, eyes(0.9), NOW + 200_000_000);
        assert_eq!(stale.fresh, [false; 2]);
        assert!(!stale.present());
        // Stamped in the future beyond rounding: not trusted
        assert_eq!(live(true, eyes(0.9), NOW - 3_000_000).fresh, [true; 2]);
        assert_eq!(live(true, eyes(0.9), NOW - 10_000_000).fresh, [false, true]);
        // Stopped (live 0), or an eye not valid
        assert!(!live(false, eyes(0.9), NOW).present());
        let mut one = eyes(0.9);
        one[1].valid = false;
        assert_eq!(live(true, one, NOW).fresh, [true, false]);
        // Never written
        assert_eq!(live(true, [TestEye { time_ns: 0, ..one[0] }; 2], NOW).fresh, [false; 2]);
    }

    #[test]
    fn slow_eye_cameras_keep_their_values_fresh_for_two_frames() {
        let mut gaps = FrameGaps::default();
        assert_eq!(gaps.fresh_ns(), FRESH_NS);
        // 90 frames a second (11.1 ms apart), read several times per frame: 100 ms as before
        for k in 0..20u64 {
            for _ in 0..3 {
                gaps.note(NOW + k * 11_111_111);
            }
        }
        assert_eq!(gaps.fresh_ns(), FRESH_NS);
        // 15 frames a second (66.7 ms apart): two frames, 133 ms
        let mut slow = FrameGaps::default();
        for k in 0..20u64 {
            slow.note(NOW + k * 66_666_667);
        }
        assert_eq!(slow.fresh_ns(), 133_333_334);
        // A pause of a second or more is not a frame interval
        slow.note(NOW + 60_000_000_000);
        assert_eq!(slow.fresh_ns(), 133_333_334);
        // Through the reader: a value 120 ms old is still used at 15 frames a second
        let dir = TempDir::new("slow");
        let path = dir.0.join("live");
        let start = Instant::now();
        let mut reader = LiveReader::new(path.clone(), start);
        let mut values = None;
        for k in 0..10u64 {
            let at = NOW + k * 66_666_667;
            write_live(&path, &live_bytes(1, 1, true, [TestEye::at(at, 0.9, 0.0); 2]));
            if k == 0 {
                reader.check(start);
            }
            values = reader.read(at + 120_000_000);
        }
        assert_eq!(values.unwrap().fresh, [true; 2]);
        assert_eq!(reader.read(NOW + 9 * 66_666_667 + 140_000_000).unwrap().fresh, [false; 2]);
    }

    #[test]
    fn eyelids_need_a_wear_calibration_and_pupils_only_fresh_values() {
        let live = |calib_state: u32, eyes: [TestEye; 2]| {
            Live::new(&parse(&live_bytes(1, calib_state, true, eyes)).unwrap(), NOW, [FRESH_NS; 2])
        };
        for (calib_state, lids) in [(0, false), (1, true), (2, false), (3, true), (4, true), (6, true)] {
            let values = live(calib_state, eyes(0.9));
            assert_eq!(values.lids_usable(), [lids; 2], "{calib_state}");
            assert_eq!(values.pupil_usable(), [true; 2], "{calib_state}");
        }
        // Each needs its own numbers
        let mut odd = eyes(f32::NAN);
        odd[1].pupil_mm = f32::INFINITY;
        let values = live(1, odd);
        assert_eq!(values.lids_usable(), [false, false]);
        assert_eq!(values.pupil_usable(), [true, false]);
        // Neither from a stale eye
        let values = Live::new(&parse(&live_bytes(1, 1, true, eyes(0.9))).unwrap(), NOW + 95_000_000, [FRESH_NS; 2]);
        assert_eq!((values.lids_usable(), values.pupil_usable()), ([true, false], [true, false]));
    }

    #[test]
    fn the_reader_opens_follows_and_reopens_the_file() {
        let dir = TempDir::new("reader");
        let path = dir.0.join("live");
        let start = Instant::now();
        let mut reader = LiveReader::new(path.clone(), start);
        // Not there: said once, tried again each second
        let line = reader.check(start).unwrap();
        assert!(line.starts_with("No eye-camera values from") && line.contains("No such file"), "{line}");
        assert!(!reader.opened() && reader.read(NOW).is_none());
        assert_eq!(reader.check(start + Duration::from_millis(500)), None);
        assert_eq!(reader.check(start + Duration::from_secs(1)), None);
        // There but not right: opened, with the reason
        write_live(&path, &[0u8; STRUCT_SIZE]);
        let line = reader.check(start + Duration::from_secs(2)).unwrap();
        assert!(line.contains("magic 0x00000000"), "{line}");
        assert!(reader.opened() && reader.error().unwrap().contains("magic"));
        // Right: read without opening it again, and every write shows at once
        write_live(&path, &live_bytes(10, 1, true, eyes(0.9)));
        let line = reader.check(start + Duration::from_secs(3)).unwrap();
        assert!(line.starts_with("Reading eye-camera values from") && line.ends_with("(eyecam-rec pid 10)"), "{line}");
        assert_eq!(reader.error(), None);
        assert_eq!(reader.read(NOW).unwrap().eyes[0].lid, 0.9);
        write_live(&path, &live_bytes(10, 1, true, eyes(0.95)));
        assert_eq!(reader.read(NOW).unwrap().eyes[0].lid, 0.95);
        assert_eq!(reader.check(start + Duration::from_secs(4)), None);
        // Another eyecam-rec in the same file: still read, and opened again at the next check
        write_live(&path, &live_bytes(11, 1, true, eyes(0.8)));
        assert_eq!(reader.read(NOW).unwrap().eyes[0].lid, 0.8);
        assert_eq!(reader.check(start + Duration::from_millis(4500)), None);
        assert!(reader.check(start + Duration::from_secs(5)).unwrap().ends_with("(eyecam-rec pid 11)"));
        // Replaced by a new file: opened again at the next check
        let next = dir.0.join("live.new");
        write_live(&next, &live_bytes(12, 1, true, eyes(0.85)));
        fs::rename(&next, &path).unwrap();
        assert_eq!(reader.read(NOW).unwrap().eyes[0].lid, 0.8);
        assert!(reader.check(start + Duration::from_secs(6)).unwrap().ends_with("(eyecam-rec pid 12)"));
        assert_eq!(reader.read(NOW).unwrap().eyes[0].lid, 0.85);
        // Rewritten as something else: let go at once, and why is said at the next check
        let mut bytes = live_bytes(12, 1, true, eyes(0.85));
        bytes[4] = 9;
        write_live(&path, &bytes);
        assert!(reader.read(NOW).is_none() && reader.read(NOW).is_none());
        assert!(reader.check(start + Duration::from_secs(7)).unwrap().contains("unsupported version 9"));
        // Gone: not opened
        fs::remove_file(&path).unwrap();
        assert!(reader.check(start + Duration::from_secs(8)).unwrap().contains("No such file"));
        assert!(!reader.opened());
    }

    #[test]
    fn only_a_regular_file_is_opened_and_never_waited_for() {
        let dir = TempDir::new("kinds");
        let start = Instant::now();
        // A FIFO in its place: refused at once (a blocking open would wait for a writer forever)
        let fifo = dir.0.join("fifo");
        let name = std::ffi::CString::new(fifo.as_os_str().as_encoded_bytes()).unwrap();
        assert_eq!(unsafe { libc::mkfifo(name.as_ptr(), 0o600) }, 0);
        let mut reader = LiveReader::new(fifo, start);
        let line = reader.check(start).unwrap();
        assert!(line.contains("not a regular file"), "{line}");
        assert!(reader.read(NOW).is_none());
        // A symlink to a good file: not followed
        let real = dir.0.join("real");
        write_live(&real, &live_bytes(10, 1, true, eyes(0.9)));
        let link = dir.0.join("link");
        std::os::unix::fs::symlink(&real, &link).unwrap();
        let mut reader = LiveReader::new(link, start);
        assert!(reader.check(start).unwrap().starts_with("No eye-camera values from"));
        assert!(!reader.opened() && reader.read(NOW).is_none());
        // A directory: not a regular file either
        let mut reader = LiveReader::new(dir.0.clone(), start);
        assert!(reader.check(start).unwrap().contains("not a regular file"));
        // The good file itself: read
        let mut reader = LiveReader::new(real, start);
        assert!(reader.check(start).unwrap().starts_with("Reading eye-camera values from"));
        assert_eq!(reader.read(NOW).unwrap().eyes[0].lid, 0.9);
    }

    #[test]
    fn the_file_is_never_written() {
        let dir = TempDir::new("readonly");
        let path = dir.0.join("live");
        let bytes = live_bytes(10, 1, true, eyes(0.9));
        write_live(&path, &bytes);
        // Read-only to its owner too, as far as opening goes
        let mut permissions = fs::metadata(&path).unwrap().permissions();
        std::os::unix::fs::PermissionsExt::set_mode(&mut permissions, 0o444);
        fs::set_permissions(&path, permissions).unwrap();
        let mut reader = LiveReader::new(path.clone(), Instant::now());
        assert!(reader.check(Instant::now()).unwrap().starts_with("Reading"));
        for _ in 0..10 {
            reader.read(NOW).unwrap();
        }
        let written = fs::read(&path).unwrap();
        assert_eq!(written[..STRUCT_SIZE], bytes);
        assert!(written[STRUCT_SIZE..].iter().all(|byte| *byte == 0));
    }
}
