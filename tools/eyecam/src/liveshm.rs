//! The live output shared memory, `/run/user/1000/eyecam/live`: one fixed little-endian struct, written by
//! eyecam-rec with a seqlock, read by frameeyeosc (same uid, O_RDONLY + mmap, never writes). The README's
//! "live shared memory" section is the contract; bump VERSION on any change to the layout or meaning.

use std::fs::{File, OpenOptions};
use std::mem::{offset_of, size_of};
use std::os::fd::AsRawFd;
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering, fence};

/// "EYCM" as little-endian bytes.
pub const MAGIC: u32 = u32::from_le_bytes(*b"EYCM");
pub const VERSION: u32 = 1;
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
    assert!(size_of::<LiveShm>() == 200);
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
            s.writer_pid = std::process::id();
            s.writer_start_ns = start;
            s.magic = MAGIC;
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
            assert_eq!((r.magic, r.version, r.struct_size, r.live), (MAGIC, 1, 200, 1));
            assert_eq!(r.seq % 2, 0);
            assert_eq!(r.eyes[1].eye_wide, 0.5);
        }
        let bytes = std::fs::read(&path).unwrap();
        assert_eq!(&bytes[0..4], b"EYCM");
        assert_eq!(u32::from_le_bytes(bytes[52..56].try_into().unwrap()), 0, "live cleared on drop");
        assert_eq!(f32::from_le_bytes(bytes[128 + 36..128 + 40].try_into().unwrap()), 0.5);
        let mode = std::fs::metadata(&path).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode, 0o644);
        std::fs::remove_file(&path).unwrap();
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
