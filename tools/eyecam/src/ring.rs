//! Finding the ring of eye camera frames inside the shared udmabufs.
//!
//! Steps 1-6 and the eye grouping are ported from FrameEyeCameraFeed framestream.c (discover_ring() and its
//! helpers), Copyright (c) 2026 Curtis English, MIT License (see NOTICE). What it measured on the device:
//!   - Eight frames sit in one ring, 512 bytes per row x 400 rows, 8-bit grey.
//!   - The spacing is 262208 bytes: 256 KiB plus a 64-byte header before each frame.
//!   - Slots 0-3 hold one eye and 4-7 the other, so the eye is fixed per slot and decided once at lock.
//!
//! Measured by eyecam on the device (first recording, 2026-10-03):
//!   - Within each eye's group the spacing is 262208, but the second group starts 64 bytes later than that
//!     spacing predicts (262272 from slot 3 to slot 4). The groups are separate allocations, so one grid fits only
//!     one of them.
//!   - The 64-byte header starts with the camera timestamp: u64 little-endian nanoseconds of CLOCK_MONOTONIC_RAW
//!     (frames 11.11 ms apart at 90 Hz). The rest of it was zero.
//!   - Pixels are never exactly 0 (black level about 4); the row padding and the rest of the slot are 0.
//!   - The lower-address group is the left camera, the later (+64) group the right one, in all six sessions.
//!   - The buffer is a general heap that gets recycled, so a lock needs: the headset worn (checked by the caller),
//!     at least 20% of the frame lit, and the candidate refreshing for a full second.
//!   - Valve's eye tracker sets the cameras' frame rate (72, 80, 90, 120, and as low as 15 on some headsets). With
//!     4 slots per eye, a slot is refilled a quarter as often: 3.75 times a second at 15 fps.
//!
//! Changes from framestream.c:
//!   - Which camera a slot group is comes from the memory layout (`assign_cameras`), not from where the picture
//!     sits: framestream's "further left = camera 0" flipped with the face's position.
//!   - The spacing is measured against whichever neighbouring slot matches best (the one holding the same eye),
//!     not always the next one, which may hold the other eye.
//!   - Each slot's exact frame start is settled on its own (`refine_start`), from where the zero, unchanging
//!     row padding and gap sit, rather than from dark edges or one grid for the whole ring. That makes "strip
//!     columns 400..512" exact and puts the 64-byte header right before the first pixel.

use crate::mem::{Arena, SCAN_BLOCK};

pub const STRIDE: usize = 512;
pub const HEIGHT: usize = 400;
pub const WIDTH: usize = 400;
pub const FRAME_BYTES: usize = STRIDE * HEIGHT;
pub const HEADER_BYTES: usize = 64;
pub const SLOT: usize = 262144;
pub const EXPECTED_PITCH: usize = SLOT + HEADER_BYTES;
pub const MAX_RING: usize = 32;
// How far (in rows) the framing refinement may move a frame start up or down.
const MARGIN_ROWS: usize = 16;

/// Time and pacing for discovery, so tests can drive it with a simulated camera.
pub trait Clock {
    fn now(&mut self) -> f64;
    /// Pause a few milliseconds between polls.
    fn wait(&mut self);
    fn stopped(&self) -> bool {
        false
    }
}

/// How each slot's exact frame start was settled (for the session log).
#[derive(Clone, Debug, Default)]
pub struct Framing {
    /// Bytes each slot's start moved from where the ring walk put it.
    pub shift: Vec<i64>,
    /// Per slot, the share of padding bytes (columns 400..512) that were nonzero or changed (0 = clean).
    pub pad_dirty: Vec<f64>,
}

/// Where the last look stopped (`Look::stopped_at`), for status.json's `search_detail`.
pub const STOP_NO_CANDIDATES: &str = "no_candidates";
/// Two or more candidates in all, but fewer than 2 in the buffer holding the most (the rest are elsewhere).
pub const STOP_SPLIT_BUFFERS: &str = "split_buffers";
pub const STOP_NOT_REFRESHING: &str = "not_refreshing";

/// A candidate slot has to be refilled at least this often in its second of watching. Each eye has 4 slots, so a
/// slot is refilled at a quarter of the camera's frame rate: 22.5 times at 90 fps, 3.75 at 15 (3 or 4 in a second).
/// It was 5, which the cameras at 15 fps could miss; a stale picture is not refilled at all.
const MIN_REFRESHES: u32 = 3;
pub const STOP_FEW_SLOTS: &str = "few_slots";
pub const STOP_ONE_EYE: &str = "one_eye";

/// What one `discover` saw, whether or not it found the ring (status.json `search_detail`, for the panel's
/// diagnostics).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Look {
    /// Picture-like candidate frames in changed memory (step 1, all buffers).
    pub candidates: usize,
    /// The median refreshes a second of the candidates in the chosen buffer (step 3; 0 before it).
    pub refresh_hz: f64,
    /// The slots of the ring found (0 if it stopped before).
    pub slots: usize,
    /// Both cameras' slots were found.
    pub both_eyes: bool,
    /// "" (found, both eyes), STOP_NO_CANDIDATES, STOP_SPLIT_BUFFERS, STOP_NOT_REFRESHING, STOP_FEW_SLOTS or
    /// STOP_ONE_EYE.
    pub stopped_at: &'static str,
}

#[derive(Clone, Debug)]
pub struct Ring {
    pub arena: usize,
    pub pitch: usize,
    /// The first pixel of each slot's frame, in memory order. The slot's 64-byte header is right before it.
    pub off: Vec<usize>,
    /// Which camera each slot holds: 0 = L, 1 = R (before --swap), from the memory layout.
    pub eye: Vec<u8>,
    pub both_eyes: bool,
    pub framing: Framing,
    /// How `eye` was decided, and any disagreement found (for meta.txt).
    pub camera_reason: String,
}

/// Lit fraction and roughness of a frame, sampled on a 4-pixel grid. `p` must hold at least FRAME_BYTES.
pub fn frame_stats(p: &[u8]) -> (f64, f64) {
    let (mut lit, mut n, mut nr, mut rough) = (0usize, 0usize, 0usize, 0f64);
    for y in (0..HEIGHT - 1).step_by(4) {
        for x in (0..STRIDE - 1).step_by(4) {
            let v = p[y * STRIDE + x] as i32;
            n += 1;
            if v > 16 {
                lit += 1;
                rough += (v - p[(y + 1) * STRIDE + x] as i32).abs() as f64;
                rough += (v - p[y * STRIDE + x + 1] as i32).abs() as f64;
                nr += 2;
            }
        }
    }
    (
        if n > 0 { lit as f64 / n as f64 } else { 0.0 },
        if nr > 0 { rough / nr as f64 } else { 1e9 },
    )
}

/// Real eye frames are 27-34% lit; the recycled heap this must not lock onto is 9-15% lit.
pub fn looks_like_eye_frame(p: &[u8]) -> bool {
    let (lit, smooth) = frame_stats(p);
    (0.20..=0.96).contains(&lit) && smooth <= 15.0
}

/// The mean of each column (every 8th row), the shape all the alignment maths uses.
fn column_profile(p: &[u8]) -> [f32; STRIDE] {
    let mut out = [0f32; STRIDE];
    for (x, value) in out.iter_mut().enumerate() {
        let acc: u32 = (0..HEIGHT).step_by(8).map(|y| p[y * STRIDE + x] as u32).sum();
        *value = acc as f32 / (HEIGHT / 8) as f32;
    }
    out
}

fn profile_distance(a: &[f32; STRIDE], b: &[f32; STRIDE], rot: usize) -> f64 {
    let d: f64 = (0..STRIDE).step_by(2).map(|x| (a[(x + rot) % STRIDE] - b[x]).abs() as f64).sum();
    d / (STRIDE / 2) as f64
}

/// How far `a` has to be rotated to line up with `b`, and the remaining distance.
fn best_rotation(a: &[f32; STRIDE], b: &[f32; STRIDE]) -> (usize, f64) {
    (0..STRIDE)
        .map(|r| (r, profile_distance(a, b, r)))
        .fold((0, f64::INFINITY), |best, cur| if cur.1 < best.1 { cur } else { best })
}

/// Mean brightness of the 24 columns at each edge.
fn edge_darkness(p: &[u8]) -> f64 {
    let (mut acc, mut n) = (0f64, 0usize);
    for y in (0..HEIGHT).step_by(4) {
        let r = &p[y * STRIDE..(y + 1) * STRIDE];
        for x in 0..24 {
            acc += r[x] as f64 + r[STRIDE - 1 - x] as f64;
            n += 2;
        }
    }
    if n > 0 { acc / n as f64 } else { 1e9 }
}

/// Where the picture around `hint` starts, row-aligned: the first lit row after a run of dark ones.
fn find_frame_start(base: &[u8], hint: usize) -> Option<usize> {
    let hint_row = (hint / STRIDE) as i64;
    let max_row = (base.len() / STRIDE) as i64 - 1;
    let lo = (hint_row - 560).max(0);
    let hi = (hint_row + 560).min(max_row);
    if hi - lo < 200 {
        return None;
    }
    let nrows = ((hi - lo + 1) as usize).min(1200);
    let mean: Vec<f64> = (0..nrows)
        .map(|k| {
            let r = &base[(lo as usize + k) * STRIDE..];
            (0..STRIDE).step_by(8).map(|x| r[x] as f64).sum::<f64>() / (STRIDE / 8) as f64
        })
        .collect();
    let avg = mean.iter().sum::<f64>() / nrows as f64;
    let dark = (avg * 0.18).max(1.5);
    let (mut best, mut best_d, mut run) = (None, i64::MAX, 0);
    for (k, &m) in mean.iter().enumerate() {
        if m <= dark {
            run += 1;
            continue;
        }
        if run >= 8 {
            let row = lo + k as i64;
            let d = (row - hint_row).abs();
            if d < best_d {
                best_d = d;
                best = Some(row as usize * STRIDE);
            }
        }
        run = 0;
    }
    best
}

/// The exact first pixel of a frame, given `region`: the memory from MARGIN_ROWS rows before a guessed start
/// (within a row or so of the truth) to MARGIN_ROWS + 1 rows past its end, and optionally how often each byte of
/// it changed while watched. Returns the start relative to the guess, and the padding's dirty share.
///
/// A byte counts as live when it is nonzero, and again when it changed. Pixels are practically never exactly 0
/// (the sensor's black level is about 4) and change with every frame; the row padding (columns 400..512) and the
/// unused rest of the slot are zero on the device and never change. So: first the column phase that leaves the
/// least live 112-column band at columns 400..512 (ties: the most live picture), then the 400-row window holding
/// the most live rows (ties: the smallest move).
pub fn refine_start(region: &[u8], changes: Option<&[u16]>) -> (i64, f64) {
    let rows = region.len() / STRIDE;
    assert!(rows > HEIGHT + 2 * MARGIN_ROWS);
    let live = |i: usize| (region[i] != 0) as u32 + changes.map_or(0, |c| (c[i] > 0) as u32);
    let mut col = [0u32; STRIDE];
    for r in 0..HEIGHT {
        let row = (MARGIN_ROWS + r) * STRIDE;
        for (x, c) in col.iter_mut().enumerate() {
            *c += live(row + x);
        }
    }
    let band = |d: usize, from: usize, to: usize| (from..to).map(|j| col[(d + j) % STRIDE]).sum::<u32>();
    let d = (0..STRIDE).min_by_key(|&d| (band(d, WIDTH, STRIDE), std::cmp::Reverse(band(d, 0, WIDTH)))).unwrap();
    let most = (STRIDE - WIDTH) * HEIGHT * if changes.is_some() { 2 } else { 1 };
    let pad_dirty = band(d, WIDTH, STRIDE) as f64 / most as f64;
    let act: Vec<u64> = (0..rows - 1).map(|r| (0..WIDTH).map(|x| live(r * STRIDE + d + x) as u64).sum()).collect();
    let margin = MARGIN_ROWS as i64;
    let window = |m: i64| -> u64 { act[(margin + m) as usize..][..HEIGHT].iter().sum() };
    let busiest = (-margin..=margin).map(window).max().unwrap_or(0);
    let m = (-margin..=margin)
        .filter(|&m| window(m) as f64 >= busiest as f64 * 0.999)
        .min_by_key(|m| m.abs())
        .unwrap_or(0);
    (d as i64 + m * STRIDE as i64, pad_dirty)
}

/// Look for the ring in memory that changed recently (`Arena::block_changed`). Takes about 1.6 s when there are
/// candidates. `log` gets a line per decision; `look` what was seen and where it stopped.
pub fn discover(arenas: &[Arena], clock: &mut dyn Clock, log: &mut dyn FnMut(String), look: &mut Look) -> Option<Ring> {
    *look = Look { stopped_at: STOP_NO_CANDIDATES, ..Look::default() };
    // ---- 1. Rough candidates: changed regions that look like pictures.
    let mut cand: Vec<(usize, usize)> = Vec::new();
    for (a, arena) in arenas.iter().enumerate() {
        if cand.len() >= 32 || !arena.block_changed.contains(&true) {
            continue;
        }
        let snap = arena.snapshot();
        let mut off = 0;
        while off + FRAME_BYTES <= snap.len() {
            let blk = off / SCAN_BLOCK;
            if blk < arena.block_changed.len()
                && arena.block_changed[blk]
                && looks_like_eye_frame(&snap[off..])
                && let Some(start) = find_frame_start(&snap, off).filter(|s| s + FRAME_BYTES <= snap.len())
            {
                let dup = cand.iter().any(|&(ca, co)| ca == a && co.abs_diff(start) < FRAME_BYTES / 2);
                if !dup && cand.len() < 32 {
                    cand.push((a, start));
                }
                off += FRAME_BYTES;
                continue;
            }
            off += 4096;
        }
    }
    look.candidates = cand.len();
    if cand.len() < 2 {
        return None;
    }
    log(format!("search: {} candidate frame(s) in changed memory", cand.len()));

    // ---- 2. Work only in the arena holding the most candidates (the other buffer has unrelated traffic too).
    cand.sort();
    let a = (0..arenas.len())
        .max_by_key(|&k| (cand.iter().filter(|c| c.0 == k).count(), std::cmp::Reverse(k)))
        .unwrap();
    let mut cand: Vec<usize> = cand.into_iter().filter(|c| c.0 == a).map(|c| c.1).collect();
    let arena = &arenas[a];
    if cand.len() < 2 {
        look.stopped_at = STOP_SPLIT_BUFFERS;
        return None;
    }

    // ---- 3. Keep only those being refilled (at least MIN_REFRESHES times in a second), not stale pictures.
    let mut fp: Vec<u64> = cand.iter().map(|&o| arena.fingerprint(o, FRAME_BYTES)).collect();
    let mut hits = vec![0u32; cand.len()];
    let t0 = clock.now();
    while clock.now() - t0 < 1.0 && !clock.stopped() {
        for (k, &o) in cand.iter().enumerate() {
            let f = arena.fingerprint(o, FRAME_BYTES);
            if f != fp[k] {
                fp[k] = f;
                hits[k] += 1;
            }
        }
        clock.wait();
    }
    log(format!("refreshes in 1 s: {hits:?}"));
    let mut sorted = hits.clone();
    sorted.sort_unstable();
    look.refresh_hz = sorted[sorted.len() / 2] as f64 / (clock.now() - t0).max(1.0);
    look.stopped_at = STOP_NOT_REFRESHING;
    cand = cand.into_iter().zip(&hits).filter(|(_, h)| **h >= MIN_REFRESHES).map(|(o, _)| o).collect();
    if cand.len() < 2 {
        return None;
    }
    look.stopped_at = STOP_FEW_SLOTS;

    // ---- 4. Reference: the candidate with the darkest edges (least likely to wrap around the frame edge).
    let snap = arena.snapshot();
    let base = *cand
        .iter()
        .min_by(|x, y| edge_darkness(&snap[**x..]).total_cmp(&edge_darkness(&snap[**y..])))
        .unwrap();

    // ---- 5. Spacing. Read on a 256 KiB grid, a neighbour comes out rotated by exactly how much the real spacing
    // differs from 256 KiB. Use whichever neighbour lines up best: that one holds the same eye.
    let pref = column_profile(&snap[base..]);
    let mut options = Vec::new();
    if base + SLOT + FRAME_BYTES <= snap.len() {
        let (rot, d) = best_rotation(&column_profile(&snap[base + SLOT..]), &pref);
        options.push((rot, d));
    }
    if base >= SLOT {
        let (rot, d) = best_rotation(&pref, &column_profile(&snap[base - SLOT..]));
        options.push((rot, d));
    }
    let pitch = options
        .iter()
        .min_by(|x, y| x.1.total_cmp(&y.1))
        .map_or(SLOT as i64, |&(rot, _)| {
            // A rotation past halfway is really a small negative one.
            SLOT as i64 + if rot > STRIDE / 2 { rot as i64 - STRIDE as i64 } else { rot as i64 }
        });
    if pitch != EXPECTED_PITCH as i64 {
        log(format!("note: measured slot spacing {pitch}, expected {EXPECTED_PITCH}"));
    }

    // ---- 6. Lay the ring out on that spacing, as far as frames keep looking like pictures.
    let len = snap.len() as i64;
    let mut first = base as i64;
    let mut walked = 0;
    while first - pitch >= 0 && walked < MAX_RING && first - pitch + (FRAME_BYTES as i64) <= len {
        if !looks_like_eye_frame(&snap[(first - pitch) as usize..]) {
            break;
        }
        first -= pitch;
        walked += 1;
    }
    let mut off = Vec::new();
    let mut o = first;
    while o + (FRAME_BYTES + STRIDE) as i64 <= len && off.len() < MAX_RING {
        if !looks_like_eye_frame(&snap[o as usize..]) {
            break;
        }
        off.push(o as usize);
        o += pitch;
    }
    drop(snap);
    log(format!("ring walk: {} slot(s) from 0x{first:x}, spacing {pitch}", off.len()));
    if off.len() < 2 {
        return None;
    }

    // ---- 7. Watch the slots for 0.6 s: drop any that do not refresh (the walk can run one past the end of the
    // ring onto a stale frame), and count how often each byte around each frame changes, for the framing.
    let rows = HEIGHT + 2 * MARGIN_ROWS + 1;
    let region = rows * STRIDE;
    let starts: Vec<Option<usize>> = off
        .iter()
        .map(|&o| o.checked_sub(MARGIN_ROWS * STRIDE).filter(|s| s + region <= arena.len()))
        .collect();
    let mut prev: Vec<Vec<u8>> = starts.iter().map(|s| if s.is_some() { vec![0; region] } else { vec![] }).collect();
    let mut cur = vec![0u8; region];
    let mut counts: Vec<Vec<u16>> = prev.iter().map(|p| vec![0; p.len()]).collect();
    for (k, s) in starts.iter().enumerate() {
        if let Some(s) = *s {
            arena.copy(s, &mut prev[k]);
        }
    }
    let before: Vec<u64> = off.iter().map(|&o| arena.fingerprint(o, FRAME_BYTES)).collect();
    let mut seen = vec![false; off.len()];
    let t1 = clock.now();
    let mut next_snap = t1 + 0.04;
    while clock.now() - t1 < 0.6 && !clock.stopped() {
        for (k, &o) in off.iter().enumerate() {
            if arena.fingerprint(o, FRAME_BYTES) != before[k] {
                seen[k] = true;
            }
        }
        if clock.now() >= next_snap {
            next_snap += 0.04;
            for (k, s) in starts.iter().enumerate() {
                let Some(s) = *s else { continue };
                arena.copy(s, &mut cur);
                for (i, c) in counts[k].iter_mut().enumerate() {
                    *c += (cur[i] != prev[k][i]) as u16;
                }
                std::mem::swap(&mut prev[k], &mut cur);
            }
        }
        clock.wait();
    }
    if seen.iter().filter(|s| **s).count() >= 2 {
        for k in (0..off.len()).rev() {
            if !seen[k] {
                log(format!("slot at 0x{:x} did not refresh in 0.6 s, dropped", off[k]));
                off.remove(k);
                prev.remove(k);
                counts.remove(k);
            }
        }
    }

    // ---- 8. Exact framing, per slot: the two eyes' groups are not on one grid (on the device the second group
    // starts 64 bytes later than the first group's spacing predicts), so each slot settles its own start.
    let mut framing = Framing::default();
    let mut exact = Vec::new();
    for (k, &o) in off.iter().enumerate() {
        if prev[k].is_empty() {
            continue;
        }
        let (shift, pad_dirty) = refine_start(&prev[k], Some(&counts[k]));
        let start = o as i64 + shift;
        if start >= HEADER_BYTES as i64 && start + (FRAME_BYTES as i64) <= arena.len() as i64 {
            exact.push(start as usize);
            framing.shift.push(shift);
            framing.pad_dirty.push(pad_dirty);
        }
    }
    log(format!(
        "framing: start shifts {:?}, dirty padding {:?} (0 = clean)",
        framing.shift,
        framing.pad_dirty.iter().map(|d| (d * 1000.0).round() / 1000.0).collect::<Vec<_>>()
    ));
    let off = exact;
    if off.len() < 2 {
        return None;
    }

    // ---- 9. Which camera each slot holds, from the memory layout (see assign_cameras), decided once per lock.
    // Picture similarity still tells whether both cameras are streaming, and is checked against the layout, but
    // never decides: the old rule (camera 0 = the group whose picture sits further left) flipped with the face's
    // position in the frame on 2026-10-04.
    let mut frame = vec![0u8; FRAME_BYTES];
    let prof: Vec<[f32; STRIDE]> = off
        .iter()
        .map(|&o| {
            arena.copy(o, &mut frame);
            column_profile(&frame)
        })
        .collect();
    let dist: Vec<f64> = prof.iter().map(|p| profile_distance(p, &prof[0], 0)).collect();
    let dmin = dist.iter().copied().fold(f64::INFINITY, f64::min);
    let dmax = dist.iter().copied().fold(f64::NEG_INFINITY, f64::max);
    let split = (dmin + dmax) / 2.0;
    let similar: Vec<u8> = dist.iter().map(|&d| (d > split) as u8).collect();
    log(format!("distance from slot 0: {:?} (spread under 0.8 means one eye only)", dist
        .iter()
        .map(|d| (d * 100.0).round() / 100.0)
        .collect::<Vec<_>>()));
    let pictures_differ = dmax - dmin >= 0.8;
    let pitch = pitch as usize;
    let (eye, mut camera_reason, both_eyes) = match assign_cameras(&off, pitch) {
        Some((eye, reason)) => (eye, reason, true),
        None if pictures_differ => {
            // No layout cue: group by picture, but still number the groups by address (lower = camera 0).
            let eye: Vec<u8> = similar.iter().map(|&g| g ^ similar[0]).collect();
            (eye, "picture: no group boundary in the layout; grouped by similarity, lower addresses = L".into(), true)
        }
        None => (vec![0; off.len()], "one group only (one camera streaming)".into(), false),
    };
    if pictures_differ && similar.iter().map(|&g| g ^ similar[0]).ne(eye.iter().map(|&e| e ^ eye[0])) {
        camera_reason += &format!("; WARNING: picture similarity groups the slots as {similar:?}");
    }
    log(format!("cameras {eye:?}: {camera_reason}"));
    look.slots = off.len();
    look.both_eyes = both_eyes;
    look.stopped_at = if both_eyes { "" } else { STOP_ONE_EYE };
    Some(Ring { arena: a, pitch, off, eye, both_eyes, framing, camera_reason })
}

/// Which camera (0 = L, 1 = R) each slot holds, from the layout alone: each camera's four slots are one
/// allocation with the regular spacing, and the second allocation starts 64 bytes later than that spacing predicts.
/// In every session so far the lower-address group was the left camera (checked against the pictures: pupil
/// movement on look_up, iris size). `off` is in memory order. None when the layout does not show two groups.
pub fn assign_cameras(off: &[usize], pitch: usize) -> Option<(Vec<u8>, String)> {
    let breaks: Vec<usize> = (1..off.len()).filter(|&k| off[k] - off[k - 1] != pitch).collect();
    let (boundary, reason) = match breaks[..] {
        [b] => {
            let extra = off[b] as i64 - off[b - 1] as i64 - pitch as i64;
            (b, format!("layout: group boundary before slot {b} (spacing {pitch}{extra:+}); lower addresses = L"))
        }
        [] if off.len() == 8 => (4, "layout: no boundary in the spacing; 4 + 4 slots by address, lower = L".into()),
        _ => return None,
    };
    Some(((0..off.len()).map(|k| (k >= boundary) as u8).collect(), reason))
}

#[cfg(test)]
mod tests {
    use super::*;

    const ORIGIN: usize = 0x51_2345; // header of slot 0; deliberately not row-aligned
    const PITCH: usize = EXPECTED_PITCH;
    // As on the device, the second eye's group starts this much later than the spacing predicts.
    const GROUP_GAP: usize = 64;

    fn header_at(slot: usize) -> usize {
        ORIGIN + slot * PITCH + if slot >= 4 { GROUP_GAP } else { 0 }
    }

    /// A fake camera: writes a new frame into the next slot of each eye at `fps` per eye, plus heap churn.
    struct Sim {
        ring: *mut u8,
        other: *mut u8,
        fps: f64,
        t: f64,
        next: [f64; 2],
        count: [usize; 2],
        rng: u64,
        /// What the row padding holds (never changes): 0 as on the device, or stale junk.
        pad: u8,
        /// Draw camera 0's eye on the right and camera 1's on the left (what fooled the old picture rule).
        mirrored: bool,
    }

    impl Sim {
        fn rand(&mut self) -> u32 {
            self.rng ^= self.rng << 13;
            self.rng ^= self.rng >> 7;
            self.rng ^= self.rng << 17;
            (self.rng >> 32) as u32
        }

        fn write_frame(&mut self, eye: usize) {
            let slot = eye * 4 + self.count[eye] % 4;
            self.count[eye] += 1;
            let hdr = header_at(slot);
            let frame = hdr + HEADER_BYTES;
            for (i, b) in ((self.t * 1e9) as u64).to_le_bytes().iter().enumerate() {
                unsafe { *self.ring.add(hdr + i) = *b };
            }
            // Eye 0 sits left of eye 1; both have a dim, noisy top (which the row-mean search calls dark).
            let cx = if (eye == 0) != self.mirrored { 150.0 } else { 250.0 };
            for y in 0..HEIGHT {
                for x in 0..WIDTH {
                    let noise = self.rand() % 8;
                    let r2 = ((x as f64 - cx).powi(2) + (y as f64 - 200.0).powi(2)) / 1600.0;
                    let v = if y < 4 {
                        2 + noise % 3
                    } else if r2 < 1.0 {
                        12 + noise
                    } else {
                        40 + (60.0 * (-r2 / 30.0).exp()) as u32 + noise
                    };
                    unsafe { *self.ring.add(frame + y * STRIDE + x) = v as u8 };
                }
                for x in WIDTH..STRIDE {
                    unsafe { *self.ring.add(frame + y * STRIDE + x) = self.pad };
                }
            }
        }

        fn churn(&mut self) {
            // A mostly black block with a thin noisy band, rewritten constantly (22 MiB into the other buffer).
            for i in 0..FRAME_BYTES {
                let v = if (i / STRIDE) % 100 < 8 { (self.rand() % 255) as u8 } else { 0 };
                unsafe { *self.other.add(22 << 20 | i) = v };
            }
        }
    }

    impl Clock for Sim {
        fn now(&mut self) -> f64 {
            self.t
        }

        fn wait(&mut self) {
            self.t += 0.004;
            for eye in 0..2 {
                while self.next[eye] <= self.t {
                    self.next[eye] += 1.0 / self.fps;
                    self.write_frame(eye);
                }
            }
            if (self.t * 1000.0) as u64 % 20 < 4 {
                self.churn();
            }
        }
    }

    #[test]
    fn finds_the_ring_and_exact_frame_starts() {
        find_ring(0, false, 90.0);
    }

    #[test]
    fn finds_exact_frame_starts_with_stale_padding() {
        // Nonzero but under the lit threshold (16), so the frame test still sees a plausible lit fraction.
        find_ring(0x0c, false, 90.0);
    }

    #[test]
    fn cameras_follow_the_layout_not_the_picture() {
        find_ring(0, true, 90.0);
    }

    #[test]
    fn finds_the_ring_of_cameras_at_15_fps() {
        // Each slot is refilled 3.75 times a second.
        find_ring(0, false, 15.0);
    }

    /// A clock that never waits (nothing to look at refreshes).
    struct Still(f64);

    impl Clock for Still {
        fn now(&mut self) -> f64 {
            self.0
        }
        fn wait(&mut self) {
            self.0 += 0.01;
        }
    }

    #[test]
    fn a_look_at_unchanged_memory_stops_at_no_candidates() {
        let buf = vec![0u8; 4 << 20];
        let mut arena = unsafe { Arena::new(buf.as_ptr(), buf.len()) };
        arena.note_block_changes();
        let mut look = Look { candidates: 9, slots: 8, both_eyes: true, ..Look::default() };
        assert!(discover(&[arena], &mut Still(0.0), &mut |_| {}, &mut look).is_none());
        assert_eq!(look, Look { stopped_at: STOP_NO_CANDIDATES, ..Look::default() });
    }

    #[test]
    fn candidates_split_between_buffers_stop_at_split_buffers() {
        // One eye-like picture in each of two buffers: 2 candidates in all, 1 in the buffer it would work in
        let mut bufs = [vec![0u8; 2 << 20], vec![0u8; 2 << 20]];
        for buf in &mut bufs {
            for y in 0..HEIGHT {
                for x in 0..WIDTH {
                    buf[(1 << 20) + y * STRIDE + x] = (40 + (x * 7 + y * 3) % 9) as u8;
                }
            }
        }
        let mut arenas: Vec<Arena> = bufs.iter().map(|b| unsafe { Arena::new(b.as_ptr(), b.len()) }).collect();
        for arena in &mut arenas {
            arena.block_changed.fill(true);
        }
        let mut look = Look::default();
        assert!(discover(&arenas, &mut Still(0.0), &mut |_| {}, &mut look).is_none());
        assert_eq!(look, Look { candidates: 2, stopped_at: STOP_SPLIT_BUFFERS, ..Look::default() });
        drop(arenas);
        drop(bufs);
    }

    fn find_ring(pad: u8, mirrored: bool, fps: f64) {
        let mut ring_buf = vec![0u8; 16 << 20];
        let mut other_buf = vec![0u8; 32 << 20];
        let (ring_ptr, other_ptr) = (ring_buf.as_mut_ptr(), other_buf.as_mut_ptr());
        let mut sim = Sim {
            ring: ring_ptr,
            other: other_ptr,
            fps,
            t: 0.0,
            next: [0.0, 0.005],
            count: [0, 0],
            rng: 0x9e3779b97f4a7c15,
            pad,
            mirrored,
        };
        // A stale but eye-like picture elsewhere in the heap, which must not be picked.
        for y in 0..HEIGHT {
            for x in 0..WIDTH {
                unsafe { *ring_ptr.add(0x10_0000 + y * STRIDE + x) = (50 + (x + y) % 7) as u8 };
            }
        }
        for _ in 0..8 {
            sim.wait_frames();
        }
        let mut arenas =
            unsafe { vec![Arena::new(ring_ptr, ring_buf.len()), Arena::new(other_ptr, other_buf.len())] };
        // Long enough for every slot to be rewritten (200 ms at 90 fps)
        for _ in 0..(50.0 * 90.0 / fps) as usize {
            sim.wait();
        }
        for arena in &mut arenas {
            arena.note_block_changes();
        }
        let mut lines = Vec::new();
        let mut look = Look::default();
        let ring = discover(&arenas, &mut sim, &mut |line| lines.push(line), &mut look).expect("ring");
        for line in &lines {
            eprintln!("{line}");
        }
        assert_eq!(ring.arena, 0);
        assert_eq!(ring.pitch, PITCH);
        let expected: Vec<usize> = (0..8).map(|k| header_at(k) + HEADER_BYTES).collect();
        assert_eq!(ring.off, expected);
        assert_eq!(ring.eye, vec![0, 0, 0, 0, 1, 1, 1, 1]);
        assert!(ring.both_eyes);
        // What the panel's diagnostics show of it: the candidates, how often they refreshed, the slots, both eyes
        assert!(look.candidates >= 8, "{look:?}");
        assert!(look.refresh_hz >= if fps < 20.0 { 3.0 } else { 5.0 }, "{look:?}");
        assert_eq!((look.slots, look.both_eyes, look.stopped_at), (8, true, ""), "{look:?}");
        assert!(ring.camera_reason.starts_with("layout: group boundary before slot 4 (spacing 262208+64)"));
        assert!(!ring.camera_reason.contains("WARNING"), "{}", ring.camera_reason);
        let clean = if pad == 0 { 0.001 } else { 0.51 };
        assert!(ring.framing.pad_dirty.iter().all(|&d| d < clean), "{:?}", ring.framing.pad_dirty);
        drop(arenas);
        drop((ring_buf, other_buf));
    }

    impl Sim {
        /// Fill every slot once, as a running camera would have before we look.
        fn wait_frames(&mut self) {
            for eye in 0..2 {
                self.write_frame(eye);
            }
        }
    }

    /// The lock dump of the first recording on the device (lens interior; not in git), at the address it was
    /// taken from. Run with `EYECAM_DUMP=.../lock_dump.bin cargo test --release -- --ignored real_dump`.
    #[test]
    #[ignore]
    fn real_dump_frame_starts() {
        const DUMP_AT: usize = 0x22_4100;
        let dump = std::fs::read(std::env::var("EYECAM_DUMP").expect("set EYECAM_DUMP")).unwrap();
        // Where the ring walk put the slots that time (spacing 262208 from slot 0), and where they really start:
        // the second group is 64 bytes later. The old single-grid framing locked at walk + 64 for every slot.
        let walk: Vec<usize> = (0..8).map(|k| 0x23_4100 + k * EXPECTED_PITCH).collect();
        for (k, &w) in walk.iter().enumerate() {
            let truth = w + if k >= 4 { 64 } else { 0 };
            for guess in [w, w + 64] {
                let rel = guess - DUMP_AT;
                let region = &dump[rel - MARGIN_ROWS * STRIDE..rel + (HEIGHT + MARGIN_ROWS + 1) * STRIDE];
                let (shift, dirty) = refine_start(region, None);
                assert_eq!((guess as i64 + shift) as usize, truth, "slot {k}, guess 0x{guess:x}");
                assert!(dirty < 0.001, "slot {k}: dirty padding {dirty}");
            }
            // The header is the camera timestamp (ns), 90 Hz frames, both eyes within a frame of each other.
            let at = truth - DUMP_AT - HEADER_BYTES;
            let ns = u64::from_le_bytes(dump[at..at + 8].try_into().unwrap());
            assert!((84_900e9..85_000e9).contains(&(ns as f64)), "slot {k}: header {ns}");
            assert!(dump[at + 8..at + HEADER_BYTES].iter().all(|&b| b == 0));
        }
        let truth: Vec<usize> = walk.iter().enumerate().map(|(k, &w)| w + if k >= 4 { 64 } else { 0 }).collect();
        assert_eq!(assign_cameras(&truth, EXPECTED_PITCH).unwrap().0, vec![0, 0, 0, 0, 1, 1, 1, 1]);
    }

    #[test]
    fn assigns_cameras_from_the_layout() {
        let p = EXPECTED_PITCH;
        // As on the device: the second group 64 bytes late.
        let off: Vec<usize> = (0..8).map(|k| 0x23_4100 + k * p + if k >= 4 { 64 } else { 0 }).collect();
        let (eye, reason) = assign_cameras(&off, p).unwrap();
        assert_eq!(eye, vec![0, 0, 0, 0, 1, 1, 1, 1]);
        assert!(reason.contains("before slot 4"), "{reason}");
        // A boundary elsewhere (a slot not refreshing was dropped) still splits there.
        let (eye, _) = assign_cameras(&[off[1], off[2], off[3], off[4], off[5]], p).unwrap();
        assert_eq!(eye, vec![0, 0, 0, 1, 1]);
        // No boundary: 4 + 4 by address; anything else is left to the caller.
        let even: Vec<usize> = (0..8).map(|k| 0x10_0000 + k * p).collect();
        assert_eq!(assign_cameras(&even, p).unwrap().0, vec![0, 0, 0, 0, 1, 1, 1, 1]);
        assert!(assign_cameras(&even[..4], p).is_none());
        assert!(assign_cameras(&[off[0], off[1], off[4], off[5] + 128], p).is_none());
    }

    #[test]
    fn frame_test_separates_eyes_from_junk() {
        let mut eye = vec![0u8; FRAME_BYTES];
        for y in 0..HEIGHT {
            for x in 0..WIDTH {
                eye[y * STRIDE + x] = (40 + (x * 7 + y * 3) % 9) as u8;
            }
        }
        assert!(looks_like_eye_frame(&eye));
        let mut junk = vec![0u8; FRAME_BYTES];
        for y in 0..40 {
            for x in 0..STRIDE {
                junk[y * STRIDE + x] = 60;
            }
        }
        assert!(!looks_like_eye_frame(&junk));
    }
}
