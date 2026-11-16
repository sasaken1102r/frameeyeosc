//! The live output shared memory, `/run/user/1000/eyecam/live`: one fixed little-endian struct, written by
//! eyecam-rec with a seqlock, read by frameeyeosc (same uid, O_RDONLY + mmap, never writes). The README's
//! "live shared memory" section is the contract; bump VERSION on any change to the layout or meaning of what is there.
//! Additions go after it, found by `struct_size` (a reader of version 1 reads the first 200 bytes and checks only that
//! struct_size is at least that): the glint part (`glint_version`, GLINT_VERSION) since 2026-11, then the pupil shape
//! part (`shape_version`, SHAPE_VERSION; a reader of the glint part reads 304 bytes and checks only that struct_size is
//! at least that), then the lower-lid part (`lower_version`, LOWER_VERSION; a reader of the shape part reads 344 bytes
//! and checks only that struct_size is at least that).

use std::fs::{File, OpenOptions};
use std::mem::{offset_of, size_of};
use std::os::fd::AsRawFd;
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering, fence};

/// "EYCM" as little-endian bytes.
pub const MAGIC: u32 = u32::from_le_bytes(*b"EYCM");
pub const VERSION: u32 = 1;
/// The glint part's version (`glint_version`; 0 = not written).
pub const GLINT_VERSION: u32 = 1;
/// The pupil shape part's version (`shape_version`; 0 = not written). 2 since 2026-11-15: the ellipse is the pupil's
/// outline for the eye-camera gaze (feat::Pupil::outline), the layout unchanged; 1 had the pupil's own fit, which a
/// lighter part of the pupil (next to glints on its rim) could cut.
pub const SHAPE_VERSION: u32 = 2;
/// The lower-lid part's version (`lower_version`; 0 = not written).
pub const LOWER_VERSION: u32 = 1;
/// The file is one page; struct_size says how much of it is the struct.
const FILE_SIZE: usize = 4096;

/// Per eye (index 0 = left, 1 = right, anatomical).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct LiveEye {
    /// CLOCK_MONOTONIC ns when this eye's latest frame was captured (converted from the camera's timestamp).
    pub t_mono_ns: u64,
    /// The camera's own timestamp of that frame (CLOCK_MONOTONIC_RAW ns).
    pub t_cam_raw_ns: u64,
    /// Frames processed for this eye since eyecam-rec started.
    pub frame_count: u64,
    /// 1 when the values below describe this frame (pupil, closed lid or lids seen); 0 otherwise.
    pub valid: u32,
    /// 1 when the eye is closed.
    pub closed: u32,
    /// VRCFT scale: 0 closed, 0.75 calibrated normal, 1 full widen.
    pub eye_lid: f32,
    pub eye_wide: f32,
    pub eye_squint: f32,
    /// Pupil diameter / iris radius (2a / R); about 0.6 bright .. 1.05 dark.
    pub pupil_ratio: f32,
    /// pupil_ratio x 5.9 mm (iris assumed 11.8 mm across).
    pub pupil_mm: f32,
    /// 0..1 within the wearer's pupil range (measured, or 0.6..1.05).
    pub pupil_dilation: f32,
    pub confidence: f32,
    /// Raw features for debugging (iris radii): pupil centre - upper lid skin line, lower - upper lid.
    pub skin_up: f32,
    pub aperture: f32,
    pub _pad: u32,
}

/// Per eye, the glint part (index 0 = left, 1 = right): the same frame as the eye's values above (written with them).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct LiveGlint {
    /// Bit 0: a pupil in this frame (pupil_* are its centre); bit 1: the slip was worked out from this frame (else held);
    /// bit 2: there is a template (slip_* mean something).
    pub flags: u32,
    /// The template in use: 0 before the first, counting up (each new one starts from slip 0).
    pub template_id: u32,
    /// When it was taken (CLOCK_MONOTONIC ns).
    pub template_mono_ns: u64,
    /// The pupil centre (px, the upright 400x400 picture: x right, y down; NaN without a pupil).
    pub pupil_x: f32,
    pub pupil_y: f32,
    /// The glints' offset from the template (px, the same axes): the headset's slip since it was taken.
    pub slip_x: f32,
    pub slip_y: f32,
    /// Glints matched to the template in this frame, frames since the slip was last worked out, glints found.
    pub matches: u32,
    pub since_good: u32,
    pub glints: u32,
    pub _pad: u32,
}

/// Per eye, the pupil shape part (index 0 = left, 1 = right): the pupil's outline (feat::Pupil::outline) of the same
/// frame as the eye's values and glint part above (written with them), in the same picture as `LiveGlint::pupil_x` /
/// `pupil_y`. Meaningful only with a pupil in this frame (glint flags bit 0); NaN without one or without an outline
/// (0 before the eye's first frame).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct LiveShape {
    /// The ellipse's semi-axes (px), a >= b.
    pub a: f32,
    pub b: f32,
    /// The direction of its long axis in the picture, degrees from +x towards +y, in [0, 180).
    pub angle_deg: f32,
    /// The share of its rays that found its edge (0..1; lids and lashes hide the rest).
    pub vis: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct LiveShm {
    pub magic: u32,
    pub version: u32,
    pub struct_size: u32,
    pub writer_pid: u32,
    /// CLOCK_MONOTONIC ns when the writer started (a restart changes it).
    pub writer_start_ns: u64,
    /// Seqlock: odd while being written. Read it, copy the struct, read it again; retry if odd or changed.
    pub seq: u32,
    /// 0 none, 1 wear calibrated, 2 user calibrated, 3 both.
    pub calib_state: u32,
    /// The latest frame of either eye.
    pub t_mono_ns: u64,
    pub t_cam_raw_ns: u64,
    pub recalib_suggested: u32,
    /// 1 while live processing is on (fds held, `live on`); 0 when off or eyecam-rec stopped.
    pub live: u32,
    pub eyes: [LiveEye; 2],
    /// GLINT_VERSION once the glint part is written (eyecam-rec since 2026-11).
    pub glint_version: u32,
    pub _pad: u32,
    pub glint: [LiveGlint; 2],
    /// SHAPE_VERSION once the pupil shape part is written.
    pub shape_version: u32,
    /// Bit e (0 = left, 1 = right): that eye's latest picture was turned upright (flipped top to bottom from the
    /// sensor's) before it was processed, so its pupil and ellipse are in the flipped picture.
    pub flipped: u32,
    pub shape: [LiveShape; 2],
    /// LOWER_VERSION once the lower-lid part is written.
    pub lower_version: u32,
    pub _pad2: u32,
    /// Each eye's lower-lid raise trial (`live::Out::lower_lid`), 0 relaxed .. 1 raised; NaN while settings.json
    /// `lower_lid` is off.
    pub lower_lid: [f32; 2],
}

const _: () = {
    assert!(offset_of!(LiveShm, writer_start_ns) == 16);
    assert!(offset_of!(LiveShm, seq) == 24);
    assert!(offset_of!(LiveShm, calib_state) == 28);
    assert!(offset_of!(LiveShm, t_mono_ns) == 32);
    assert!(offset_of!(LiveShm, t_cam_raw_ns) == 40);
    assert!(offset_of!(LiveShm, recalib_suggested) == 48);
    assert!(offset_of!(LiveShm, live) == 52);
    assert!(offset_of!(LiveShm, eyes) == 56);
    assert!(size_of::<LiveEye>() == 72);
    assert!(offset_of!(LiveEye, valid) == 24);
    assert!(offset_of!(LiveEye, eye_lid) == 32);
    assert!(offset_of!(LiveEye, aperture) == 64);
    assert!(offset_of!(LiveShm, glint_version) == 200);
    assert!(offset_of!(LiveShm, glint) == 208);
    assert!(size_of::<LiveGlint>() == 48);
    assert!(offset_of!(LiveGlint, template_mono_ns) == 8);
    assert!(offset_of!(LiveGlint, pupil_x) == 16);
    assert!(offset_of!(LiveGlint, slip_x) == 24);
    assert!(offset_of!(LiveGlint, matches) == 32);
    assert!(offset_of!(LiveShm, shape_version) == 304);
    assert!(offset_of!(LiveShm, shape) == 312);
    assert!(size_of::<LiveShape>() == 16);
    assert!(offset_of!(LiveShape, angle_deg) == 8);
    assert!(offset_of!(LiveShm, lower_version) == 344);
    assert!(offset_of!(LiveShm, lower_lid) == 352);
    assert!(size_of::<LiveShm>() == 360);
};

pub struct LiveWriter {
    ptr: *mut LiveShm,
    _file: File,
    pub path: PathBuf,
}

// The mapping is only touched through &mut self.
unsafe impl Send for LiveWriter {}

pub fn clock_ns(clock: libc::clockid_t) -> u64 {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(clock, &mut ts) };
    ts.tv_sec as u64 * 1_000_000_000 + ts.tv_nsec as u64
}

impl LiveWriter {
    /// Create the file (mode 0644, 4096 bytes) with a fresh header. A file left by an earlier run is never resized
    /// or rewritten in place (a reader may still have it mapped): a new file is prepared next to it and renamed over
    /// it, so the old inode stays intact for whoever still maps it. The file keeps its size for the writer's life.
    pub fn create(path: &Path) -> Result<Self, String> {
        let name = path.file_name().and_then(|n| n.to_str()).ok_or("bad live path")?;
        let tmp = path.with_file_name(format!(".{name}.new"));
        let _ = std::fs::remove_file(&tmp);
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .create_new(true)
            .mode(0o644)
            .custom_flags(libc::O_NOFOLLOW)
            .open(&tmp)
            .map_err(|e| format!("{}: {e}", tmp.display()))?;
        file.set_permissions(std::fs::Permissions::from_mode(0o644)).map_err(|e| e.to_string())?;
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
        w.update(|s| {
            let seq = s.seq;
            *s = LiveShm::default();
            s.seq = seq;
            s.version = VERSION;
            s.struct_size = size_of::<LiveShm>() as u32;
            s.glint_version = GLINT_VERSION;
            s.shape_version = SHAPE_VERSION;
            s.lower_version = LOWER_VERSION;
            s.writer_pid = std::process::id();
            s.writer_start_ns = start;
            s.magic = MAGIC;
            s.lower_lid = [f32::NAN; 2];
        });
        std::fs::rename(&tmp, path).map_err(|e| format!("{} -> {}: {e}", tmp.display(), path.display()))?;
        Ok(w)
    }

    /// Change the struct under the seqlock.
    pub fn update(&mut self, f: impl FnOnce(&mut LiveShm)) {
        let seq = unsafe { AtomicU32::from_ptr(&raw mut (*self.ptr).seq) };
        let s0 = seq.load(Ordering::Relaxed);
        let s0 = s0 & !1; // never leave it odd, even after a crash mid-write
        seq.store(s0.wrapping_add(1), Ordering::Relaxed);
        fence(Ordering::Release);
        let mut copy = unsafe { std::ptr::read_volatile(self.ptr) };
        f(&mut copy);
        copy.seq = s0.wrapping_add(1);
        unsafe { std::ptr::write_volatile(self.ptr, copy) };
        seq.store(s0.wrapping_add(2), Ordering::Release);
    }

    /// The current contents (for tests).
    pub fn read(&self) -> LiveShm {
        unsafe { std::ptr::read_volatile(self.ptr) }
    }
}

impl Drop for LiveWriter {
    fn drop(&mut self) {
        self.update(|s| {
            s.live = 0;
            s.eyes.iter_mut().for_each(|e| e.valid = 0);
        });
        unsafe { libc::munmap(self.ptr.cast(), FILE_SIZE) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn writes_a_readable_struct() {
        let path = std::env::temp_dir().join(format!("eyecam-live-test-{}", std::process::id()));
        {
            let mut w = LiveWriter::create(&path).unwrap();
            w.update(|s| {
                s.live = 1;
                s.eyes[1].eye_wide = 0.5;
            });
            let r = w.read();
            assert_eq!((r.magic, r.version, r.struct_size, r.live), (MAGIC, 1, 360, 1));
            assert_eq!(r.glint_version, GLINT_VERSION);
            assert_eq!(r.shape_version, SHAPE_VERSION);
            assert_eq!(r.lower_version, LOWER_VERSION);
            assert!(r.lower_lid.iter().all(|v| v.is_nan()), "{:?}", r.lower_lid);
            assert_eq!(r.seq % 2, 0);
            assert_eq!(r.eyes[1].eye_wide, 0.5);
        }
        let bytes = std::fs::read(&path).unwrap();
        assert_eq!(&bytes[0..4], b"EYCM");
        assert_eq!(u32::from_le_bytes(bytes[52..56].try_into().unwrap()), 0, "live cleared on drop");
        assert_eq!(f32::from_le_bytes(bytes[128 + 36..128 + 40].try_into().unwrap()), 0.5);
        // The version 1 struct is unchanged in its 200 bytes; the glint part comes after it, then the shape part, then
        // the lower-lid part (NaN while its switch is off)
        assert_eq!(u32::from_le_bytes(bytes[4..8].try_into().unwrap()), 1);
        assert_eq!(u32::from_le_bytes(bytes[8..12].try_into().unwrap()), 360);
        assert_eq!(u32::from_le_bytes(bytes[200..204].try_into().unwrap()), GLINT_VERSION);
        assert_eq!(u32::from_le_bytes(bytes[304..308].try_into().unwrap()), SHAPE_VERSION);
        assert_eq!(u32::from_le_bytes(bytes[344..348].try_into().unwrap()), LOWER_VERSION);
        assert!(f32::from_le_bytes(bytes[352..356].try_into().unwrap()).is_nan());
        assert!(f32::from_le_bytes(bytes[356..360].try_into().unwrap()).is_nan());
        let mode = std::fs::metadata(&path).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode, 0o644);
        std::fs::remove_file(&path).unwrap();
    }

    /// The struct as it was before the shape part (version 1 with the glint part, 304 bytes).
    #[repr(C)]
    #[derive(Clone, Copy, Default)]
    struct ShmBefore {
        magic: u32,
        version: u32,
        struct_size: u32,
        writer_pid: u32,
        writer_start_ns: u64,
        seq: u32,
        calib_state: u32,
        t_mono_ns: u64,
        t_cam_raw_ns: u64,
        recalib_suggested: u32,
        live: u32,
        eyes: [LiveEye; 2],
        glint_version: u32,
        _pad: u32,
        glint: [LiveGlint; 2],
    }

    fn bytes_of<T: Copy>(value: &T) -> Vec<u8> {
        let p = (value as *const T).cast::<u8>();
        (0..size_of::<T>()).map(|i| unsafe { *p.add(i) }).collect()
    }

    #[test]
    fn the_shape_part_leaves_every_byte_before_it_as_it_was() {
        assert_eq!(size_of::<ShmBefore>(), 304);
        let eye = |k: f32| LiveEye {
            t_mono_ns: 11 + k as u64,
            t_cam_raw_ns: 22 + k as u64,
            frame_count: 33,
            valid: 1,
            closed: 0,
            eye_lid: 0.75 + k,
            eye_wide: 0.1,
            eye_squint: 0.2,
            pupil_ratio: 0.8,
            pupil_mm: 4.7,
            pupil_dilation: 0.4,
            confidence: 0.9,
            skin_up: 0.6,
            aperture: 1.4 + k,
            _pad: 0,
        };
        let glint = |k: u32| LiveGlint {
            flags: 7,
            template_id: 3 + k,
            template_mono_ns: 1234,
            pupil_x: 201.5,
            pupil_y: 150.25,
            slip_x: 1.5,
            slip_y: -2.25,
            matches: 4,
            since_good: k,
            glints: 5,
            _pad: 0,
        };
        let before = ShmBefore {
            magic: MAGIC,
            version: VERSION,
            struct_size: 304,
            writer_pid: 42,
            writer_start_ns: 99,
            seq: 6,
            calib_state: 5,
            t_mono_ns: 77,
            t_cam_raw_ns: 88,
            recalib_suggested: 0,
            live: 1,
            eyes: [eye(0.0), eye(0.1)],
            glint_version: GLINT_VERSION,
            _pad: 0,
            glint: [glint(0), glint(1)],
        };
        let now = LiveShm {
            magic: MAGIC,
            version: VERSION,
            struct_size: size_of::<LiveShm>() as u32,
            writer_pid: 42,
            writer_start_ns: 99,
            seq: 6,
            calib_state: 5,
            t_mono_ns: 77,
            t_cam_raw_ns: 88,
            recalib_suggested: 0,
            live: 1,
            eyes: [eye(0.0), eye(0.1)],
            glint_version: GLINT_VERSION,
            _pad: 0,
            glint: [glint(0), glint(1)],
            shape_version: SHAPE_VERSION,
            flipped: 1,
            shape: [LiveShape { a: 21.0, b: 19.5, angle_deg: 33.0, vis: 1.0 }; 2],
            lower_version: LOWER_VERSION,
            _pad2: 0,
            lower_lid: [0.25, 0.5],
        };
        let (old, new) = (bytes_of(&before), bytes_of(&now));
        // Only struct_size differs in the first 304 bytes
        assert_eq!(old[..8], new[..8]);
        assert_eq!(old[12..304], new[12..304]);
        assert_eq!(u32::from_le_bytes(new[8..12].try_into().unwrap()), 360);
        // The shape part: its version, then each eye's a, b, angle and rim share
        assert_eq!(u32::from_le_bytes(new[304..308].try_into().unwrap()), SHAPE_VERSION);
        assert_eq!(u32::from_le_bytes(new[308..312].try_into().unwrap()), 1);
        assert_eq!(f32::from_le_bytes(new[312..316].try_into().unwrap()), 21.0);
        assert_eq!(f32::from_le_bytes(new[328 + 8..328 + 12].try_into().unwrap()), 33.0);
        // The lower-lid part: its version, then each eye's value
        assert_eq!(u32::from_le_bytes(new[344..348].try_into().unwrap()), LOWER_VERSION);
        assert_eq!(f32::from_le_bytes(new[352..356].try_into().unwrap()), 0.25);
        assert_eq!(f32::from_le_bytes(new[356..360].try_into().unwrap()), 0.5);
    }

    #[test]
    fn a_new_writer_never_shrinks_a_mapped_file() {
        use std::os::unix::fs::MetadataExt;
        let path = std::env::temp_dir().join(format!("eyecam-live-rename-test-{}", std::process::id()));
        let first = LiveWriter::create(&path).unwrap();
        let old_ino = std::fs::metadata(&path).unwrap().ino();
        // A reader maps the file, as frameeyeosc does.
        let f = File::open(&path).unwrap();
        let p = unsafe { libc::mmap(std::ptr::null_mut(), FILE_SIZE, libc::PROT_READ, libc::MAP_SHARED, f.as_raw_fd(), 0) };
        assert_ne!(p, libc::MAP_FAILED);
        drop(first);
        // A restarted writer replaces the file (new inode, full size); the old one stays readable to the end.
        let second = LiveWriter::create(&path).unwrap();
        let meta = std::fs::metadata(&path).unwrap();
        assert_ne!(meta.ino(), old_ino);
        assert_eq!(meta.len(), FILE_SIZE as u64);
        assert_eq!(f.metadata().unwrap().len(), FILE_SIZE as u64);
        let last = unsafe { std::ptr::read_volatile((p as *const u8).add(FILE_SIZE - 1)) };
        assert_eq!(last, 0);
        assert_eq!(unsafe { std::ptr::read_volatile(p as *const u32) }, MAGIC);
        unsafe { libc::munmap(p, FILE_SIZE) };
        drop(second);
        std::fs::remove_file(&path).unwrap();
    }
}
