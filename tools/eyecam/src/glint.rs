//! Glints (the eye-tracking LEDs' reflections on the cornea) near the pupil, and how far the headset has slipped since a
//! template of them was taken: a camera-to-eye shift moves the pupil and the glints alike, while the eye turning moves
//! the glints far less than the pupil (about a tenth). frameeyeosc takes this slip out of the pupil before its pupil map
//! (`pupil_slip_correction`), so a pushed or re-seated headset doesn't move the pupils' gaze.
//!
//! Per frame: the bright spots the 9x9 opening removes (frame minus opening over TOPHAT, brighter than BRIGHT, at most
//! MAX_AREA px each) within REACH iris radii of the pupil, brightest first (FindGlints). The template is the glints'
//! places while the eye was still for TEMPLATE_S (each place seen in at least TEMPLATE_SHARE of those frames) where it
//! usually looks (CENTRAL_PX of the pupil's median over HISTORY_S), taken by itself at the first such moment and again
//! when asked (`glint template`, or a new wear). Each frame the glints are
//! matched to the template at its predicted offset (the last offset, plus K_PRED times the pupil's move since: the
//! glints move a little with the eye), within GATE px, one glint per template place; at least MIN_MATCH matched give
//! the new offset (their median shift) unless it is a step of more than JUMP px (about half a glint spacing: a match one
//! glint over). Not tracked for LOST_S, the whole pattern is matched anywhere and the fit nearest the prediction taken.
//! On the two recorded slip tests of 2026-11-14, replayed through this and frameeyeosc's correction (pupil_slip.rs),
//! the pupil gaze's median error per step was 5.1° / 4.0° against 9.0° / 8.3° without it.

/// The top-hat (frame minus its 9x9 opening) a glint pixel has at least...
pub const TOPHAT: u8 = 40;
/// ...and how bright it is at least.
pub const BRIGHT: u8 = 120;
/// A glint is at most this many px (larger bright areas are not glints).
pub const MAX_AREA: u32 = 120;
/// Glints within this many iris radii of the pupil centre...
pub const REACH: f32 = 1.3;
/// ...or this many px without an iris radius.
pub const DEFAULT_REACH: f32 = 100.0;
/// At most this many glints are kept (the brightest by peak x area).
pub const MAX_GLINTS: usize = 10;
/// A glint belongs to a template place predicted within this many px.
pub const GATE: f32 = 3.5;
/// A frame's offset this far from the prediction is not taken (px).
pub const JUMP: f32 = 4.0;
/// Glints matched for an offset, at least.
pub const MIN_MATCH: usize = 2;
/// The glints' move with the eye per px of the pupil's, for the prediction.
pub const K_PRED: f32 = 0.2;
/// Not tracked this long (s): the whole pattern is matched again.
pub const LOST_S: f64 = 0.3;
/// A template is the glints of this long a still stretch (s)...
pub const TEMPLATE_S: f64 = 1.0;
/// ...with at least this many frames...
pub const TEMPLATE_FRAMES: usize = 10;
/// ...the pupil within this many px of its median all through...
pub const STILL_PX: f32 = 3.0;
/// ...each place seen in at least this share of its frames, and at least TEMPLATE_PLACES places.
pub const TEMPLATE_SHARE: f32 = 0.4;
pub const TEMPLATE_PLACES: usize = 3;
/// ...and only where the eye usually looks: the stretch's pupil within this many px (about 6°) of the pupil's median
/// over the last HISTORY_S (once there are HISTORY_MIN_S of it), so a still moment while looking aside makes none (the
/// glints' pattern changes as the eye turns; most of the time the eyes look ahead).
pub const CENTRAL_PX: f32 = 6.0;
pub const HISTORY_S: f64 = 20.0;
pub const HISTORY_MIN_S: f64 = 5.0;
/// Glints closer than this are one place in the template (px).
const CLUSTER_PX: f32 = 3.0;

/// One glint.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Glint {
    pub x: f32,
    pub y: f32,
    pub area: u32,
    pub peak: u8,
}

/// Scratch for `find` (kept between frames).
#[derive(Default)]
pub struct FindGlints {
    label: Vec<u32>,
    stack: Vec<usize>,
}

impl FindGlints {
    /// The glints of one upright frame near its pupil, brightest first, into `out`. `open` is the frame's 9x9 opening,
    /// valid from column `open_x0` on (columns left of it are not looked at).
    #[allow(clippy::too_many_arguments)]
    pub fn find(
        &mut self,
        img: &[u8],
        open: &[u8],
        open_x0: usize,
        w: usize,
        h: usize,
        centre: [f32; 2],
        iris_r: f32,
        out: &mut Vec<Glint>,
    ) {
        out.clear();
        if img.len() < w * h || open.len() < w * h || !centre[0].is_finite() || !centre[1].is_finite() {
            return;
        }
        let reach = if iris_r.is_finite() && iris_r > 0.0 { iris_r * REACH } else { DEFAULT_REACH };
        let x0 = ((centre[0] - reach).floor().max(0.0) as usize).max(open_x0).min(w);
        let x1 = ((centre[0] + reach).ceil().max(0.0) as usize).min(w);
        let y0 = ((centre[1] - reach).floor().max(0.0) as usize).min(h);
        let y1 = ((centre[1] + reach).ceil().max(0.0) as usize).min(h);
        if x1 <= x0 || y1 <= y0 {
            return;
        }
        let (ww, wh) = (x1 - x0, y1 - y0);
        self.label.clear();
        self.label.resize(ww * wh, 0);
        let bright = |x: usize, y: usize| {
            let i = y * w + x;
            img[i] > BRIGHT && img[i].saturating_sub(open[i]) > TOPHAT
        };
        let mut next = 1u32;
        for wy in 0..wh {
            for wx in 0..ww {
                let li = wy * ww + wx;
                if self.label[li] != 0 || !bright(x0 + wx, y0 + wy) {
                    continue;
                }
                // One component (4-neighbour)
                self.label[li] = next;
                self.stack.clear();
                self.stack.push(li);
                let (mut n, mut sx, mut sy, mut peak) = (0u32, 0.0f64, 0.0f64, 0u8);
                while let Some(j) = self.stack.pop() {
                    let (jx, jy) = (j % ww, j / ww);
                    n += 1;
                    sx += jx as f64;
                    sy += jy as f64;
                    peak = peak.max(img[(y0 + jy) * w + x0 + jx]);
                    let mut push = |nx: usize, ny: usize| {
                        let k = ny * ww + nx;
                        if self.label[k] == 0 && bright(x0 + nx, y0 + ny) {
                            self.label[k] = next;
                            self.stack.push(k);
                        }
                    };
                    if jx > 0 {
                        push(jx - 1, jy);
                    }
                    if jx + 1 < ww {
                        push(jx + 1, jy);
                    }
                    if jy > 0 {
                        push(jx, jy - 1);
                    }
                    if jy + 1 < wh {
                        push(jx, jy + 1);
                    }
                }
                next += 1;
                if n > MAX_AREA {
                    continue;
                }
                let g = Glint { x: (x0 as f64 + sx / n as f64) as f32, y: (y0 as f64 + sy / n as f64) as f32, area: n, peak };
                if (g.x - centre[0]).hypot(g.y - centre[1]) <= reach {
                    out.push(g);
                }
            }
        }
        out.sort_by(|a, b| (b.peak as u32 * b.area).cmp(&(a.peak as u32 * a.area)));
        out.truncate(MAX_GLINTS);
    }
}

/// What the tracker gives for one frame.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Slip {
    /// Glints found in this frame.
    pub glints: u32,
    /// The offset was worked out from this frame (else it is the last one, held).
    pub tracked: bool,
    /// Glints matched to the template in this frame (0 when not tracked).
    pub matches: u32,
    /// Frames since the offset was last worked out (0 when tracked).
    pub since_good: u32,
    /// The glints' offset from the template (px, upright image: x right, y down), held while not tracked.
    pub offset: [f32; 2],
    /// The template in use: 0 before the first, then counting up (a new one starts from offset 0 again).
    pub template_id: u32,
    /// When it was taken (the frame's time, s).
    pub template_t: f64,
}

/// One eye's template and offset.
#[derive(Default)]
pub struct Tracker {
    template: Vec<[f32; 2]>,
    template_id: u32,
    template_t: f64,
    /// A new template is wanted (none yet, asked for, a new wear).
    wanted: bool,
    /// The still stretch so far: each frame's time, pupil and glints.
    still: Vec<(f64, [f32; 2], Vec<[f32; 2]>)>,
    /// The pupil over the last HISTORY_S (where the eye usually looks).
    history: std::collections::VecDeque<(f64, [f32; 2])>,
    offset: [f32; 2],
    /// The pupil when the offset was last worked out.
    pupil_at: Option<[f32; 2]>,
    last_good: f64,
    since_good: u32,
}

fn median2(v: &mut [[f32; 2]]) -> [f32; 2] {
    let mut out = [0.0; 2];
    for (axis, o) in out.iter_mut().enumerate() {
        let mut a: Vec<f32> = v.iter().map(|p| p[axis]).collect();
        a.sort_by(f32::total_cmp);
        let n = a.len();
        *o = if n % 2 == 1 { a[n / 2] } else { (a[n / 2 - 1] + a[n / 2]) / 2.0 };
    }
    out
}

fn dist(a: [f32; 2], b: [f32; 2]) -> f32 {
    (a[0] - b[0]).hypot(a[1] - b[1])
}

/// Places seen close together in many frames: the densest point's neighbourhood, its mean refined, taken out, and again
/// while a place has at least `min_count` points.
fn cluster(points: &[[f32; 2]], min_count: usize) -> Vec<[f32; 2]> {
    let mut left: Vec<[f32; 2]> = points.to_vec();
    let mut out = Vec::new();
    while !left.is_empty() {
        let (best, _) = left
            .iter()
            .enumerate()
            .map(|(i, p)| (i, left.iter().filter(|q| dist(*p, **q) < CLUSTER_PX).count()))
            .max_by_key(|(_, n)| *n)
            .unwrap();
        let mut c = left[best];
        for _ in 0..5 {
            let near: Vec<&[f32; 2]> = left.iter().filter(|q| dist(c, **q) < CLUSTER_PX).collect();
            let n = near.len() as f32;
            c = [near.iter().map(|q| q[0]).sum::<f32>() / n, near.iter().map(|q| q[1]).sum::<f32>() / n];
        }
        let members = left.iter().filter(|q| dist(c, **q) < CLUSTER_PX * 1.5).count();
        if members < min_count {
            break;
        }
        out.push(c);
        left.retain(|q| dist(c, *q) >= CLUSTER_PX * 1.5);
    }
    out
}

impl Tracker {
    /// Take a new template at the next still moment (`glint template`, a new wear). The one in use stays until then.
    pub fn want_template(&mut self) {
        self.wanted = true;
        self.still.clear();
    }

    /// The template's places (for tests and the replay).
    pub fn template(&self) -> &[[f32; 2]] {
        &self.template
    }

    /// The template in use (0 before the first).
    pub fn template_id_now(&self) -> u32 {
        self.template_id
    }

    /// One frame: its time (s), the pupil centre (None without a pupil), and its glints.
    pub fn frame(&mut self, t: f64, pupil: Option<[f32; 2]>, glints: &[Glint]) -> Slip {
        let points: Vec<[f32; 2]> = glints.iter().map(|g| [g.x, g.y]).collect();
        // (ten a second is plenty for where the eye usually looks)
        if let Some(p) = pupil
            && self.history.back().is_none_or(|(at, _)| t - at >= 0.1)
        {
            self.history.push_back((t, p));
        }
        while self.history.front().is_some_and(|(at, _)| t - at > HISTORY_S) {
            self.history.pop_front();
        }
        if self.template.is_empty() || self.wanted {
            self.collect(t, pupil, &points);
        }
        let mut out = Slip { glints: glints.len() as u32, ..Slip::default() };
        if !self.template.is_empty() {
            let predicted = match (pupil, self.pupil_at) {
                (Some(p), Some(at)) => [
                    self.offset[0] + K_PRED * (p[0] - at[0]),
                    self.offset[1] + K_PRED * (p[1] - at[1]),
                ],
                _ => self.offset,
            };
            let mut got = self.matched(&points, predicted);
            if got.is_none() && t - self.last_good > LOST_S && points.len() >= 3 {
                got = self.found_again(&points, predicted);
            }
            match got {
                Some((offset, n)) => {
                    self.offset = offset;
                    if pupil.is_some() {
                        self.pupil_at = pupil;
                    }
                    self.last_good = t;
                    self.since_good = 0;
                    out.tracked = true;
                    out.matches = n as u32;
                }
                None => self.since_good = self.since_good.saturating_add(1),
            }
        }
        out.since_good = self.since_good;
        out.offset = self.offset;
        out.template_id = self.template_id;
        out.template_t = self.template_t;
        out
    }

    /// The glints matched to the template at `predicted` (each to the nearest place within GATE, nearest first, one
    /// glint per place): their median shift and how many, if enough and not a step.
    fn matched(&self, points: &[[f32; 2]], predicted: [f32; 2]) -> Option<([f32; 2], usize)> {
        let near = |p: [f32; 2]| {
            self.template
                .iter()
                .enumerate()
                .map(|(j, q)| (j, dist(p, [q[0] + predicted[0], q[1] + predicted[1]])))
                .min_by(|a, b| a.1.total_cmp(&b.1))
        };
        let mut order: Vec<(usize, usize, f32)> =
            points.iter().enumerate().filter_map(|(i, p)| near(*p).map(|(j, d)| (i, j, d))).collect();
        order.sort_by(|a, b| a.2.total_cmp(&b.2));
        let mut used = vec![false; self.template.len()];
        let mut shifts = Vec::new();
        for (i, j, d) in order {
            if d < GATE && !used[j] {
                used[j] = true;
                shifts.push([points[i][0] - self.template[j][0], points[i][1] - self.template[j][1]]);
            }
        }
        if shifts.len() < MIN_MATCH {
            return None;
        }
        let n = shifts.len();
        let offset = median2(&mut shifts);
        (dist(offset, predicted) <= JUMP).then_some((offset, n))
    }

    /// The whole pattern matched anywhere: every shift that lines up at least 3 glints with the template, the one
    /// nearest the prediction (then the most glints).
    fn found_again(&self, points: &[[f32; 2]], predicted: [f32; 2]) -> Option<([f32; 2], usize)> {
        let mut best: Option<([f32; 2], usize, f32)> = None;
        for p in points {
            for q in &self.template {
                let shift = [p[0] - q[0], p[1] - q[1]];
                let mut sum = [0.0f32; 2];
                let mut n = 0usize;
                for g in points {
                    let (j, d) = self
                        .template
                        .iter()
                        .enumerate()
                        .map(|(j, q)| (j, dist(*g, [q[0] + shift[0], q[1] + shift[1]])))
                        .min_by(|a, b| a.1.total_cmp(&b.1))
                        .unwrap();
                    if d < GATE {
                        sum[0] += g[0] - self.template[j][0];
                        sum[1] += g[1] - self.template[j][1];
                        n += 1;
                    }
                }
                if n < 3 {
                    continue;
                }
                let offset = [sum[0] / n as f32, sum[1] / n as f32];
                let d = dist(offset, predicted);
                if best.is_none_or(|(_, bn, bd)| d < bd || (d == bd && n > bn)) {
                    best = Some((offset, n, d));
                }
            }
        }
        best.map(|(offset, n, _)| (offset, n))
    }

    /// A frame towards a template: kept while the pupil stays still with at least 3 glints; once TEMPLATE_S of them
    /// are there, the template is made from them (and the offset starts from 0 again).
    fn collect(&mut self, t: f64, pupil: Option<[f32; 2]>, points: &[[f32; 2]]) {
        let Some(p) = pupil.filter(|_| points.len() >= 3) else {
            self.still.clear();
            return;
        };
        self.still.push((t, p, points.to_vec()));
        // The last TEMPLATE_S only (a sliding window)
        while self.still.len() > 1 && t - self.still[1].0 >= TEMPLATE_S {
            self.still.remove(0);
        }
        // Still: every pupil within STILL_PX of the stretch's median; else start the stretch from this frame
        let mut pupils: Vec<[f32; 2]> = self.still.iter().map(|s| s.1).collect();
        let m = median2(&mut pupils);
        if self.still.iter().any(|s| dist(s.1, m) > STILL_PX) {
            self.still.drain(..self.still.len() - 1);
            return;
        }
        let span = t - self.still[0].0;
        if span < TEMPLATE_S || self.still.len() < TEMPLATE_FRAMES {
            return;
        }
        // Where the eye usually looks: enough of the pupil's history, and the stretch near its median (else the
        // window slides on)
        if self.history.front().is_none_or(|(at, _)| t - at < HISTORY_MIN_S) {
            return;
        }
        let mut seen: Vec<[f32; 2]> = self.history.iter().map(|h| h.1).collect();
        if dist(m, median2(&mut seen)) > CENTRAL_PX {
            return;
        }
        let all: Vec<[f32; 2]> = self.still.iter().flat_map(|s| s.2.iter().copied()).collect();
        let min_count = ((self.still.len() as f32 * TEMPLATE_SHARE).ceil() as usize).max(3);
        let places = cluster(&all, min_count);
        if places.len() < TEMPLATE_PLACES {
            // Not enough steady places: try again with the frames from now on
            self.still.clear();
            return;
        }
        self.template = places;
        self.template_id += 1;
        self.template_t = t;
        self.wanted = false;
        self.still.clear();
        self.offset = [0.0; 2];
        self.pupil_at = Some(p);
        self.last_good = t;
        self.since_good = 0;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const W: usize = 120;
    const H: usize = 100;

    /// A frame of grey 60 with a dark pupil disc and bright 3x3 glints at `glints`, and its 9x9 opening (the glints
    /// gone, as vision::open9 does).
    fn frame(glints: &[[usize; 2]]) -> (Vec<u8>, Vec<u8>) {
        let mut img = vec![60u8; W * H];
        for y in 0..H {
            for x in 0..W {
                if (x as f32 - 60.0).hypot(y as f32 - 50.0) < 12.0 {
                    img[y * W + x] = 15;
                }
            }
        }
        let open = img.clone();
        for g in glints {
            for dy in 0..3 {
                for dx in 0..3 {
                    img[(g[1] + dy - 1) * W + g[0] + dx - 1] = if dx == 1 && dy == 1 { 250 } else { 200 };
                }
            }
        }
        (img, open)
    }

    #[test]
    fn glints_near_the_pupil_are_found_brightest_first() {
        let (mut img, open) = frame(&[[50, 45], [70, 45], [60, 58]]);
        // A big bright patch (not a glint) and one too far away
        for y in 80..95 {
            for x in 5..20 {
                img[y * W + x] = 255;
            }
        }
        img[10 * W + 110] = 255;
        let mut find = FindGlints::default();
        let mut out = Vec::new();
        find.find(&img, &open, 0, W, H, [60.0, 50.0], 20.0, &mut out);
        assert_eq!(out.len(), 3, "{out:?}");
        assert!(out.iter().all(|g| g.area == 9 && g.peak == 250));
        let mut xs: Vec<(f32, f32)> = out.iter().map(|g| (g.x, g.y)).collect();
        xs.sort_by(|a, b| a.0.total_cmp(&b.0));
        assert_eq!(xs, vec![(50.0, 45.0), (60.0, 58.0), (70.0, 45.0)]);
        // Columns left of the opening's valid ones are not looked at
        find.find(&img, &open, 55, W, H, [60.0, 50.0], 20.0, &mut out);
        assert_eq!(out.len(), 2);
        // No pupil: nothing
        find.find(&img, &open, 0, W, H, [f32::NAN, 50.0], 20.0, &mut out);
        assert!(out.is_empty());
    }

    fn glints(at: &[[f32; 2]]) -> Vec<Glint> {
        at.iter().map(|p| Glint { x: p[0], y: p[1], area: 9, peak: 250 }).collect()
    }

    /// Four LEDs' glints around the pupil, moved by `shift`.
    fn pattern(shift: [f32; 2]) -> Vec<Glint> {
        glints(&[[50.0, 45.0], [70.0, 45.0], [60.0, 58.0], [62.0, 40.0]].map(|p| [p[0] + shift[0], p[1] + shift[1]]))
    }

    #[test]
    fn a_template_is_taken_once_the_eye_is_still_and_the_slip_followed() {
        let mut tr = Tracker::default();
        let dt = 1.0 / 90.0;
        let mut t = 0.0;
        // The pupil wanders at first: no template
        for i in 0..60 {
            let s = tr.frame(t, Some([60.0 + (i % 10) as f32, 50.0]), &pattern([0.0, 0.0]));
            assert_eq!((s.template_id, s.tracked), (0, false));
            t += dt;
        }
        // Still for a second: the template, offset 0 (once HISTORY_MIN_S of the pupil is known)
        let mut first = None;
        for _ in 0..500 {
            let s = tr.frame(t, Some([60.0, 50.0]), &pattern([0.0, 0.0]));
            if s.template_id == 1 && first.is_none() {
                first = Some(t);
            }
            t += dt;
        }
        assert!(first.is_some_and(|f| (HISTORY_MIN_S..HISTORY_MIN_S + 0.12).contains(&f)), "{first:?}");
        assert_eq!(tr.template().len(), 4);
        // The headset slips 12 px down over half a second: the pupil and the glints alike; followed
        let mut last = Slip::default();
        for i in 1..=45 {
            let d = 12.0 * i as f32 / 45.0;
            last = tr.frame(t, Some([60.0, 50.0 + d]), &pattern([0.0, d]));
            assert!(last.tracked, "frame {i}: {last:?}");
            t += dt;
        }
        assert!((last.offset[0]).abs() < 0.01 && (last.offset[1] - 12.0).abs() < 0.01, "{last:?}");
        assert_eq!((last.matches, last.since_good, last.template_id), (4, 0, 1));
        // A blink: no pupil, no glints; the offset held
        for i in 0..20 {
            let s = tr.frame(t, None, &[]);
            assert_eq!((s.tracked, s.since_good, s.offset), (false, i + 1, last.offset));
            t += dt;
        }
        // Back, where it was: found again
        let s = tr.frame(t, Some([60.0, 62.0]), &pattern([0.0, 12.0]));
        assert!(s.tracked && (s.offset[1] - 12.0).abs() < 0.01, "{s:?}");
    }

    #[test]
    fn no_template_while_looking_aside() {
        let mut tr = Tracker::default();
        let mut t = 0.0;
        // Ahead for 10 s, moving a little (no still second: 8 px back and forth), then still while looking 13 px aside:
        // no template
        for i in 0..900 {
            tr.frame(t, Some([58.0 + (i % 9) as f32, 50.0]), &pattern([0.0, 0.0]));
            t += 0.011;
        }
        for _ in 0..200 {
            assert_eq!(tr.frame(t, Some([75.0, 50.0]), &pattern([1.5, 0.0])).template_id, 0);
            t += 0.011;
        }
        // Back ahead and still: there it is
        let mut s = Slip::default();
        for _ in 0..120 {
            s = tr.frame(t, Some([62.0, 50.0]), &pattern([0.0, 0.0]));
            t += 0.011;
        }
        assert_eq!(s.template_id, 1);
    }

    #[test]
    fn a_match_one_glint_over_is_not_taken() {
        let mut tr = Tracker::default();
        let mut t = 0.0;
        for _ in 0..600 {
            tr.frame(t, Some([60.0, 50.0]), &pattern([0.0, 0.0]));
            t += 0.011;
        }
        assert_eq!(tr.template().len(), 4);
        // Two stray spots that line up with the template 8 px over (a jump): not taken, the offset stays 0
        let s = tr.frame(t, Some([60.0, 50.0]), &glints(&[[58.0, 45.0], [78.0, 45.0]]));
        assert!(!s.tracked && s.offset == [0.0, 0.0], "{s:?}");
        // The eye turns: the pupil 20 px right, the glints 4 px: the prediction (K_PRED) keeps them matched
        let s = tr.frame(t + 0.011, Some([80.0, 50.0]), &pattern([4.0, 0.0]));
        assert!(s.tracked && (s.offset[0] - 4.0).abs() < 0.01, "{s:?}");
    }

    #[test]
    fn a_new_template_is_taken_when_asked_and_starts_from_zero() {
        let mut tr = Tracker::default();
        let mut t = 0.0;
        for _ in 0..600 {
            tr.frame(t, Some([60.0, 50.0]), &pattern([0.0, 0.0]));
            t += 0.011;
        }
        for _ in 0..30 {
            tr.frame(t, Some([60.0, 53.0]), &pattern([0.0, 3.0]));
            t += 0.011;
        }
        tr.want_template();
        let mut s = Slip::default();
        for _ in 0..120 {
            s = tr.frame(t, Some([60.0, 53.0]), &pattern([0.0, 3.0]));
            t += 0.011;
        }
        assert_eq!(s.template_id, 2);
        assert!(s.tracked && s.offset[1].abs() < 0.01 && s.template_t > 1.3, "{s:?}");
    }
}
