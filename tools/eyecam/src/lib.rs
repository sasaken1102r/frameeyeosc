//! Pieces of eyecam-rec, kept in a library so they can be unit tested. eyecam-grab does not use any of it.

pub mod autograb;
pub mod cues;
pub mod feat;
pub mod json;
pub mod live;
pub mod liveshm;
pub mod livesvc;
pub mod mem;
pub mod message_en;
pub mod proto;
pub mod replay;
pub mod ring;
pub mod settings;
pub mod shm;
pub mod status;
pub mod vision;

/// CLOCK_MONOTONIC_RAW in seconds, the clock the eye server stamps its samples with.
pub fn now_raw() -> f64 {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC_RAW, &mut ts) };
    ts.tv_sec as f64 + ts.tv_nsec as f64 * 1e-9
}
