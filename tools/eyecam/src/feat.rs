//! Per-eye, per-frame classical features: a port of tools/eyecam/analysis/feat2.py (the "base" variant the
//! cross-wear evaluation chose). Works on upright 400x400 frames (the right eye flipped vertically).
//!
//! Pipeline: grey opening 9x9 (removes glints) -> darkest compact blob = pupil -> 64 rays from the blob centre,
//! sub-pixel pupil->iris crossing, robust ellipse (diameter = 2a) -> iris radius from the limbus -> upper lid
//! "skin line" (bright skin -> dark band, DP path along the vertical gradient, pupil masked) and its margin below,
//! lower lid (dark -> bright skin) -> box brightness where the iris should be (closed lids are bright skin).
//!
//! Numerical details follow the prototype (numpy's int() truncation, banker's rounding, percentile interpolation,
//! first-index argmin/argmax) so features agree with it; see the agreement report in the README.

use crate::vision::{self, CcBufs, Component, Ellipse, GaussBufs, H, MorphBufs, Quad, Rect, W};

const NRAY: usize = 64;
/// Median margin - skin line offset (px) when the margin itself is not found (feat2.MARGIN_OFF).
pub const MARGIN_OFF: f64 = 7.0;
/// Left edge of every search region in the prototype. The small-x side of both upright images is the NASAL side
/// (pupils move toward it when the eyes converge); it is dark shading there, as dark as the pupil.
pub const X_MIN: f64 = 186.0;
/// The live search's left edge adapts per wear (`search_x_min`) within these bounds.
pub const X_MIN_LO: f64 = 100.0;
/// The prototype's opening is computed from this column on (exact from 8 columns further), the blurred image from
/// OPF_X0. The leftmost samples ever taken are the iris rays on the nasal side, 95 px left of a pupil that is right
/// of X_MIN. The live search starts them `OPEN_BEFORE` columns left of its own left edge instead (room for a pupil
/// centred up to 20 px left of it, cut by the window; each 10 columns cost about 1% of the frame time).
const OPEN_X0: usize = 72;
const OPF_X0: usize = 80;
const OPEN_BEFORE: usize = 124;
/// Blobs refined per frame in the live search (in score order; the first that refines is the pupil).
const MAX_CANDIDATES: usize = 3;
/// A blob that touches the search window's left edge only counts as a pupil when at least this share of the rays
/// pointing left (outside the window) find the pupil's edge: dark shading cut by the window has no edge there.
const EDGE_LEFT_SHARE: f64 = 0.4;
/// The second search without the prior only looks at blobs at least this far (px) from it.
const RETRY_MIN_JUMP: f64 = 40.0;

/// How the pupil is searched for.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Search {
    /// Left edge of the search window (and of the lid and box regions).
    pub x_min: f64,
    /// The prototype's rules (`--compat`): left edge fixed at X_MIN, blobs touching it dropped, only the best blob
    /// refined, no second try without the prior.
    pub legacy: bool,
}

impl Search {
    pub const LEGACY: Search = Search { x_min: X_MIN, legacy: true };

    /// The live rules with this left edge.
    pub fn live(x_min: f64) -> Search {
        Search { x_min: x_min.clamp(X_MIN_LO, X_MIN), legacy: false }
    }
}

/// Why blobs were dropped, as bits of `Diag::rejected`.
pub const REJ_LEFT: u8 = 1;
pub const REJ_TOP: u8 = 2;
pub const REJ_BOTTOM: u8 = 4;
pub const REJ_BIG: u8 = 8;
pub const REJ_SHAPE: u8 = 16;

/// What the pupil search saw in one frame (for the calibration report).
#[derive(Clone, Copy, Debug)]
pub struct Diag {
    /// The search window's dark level (0.5th percentile) and median, in the opened, blurred image.
    pub lo: f64,
    pub p50: f64,
    /// The left edge used.
    pub x_min: f64,
    /// Blobs that passed the size and shape tests (the most of the two searches when it searched again).
    pub candidates: u8,
    /// REJ_* bits for blobs of at least 120 px that were dropped.
    pub rejected: u8,
    /// No candidate near the prior refined, and the search ran again without it.
    pub retried: bool,
    /// The pupil came from a blob that touches the window's left edge.
    pub left_edge: bool,
    /// The engine dropped the pupil: found while the iris box was as bright as lid skin and unlike the open pupil.
    pub gated: bool,
    /// The engine forgot its search prior on this frame (too many frames without a pupil).
    pub prior_reset: bool,
}

impl Default for Diag {
    fn default() -> Self {
        Self {
            lo: f64::NAN,
            p50: f64::NAN,
            x_min: f64::NAN,
            candidates: 0,
            rejected: 0,
            retried: false,
            left_edge: false,
            gated: false,
            prior_reset: false,
        }
    }
}

/// A dark blob that may be the pupil: score, centre, radius estimate, touches the window's left edge.
#[derive(Clone, Copy, Debug)]
struct Cand {
    score: f64,
    c: (f64, f64),
    r: f64,
    left: bool,
}

/// A found pupil: ellipse centre, semi-axes (a >= b), the fitted ellipse, the dark level, visible ray share.
#[derive(Clone, Copy, Debug)]
pub struct Pupil {
    pub cx: f64,
    pub cy: f64,
    pub a: f64,
    pub b: f64,
    pub ell: Ellipse,
    pub inner: f64,
    pub vis: f64,
}

/// One lid line: quadratic over the iris columns, its height at the pupil column, inlier share, edge strength.
#[derive(Clone, Copy, Debug)]
struct Lid {
    coef: Quad,
    y_at: f64,
    frac: f64,
    grad: f64,
}

/// The per-frame features (NaN = not measured), named as in the prototype's CSV.
#[derive(Clone, Copy, Debug)]
pub struct Features {
    pub ok_pupil: bool,
    pub pupil_cx: f64,
    pub pupil_cy: f64,
    pub pupil_a: f64,
    pub pupil_b: f64,
    pub pupil_vis: f64,
    pub iris_r: f64,
    pub iris_n: u32,
    pub upper_skin_y: f64,
    pub upper_y: f64,
    pub lower_y: f64,
    pub upper_frac: f64,
    pub lower_frac: f64,
    pub box_mean: f64,
    /// How the search went (not part of the prototype's features).
    pub diag: Diag,
}

impl Default for Features {
    fn default() -> Self {
        let n = f64::NAN;
        Self {
            diag: Diag::default(),
            ok_pupil: false,
            pupil_cx: n,
            pupil_cy: n,
            pupil_a: n,
            pupil_b: n,
            pupil_vis: n,
            iris_r: n,
            iris_n: 0,
            upper_skin_y: n,
            upper_y: n,
            lower_y: n,
            upper_frac: n,
            lower_frac: n,
            box_mean: n,
        }
    }
}

/// The lens-edge column: where the black band on the temporal (large-x) side starts (feat2.occluder_x), from column
/// means over rows 60..340 of a frame or of a mean image.
pub fn occluder_x(col_mean: &[f64]) -> f64 {
    let first = col_mean[280..].iter().position(|&m| m < 25.0).unwrap_or(120);
    (280 + first) as f64 - 3.0
}

/// Column mean that counts as out of the dark nasal shading (`search_x_min`).
const SHADE_END_LEVEL: f64 = 40.0;
/// How far into the shading the live search may start, px.
const SHADE_MARGIN: f64 = 10.0;

/// The live search's left edge for this wear, from the same column means as `occluder_x`: where the dark shading
/// on the nasal side ends, minus `SHADE_MARGIN`, within [X_MIN_LO, X_MIN]. The end is that of the bright region the
/// iris is in: from X_MIN + SHADE_MARGIN (it and the next 3 columns at `SHADE_END_LEVEL` or brighter, else the
/// shading reaches past it and the edge stays at X_MIN) leftwards to the last column at that level, x = 60 at the
/// most. Something bright further toward the nose (a lit nose bridge) with shading between it and the iris does not
/// pull the window over the shading. The shading follows the face: when the eyes sit further toward the nose in the
/// image, so does it, and the window follows. The prototype's X_MIN is the upper bound (the developer's recordings
/// all give it).
pub fn search_x_min(col_mean: &[f64]) -> f64 {
    let bright = |x: usize| col_mean[x] >= SHADE_END_LEVEL;
    let top = (X_MIN + SHADE_MARGIN) as usize;
    if !(top..top + 4).all(bright) {
        return X_MIN;
    }
    let mut end = top;
    while end > 60 && bright(end - 1) {
        end -= 1;
    }
    (end as f64 - SHADE_MARGIN).clamp(X_MIN_LO, X_MIN)
}

/// Column means over rows 60..340 of one 400x400 image.
pub fn column_means(img: &[u8]) -> Vec<f64> {
    let mut m = vec![0.0; W];
    for y in 60..340 {
        for (x, v) in m.iter_mut().enumerate() {
            *v += img[y * W + x] as f64;
        }
    }
    m.iter_mut().for_each(|v| *v /= 280.0);
    m
}

/// Python's int() on a float: truncation toward zero.
fn pint(v: f64) -> isize {
    v.trunc() as isize
}

/// Reusable buffers and kernels; one per eye (or per thread).
pub struct Extractor {
    op: Vec<u8>,
    opf: Vec<u8>,
    smf: Vec<u8>,
    gy: Vec<i16>,
    morph: MorphBufs,
    gauss: GaussBufs,
    k1: Vec<u32>,
    k15: Vec<u32>,
    cc: CcBufs,
    comps: Vec<Component>,
    mask: Vec<bool>,
    cands: Vec<Cand>,
    /// The search rules (default: the prototype's).
    pub search: Search,
    /// First column of `opf` computed for the current frame (left of it: zeros, or an earlier frame's pixels).
    opf_x0: usize,
}

impl Default for Extractor {
    fn default() -> Self {
        Self {
            cands: Vec::new(),
            search: Search::LEGACY,
            opf_x0: OPF_X0,
            op: Vec::new(),
            opf: Vec::new(),
            smf: Vec::new(),
            gy: vec![0; W * H],
            morph: MorphBufs::default(),
            gauss: GaussBufs::default(),
            k1: vision::gauss_kernel_fixed(1.0),
            k15: vision::gauss_kernel_fixed(1.5),
            cc: CcBufs::default(),
            comps: Vec::new(),
            mask: Vec::new(),
        }
    }
}

impl Extractor {
    /// feat2.extract. `img`: upright 400x400. `prev`: last pupil centre (search prior). `r_ref`: iris radius for the
    /// search windows (else this frame's own). `prev_p`: last good pupil, used to still measure lids and the box when
    /// no pupil is found (closed, or lashes over it). Returns the features and the pupil if found.
    pub fn extract(
        &mut self,
        img: &[u8],
        xmax: f64,
        prev: Option<(f64, f64)>,
        r_ref: Option<f64>,
        prev_p: Option<&Pupil>,
    ) -> (Features, Option<Pupil>) {
        let mut f = Features::default();
        let search = self.search;
        let (open_x0, opf_x0) = if search.legacy {
            (OPEN_X0, OPF_X0)
        } else {
            let x0 = (search.x_min as usize).saturating_sub(OPEN_BEFORE);
            (x0, x0 + 8)
        };
        vision::open9_cols(img, W, H, open_x0, W, &mut self.morph, &mut self.op);
        vision::gauss_u8(&self.op, W, H, &self.k1, Rect { x0: opf_x0, y0: 0, x1: W, y1: H }, &mut self.gauss, &mut self.opf);
        self.opf_x0 = opf_x0;
        f.diag.x_min = search.x_min;
        let p = if search.legacy {
            self.find_pupil_blob(xmax, prev, &mut f.diag);
            let blob = self.cands.first().copied();
            blob.and_then(|b| self.refine_pupil(b.c, b.r, xmax)).map(|x| x.0)
        } else {
            self.find_pupil_blob(xmax, prev, &mut f.diag);
            let mut p = self.refine_candidates(xmax, None, &mut f.diag);
            if p.is_none()
                && let Some(pv) = prev
            {
                // The prior may be on something that is not the pupil: once more without it, for a pupil somewhere
                // else (near the prior this would only find the same blob at another threshold: lids half over the
                // pupil, as in a squint, which the prototype does not count as a pupil either).
                f.diag.retried = true;
                self.find_pupil_blob(xmax, None, &mut f.diag);
                p = self.refine_candidates(xmax, Some(pv), &mut f.diag);
            }
            p
        };
        f.ok_pupil = p.is_some();
        let r_ref = r_ref.filter(|&r| r != 0.0);
        let Some(p) = p else {
            let Some(q) = prev_p else { return (f, None) };
            let rn = r_ref.unwrap_or(2.0 * q.a);
            f.box_mean = self.box_mean(q.cx, q.cy, rn, xmax);
            let (upper, lower, margin) = self.lids(q, rn, xmax);
            if let Some(u) = upper {
                f.upper_skin_y = u.y_at;
                f.upper_y = u.y_at + margin.unwrap_or(MARGIN_OFF);
                f.upper_frac = u.frac;
            }
            if let Some(l) = lower {
                f.lower_y = l.y_at;
                f.lower_frac = l.frac;
            }
            return (f, None);
        };
        f.pupil_cx = p.cx;
        f.pupil_cy = p.cy;
        f.pupil_a = p.a;
        f.pupil_b = p.b;
        f.pupil_vis = p.vis;
        let (r, n) = self.iris_fit(&p, xmax);
        f.iris_r = r;
        f.iris_n = n;
        let rn = r_ref.unwrap_or(if r.is_finite() { r } else { 2.0 * p.a });
        let (upper, lower, margin) = self.lids(&p, rn, xmax);
        if let Some(u) = upper {
            f.upper_skin_y = u.y_at;
            f.upper_y = u.y_at + margin.unwrap_or(MARGIN_OFF);
            f.upper_frac = u.frac;
        }
        if let Some(l) = lower {
            f.lower_y = l.y_at;
            f.lower_frac = l.frac;
        }
        f.box_mean = self.box_mean(p.cx, p.cy, rn, xmax);
        (f, Some(p))
    }

    /// Dark compact blobs in the opened, blurred image, best first, into `self.cands`. The prototype's rules keep
    /// only the best one and drop blobs that touch the window's left edge; the live ones keep up to MAX_CANDIDATES
    /// and keep blobs cut by the left edge (`refine_candidates` decides).
    fn find_pupil_blob(&mut self, xmax: f64, prev: Option<(f64, f64)>, diag: &mut Diag) {
        let legacy = self.search.legacy;
        let xs = self.search.x_min;
        let (x0, y0, x1, y1) = (xs as usize, 70usize, (pint(xmax).clamp(xs as isize + 1, W as isize)) as usize, 320usize);
        let (sw, sh) = (x1 - x0, y1 - y0);
        let opf = &self.opf;
        let at = |x: usize, y: usize| opf[(y0 + y) * W + x0 + x];
        let mut hist = [0u32; 256];
        let window = |xa: usize, ya: usize, xb: usize, yb: usize, hist: &mut [u32; 256]| {
            for y in ya..yb {
                for x in xa..xb {
                    hist[at(x, y) as usize] += 1;
                }
            }
        };
        match prev {
            Some((px, py)) => {
                let wx0 = pint(px - 70.0 - x0 as f64).max(0) as usize;
                let wy0 = pint(py - 70.0 - y0 as f64).max(0) as usize;
                let (wx1, wy1) = ((wx0 + 140).min(sw), (wy0 + 140).min(sh));
                if wx0 < wx1 && wy0 < wy1 {
                    window(wx0, wy0, wx1, wy1, &mut hist);
                } else {
                    window(0, 0, sw, sh, &mut hist);
                }
            }
            None => window(0, 0, sw, sh, &mut hist),
        }
        let lo = vision::percentile_hist(&hist, 0.5);
        diag.lo = lo;
        diag.p50 = vision::percentile_hist(&hist, 50.0);
        self.cands.clear();
        let mut best: Option<Cand> = None;
        for dth in [14.0, 22.0, 32.0] {
            self.mask.clear();
            self.mask.extend((0..sh).flat_map(|y| (0..sw).map(move |x| (y, x))).map(|(y, x)| (at(x, y) as f64) < lo + dth));
            vision::components(&self.mask, sw, sh, &mut self.cc, &mut self.comps);
            for c in &self.comps {
                let (w, h, a) = (c.w as f64, c.h as f64, c.area as f64);
                if c.area < 120 {
                    continue;
                }
                let rej = if c.area > 5500 { REJ_BIG } else { 0 }
                    | if c.x == 0 && legacy { REJ_LEFT } else { 0 }
                    | if c.y == 0 { REJ_TOP } else { 0 }
                    | if c.y + c.h >= sh { REJ_BOTTOM } else { 0 };
                let fill = a / (w * h);
                let rej = rej | if w > 2.6 * h || h > 2.6 * w || fill < 0.5 { REJ_SHAPE } else { 0 };
                if rej != 0 {
                    diag.rejected |= rej;
                    continue;
                }
                let left = c.x == 0;
                let (centre, r) = if left {
                    // Cut by the window: the height is still the pupil's diameter, and its centre is that radius
                    // left of the blob's right end.
                    let r = 0.5 * h;
                    ((x0 as f64 + c.x as f64 + w - r, y0 as f64 + c.y as f64 + r), r.max((a / std::f64::consts::PI).sqrt()))
                } else {
                    ((c.cx + x0 as f64, c.cy + y0 as f64), (a / std::f64::consts::PI).sqrt())
                };
                let d = prev.map_or(0.0, |(px, py)| (centre.0 - px).hypot(centre.1 - py));
                let sc = a * fill / (1.0 + (d - 20.0).max(0.0).powi(2) / 400.0);
                let cand = Cand { score: sc, c: centre, r, left };
                if best.is_none_or(|b| sc > b.score) {
                    best = Some(cand);
                }
                if !legacy {
                    self.cands.push(cand);
                }
            }
            if best.is_some_and(|b| b.r > 9.0) {
                break;
            }
        }
        if legacy {
            self.cands.extend(best);
        } else {
            // Best first; the same blob at a higher threshold level only once.
            self.cands.sort_by(|a, b| b.score.total_cmp(&a.score));
            let mut kept: Vec<Cand> = Vec::with_capacity(MAX_CANDIDATES);
            for c in &self.cands {
                if kept.len() < MAX_CANDIDATES && kept.iter().all(|k| (k.c.0 - c.c.0).hypot(k.c.1 - c.c.1) > 6.0) {
                    kept.push(*c);
                }
            }
            self.cands = kept;
        }
        diag.candidates = diag.candidates.max(self.cands.len() as u8);
    }

    /// Refine the candidates in order; the first that gives a pupil wins. A blob cut by the window's left edge also
    /// needs the pupil's edge on the rays pointing left (outside the window), and no pupil may be centred past the
    /// lens edge (an arc of dark lens band fitted as an ellipse). Blobs within RETRY_MIN_JUMP of `away_from` are
    /// skipped.
    fn refine_candidates(&mut self, xmax: f64, away_from: Option<(f64, f64)>, diag: &mut Diag) -> Option<Pupil> {
        for i in 0..self.cands.len() {
            let c = self.cands[i];
            if away_from.is_some_and(|(x, y)| (c.c.0 - x).hypot(c.c.1 - y) < RETRY_MIN_JUMP) {
                continue;
            }
            if let Some((p, left_share)) = self.refine_pupil(c.c, c.r, xmax)
                && (!c.left || left_share >= EDGE_LEFT_SHARE)
                && p.cx < xmax
            {
                diag.left_edge = c.left;
                return Some(p);
            }
        }
        None
    }

    /// Rays from the blob centre; the sub-pixel crossing of the middle level between the pupil and the iris on
    /// each, rejecting rays that end past the lens edge or stay dark outside (lashes, lid, vignette); ellipse fit.
    /// Also returns the share of the rays pointing left (more than 60 degrees from vertical) that found the edge.
    /// Rays that reach left of the columns computed for this frame (a tall blob cut by the window's left edge) are
    /// skipped and not counted among the left ones: those columns hold nothing of this frame.
    fn refine_pupil(&self, c: (f64, f64), r0: f64, xmax: f64) -> Option<(Pupil, f64)> {
        let (cx, cy) = c;
        let (start, stop) = (0.4 * r0, 2.0 * r0 + 6.0);
        let nr = ((stop - start) / 0.5).ceil().max(0.0) as usize;
        let rs: Vec<f64> = (0..nr).map(|i| start + i as f64 * 0.5).collect();
        if rs.len() < 4 {
            return None;
        }
        let mut prof = vec![0f32; NRAY * nr];
        let mut last_x = [0f64; NRAY];
        for k in 0..NRAY {
            let th = k as f64 * (std::f64::consts::TAU / NRAY as f64);
            let (co, si) = (th.cos(), th.sin());
            for (j, &r) in rs.iter().enumerate() {
                let (x, y) = (cx + co * r, cy + si * r);
                prof[k * nr + j] = vision::sample(&self.opf, W, H, x as f32, y as f32);
            }
            last_x[k] = cx + co * rs[nr - 1];
        }
        let mut inner_v: Vec<f64> = Vec::new();
        for k in 0..NRAY {
            for (j, &r) in rs.iter().enumerate() {
                if r < 0.6 * r0 {
                    inner_v.push(prof[k * nr + j] as f64);
                }
            }
        }
        let inner = vision::median(&mut inner_v);
        let mut pts: Vec<(f64, f64)> = Vec::with_capacity(NRAY);
        let mut ok = 0usize;
        let (mut left_n, mut left_ok) = (0usize, 0usize);
        let mut ov = Vec::with_capacity(nr);
        let x_lo = self.opf_x0 as f64;
        for k in 0..NRAY {
            if last_x[k] < x_lo {
                continue;
            }
            let left = (k as f64 * (std::f64::consts::TAU / NRAY as f64)).cos() < -0.5;
            left_n += left as usize;
            let row = &prof[k * nr..(k + 1) * nr];
            ov.clear();
            ov.extend(rs.iter().zip(row).filter(|(r, _)| **r > 1.25 * r0 && **r < 1.7 * r0).map(|(_, &v)| v as f64));
            let outer = vision::median(&mut ov);
            if outer - inner < 14.0 || outer.is_nan() || last_x[k] >= xmax {
                continue;
            }
            let th = inner + 0.5 * (outer - inner);
            let Some(j) = row.iter().position(|&v| v as f64 > th) else { continue };
            if j == 0 {
                continue;
            }
            let (p0, p1) = (row[j - 1] as f64, row[j] as f64);
            let fr = (th - p0) / (p1 - p0).max(1e-3);
            let r = rs[j - 1] + fr * (rs[j] - rs[j - 1]);
            if r < 0.5 * r0 || r > 1.9 * r0 {
                continue;
            }
            ok += 1;
            left_ok += left as usize;
            let ang = k as f64 * (std::f64::consts::TAU / NRAY as f64);
            pts.push((cx + ang.cos() * r, cy + ang.sin() * r));
        }
        if pts.len() < 10 {
            return None;
        }
        let e = fit_ellipse_robust(pts)?;
        let (a, b) = (e.a.max(e.b) / 2.0, e.a.min(e.b) / 2.0);
        if !(5.0 < b && a < 60.0 && b / a > 0.35) {
            return None;
        }
        Some((Pupil { cx: e.cx, cy: e.cy, a, b, ell: e, inner, vis: ok as f64 / NRAY as f64 }, left_ok as f64 / left_n as f64))
    }

    /// Iris radius from limbus crossings on rays to the left and right (top and bottom excluded), mapped into the
    /// pupil ellipse's frame; crossings on the right (large x: the temporal side of both upright images, dark iris ->
    /// bright sclera) are preferred (feat2 calls them "nasal"). (R, inliers).
    fn iris_fit(&self, p: &Pupil, xmax: f64) -> (f64, u32) {
        let (cx, cy) = (p.cx, p.cy);
        let t = p.ell.angle_deg.to_radians();
        let ratio = p.ell.b.min(p.ell.a) / p.ell.a.max(p.ell.b);
        // Our ellipse's `a` is the major axis along `angle_deg`.
        let ua = (t.cos(), t.sin());
        let ub = (-ua.1, ua.0);
        let start = p.a * 1.25 + 3.0;
        let nr = ((95.0 - start) / 0.75).ceil().max(0.0) as usize;
        let rs: Vec<f64> = (0..nr).map(|i| start + i as f64 * 0.75).collect();
        let mut res: Vec<(f64, bool)> = Vec::new();
        let mut prof: Vec<f64> = Vec::with_capacity(nr);
        let mut rok: Vec<f64> = Vec::with_capacity(nr);
        let degs = (0..17).map(|i| -40 + 5 * i).chain((0..17).map(|i| 140 + 5 * i));
        for deg in degs {
            let th = (deg as f64).to_radians();
            let d = (th.cos(), th.sin());
            prof.clear();
            rok.clear();
            for &r in &rs {
                let (x, y) = (cx + d.0 * r, cy + d.1 * r);
                if x < xmax - 3.0 && x > 2.0 {
                    prof.push(vision::sample(&self.opf, W, H, x as f32, y as f32) as f64);
                    rok.push(r);
                }
            }
            let n = prof.len();
            if n < 12 {
                continue;
            }
            // np.convolve(prof, ones(3)/3, 'same'): zero outside.
            let sm: Vec<f64> = (0..n)
                .map(|i| {
                    let a = if i > 0 { prof[i - 1] } else { 0.0 };
                    let c = if i + 1 < n { prof[i + 1] } else { 0.0 };
                    (a + prof[i] + c) / 3.0
                })
                .collect();
            let temporal = d.0 > 0.0;
            let gg: Vec<f64> = (0..n - 3)
                .map(|i| {
                    let g = sm[i + 3] - sm[i];
                    if temporal || g > 0.0 { g } else { -0.8 * g }
                })
                .collect();
            let mut k = 2;
            for i in 2..gg.len() - 2 {
                if gg[i] > gg[k] {
                    k = i;
                }
            }
            if gg[k] < if temporal { 12.0 } else { 9.0 } {
                continue;
            }
            let r = rok[k + 1] + 0.375;
            let v = (d.0 * r, d.1 * r);
            let (ra, rb) = (v.0 * ua.0 + v.1 * ua.1, v.0 * ub.0 + v.1 * ub.1);
            res.push((ra.hypot(rb / ratio), temporal));
        }
        if res.len() < 4 {
            return (f64::NAN, 0);
        }
        let temporal_n = res.iter().filter(|r| r.1).count();
        let mut rho: Vec<f64> = res.iter().filter(|r| temporal_n < 4 || r.1).map(|r| r.0).collect();
        let med = vision::median(&mut rho.clone());
        rho.retain(|&r| (r - med).abs() < 6.0);
        let n = rho.len() as u32;
        let r = if n >= 3 { vision::median(&mut rho) } else { f64::NAN };
        (r, n)
    }

    /// Vertical gradient of the opened image blurred with sigma 1.5, gy[y] = smf[y+2] - smf[y-2] (0 in the two rows
    /// at each image edge), computed inside `rect` only.
    fn grad_image(&mut self, rect: Rect) {
        let sm = Rect { x0: rect.x0, y0: rect.y0.saturating_sub(2), x1: rect.x1, y1: (rect.y1 + 2).min(H) };
        vision::gauss_u8(&self.op, W, H, &self.k15, sm, &mut self.gauss, &mut self.smf);
        for y in rect.y0..rect.y1 {
            for x in rect.x0..rect.x1 {
                self.gy[y * W + x] =
                    if (2..H - 2).contains(&y) { self.smf[(y + 2) * W + x] as i16 - self.smf[(y - 2) * W + x] as i16 } else { 0 };
            }
        }
    }

    /// Upper lid skin line (bright skin -> dark going down), its margin offset, and the lower lid (dark -> bright),
    /// over the iris columns with the pupil masked (feat2.lids, base variant).
    fn lids(&mut self, p: &Pupil, r: f64, xmax: f64) -> (Option<Lid>, Option<Lid>, Option<f64>) {
        let (cx, cy) = (p.cx, p.cy);
        let xa = pint((cx - 1.1 * r).max(self.search.x_min)).max(0) as usize;
        let xb = pint((xmax - 3.0).min(cx + 1.45 * r)).clamp(0, W as isize) as usize;
        if xb < xa + 20 {
            return (None, None, None);
        }
        // Everything below reads gy over these columns, from the top of the upper search band to the bottom of the
        // lower one (the margin search stays within those rows: it starts at the skin line, inside the upper band,
        // and looks at most 17 rows further).
        let top = pint((cy - 2.1 * r).max(4.0)).clamp(0, H as isize) as usize;
        let bottom = (pint((cy + 1.9 * r).min(396.0)).max(pint((cy + 0.45 * r).min(396.0)) + 17)).clamp(0, H as isize) as usize;
        if bottom > top {
            self.grad_image(Rect { x0: xa, y0: top, x1: xb, y1: bottom });
        }
        let gy_ok = (top, bottom);
        let xs: Vec<f64> = (xa..xb).map(|x| x as f64).collect();
        let nx = xb - xa;
        let mut out = [None, None];
        for (which, slot) in out.iter_mut().enumerate() {
            let (ya, yb) = if which == 0 {
                (pint((cy - 2.1 * r).max(4.0)), pint((cy + 0.45 * r).min(396.0)))
            } else {
                (pint(cy + 0.25 * r), pint((cy + 1.9 * r).min(396.0)))
            };
            let (ya, yb) = (ya.clamp(0, H as isize) as usize, yb.clamp(0, H as isize) as usize);
            if yb < ya + 6 {
                continue;
            }
            let ny = yb - ya;
            if ya < gy_ok.0 || yb > gy_ok.1 {
                continue;
            }
            let inside = p.ell.inside_test(8.0);
            let mut e = vec![0u8; ny * nx];
            let mut hist = [0u32; 256];
            for y in ya..yb {
                for x in xa..xb {
                    let g = self.gy[y * W + x] as i32;
                    let v = if which == 0 { -g } else { g }.clamp(0, 255) as u8;
                    let v = if inside(x as f64, y as f64) { 0 } else { v };
                    e[(y - ya) * nx + (x - xa)] = v;
                    hist[v as usize] += 1;
                }
            }
            let nrm = vision::percentile_hist(&hist, 99.0) + 1e-3;
            let path = dp_path(&e, ny, nx, nrm);
            let ys: Vec<f64> = path.iter().map(|&y| (y + ya) as f64).collect();
            let strength: Vec<f64> = path.iter().enumerate().map(|(j, &y)| e[y * nx + j] as f64).collect();
            let good: Vec<bool> = strength.iter().map(|&s| s > 0.2 * nrm).collect();
            let Some((coef, w)) = robust_parabola(&xs, &ys, good) else { continue };
            let mut sw: Vec<f64> = strength.iter().zip(&w).filter(|(_, k)| **k).map(|(s, _)| *s).collect();
            let frac = w.iter().filter(|&&k| k).count() as f64 / w.len() as f64;
            let grad = if sw.is_empty() { 0.0 } else { vision::median(&mut sw) };
            *slot = Some(Lid { coef, y_at: coef.at(cx), frac, grad });
        }
        let [upper, lower] = out;
        let margin = upper.and_then(|u| self.margin(p, &u, xa, xb, gy_ok));
        (upper, lower, margin)
    }

    /// The thin dark band below the skin line has a nearly constant width: measure where the eyeball step below it
    /// is clear (not over the pupil) and return the median offset (margin - skin line), if consistent.
    fn margin(&self, p: &Pupil, u: &Lid, xa: usize, xb: usize, gy_rows: (usize, usize)) -> Option<f64> {
        // OpenCV's RotatedRect from fitEllipse has width <= height: A = minor, B = major (full axes).
        let (ex, ey, big_a, big_b) = (p.ell.cx, p.ell.cy, 2.0 * p.b, 2.0 * p.a);
        let mut offs: Vec<f64> = Vec::new();
        for x in xa..xb {
            let ysk = u.coef.at(x as f64);
            let y0 = ysk.round_ties_even() as isize + 3;
            let y1 = (y0 + 14).min(395);
            if y1 <= y0 + 2 || y0 < gy_rows.0 as isize || y1 > gy_rows.1 as isize {
                continue;
            }
            if (x as f64 - ex).abs() < big_a / 2.0 + 4.0 && (y1 as f64) > ey - big_b / 2.0 - 3.0 {
                continue;
            }
            let (y0, y1) = (y0 as usize, y1 as usize);
            let mut k = y0;
            for y in y0..y1 {
                if self.gy[y * W + x] > self.gy[k * W + x] {
                    k = y;
                }
            }
            if self.gy[k * W + x] as f64 > (0.15 * u.grad).max(5.0) {
                offs.push(k as f64 - ysk);
            }
        }
        if offs.len() < 6 {
            return None;
        }
        let o = vision::median(&mut offs.clone());
        let mut inl: Vec<f64> = offs.into_iter().filter(|v| (v - o).abs() < 3.0).collect();
        if inl.len() < 6 {
            return None;
        }
        Some(vision::median(&mut inl))
    }

    /// Mean of the opened image in the box where the iris should be.
    fn box_mean(&self, cx: f64, cy: f64, r: f64, xmax: f64) -> f64 {
        let xa = pint((cx - 0.9 * r).max(self.search.x_min));
        let xb = pint((xmax - 3.0).min(cx + 0.9 * r));
        let ya = pint((cy - 0.45 * r).max(0.0));
        let yb = pint((cy + 0.45 * r).min(400.0));
        if xb - xa < 5 || yb - ya < 5 {
            return f64::NAN;
        }
        let (xa, xb) = (xa.max(0) as usize, xb.min(W as isize) as usize);
        let (ya, yb) = (ya.max(0) as usize, yb.min(H as isize) as usize);
        let mut sum = 0u64;
        for y in ya..yb {
            sum += self.op[y * W + xa..y * W + xb].iter().map(|&v| v as u64).sum::<u64>();
        }
        sum as f64 / ((xb - xa) * (yb - ya)) as f64
    }
}

/// fitEllipse with residual rejection (feat2.fit_ellipse_robust).
fn fit_ellipse_robust(mut pts: Vec<(f64, f64)>) -> Option<Ellipse> {
    if pts.len() < 6 {
        return None;
    }
    let mut e = vision::fit_ellipse(&pts)?;
    for _ in 0..2 {
        let r: Vec<f64> = pts.iter().map(|&(x, y)| e.resid(x, y)).collect();
        let mut ab: Vec<f64> = r.iter().map(|v| v.abs()).collect();
        let s = (1.4826 * vision::median(&mut ab)).max(0.7);
        let keep: Vec<bool> = r.iter().map(|v| v.abs() < 2.5 * s).collect();
        let nk = keep.iter().filter(|&&k| k).count();
        if nk < 6 || nk == keep.len() {
            break;
        }
        pts = pts.into_iter().zip(&keep).filter(|(_, k)| **k).map(|(p, _)| p).collect();
        e = vision::fit_ellipse(&pts)?;
    }
    Some(e)
}

/// Minimum-cost left-to-right path through cost = -min(e / nrm, 1), moving at most one row per column (each move
/// costs 0.15); ties go to staying, then up, then down, like the prototype's argmin.
fn dp_path(e: &[u8], ny: usize, nx: usize, nrm: f64) -> Vec<usize> {
    let cost = |y: usize, x: usize| -(e[y * nx + x] as f64 / nrm).min(1.0);
    let mut acc: Vec<f64> = (0..ny).map(|y| cost(y, 0)).collect();
    let mut next = vec![0.0; ny];
    let mut back = vec![0i8; ny * nx];
    for x in 1..nx {
        for y in 0..ny {
            let stay = acc[y];
            let up = if y >= 1 { acc[y - 1] + 0.15 } else { 1e9 };
            let dn = if y + 1 < ny { acc[y + 1] + 0.15 } else { 1e9 };
            let (mut v, mut off) = (stay, 0i8);
            if up < v {
                v = up;
                off = -1;
            }
            if dn < v {
                v = dn;
                off = 1;
            }
            next[y] = v + cost(y, x);
            back[y * nx + x] = off;
        }
        std::mem::swap(&mut acc, &mut next);
    }
    let mut ys = vec![0usize; nx];
    let mut y = 0;
    for (i, &v) in acc.iter().enumerate() {
        if v < acc[y] {
            y = i;
        }
    }
    ys[nx - 1] = y;
    for x in (1..nx).rev() {
        ys[x - 1] = (ys[x] as isize + back[ys[x] * nx + x] as isize) as usize;
    }
    ys
}

/// Quadratic fit with MAD rejection (feat2.robust_parabola): (coefficients, inliers).
fn robust_parabola(xs: &[f64], ys: &[f64], mut w: Vec<bool>) -> Option<(Quad, Vec<bool>)> {
    let count = |w: &[bool]| w.iter().filter(|&&k| k).count();
    for _ in 0..4 {
        if count(&w) < 8 {
            return None;
        }
        let c = Quad::fit(xs, ys, &w)?;
        let r: Vec<f64> = xs.iter().zip(ys).map(|(&x, &y)| y - c.at(x)).collect();
        let mut ab: Vec<f64> = r.iter().zip(&w).filter(|(_, k)| **k).map(|(v, _)| v.abs()).collect();
        let s = (1.4826 * vision::median(&mut ab)).max(1.0);
        let w2: Vec<bool> = w.iter().zip(&r).map(|(&k, v)| k && v.abs() < 2.5 * s).collect();
        if w2 == w {
            break;
        }
        w = w2;
    }
    if count(&w) < 8 {
        return None;
    }
    Some((Quad::fit(xs, ys, &w)?, w))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A synthetic eye: mid-grey skin, a darker iris disc, a black pupil, an upper lid edge, a lens edge band.
    fn synthetic_eye(px: f64, py: f64, pr: f64) -> Vec<u8> {
        let mut img = vec![0u8; W * H];
        for y in 0..H {
            for x in 0..W {
                let (dx, dy) = (x as f64 - px, y as f64 - py);
                let d = dx.hypot(dy);
                let mut v = if (y as f64) < py - 45.0 { 150.0 } else { 120.0 };
                if d < 55.0 && (y as f64) >= py - 45.0 {
                    v = 70.0;
                }
                if d < pr {
                    v = 12.0;
                }
                if x >= 350 {
                    v = 4.0;
                }
                img[y * W + x] = v as u8;
            }
        }
        img
    }

    #[test]
    fn finds_pupil_and_lids_on_a_synthetic_eye() {
        let img = synthetic_eye(250.0, 200.0, 25.0);
        let mut ex = Extractor::default();
        let xmax = occluder_x(&column_means(&img));
        assert_eq!(xmax, 347.0);
        let (f, p) = ex.extract(&img, xmax, None, None, None);
        let p = p.expect("pupil");
        assert!((p.cx - 250.0).abs() < 1.0 && (p.cy - 200.0).abs() < 1.0, "{p:?}");
        assert!((p.a - 25.0).abs() < 1.5 && (p.b - 25.0).abs() < 1.5, "{p:?}");
        assert!(f.ok_pupil);
        // The skin line is the bright -> darker step 45 px above the pupil centre.
        assert!((f.upper_skin_y - 155.0).abs() < 3.0, "{f:?}");
        // Without a pupil, lids are still measured at the last pupil's geometry.
        let blank = vec![120u8; W * H];
        let (f2, p2) = ex.extract(&blank, xmax, Some((250.0, 200.0)), Some(55.0), Some(&p));
        assert!(p2.is_none() && !f2.ok_pupil);
        assert!((f2.box_mean - 120.0).abs() < 1e-9);
    }

    /// Nasal shading: everything left of x = 186 as dark as a pupil, with a 40 x 70 bulge into the search window.
    fn shade(img: &mut [u8]) {
        for y in 0..H {
            for x in 0..W {
                if x < 186 || (x < 226 && (160..230).contains(&y)) {
                    img[y * W + x] = 14;
                }
            }
        }
    }

    #[test]
    fn a_pupil_cut_by_the_left_edge_is_found_live_but_not_by_the_prototype() {
        // The pupil spans x 175..225: the window's left edge (186) cuts it.
        let img = synthetic_eye(200.0, 200.0, 25.0);
        let mut ex = Extractor::default();
        let (f, p) = ex.extract(&img, 347.0, None, None, None);
        assert!(p.is_none() && !f.ok_pupil, "the prototype drops blobs touching the left edge");
        assert!(f.diag.rejected & REJ_LEFT != 0, "{:?}", f.diag);
        ex.search = Search::live(X_MIN);
        let (f, p) = ex.extract(&img, 347.0, None, None, None);
        let p = p.expect("pupil");
        assert!((p.cx - 200.0).abs() < 1.5 && (p.cy - 200.0).abs() < 1.5 && (p.a - 25.0).abs() < 1.5, "{p:?}");
        assert!(f.diag.left_edge && f.diag.candidates >= 1, "{:?}", f.diag);
        // With the prior somewhere else, it is still found.
        let (_, p) = ex.extract(&img, 347.0, Some((300.0, 120.0)), None, None);
        assert!(p.is_some_and(|p| (p.cx - 200.0).abs() < 1.5));
    }

    #[test]
    fn dark_shading_at_the_left_edge_is_not_a_pupil() {
        let mut img = vec![120u8; W * H];
        for v in img.iter_mut().skip(350).step_by(W) {
            *v = 4;
        }
        shade(&mut img);
        let mut ex = Extractor { search: Search::live(X_MIN), ..Extractor::default() };
        let (f, p) = ex.extract(&img, 347.0, None, None, None);
        assert!(p.is_none() && !f.ok_pupil, "{p:?}");
        assert!(f.diag.candidates >= 1, "the bulge is a candidate, refused for its missing left edge: {:?}", f.diag);
        // With a real pupil beside it, the pupil is found even though the bulge scores higher.
        let mut img = synthetic_eye(275.0, 195.0, 22.0);
        shade(&mut img);
        let (f, p) = ex.extract(&img, 347.0, None, None, None);
        let p = p.expect("pupil");
        assert!((p.cx - 275.0).abs() < 1.5 && (p.cy - 195.0).abs() < 1.5, "{p:?}");
        assert!(!f.diag.left_edge && f.diag.candidates >= 2, "{:?}", f.diag);
    }

    #[test]
    fn rays_left_of_the_computed_columns_do_not_count() {
        // Only x >= 200 belongs to this frame; left of it are an earlier frame's bright pixels, which would look
        // like the pupil's edge on the rays pointing left.
        let mut ex = Extractor { opf: vec![120; W * H], opf_x0: 200, ..Extractor::default() };
        for y in 0..H {
            for x in 0..W {
                if (x as f64 - 205.0).hypot(y as f64 - 200.0) < 20.0 {
                    ex.opf[y * W + x] = 12;
                }
            }
        }
        let (p, left_share) = ex.refine_pupil((205.0, 200.0), 20.0, 347.0).expect("the other rays still fit it");
        assert!((p.cy - 200.0).abs() < 1.5 && (p.a - 20.0).abs() < 2.0, "{p:?}");
        assert!(left_share.is_nan() || left_share < EDGE_LEFT_SHARE, "no left ray reached only this frame's columns: {left_share}");
        ex.opf_x0 = 0;
        assert!(ex.refine_pupil((205.0, 200.0), 20.0, 347.0).is_some_and(|x| x.1 >= EDGE_LEFT_SHARE));
    }

    #[test]
    fn the_search_window_follows_the_nasal_shading() {
        let profile = |end: usize| (0..W).map(|x| if x < end { 8.0 } else { 80.0 }).collect::<Vec<f64>>();
        assert_eq!(search_x_min(&profile(198)), X_MIN, "the developer's recordings: the prototype's edge");
        assert_eq!(search_x_min(&profile(150)), 140.0);
        assert_eq!(search_x_min(&profile(40)), X_MIN_LO);
        assert_eq!(search_x_min(&vec![10.0; W]), X_MIN, "no bright columns: the prototype's edge");
        assert_eq!(search_x_min(&profile(230)), X_MIN, "shading past the prototype's edge: it stays");
        // A lit nose bridge at x 60..100 with shading between it and the iris: the shading's end counts.
        let mut nose = profile(150);
        nose[60..100].fill(90.0);
        assert_eq!(search_x_min(&nose), 140.0);
        assert_eq!(Search::live(20.0).x_min, X_MIN_LO);
        // A pupil left of the prototype's window is found with the window moved left.
        let mut img = synthetic_eye(170.0, 200.0, 24.0);
        for y in 0..H {
            for x in 0..100 {
                img[y * W + x] = 8;
            }
        }
        let mut ex = Extractor { search: Search::live(search_x_min(&column_means(&img))), ..Extractor::default() };
        assert!(ex.search.x_min < 120.0, "{:?}", ex.search);
        let (f, p) = ex.extract(&img, 347.0, None, None, None);
        assert!(p.is_some_and(|p| (p.cx - 170.0).abs() < 1.5), "{p:?}");
        assert!(f.upper_skin_y.is_finite() && f.box_mean.is_finite(), "{f:?}");
    }
}

/// Stage timings on real frames: `EYECAM_SESSION=dir cargo test --release -- --ignored bench_stages --nocapture`.
#[cfg(test)]
mod bench {
    use super::*;
    use std::time::Instant;

    #[test]
    #[ignore]
    fn bench_stages() {
        let s = crate::replay::Session::open(std::path::Path::new(&std::env::var("EYECAM_SESSION").unwrap()), false).unwrap();
        let mut img = vec![0u8; W * H];
        let mut ex = Extractor::default();
        let n = 300;
        let mut t = [0f64; 5];
        let mut prev = None;
        let mut prev_p: Option<Pupil> = None;
        for k in 0..n {
            s.read(0, 1000 + k, &mut img).unwrap();
            let a = Instant::now();
            vision::open9_cols(&img, W, H, OPEN_X0, W, &mut ex.morph, &mut ex.op);
            let b = Instant::now();
            vision::gauss_u8(&ex.op, W, H, &ex.k1, Rect { x0: OPF_X0, y0: 0, x1: W, y1: H }, &mut ex.gauss, &mut ex.opf);
            let c = Instant::now();
            ex.find_pupil_blob(346.0, prev, &mut Diag::default());
            let d = Instant::now();
            let p = ex.cands.first().copied().and_then(|b| ex.refine_pupil(b.c, b.r, 346.0));
            let e = Instant::now();
            let (_, p2) = ex.extract(&img, 346.0, prev, Some(54.0), prev_p.as_ref());
            let f = Instant::now();
            if let Some(p) = p2 {
                prev = Some((p.cx, p.cy));
                prev_p = Some(p);
            }
            let _ = p;
            for (i, (x, y)) in [(a, b), (b, c), (c, d), (d, e), (e, f)].iter().enumerate() {
                t[i] += (*y - *x).as_secs_f64() * 1e3 / n as f64;
            }
        }
        eprintln!("ms/frame: open {:.2} blur {:.2} blob {:.2} rays {:.2} | whole extract {:.2}", t[0], t[1], t[2], t[3], t[4]);
    }
}
