//! Per-eye, per-frame classical features: a port of tools/eyecam/analysis/feat2.py (the "base" variant the
//! cross-wear evaluation chose). Works on upright 400x400 frames (the right eye flipped vertically).
//!
//! Pipeline: grey opening 9x9 (removes glints) -> darkest compact blob = pupil -> 64 rays from the blob centre,
//! sub-pixel pupil->iris crossing, robust ellipse (diameter = 2a) -> iris radius from the nasal limbus -> upper lid
//! "skin line" (bright skin -> dark band, DP path along the vertical gradient, pupil masked) and its margin below,
//! lower lid (dark -> bright skin) -> box brightness where the iris should be (closed lids are bright skin).
//!
//! Numerical details follow the prototype (numpy's int() truncation, banker's rounding, percentile interpolation,
//! first-index argmin/argmax) so features agree with it; see the agreement report in the README.

use crate::vision::{self, CcBufs, Component, Ellipse, GaussBufs, H, MorphBufs, Quad, Rect, W};

const NRAY: usize = 64;
/// Median margin - skin line offset (px) when the margin itself is not found (feat2.MARGIN_OFF).
pub const MARGIN_OFF: f64 = 7.0;
/// Left edge of every search region (the temporal side of the image is skin/shadow).
const X_MIN: f64 = 186.0;
/// The opening is computed from this column on (exact from 8 columns further), the blurred image from OPF_X0. The
/// leftmost samples ever taken are temporal iris rays, 95 px left of a pupil that is right of X_MIN.
const OPEN_X0: usize = 72;
const OPF_X0: usize = 80;

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
}

impl Default for Features {
    fn default() -> Self {
        let n = f64::NAN;
        Self {
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

/// The lens-edge column: where the black nasal band starts (feat2.occluder_x), from column means over rows 60..340
/// of a frame or of a mean image.
pub fn occluder_x(col_mean: &[f64]) -> f64 {
    let first = col_mean[280..].iter().position(|&m| m < 25.0).unwrap_or(120);
    (280 + first) as f64 - 3.0
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
}

impl Default for Extractor {
    fn default() -> Self {
        Self {
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
        vision::open9_cols(img, W, H, OPEN_X0, W, &mut self.morph, &mut self.op);
        vision::gauss_u8(&self.op, W, H, &self.k1, Rect { x0: OPF_X0, y0: 0, x1: W, y1: H }, &mut self.gauss, &mut self.opf);
        let blob = self.find_pupil_blob(xmax, prev);
        let p = blob.and_then(|(c, r0)| self.refine_pupil(c, r0, xmax));
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

    /// Dark compact blob in the opened, blurred image: (centre, equivalent radius).
    fn find_pupil_blob(&mut self, xmax: f64, prev: Option<(f64, f64)>) -> Option<((f64, f64), f64)> {
        let (x0, y0, x1, y1) = (X_MIN as usize, 70usize, (pint(xmax).clamp(X_MIN as isize + 1, W as isize)) as usize, 320usize);
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
        // (score, centre, r_eq)
        let mut best: Option<(f64, (f64, f64), f64)> = None;
        for dth in [14.0, 22.0, 32.0] {
            self.mask.clear();
            self.mask.extend((0..sh).flat_map(|y| (0..sw).map(move |x| (y, x))).map(|(y, x)| (at(x, y) as f64) < lo + dth));
            vision::components(&self.mask, sw, sh, &mut self.cc, &mut self.comps);
            for c in &self.comps {
                let (w, h, a) = (c.w as f64, c.h as f64, c.area as f64);
                if c.area < 120 || c.area > 5500 || c.x == 0 || c.y == 0 || c.y + c.h >= sh {
                    continue;
                }
                if w > 2.6 * h || h > 2.6 * w {
                    continue;
                }
                let fill = a / (w * h);
                if fill < 0.5 {
                    continue;
                }
                let centre = (c.cx + x0 as f64, c.cy + y0 as f64);
                let d = prev.map_or(0.0, |(px, py)| (centre.0 - px).hypot(centre.1 - py));
                let sc = a * fill / (1.0 + (d - 20.0).max(0.0).powi(2) / 400.0);
                if best.is_none_or(|b| sc > b.0) {
                    best = Some((sc, centre, (a / std::f64::consts::PI).sqrt()));
                }
            }
            if best.is_some_and(|b| b.2 > 9.0) {
                break;
            }
        }
        best.map(|(_, c, r)| (c, r))
    }

    /// Rays from the blob centre; the sub-pixel crossing of the middle level between the pupil and the iris on
    /// each, rejecting rays that end past the lens edge or stay dark outside (lashes, lid, vignette); ellipse fit.
    fn refine_pupil(&self, c: (f64, f64), r0: f64, xmax: f64) -> Option<Pupil> {
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
        let mut ov = Vec::with_capacity(nr);
        for k in 0..NRAY {
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
        Some(Pupil { cx: e.cx, cy: e.cy, a, b, ell: e, inner, vis: ok as f64 / NRAY as f64 })
    }

    /// Iris radius from limbus crossings on rays to the nasal and temporal sides (top and bottom excluded), mapped
    /// into the pupil ellipse's frame; nasal crossings (dark iris -> bright sclera) are preferred. (R, inliers).
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
            let nasal = d.0 > 0.0;
            let gg: Vec<f64> = (0..n - 3)
                .map(|i| {
                    let g = sm[i + 3] - sm[i];
                    if nasal || g > 0.0 { g } else { -0.8 * g }
                })
                .collect();
            let mut k = 2;
            for i in 2..gg.len() - 2 {
                if gg[i] > gg[k] {
                    k = i;
                }
            }
            if gg[k] < if nasal { 12.0 } else { 9.0 } {
                continue;
            }
            let r = rok[k + 1] + 0.375;
            let v = (d.0 * r, d.1 * r);
            let (ra, rb) = (v.0 * ua.0 + v.1 * ua.1, v.0 * ub.0 + v.1 * ub.1);
            res.push((ra.hypot(rb / ratio), nasal));
        }
        if res.len() < 4 {
            return (f64::NAN, 0);
        }
        let nasal_n = res.iter().filter(|r| r.1).count();
        let mut rho: Vec<f64> = res.iter().filter(|r| nasal_n < 4 || r.1).map(|r| r.0).collect();
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
        let xa = pint((cx - 1.1 * r).max(X_MIN)).max(0) as usize;
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
        let xa = pint((cx - 0.9 * r).max(X_MIN));
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
            let blob = ex.find_pupil_blob(346.0, prev);
            let d = Instant::now();
            let p = blob.and_then(|(c, r0)| ex.refine_pupil(c, r0, 346.0));
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
