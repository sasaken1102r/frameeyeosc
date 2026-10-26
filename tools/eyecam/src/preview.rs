//! The eye pictures for the panel's eye preview, `/run/user/1000/eyecam/preview`: while the panel asks (`preview on` on
//! ctl.sock, renewed every second while its preview shows; off by itself HOLD after the last, so a panel that crashed
//! leaves nothing running), each eye's frame as the live engine saw it, halved to SIDE x SIDE and about RATE a second,
//! with what the engine found on it. Only ever in this file, in the run folder (tmpfs), mode 0600, and only while on:
//! turned off, the pictures are zeroed and the file removed. Nothing is ever written to disk.
//!
//! One fixed little-endian layout (FILE_SIZE bytes), written with a seqlock (as the live shared memory):
//!   0   u32  magic (MAGIC, "EYPV")     4   u32  version (VERSION)    8   u32  header size (HEADER)
//!   12  u32  one eye's size (EYE_SIZE)  16  u32  picture width (SIDE)  20  u32  picture height (SIDE)
//!   24  u32  seq (odd while being written)   28  u32  writer pid   32  u64  writer start (CLOCK_MONOTONIC ns)
//!   40  u64  last write (CLOCK_MONOTONIC ns) 48  u32  on (1 while on; 0 once turned off)
//!   52  u32  where the lid lines start (LIDS_AT)   56  u32  one eye's lid lines' size (LID_SIZE)   60..64 reserved
//! Then each eye (the left eye at HEADER, the right at HEADER + EYE_SIZE; anatomical):
//!   0   u64  the frame's capture time (CLOCK_MONOTONIC ns; 0: no picture yet)   8  u64  pictures written of this eye
//!   16  u32  flags: bit 0 the pupil was found, bit 1 the eye reads closed
//!   20  f32  pupil centre x   24  f32  ...y   28  f32  pupil radius   (picture px; NaN without a pupil)
//!   32  f32  pupil search window left   36  f32  ...top   40  f32  ...right   44  f32  ...bottom   (picture px)
//!   48..64 reserved
//!   64  SIDE x SIDE bytes: the picture, 8-bit grey, row by row from the top, upright (the left camera's frames, stored
//!       upside down, are turned the right way first), the temporal (ear) side at small x for both eyes, as the engine
//!       sees them. The coordinates above are in the same picture.
//! Then, after both eyes (at LIDS_AT, in the last page's space version 1 left unused: a reader that doesn't know them
//! reads the rest as before), each eye's lid lines as the engine found them on the same frame, LID_SIZE bytes each:
//!   0   f32  the EyeLid sent (NaN while the eye's values aren't valid)
//!   4   f32  the upper lid's skin line: its first column   8  ...last column   12  its row at the first column
//!   16  ...halfway between   20  ...at the last column (the line is the parabola through these three; picture px;
//!       all NaN without one on this frame)
//!   24  f32  the lid margin's offset below the skin line (picture px; the line the aperture is measured from)
//!   28  f32  the lower lid's line, as the upper one's (28 first column .. 44 row at the last column)
//!   48..64 reserved

use crate::feat::LidCurve;
use crate::live::Out;
use crate::liveshm::clock_ns;
use crate::vision::{H, W};
use std::fs::{File, OpenOptions};
use std::os::fd::AsRawFd;
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering, fence};
use std::time::Duration;

/// "EYPV" as little-endian bytes.
pub const MAGIC: u32 = u32::from_le_bytes(*b"EYPV");
pub const VERSION: u32 = 1;
pub const FILE_NAME: &str = "preview";
/// The pictures are the frames halved (2 x 2 averages).
pub const SIDE: usize = W / 2;
pub const HEADER: usize = 64;
const EYE_HEADER: usize = 64;
pub const EYE_SIZE: usize = EYE_HEADER + SIDE * SIDE;
/// The lid lines, after both eyes' pictures (in the last page's space that version 1 left unused).
pub const LIDS_AT: usize = HEADER + 2 * EYE_SIZE;
pub const LID_SIZE: usize = 64;
/// The whole file, in whole pages.
pub const FILE_SIZE: usize = (LIDS_AT + 2 * LID_SIZE).div_ceil(4096) * 4096;
const _: () = assert!(FILE_SIZE == 81920, "version 1's size");
/// Pictures a second per eye, at most (the frames between are skipped).
pub const RATE: f64 = 15.0;
/// How long one `preview on` keeps it on.
pub const HOLD: Duration = Duration::from_secs(3);
/// The rows the pupil search looks in (feat's window), and halved for the picture.
const WINDOW_Y: (f64, f64) = (crate::feat::SEARCH_Y0 as f64, crate::feat::SEARCH_Y1 as f64);
const SEQ: usize = 24;
const PUPIL_FOUND: u32 = 1;
const CLOSED: u32 = 2;
/// tmpfs's f_type (statfs), where the pictures may go.
const TMPFS_MAGIC: i64 = 0x0102_1994;

/// What one picture shows besides the frame: where the pupil was found, where it was looked for, and the lids.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Marks {
    /// The pupil's centre and radius (picture px), if it was found.
    pub pupil: Option<(f32, f32, f32)>,
    /// The search window: left, top, right, bottom (picture px).
    pub window: [f32; 4],
    pub closed: bool,
    /// The upper lid's skin line and the lower lid's line, if this frame has one: first and last column, and the row
    /// at the first, halfway and at the last (picture px).
    pub upper: Option<[f32; 5]>,
    pub lower: Option<[f32; 5]>,
    /// The lid margin's offset below the skin line (picture px; NaN without the upper line).
    pub margin: f32,
    /// The EyeLid sent (NaN while the values aren't valid).
    pub eye_lid: f32,
}

/// A lid line's ends and middle, halved like the picture.
fn lid_line(c: &LidCurve) -> [f32; 5] {
    let xm = (c.x0 + c.x1) / 2.0;
    [c.x0, c.x1, c.coef.at(c.x0), c.coef.at(xm), c.coef.at(c.x1)].map(|v| (v / 2.0) as f32)
}

impl Marks {
    /// What the engine found on a frame (`o`), halved like the picture.
    pub fn of(o: &Out) -> Self {
        let f = &o.f;
        let seen = f.ok_pupil && f.pupil_cx.is_finite() && f.pupil_cy.is_finite() && f.pupil_a.is_finite();
        let upper = f.upper_skin_y.is_finite() && !f.upper_edge;
        Self {
            pupil: seen.then(|| ((f.pupil_cx / 2.0) as f32, (f.pupil_cy / 2.0) as f32, (f.pupil_a / 2.0) as f32)),
            // The left edge as far as the pupil was looked for: the extended search's when it reaches past x_min.
            window: [o.search_x_min, WINDOW_Y.0, o.xmax, WINDOW_Y.1].map(|v| (v / 2.0) as f32),
            closed: o.closed,
            // The lines this frame's values were measured from: none where it has none (pushed against its band's
            // edge, or a lower line the engine replaced with the last one, live::LOWER_DROP_MAX)
            upper: f.upper_curve.filter(|_| upper).map(|c| lid_line(&c)),
            lower: f.lower_curve.filter(|_| f.lower_y.is_finite() && !f.lower_edge).map(|c| lid_line(&c)),
            margin: if upper { ((f.upper_y - f.upper_skin_y) / 2.0) as f32 } else { f32::NAN },
            eye_lid: if o.valid { o.eye_lid as f32 } else { f32::NAN },
        }
    }
}

/// A 400 x 400 frame halved: each picture pixel the rounded mean of a 2 x 2 block.
pub fn halve(frame: &[u8], out: &mut [u8]) {
    for y in 0..SIDE {
        let (top, bottom) = (&frame[2 * y * W..(2 * y + 1) * W], &frame[(2 * y + 1) * W..(2 * y + 2) * W]);
        for (x, px) in out[y * SIDE..(y + 1) * SIDE].iter_mut().enumerate() {
            let sum = top[2 * x] as u32 + top[2 * x + 1] as u32 + bottom[2 * x] as u32 + bottom[2 * x + 1] as u32;
            *px = ((sum + 2) / 4) as u8;
        }
    }
}

/// Whether `dir` is on tmpfs (memory only), the only place `eyecam-rec --serve` puts the pictures.
pub fn on_tmpfs(dir: &Path) -> bool {
    use std::os::unix::ffi::OsStrExt;
    let Ok(path) = std::ffi::CString::new(dir.as_os_str().as_bytes()) else { return false };
    let mut fs: libc::statfs = unsafe { std::mem::zeroed() };
    unsafe { libc::statfs(path.as_ptr(), &mut fs) == 0 && fs.f_type as i64 == TMPFS_MAGIC }
}

/// The file while the preview is on.
pub struct PreviewWriter {
    ptr: *mut u8,
    _file: File,
    pub path: PathBuf,
}

// The mapping is only touched through &mut self.
unsafe impl Send for PreviewWriter {}

impl PreviewWriter {
    /// Create the file (mode 0600, FILE_SIZE bytes) with its header and no pictures yet. Like the live shared memory,
    /// it is prepared next to its place and renamed over it, so a file left by an earlier run is never changed in place.
    pub fn create(path: &Path) -> Result<Self, String> {
        let name = path.file_name().and_then(|n| n.to_str()).ok_or("bad preview path")?;
        let tmp = path.with_file_name(format!(".{name}.new"));
        let _ = std::fs::remove_file(&tmp);
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .create_new(true)
            .mode(0o600)
            .custom_flags(libc::O_NOFOLLOW)
            .open(&tmp)
            .map_err(|e| format!("{}: {e}", tmp.display()))?;
        file.set_permissions(std::fs::Permissions::from_mode(0o600)).map_err(|e| e.to_string())?;
        file.set_len(FILE_SIZE as u64).map_err(|e| e.to_string())?;
        let p = unsafe {
            libc::mmap(std::ptr::null_mut(), FILE_SIZE, libc::PROT_READ | libc::PROT_WRITE, libc::MAP_SHARED, file.as_raw_fd(), 0)
        };
        if p == libc::MAP_FAILED {
            let _ = std::fs::remove_file(&tmp);
            return Err(format!("mmap {}: {}", tmp.display(), std::io::Error::last_os_error()));
        }
        let mut w = Self { ptr: p.cast(), _file: file, path: path.to_path_buf() };
        let start = clock_ns(libc::CLOCK_MONOTONIC);
        w.write(|b| {
            for (at, v) in [
                (4, VERSION),
                (8, HEADER as u32),
                (12, EYE_SIZE as u32),
                (16, SIDE as u32),
                (20, SIDE as u32),
                (52, LIDS_AT as u32),
                (56, LID_SIZE as u32),
            ] {
                put_u32(b, at, v);
            }
            put_u32(b, 28, std::process::id());
            put_u64(b, 32, start);
            put_u32(b, 48, 1);
            put_u32(b, 0, MAGIC);
        });
        if let Err(e) = std::fs::rename(&tmp, path) {
            let _ = std::fs::remove_file(&tmp);
            return Err(format!("{} -> {}: {e}", tmp.display(), path.display()));
        }
        Ok(w)
    }

    /// Change the file under the seqlock.
    fn write(&mut self, f: impl FnOnce(&mut [u8])) {
        let seq = unsafe { AtomicU32::from_ptr(self.ptr.add(SEQ).cast()) };
        let s0 = seq.load(Ordering::Relaxed) & !1;
        seq.store(s0.wrapping_add(1), Ordering::Relaxed);
        fence(Ordering::Release);
        // (the seq word itself is only touched through the atomic; f never writes it)
        let bytes = unsafe { std::slice::from_raw_parts_mut(self.ptr, FILE_SIZE) };
        f(bytes);
        seq.store(s0.wrapping_add(2), Ordering::Release);
    }

    /// One eye's picture: `frame` (400 x 400, upright) halved, with its marks, captured at `t_mono_ns`.
    pub fn picture(&mut self, eye: usize, frame: &[u8], marks: &Marks, t_mono_ns: u64) {
        let at = HEADER + eye * EYE_SIZE;
        self.write(|b| {
            let count = u64::from_le_bytes(b[at + 8..at + 16].try_into().unwrap()) + 1;
            put_u64(b, at, t_mono_ns);
            put_u64(b, at + 8, count);
            let flags = if marks.pupil.is_some() { PUPIL_FOUND } else { 0 } | if marks.closed { CLOSED } else { 0 };
            put_u32(b, at + 16, flags);
            let (cx, cy, r) = marks.pupil.unwrap_or((f32::NAN, f32::NAN, f32::NAN));
            for (i, v) in [cx, cy, r].into_iter().chain(marks.window).enumerate() {
                put_u32(b, at + 20 + i * 4, v.to_bits());
            }
            halve(frame, &mut b[at + EYE_HEADER..at + EYE_SIZE]);
            let nan = [f32::NAN; 5];
            let lids = [marks.eye_lid]
                .into_iter()
                .chain(marks.upper.unwrap_or(nan))
                .chain([marks.margin])
                .chain(marks.lower.unwrap_or(nan));
            for (i, v) in lids.enumerate() {
                put_u32(b, LIDS_AT + eye * LID_SIZE + i * 4, v.to_bits());
            }
            put_u64(b, 40, t_mono_ns.max(u64::from_le_bytes(b[40..48].try_into().unwrap())));
        });
    }

    /// The contents now (for tests and the replay's check).
    pub fn read(&self) -> Vec<u8> {
        unsafe { std::slice::from_raw_parts(self.ptr, FILE_SIZE) }.to_vec()
    }
}

impl Drop for PreviewWriter {
    /// Turned off: the pictures and their marks zeroed (a reader that still maps the file sees none), then the file
    /// removed.
    fn drop(&mut self) {
        self.write(|b| {
            put_u32(b, 48, 0);
            for eye in 0..2 {
                b[HEADER + eye * EYE_SIZE..HEADER + (eye + 1) * EYE_SIZE].fill(0);
            }
            b[LIDS_AT..LIDS_AT + 2 * LID_SIZE].fill(0);
        });
        unsafe { libc::munmap(self.ptr.cast(), FILE_SIZE) };
        let _ = std::fs::remove_file(&self.path);
    }
}

fn put_u32(b: &mut [u8], at: usize, v: u32) {
    b[at..at + 4].copy_from_slice(&v.to_le_bytes());
}

fn put_u64(b: &mut [u8], at: usize, v: u64) {
    b[at..at + 8].copy_from_slice(&v.to_le_bytes());
}

/// The preview as the live worker runs it: on while asked (until `until`), the file made when the first picture is
/// due and removed once it is off, and RATE pictures a second per eye (by the frames' camera times).
pub struct Preview {
    path: PathBuf,
    writer: Option<PreviewWriter>,
    /// The camera time of each eye's last picture (s).
    last: [f64; 2],
    /// Time spent on the pictures (halving and writing) and how many, for the log.
    pub busy: Duration,
    pub pictures: u64,
    failed: bool,
}

impl Preview {
    /// `path`: where the file goes (`<run dir>/preview`).
    pub fn new(path: PathBuf) -> Self {
        Self { path, writer: None, last: [f64::NEG_INFINITY; 2], busy: Duration::ZERO, pictures: 0, failed: false }
    }

    /// Whether the file is there (the preview on).
    pub fn is_on(&self) -> bool {
        self.writer.is_some()
    }

    /// A frame of `eye` (upright, 400 x 400) at camera time `t` (s), captured at `t_mono_ns`, while the preview is
    /// `wanted` or not: a picture when one is due.
    pub fn frame(&mut self, wanted: bool, eye: usize, frame: &[u8], o: &Out, t: f64, t_mono_ns: u64) {
        if !self.keep(wanted) || frame.len() != W * H {
            return;
        }
        // Due once 1 / RATE has passed, less a little so a 90 fps camera gives every 6th frame (and a time going back,
        // a new wear's, starts over)
        let due = !(t - self.last[eye] < 0.9 / RATE && t >= self.last[eye]);
        if !due {
            return;
        }
        self.last[eye] = t;
        let started = std::time::Instant::now();
        if let Some(w) = self.writer.as_mut() {
            w.picture(eye, frame, &Marks::of(o), t_mono_ns);
        }
        self.busy += started.elapsed();
        self.pictures += 1;
    }

    /// Follow `wanted`: open the file when it is first wanted, remove it when it no longer is. Returns whether it is
    /// open.
    pub fn keep(&mut self, wanted: bool) -> bool {
        if !wanted {
            if let Some(w) = self.writer.take() {
                eprintln!("preview: off ({} pictures, {:.0} us each)", self.pictures, self.us_per_picture());
                drop(w);
                self.last = [f64::NEG_INFINITY; 2];
            }
            self.failed = false;
            return false;
        }
        if self.writer.is_none() && !self.failed {
            match PreviewWriter::create(&self.path) {
                Ok(w) => {
                    eprintln!("preview: on ({})", self.path.display());
                    self.writer = Some(w);
                    self.busy = Duration::ZERO;
                    self.pictures = 0;
                }
                Err(e) => {
                    // Once per `on`: not again on every frame
                    eprintln!("preview: {e}");
                    self.failed = true;
                }
            }
        }
        self.writer.is_some()
    }

    /// The mean time one picture took (us).
    pub fn us_per_picture(&self) -> f64 {
        if self.pictures == 0 { 0.0 } else { self.busy.as_secs_f64() * 1e6 / self.pictures as f64 }
    }
}

/// A picture as a reader sees it (tests, and the replay's check).
#[derive(Clone, Debug, PartialEq)]
pub struct Seen {
    pub on: bool,
    pub time_ns: [u64; 2],
    pub count: [u64; 2],
    pub flags: [u32; 2],
    pub marks: [[f32; 7]; 2],
    pub pictures: [Vec<u8>; 2],
    /// The lid lines: the EyeLid, the upper line (5), the margin, the lower line (5).
    pub lids: [[f32; 12]; 2],
}

/// Read the file's bytes as a reader would: the header checked (magic, version, sizes), None if it isn't one.
pub fn parse(b: &[u8]) -> Option<Seen> {
    let u32_at = |at: usize| u32::from_le_bytes(b[at..at + 4].try_into().unwrap());
    let u64_at = |at: usize| u64::from_le_bytes(b[at..at + 8].try_into().unwrap());
    if b.len() < FILE_SIZE
        || u32_at(0) != MAGIC
        || u32_at(4) != VERSION
        || u32_at(8) as usize != HEADER
        || u32_at(12) as usize != EYE_SIZE
        || (u32_at(16) as usize, u32_at(20) as usize) != (SIDE, SIDE)
        || (u32_at(52) as usize, u32_at(56) as usize) != (LIDS_AT, LID_SIZE)
        || u32_at(SEQ) % 2 == 1
    {
        return None;
    }
    let at = |eye: usize| HEADER + eye * EYE_SIZE;
    Some(Seen {
        on: u32_at(48) == 1,
        time_ns: [0, 1].map(|e| u64_at(at(e))),
        count: [0, 1].map(|e| u64_at(at(e) + 8)),
        flags: [0, 1].map(|e| u32_at(at(e) + 16)),
        marks: [0, 1].map(|e| std::array::from_fn(|i| f32::from_bits(u32_at(at(e) + 20 + i * 4)))),
        pictures: [0, 1].map(|e| b[at(e) + EYE_HEADER..at(e) + EYE_SIZE].to_vec()),
        lids: [0, 1].map(|e| std::array::from_fn(|i| f32::from_bits(u32_at(LIDS_AT + e * LID_SIZE + i * 4)))),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::feat::Features;
    use crate::vision::Quad;

    fn out(seen: bool, closed: bool) -> Out {
        let mut o = Out::default();
        o.f = Features { ok_pupil: seen, pupil_cx: 240.0, pupil_cy: 190.0, pupil_a: 21.0, ..Features::default() };
        o.closed = closed;
        o.x_min = 186.0;
        o.search_x_min = 186.0;
        o.xmax = 346.0;
        o
    }

    /// A made-up frame (no eye in it): a gradient with a dark disc.
    fn frame() -> Vec<u8> {
        (0..W * H)
            .map(|i| {
                let (x, y) = ((i % W) as f64, (i / W) as f64);
                if (x - 240.0).hypot(y - 190.0) < 21.0 { 10 } else { (60.0 + x * 0.3 + y * 0.1) as u8 }
            })
            .collect()
    }

    #[test]
    fn halving_averages_two_by_two_blocks() {
        let mut f = vec![0u8; W * H];
        f[0] = 10;
        f[1] = 11;
        f[W] = 12;
        f[W + 1] = 14;
        f[2 * W + 2] = 255;
        let mut out = vec![0u8; SIDE * SIDE];
        halve(&f, &mut out);
        // (10 + 11 + 12 + 14 + 2) / 4 = 12; a lone 255 in a block of zeros 64
        assert_eq!((out[0], out[SIDE + 1], out[1]), (12, 64, 0));
    }

    #[test]
    fn pictures_read_back_with_their_marks() {
        let path = std::env::temp_dir().join(format!("eyecam-preview-test-{}", std::process::id()));
        let mut w = PreviewWriter::create(&path).unwrap();
        let mode = std::fs::metadata(&path).unwrap().permissions().mode() & 0o777;
        assert_eq!((mode, std::fs::metadata(&path).unwrap().len()), (0o600, FILE_SIZE as u64));
        let f = frame();
        w.picture(1, &f, &Marks::of(&out(true, false)), 5_000);
        w.picture(0, &f, &Marks::of(&out(false, true)), 6_000);
        w.picture(1, &f, &Marks::of(&out(true, false)), 7_000);
        let seen = parse(&w.read()).unwrap();
        assert!(seen.on);
        assert_eq!((seen.time_ns, seen.count, seen.flags), ([6_000, 7_000], [1, 2], [CLOSED, PUPIL_FOUND]));
        assert_eq!(seen.marks[1], [120.0, 95.0, 10.5, 93.0, 35.0, 173.0, 160.0]);
        assert!(seen.marks[0][0].is_nan() && seen.marks[0][3] == 93.0);
        // The window's left edge is where the pupil was looked for: the extended search's when it reaches further
        let mut ext = out(true, false);
        ext.search_x_min = 162.0;
        assert_eq!(Marks::of(&ext).window, [81.0, 35.0, 173.0, 160.0]);
        let mut half = vec![0u8; SIDE * SIDE];
        halve(&f, &mut half);
        assert_eq!(seen.pictures[0], half);
        // The disc is dark in the picture at its halved place
        assert!(half[95 * SIDE + 120] < 20 && half[10 * SIDE + 10] > 50);
        // From the file itself, as the panel reads it (compared as printed: no pupil is NaN)
        assert_eq!(format!("{:?}", parse(&std::fs::read(&path).unwrap())), format!("{:?}", Some(seen)));
        // Turned off: zeroed in place first (whoever still maps it sees no picture), then removed
        let f2 = File::open(&path).unwrap();
        let p = unsafe { libc::mmap(std::ptr::null_mut(), FILE_SIZE, libc::PROT_READ, libc::MAP_SHARED, f2.as_raw_fd(), 0) };
        assert_ne!(p, libc::MAP_FAILED);
        drop(w);
        assert!(!path.exists());
        let mapped = unsafe { std::slice::from_raw_parts(p.cast::<u8>(), FILE_SIZE) }.to_vec();
        unsafe { libc::munmap(p, FILE_SIZE) };
        let after = parse(&mapped).unwrap();
        assert!(!after.on && after.time_ns == [0, 0] && after.pictures.iter().all(|p| p.iter().all(|&v| v == 0)));
        assert_eq!(after.lids, [[0.0; 12]; 2]);
        // Not the preview's: refused
        let mut bad = mapped.clone();
        bad[0] = b'X';
        assert!(parse(&bad).is_none());
        let mut bad = mapped.clone();
        bad[16] = 100;
        assert!(parse(&bad).is_none());
        assert!(parse(&mapped[..FILE_SIZE - 1]).is_none());
    }

    /// `out` with both lid lines (parabolas over columns 180..330 that halve to exact floats), its values valid.
    fn out_with_lids() -> Out {
        let mut o = out(true, false);
        let curve = |c2: f64, y: f64| LidCurve { coef: Quad { c: [c2, 0.0, y], xm: 240.0 }, x0: 180.0, x1: 330.0 };
        o.f.upper_skin_y = 150.0;
        o.f.upper_y = 158.0;
        o.f.upper_curve = Some(curve(1.0 / 512.0, 150.0));
        o.f.lower_y = 240.0;
        o.f.lower_curve = Some(curve(-1.0 / 512.0, 240.0));
        o.valid = true;
        o.eye_lid = 0.625;
        o
    }

    #[test]
    fn lid_lines_read_back_halved_and_nan_without_one() {
        let path = std::env::temp_dir().join(format!("eyecam-preview-lids-{}", std::process::id()));
        let mut w = PreviewWriter::create(&path).unwrap();
        let f = frame();
        // Both lines: the ends and the middle (column 255) of each parabola, halved, and the margin 8 px below
        w.picture(0, &f, &Marks::of(&out_with_lids()), 5_000);
        // None: the upper line pushed against its band's edge (feat leaves it NaN), the lower one replaced by the last
        // one (live::LOWER_DROP_MAX: lower_y is that one's, not this frame's line), the values not valid
        let mut none = out_with_lids();
        (none.f.upper_edge, none.f.upper_skin_y, none.f.upper_y) = (true, f64::NAN, f64::NAN);
        none.f.lower_edge = true;
        none.valid = false;
        w.picture(1, &f, &Marks::of(&none), 6_000);
        let seen = parse(&std::fs::read(&path).unwrap()).unwrap();
        let (up, mid, low) = (78.515625, 75.2197265625, 82.91015625);
        let (up2, mid2, low2) = (116.484375, 119.7802734375, 112.08984375);
        assert_eq!(seen.lids[0], [0.625, 90.0, 165.0, up, mid, low, 4.0, 90.0, 165.0, up2, mid2, low2]);
        assert!(seen.lids[1].iter().all(|v| v.is_nan()), "{:?}", seen.lids[1]);
        // The rest of the eye as before (a reader of version 1 reads the same)
        assert_eq!(seen.marks[0], [120.0, 95.0, 10.5, 93.0, 35.0, 173.0, 160.0]);
        assert_eq!(&w.read()[52..60], &[(LIDS_AT as u32).to_le_bytes(), (LID_SIZE as u32).to_le_bytes()].concat()[..]);
        // A lower line without the pupil is still drawn (the lids are measured where the pupil last was)
        let mut lost = out_with_lids();
        lost.f.ok_pupil = false;
        assert_eq!(Marks::of(&lost).lower, Some([90.0, 165.0, up2, mid2, low2]));
        drop(w);
    }

    #[test]
    fn on_only_while_asked_and_fifteen_a_second() {
        let dir = std::env::temp_dir().join(format!("eyecam-preview-run-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join(FILE_NAME);
        let mut p = Preview::new(path.clone());
        let f = frame();
        let o = out(true, false);
        // Not asked: no file, nothing written
        p.frame(false, 0, &f, &o, 1.0, 1);
        assert!(!path.exists() && p.pictures == 0);
        // Asked: two seconds of 90 fps frames of both eyes give 15 pictures a second of each
        for k in 0..180 {
            for eye in 0..2 {
                p.frame(true, eye, &f, &o, 10.0 + k as f64 / 90.0, k as u64 + 1);
            }
        }
        assert!(path.exists() && p.is_on());
        assert_eq!(p.pictures, 60, "every 6th frame of each eye");
        let seen = parse(&std::fs::read(&path).unwrap()).unwrap();
        assert_eq!(seen.count, [30, 30]);
        // A 72 fps camera: 14 or 15 a second
        let before = p.pictures;
        for k in 0..72 {
            p.frame(true, 0, &f, &o, 20.0 + k as f64 / 72.0, 1);
        }
        assert!((14..=15).contains(&(p.pictures - before)), "{}", p.pictures - before);
        // Not asked any more (the panel stopped renewing): removed, and the next `on` starts a new file
        assert!(!p.keep(false));
        assert!(!path.exists());
        p.frame(true, 1, &f, &o, 30.0, 1);
        assert_eq!(parse(&std::fs::read(&path).unwrap()).unwrap().count, [0, 1]);
        drop(p);
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn the_run_folder_must_be_tmpfs() {
        // /dev/shm is tmpfs on every Linux this runs on; /proc never is (proc), wherever the sources are
        assert!(on_tmpfs(Path::new("/dev/shm")));
        assert!(!on_tmpfs(Path::new("/proc")));
        assert!(!on_tmpfs(Path::new("/nonexistent/x")));
    }
}
