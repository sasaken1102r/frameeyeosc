//! A read-only peek at the eye server's samples in /dev/shm/eye-server.mmap.
//!
//! Layout as frameeyeosc reads it (src/main.rs there): `sequence` (u32) at 0x38 is bumped with every sample, and
//! the packed sample record starts at 0x152 (version 4) or 0x157 (version 5). The file is mapped read-only, so
//! nothing here can write to it: no metadata_mutex, no metadata_requested. The eye server only publishes while a
//! client (frameeyeosc) keeps requesting samples. Without the mutex a record can in principle be read while being
//! rewritten; the sequence is read before and after, and the sample is dropped if it moved.

use std::os::unix::fs::{MetadataExt, OpenOptionsExt};
use std::os::fd::AsRawFd;
use std::ptr;

pub const PATH: &str = "/dev/shm/eye-server.mmap";
const SEQUENCE: usize = 0x38;
// (version, file size, sample record offset)
const LAYOUTS: [(u32, usize, usize); 2] = [(4, 0x4f21a, 0x152), (5, 0x4f21f, 0x157)];
// The record up to and including estimate_extra.
const RECORD_LEN: usize = 0xa1;

/// One sample, as the eye server wrote it.
#[derive(Clone, Debug, Default)]
pub struct Sample {
    pub sequence: u32,
    pub producer_state: u32,
    pub sample_flag: u8,
    pub sample_time: f64,
    pub gaze: [[f32; 3]; 2],
    pub gaze_cov: [[f32; 3]; 2],
    pub fixation: [f32; 3],
    pub pre_gaze: [[f32; 3]; 2],
    pub pre_cov: [[f32; 3]; 2],
    pub openness: [f32; 2],
    pub extra: [f32; 8],
}

impl Sample {
    pub const CSV_HEADER: &str = "valve_seq,t_seen,sample_time,producer_state,sample_flag,open_l,open_r,\
gaze_l_x,gaze_l_y,gaze_l_z,gaze_r_x,gaze_r_y,gaze_r_z,cov_l_x,cov_l_y,cov_l_z,cov_r_x,cov_r_y,cov_r_z,\
fix_x,fix_y,fix_z,pre_l_x,pre_l_y,pre_l_z,pre_r_x,pre_r_y,pre_r_z,precov_l_x,precov_l_y,precov_l_z,\
precov_r_x,precov_r_y,precov_r_z,extra_0,extra_1,extra_2,extra_3,extra_4,extra_5,extra_6,extra_7";

    /// A CSV row matching CSV_HEADER; `t_seen` is when it was read (CLOCK_MONOTONIC_RAW).
    pub fn csv_row(&self, t_seen: f64) -> String {
        let mut row = format!(
            "{},{t_seen:.9},{:.9},{},{},{},{}",
            self.sequence, self.sample_time, self.producer_state, self.sample_flag, self.openness[0], self.openness[1]
        );
        let groups = [&self.gaze, &self.gaze_cov];
        for v in groups.iter().flat_map(|g| g.iter().flatten()) {
            row += &format!(",{v}");
        }
        for v in &self.fixation {
            row += &format!(",{v}");
        }
        for v in [&self.pre_gaze, &self.pre_cov].iter().flat_map(|g| g.iter().flatten()) {
            row += &format!(",{v}");
        }
        for v in &self.extra {
            row += &format!(",{v}");
        }
        row
    }
}

/// Decode a record (RECORD_LEN bytes from its start).
fn decode(sequence: u32, r: &[u8]) -> Sample {
    let f32_at = |i: usize| f32::from_le_bytes(r[i..i + 4].try_into().unwrap());
    let vec3 = |i: usize| [f32_at(i), f32_at(i + 4), f32_at(i + 8)];
    let pair = |i: usize| [vec3(i), vec3(i + 12)];
    Sample {
        sequence,
        producer_state: u32::from_le_bytes(r[0..4].try_into().unwrap()),
        sample_flag: r[4],
        sample_time: f64::from_le_bytes(r[5..13].try_into().unwrap()),
        gaze: pair(0x0d),
        gaze_cov: pair(0x25),
        fixation: vec3(0x3d),
        pre_gaze: pair(0x49),
        pre_cov: pair(0x61),
        openness: [f32_at(0x79), f32_at(0x7d)],
        extra: std::array::from_fn(|i| f32_at(0x81 + 4 * i)),
    }
}

pub struct Shm {
    ptr: *const u8,
    len: usize,
    record: usize,
    pub version: u32,
    last: Option<u32>,
}

impl Shm {
    pub fn open() -> Result<Self, String> {
        // /dev/shm is writable by everyone: no symlink, and only a regular file of this user (Valve's eye-server
        // runs as the user too) is read.
        let file = std::fs::OpenOptions::new()
            .read(true)
            .custom_flags(libc::O_NOFOLLOW)
            .open(PATH)
            .map_err(|e| format!("{PATH}: {e}"))?;
        let meta = file.metadata().map_err(|e| format!("{PATH}: {e}"))?;
        if !meta.is_file() || meta.uid() != unsafe { libc::getuid() } {
            return Err(format!("{PATH}: not a regular file of this user"));
        }
        let len = meta.len() as usize;
        if len < 8 {
            return Err(format!("{PATH}: too small"));
        }
        let map = unsafe {
            libc::mmap(ptr::null_mut(), len, libc::PROT_READ, libc::MAP_SHARED, file.as_raw_fd(), 0)
        };
        if map == libc::MAP_FAILED {
            return Err(format!("{PATH}: mmap: {}", std::io::Error::last_os_error()));
        }
        let ptr = map as *const u8;
        let version = unsafe { ptr::read_volatile(ptr.cast::<u32>()) };
        let Some(&(_, size, record)) = LAYOUTS.iter().find(|l| l.0 == version) else {
            unsafe { libc::munmap(map, len) };
            return Err(format!("{PATH}: unsupported version {version} (know 4 and 5)"));
        };
        if len < size {
            unsafe { libc::munmap(map, len) };
            return Err(format!("{PATH}: {len} bytes, version {version} needs {size}"));
        }
        Ok(Self { ptr, len, record, version, last: None })
    }

    fn sequence(&self) -> u32 {
        unsafe { ptr::read_volatile(self.ptr.add(SEQUENCE).cast::<u32>()) }
    }

    /// The sequence of the last sample returned by `poll`.
    pub fn last_sequence(&self) -> Option<u32> {
        self.last
    }

    /// The latest sample, if the sequence moved since the last one returned.
    pub fn poll(&mut self) -> Option<Sample> {
        let seq = self.sequence();
        if self.last == Some(seq) {
            return None;
        }
        let mut record = [0u8; RECORD_LEN];
        for (i, b) in record.iter_mut().enumerate() {
            *b = unsafe { ptr::read_volatile(self.ptr.add(self.record + i)) };
        }
        if self.sequence() != seq {
            return None;
        }
        self.last = Some(seq);
        Some(decode(seq, &record))
    }
}

impl Drop for Shm {
    fn drop(&mut self) {
        unsafe { libc::munmap(self.ptr as *mut libc::c_void, self.len) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decodes_the_packed_record() {
        let mut r = [0u8; RECORD_LEN];
        r[0..4].copy_from_slice(&1u32.to_le_bytes());
        r[5..13].copy_from_slice(&123.5f64.to_le_bytes());
        r[0x0d..0x11].copy_from_slice(&0.25f32.to_le_bytes());
        r[0x79..0x7d].copy_from_slice(&0.75f32.to_le_bytes());
        r[0x9d..0xa1].copy_from_slice(&9.0f32.to_le_bytes());
        let s = decode(7, &r);
        assert_eq!((s.producer_state, s.sample_time, s.gaze[0][0], s.openness[0]), (1, 123.5, 0.25, 0.75));
        assert_eq!(s.extra[7], 9.0);
        assert_eq!(s.csv_row(1.0).split(',').count(), Sample::CSV_HEADER.split(',').count());
    }

    /// On the headset: `cargo test --release -- --ignored live_peek --nocapture`. Only reads the file.
    #[test]
    #[ignore]
    fn live_peek() {
        let mut shm = Shm::open().unwrap();
        let start = crate::now_raw();
        let (mut n, mut last) = (0, None);
        while crate::now_raw() - start < 2.0 {
            if let Some(s) = shm.poll() {
                n += 1;
                last = Some(s);
            }
            std::thread::sleep(std::time::Duration::from_millis(2));
        }
        eprintln!("version {}: {n} samples in 2 s, now {:.6}, last {last:?}", shm.version, crate::now_raw());
    }
}
