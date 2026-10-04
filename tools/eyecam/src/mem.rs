//! Reading memory that a device writes without the CPU caches knowing.
//!
//! The approach (dropping exactly the cache lines about to be read with DC CIVAC, and sampled fingerprints) is
//! ported from FrameEyeCameraFeed framestream.c, Copyright (c) 2026 Curtis English, MIT License (see NOTICE).
//!
//! The camera pipeline writes the shared udmabuf without snooping the CPU caches, and a read-only mapping gives the
//! kernel no hook to sync them. So a line read once can keep returning an old frame long after memory was
//! rewritten. DMA_BUF_IOCTL_SYNC would fix that, but on a udmabuf it syncs the whole 16-32 MiB buffer per call.
//! Instead, right before reading, each line is cleaned and invalidated (DC CIVAC, allowed from user space on arm64
//! Linux), so the read comes from memory. Clean first means it can never discard anyone else's write: at worst it
//! writes a dirty line back early. It does not modify the data, and nothing here ever writes to the buffers.

use std::sync::OnceLock;

pub const SCAN_BLOCK: usize = 65536;

fn cache_line() -> usize {
    static LINE: OnceLock<usize> = OnceLock::new();
    *LINE.get_or_init(|| {
        #[cfg(target_arch = "aarch64")]
        {
            let ctr: u64;
            unsafe { std::arch::asm!("mrs {}, ctr_el0", out(reg) ctr, options(nomem, nostack)) };
            4 << ((ctr >> 16) & 0xf)
        }
        #[cfg(not(target_arch = "aarch64"))]
        64
    })
}

#[inline]
fn drop_line(p: *const u8) {
    #[cfg(target_arch = "aarch64")]
    unsafe {
        std::arch::asm!("dc civac, {}", in(reg) p, options(nostack, preserves_flags))
    };
    #[cfg(not(target_arch = "aarch64"))]
    let _ = p;
}

#[inline]
fn barrier() {
    #[cfg(target_arch = "aarch64")]
    unsafe {
        std::arch::asm!("dsb ish", options(nostack, preserves_flags))
    };
    #[cfg(not(target_arch = "aarch64"))]
    std::sync::atomic::fence(std::sync::atomic::Ordering::SeqCst);
}

/// A read-only view of one shared buffer, plus which 64 KiB blocks have changed since the last clear.
pub struct Arena {
    ptr: *const u8,
    len: usize,
    block_fp: Vec<u64>,
    pub block_changed: Vec<bool>,
}

impl Arena {
    /// # Safety
    /// `ptr..ptr+len` must stay mapped and readable for the arena's lifetime.
    pub unsafe fn new(ptr: *const u8, len: usize) -> Self {
        let blocks = len / SCAN_BLOCK;
        let mut arena = Self { ptr, len, block_fp: vec![0; blocks], block_changed: vec![false; blocks] };
        arena.reset_block_changes();
        arena
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// A hash of 512 evenly spaced bytes of `off..off+len`, read fresh from memory.
    pub fn fingerprint(&self, off: usize, len: usize) -> u64 {
        assert!(off + len <= self.len);
        let step = (len / 512).max(1);
        let mut i = 0;
        while i < len {
            drop_line(unsafe { self.ptr.add(off + i) });
            i += step;
        }
        barrier();
        let mut h: u64 = 1469598103934665603;
        let mut i = 0;
        while i < len {
            h ^= unsafe { std::ptr::read_volatile(self.ptr.add(off + i)) } as u64;
            h = h.wrapping_mul(1099511628211);
            i += step;
        }
        h
    }

    /// Copy `off..off+out.len()` out, fresh from memory.
    pub fn copy(&self, off: usize, out: &mut [u8]) {
        assert!(off + out.len() <= self.len);
        let line = cache_line();
        let start = (self.ptr as usize + off) & !(line - 1);
        let end = self.ptr as usize + off + out.len();
        let mut a = start;
        while a < end {
            drop_line(a as *const u8);
            a += line;
        }
        barrier();
        unsafe { std::ptr::copy_nonoverlapping(self.ptr.add(off), out.as_mut_ptr(), out.len()) };
        std::sync::atomic::compiler_fence(std::sync::atomic::Ordering::SeqCst);
    }

    /// The whole buffer, fresh from memory. Only used while searching.
    pub fn snapshot(&self) -> Vec<u8> {
        let mut out = vec![0; self.len];
        self.copy(0, &mut out);
        out
    }

    /// Mark every block whose fingerprint moved since the last call; true if any did.
    pub fn note_block_changes(&mut self) -> bool {
        let mut any = false;
        for b in 0..self.block_fp.len() {
            let h = self.fingerprint(b * SCAN_BLOCK, SCAN_BLOCK);
            if h != self.block_fp[b] {
                self.block_fp[b] = h;
                self.block_changed[b] = true;
                any = true;
            }
        }
        any
    }

    pub fn clear_block_changes(&mut self) {
        self.block_changed.fill(false);
    }

    /// Re-take the fingerprints so only changes from now on count.
    pub fn reset_block_changes(&mut self) {
        self.note_block_changes();
        self.clear_block_changes();
    }
}
