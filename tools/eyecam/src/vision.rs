//! Image primitives for the eye-feature extractor (`feat`). The Python prototype (tools/eyecam/analysis/feat2.py)
//! uses OpenCV and numpy; these reproduce the operations it relies on closely enough that the features agree to
//! well within a pixel:
//!   - grey opening with OpenCV's 9x9 elliptical element, pixels outside the image ignored (OpenCV's default);
//!   - GaussianBlur on 8-bit images, bit-exact with OpenCV 4's fixed-point path (kernel in 8.8 fixed point,
//!     reflect-101 borders, rounding to 8 bits);
//!   - remap-style bilinear sampling (float weights, border replicate);
//!   - 4-connected components with stats, numpy-style percentiles and medians, OpenCV's algebraic ellipse fit,
//!     quadratic least squares.
//!
//! No allocation in the per-pixel loops; the caller keeps the buffers.

pub const W: usize = 400;
pub const H: usize = 400;

/// The eye (0 = left, 1 = right) whose camera stores its picture upside down: its frames are flipped vertically
/// (not rotated 180 degrees) to be upright, everywhere in the analysis. It is the camera of the higher-address slot
/// group (see `ring`), which is the left eye's; builds before 2026-10-07 called that camera R ("the right eye is
/// stored upside down").
pub const UPSIDE_DOWN_EYE: usize = 0;

/// Whether `eye`'s frames are stored upside down (see UPSIDE_DOWN_EYE).
pub fn stored_upside_down(eye: usize) -> bool {
    eye == UPSIDE_DOWN_EYE
}

/// Half widths of the rows of `cv2.getStructuringElement(MORPH_ELLIPSE, (9, 9))`.
const SE_HALF: [usize; 9] = [0, 3, 3, 4, 4, 4, 3, 3, 0];

/// Running min (or max) over `[x - hw, x + hw]` of one row, clipped to the row.
fn hextreme(row: &[u8], hw: usize, out: &mut [u8], max: bool) {
    let n = row.len();
    let pick = |a: u8, b: u8| if max { a.max(b) } else { a.min(b) };
    if hw == 0 {
        out.copy_from_slice(row);
        return;
    }
    // Interior: elementwise over shifted slices, which the compiler vectorises.
    if n > 2 * hw {
        let m = n - 2 * hw;
        let mid = &mut out[hw..hw + m];
        mid.copy_from_slice(&row[..m]);
        for d in 1..=2 * hw {
            for (o, &v) in mid.iter_mut().zip(&row[d..d + m]) {
                *o = pick(*o, v);
            }
        }
    }
    for x in (0..hw.min(n)).chain(n.saturating_sub(hw).max(hw)..n) {
        let (a, b) = (x.saturating_sub(hw), (x + hw + 1).min(n));
        out[x] = row[a..b].iter().copied().reduce(pick).unwrap();
    }
}

/// One pass (erode or dilate) of the elliptical 9x9 element over a `w` x `h` image.
fn morph_pass(src: &[u8], w: usize, h: usize, max: bool, rows: &mut [Vec<u8>; 3], out: &mut [u8]) {
    // Horizontal extremes for the three distinct half widths (0, 3, 4).
    for (k, hw) in [0usize, 3, 4].iter().enumerate() {
        rows[k].resize(w * h, 0);
        for y in 0..h {
            hextreme(&src[y * w..(y + 1) * w], *hw, &mut rows[k][y * w..(y + 1) * w], max);
        }
    }
    let which = |hw: usize| match hw {
        0 => 0,
        3 => 1,
        _ => 2,
    };
    for y in 0..h {
        let o = &mut out[y * w..(y + 1) * w];
        let mut first = true;
        for (i, &hw) in SE_HALF.iter().enumerate() {
            let yy = y as isize + i as isize - 4;
            if yy < 0 || yy >= h as isize {
                continue;
            }
            let r = &rows[which(hw)][yy as usize * w..(yy as usize + 1) * w];
            if first {
                o.copy_from_slice(r);
                first = false;
            } else if max {
                o.iter_mut().zip(r).for_each(|(a, &b)| *a = (*a).max(b));
            } else {
                o.iter_mut().zip(r).for_each(|(a, &b)| *a = (*a).min(b));
            }
        }
    }
}

/// Scratch buffers for `open9`.
#[derive(Default)]
pub struct MorphBufs {
    rows: [Vec<u8>; 3],
    tmp: Vec<u8>,
    strip: Vec<u8>,
    res: Vec<u8>,
}

/// `cv2.morphologyEx(src, MORPH_OPEN, ellipse 9x9)`: removes bright details (glints) smaller than the element.
pub fn open9(src: &[u8], w: usize, h: usize, bufs: &mut MorphBufs, out: &mut Vec<u8>) {
    bufs.tmp.resize(w * h, 0);
    out.resize(w * h, 0);
    let mut tmp = std::mem::take(&mut bufs.tmp);
    morph_pass(src, w, h, false, &mut bufs.rows, &mut tmp);
    morph_pass(&tmp, w, h, true, &mut bufs.rows, out);
    bufs.tmp = tmp;
}

/// `open9` over the column strip [c0, c1) only, treated as an image of its own (so the result equals the
/// full-image opening from 8 columns inside the strip on); `out` is full size, other columns untouched.
pub fn open9_cols(src: &[u8], w: usize, h: usize, c0: usize, c1: usize, bufs: &mut MorphBufs, out: &mut Vec<u8>) {
    let sw = c1 - c0;
    let mut strip = std::mem::take(&mut bufs.strip);
    strip.resize(sw * h, 0);
    for y in 0..h {
        strip[y * sw..(y + 1) * sw].copy_from_slice(&src[y * w + c0..y * w + c1]);
    }
    let mut res = std::mem::take(&mut bufs.res);
    open9(&strip, sw, h, bufs, &mut res);
    out.resize(w * h, 0);
    for y in 0..h {
        out[y * w + c0..y * w + c1].copy_from_slice(&res[y * sw..(y + 1) * sw]);
    }
    bufs.strip = strip;
    bufs.res = res;
}

/// OpenCV's bit-exact Gaussian kernel for 8-bit images (`getGaussianKernelBitExact`): 8.8 fixed point, summing to
/// exactly 1.0 (256) by adjusting the centre tap.
pub fn gauss_kernel_fixed(sigma: f64) -> Vec<u32> {
    // GaussianBlur with ksize (0, 0) on 8-bit: ksize = cvRound(sigma * 3 * 2 + 1) | 1.
    let n = (((sigma * 6.0 + 1.0).round_ties_even() as usize) | 1).max(1);
    let n2 = (n - 1) / 2;
    let scale2 = -0.125 / (sigma * sigma);
    let values: Vec<f64> = (0..n2)
        .map(|i| {
            let x = 1.0 - n as f64 + 2.0 * i as f64;
            (x * x * scale2).exp()
        })
        .collect();
    let sum = 2.0 * values.iter().sum::<f64>() + 1.0;
    let mul = 1.0 / sum;
    let fixed = |v: f64| (v * 256.0).round_ties_even() as u32;
    let mut k = vec![0u32; n];
    k[n2] = fixed(mul);
    for i in 0..n2 {
        k[i] = fixed(values[i] * mul);
        k[n - 1 - i] = k[i];
    }
    let total: u32 = k.iter().sum();
    k[n2] = (k[n2] + 256).wrapping_sub(total);
    k
}

fn reflect101(i: isize, n: usize) -> usize {
    let n = n as isize;
    let mut i = i;
    if i < 0 {
        i = -i;
    }
    if i >= n {
        i = 2 * n - 2 - i;
    }
    i.clamp(0, n - 1) as usize
}

/// Scratch for `gauss_u8`.
#[derive(Default)]
pub struct GaussBufs {
    h: Vec<u16>,
    acc: Vec<u32>,
}

/// A rectangle [x0, x1) x [y0, y1) of the image.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Rect {
    pub x0: usize,
    pub y0: usize,
    pub x1: usize,
    pub y1: usize,
}

impl Rect {
    pub const fn full(w: usize, h: usize) -> Self {
        Rect { x0: 0, y0: 0, x1: w, y1: h }
    }
}

/// `cv2.GaussianBlur(src, (0, 0), sigma)` on an 8-bit image, bit-exact with OpenCV's fixed-point implementation,
/// computed only inside `rect` (`out` is the full image size; pixels outside `rect` are left as they were). Source
/// pixels are read around the rectangle as needed, reflect-101 at the image borders.
pub fn gauss_u8(src: &[u8], w: usize, h: usize, kernel: &[u32], rect: Rect, bufs: &mut GaussBufs, out: &mut Vec<u8>) {
    let n = kernel.len();
    let r = n / 2;
    out.resize(w * h, 0);
    let Rect { x0, y0, x1, y1 } = rect;
    if x1 <= x0 || y1 <= y0 {
        return;
    }
    let rw = x1 - x0;
    // Rows the vertical pass needs (before reflection), horizontally filtered: 8-bit x 8.8 -> 8.8 in 16 bits
    // (the kernel sums to 256, so 255 * 256 fits).
    let (hy0, hy1) = (y0 as isize - r as isize, y1 as isize + r as isize);
    let rows: Vec<usize> = (hy0..hy1).map(|y| reflect101(y, h)).collect();
    bufs.h.resize(rows.len() * rw, 0);
    let k16: Vec<u16> = kernel.iter().map(|&k| k as u16).collect();
    for (ri, &y) in rows.iter().enumerate() {
        let row = &src[y * w..(y + 1) * w];
        let o = &mut bufs.h[ri * rw..(ri + 1) * rw];
        // Columns whose whole window lies inside the image: shifted-slice accumulation (vectorises).
        let (ia, ib) = (x0.max(r), x1.min(w - r));
        if ib > ia {
            let m = ib - ia;
            let oi = &mut o[ia - x0..ib - x0];
            oi.fill(0);
            for (k, &kv) in k16.iter().enumerate() {
                for (a, &v) in oi.iter_mut().zip(&row[ia - r + k..ia - r + k + m]) {
                    *a += kv * v as u16;
                }
            }
        }
        for x in (x0..x1).filter(|&x| x < ia || x >= ib.max(ia)) {
            let mut acc = 0u16;
            for (k, &kv) in k16.iter().enumerate() {
                acc += kv * row[reflect101(x as isize + k as isize - r as isize, w)] as u16;
            }
            o[x - x0] = acc;
        }
    }
    // Vertical: 8.8 x 8.8 -> 16.16, rounded to 8 bits.
    bufs.acc.resize(rw, 0);
    for y in y0..y1 {
        let acc = &mut bufs.acc[..rw];
        acc.fill(0);
        for (k, &kv) in kernel.iter().enumerate() {
            let ri = y - y0 + k;
            for (a, &v) in acc.iter_mut().zip(&bufs.h[ri * rw..(ri + 1) * rw]) {
                *a += kv * v as u32;
            }
        }
        for (o, &a) in out[y * w + x0..y * w + x1].iter_mut().zip(acc.iter()) {
            *o = ((a + (1 << 15)) >> 16).min(255) as u8;
        }
    }
}

/// Bilinear sample of an 8-bit image at (x, y), as `cv2.remap(img_as_float32, ..., INTER_LINEAR,
/// BORDER_REPLICATE)` does it with float maps (OpenCV 5: exact float weights, coordinates outside clamped).
#[inline]
pub fn sample(img: &[u8], w: usize, h: usize, x: f32, y: f32) -> f32 {
    let (fx0, fy0) = (x.floor(), y.floor());
    let (ix, iy) = (fx0 as i32, fy0 as i32);
    let (fx, fy) = (x - fx0, y - fy0);
    let cx = |v: i32| v.clamp(0, w as i32 - 1) as usize;
    let cy = |v: i32| v.clamp(0, h as i32 - 1) as usize;
    let p = |xx: i32, yy: i32| img[cy(yy) * w + cx(xx)] as f32;
    let top = p(ix, iy) + fx * (p(ix + 1, iy) - p(ix, iy));
    let bot = p(ix, iy + 1) + fx * (p(ix + 1, iy + 1) - p(ix, iy + 1));
    top + fy * (bot - top)
}

/// numpy.percentile (linear interpolation) of integer-valued data given as a 256-bin histogram.
pub fn percentile_hist(hist: &[u32; 256], q: f64) -> f64 {
    let n: u64 = hist.iter().map(|&c| c as u64).sum();
    if n == 0 {
        return f64::NAN;
    }
    let rank = q / 100.0 * (n - 1) as f64;
    let lo = rank.floor() as u64;
    let frac = rank - lo as f64;
    let nth = |k: u64| -> f64 {
        let mut seen = 0u64;
        for (v, &c) in hist.iter().enumerate() {
            seen += c as u64;
            if seen > k {
                return v as f64;
            }
        }
        255.0
    };
    let a = nth(lo);
    if frac == 0.0 { a } else { a + frac * (nth((lo + 1).min(n - 1)) - a) }
}

/// numpy.median of finite values (NaN if none). Reorders `v`.
pub fn median(v: &mut [f64]) -> f64 {
    let n = v.len();
    if n == 0 {
        return f64::NAN;
    }
    v.sort_unstable_by(|a, b| a.total_cmp(b));
    if n % 2 == 1 { v[n / 2] } else { 0.5 * (v[n / 2 - 1] + v[n / 2]) }
}

/// numpy.percentile (linear) of finite values. Reorders `v`.
pub fn percentile(v: &mut [f64], q: f64) -> f64 {
    let n = v.len();
    if n == 0 {
        return f64::NAN;
    }
    v.sort_unstable_by(|a, b| a.total_cmp(b));
    let rank = q / 100.0 * (n - 1) as f64;
    let lo = rank.floor() as usize;
    let hi = (lo + 1).min(n - 1);
    v[lo] + (rank - lo as f64) * (v[hi] - v[lo])
}

/// One 4-connected component of a mask: bounding box, area and centroid (in mask coordinates).
#[derive(Clone, Copy, Debug)]
pub struct Component {
    pub x: usize,
    pub y: usize,
    pub w: usize,
    pub h: usize,
    pub area: usize,
    pub cx: f64,
    pub cy: f64,
}

/// Scratch for `components`.
#[derive(Default)]
pub struct CcBufs {
    label: Vec<u32>,
    parent: Vec<u32>,
}

fn find(parent: &mut [u32], mut a: u32) -> u32 {
    while parent[a as usize] != a {
        parent[a as usize] = parent[parent[a as usize] as usize];
        a = parent[a as usize];
    }
    a
}

/// 4-connected components of `mask` (`w` x `h`, nonzero = set), in raster order of their first pixel, like
/// `cv2.connectedComponentsWithStats(mask, connectivity=4)` without the background entry.
pub fn components(mask: &[bool], w: usize, h: usize, bufs: &mut CcBufs, out: &mut Vec<Component>) {
    out.clear();
    bufs.label.clear();
    bufs.label.resize(w * h, 0);
    bufs.parent.clear();
    bufs.parent.push(0);
    for y in 0..h {
        for x in 0..w {
            let i = y * w + x;
            if !mask[i] {
                continue;
            }
            let left = if x > 0 { bufs.label[i - 1] } else { 0 };
            let up = if y > 0 { bufs.label[i - w] } else { 0 };
            let l = match (left, up) {
                (0, 0) => {
                    let l = bufs.parent.len() as u32;
                    bufs.parent.push(l);
                    l
                }
                (a, 0) | (0, a) => a,
                (a, b) => {
                    let (ra, rb) = (find(&mut bufs.parent, a), find(&mut bufs.parent, b));
                    let (lo, hi) = (ra.min(rb), ra.max(rb));
                    bufs.parent[hi as usize] = lo;
                    lo
                }
            };
            bufs.label[i] = l;
        }
    }
    // Final labels: roots in order of first appearance (the smallest provisional label in each set is its root,
    // and provisional labels are assigned in raster order).
    let mut slot = vec![u32::MAX; bufs.parent.len()];
    let mut acc: Vec<(usize, usize, usize, usize, usize, f64, f64)> = Vec::new();
    for y in 0..h {
        for x in 0..w {
            let i = y * w + x;
            if bufs.label[i] == 0 {
                continue;
            }
            let root = find(&mut bufs.parent, bufs.label[i]) as usize;
            if slot[root] == u32::MAX {
                slot[root] = acc.len() as u32;
                acc.push((x, y, x, y, 0, 0.0, 0.0));
            }
            let a = &mut acc[slot[root] as usize];
            a.0 = a.0.min(x);
            a.1 = a.1.min(y);
            a.2 = a.2.max(x);
            a.3 = a.3.max(y);
            a.4 += 1;
            a.5 += x as f64;
            a.6 += y as f64;
        }
    }
    out.extend(acc.into_iter().map(|(x0, y0, x1, y1, n, sx, sy)| Component {
        x: x0,
        y: y0,
        w: x1 - x0 + 1,
        h: y1 - y0 + 1,
        area: n,
        cx: sx / n as f64,
        cy: sy / n as f64,
    }));
}

/// An ellipse as OpenCV's RotatedRect: centre, full axes (`a` along `angle_deg`, `b` across it).
#[derive(Clone, Copy, Debug)]
pub struct Ellipse {
    pub cx: f64,
    pub cy: f64,
    pub a: f64,
    pub b: f64,
    pub angle_deg: f64,
}

/// Solve the small dense system `m x = v` (Gaussian elimination, partial pivoting).
#[allow(clippy::needless_range_loop)]
fn solve<const N: usize>(mut m: [[f64; N]; N], mut v: [f64; N]) -> Option<[f64; N]> {
    for c in 0..N {
        let p = (c..N).max_by(|&a, &b| m[a][c].abs().total_cmp(&m[b][c].abs()))?;
        if m[p][c].abs() < 1e-300 {
            return None;
        }
        m.swap(c, p);
        v.swap(c, p);
        for r in c + 1..N {
            let f = m[r][c] / m[c][c];
            for k in c..N {
                m[r][k] -= f * m[c][k];
            }
            v[r] -= f * v[c];
        }
    }
    let mut x = [0.0; N];
    for c in (0..N).rev() {
        let s: f64 = (c + 1..N).map(|k| m[c][k] * x[k]).sum();
        x[c] = (v[c] - s) / m[c][c];
    }
    Some(x)
}

/// Least squares `rows * x ~ rhs` through the normal equations.
fn lstsq<const N: usize>(rows: impl Iterator<Item = ([f64; N], f64)>) -> Option<[f64; N]> {
    let mut ata = [[0.0; N]; N];
    let mut atb = [0.0; N];
    for (r, b) in rows {
        for i in 0..N {
            for j in 0..N {
                ata[i][j] += r[i] * r[j];
            }
            atb[i] += r[i] * b;
        }
    }
    solve(ata, atb)
}

/// `cv2.fitEllipse` (the algebraic fit OpenCV uses for more than five points): a general conic around the
/// centroid gives the centre, then a centred conic gives the axes. Points as (x, y) in f32, like OpenCV.
pub fn fit_ellipse(pts: &[(f64, f64)]) -> Option<Ellipse> {
    if pts.len() < 5 {
        return None;
    }
    let pts: Vec<(f64, f64)> = pts.iter().map(|&(x, y)| (x as f32 as f64, y as f32 as f64)).collect();
    let n = pts.len() as f64;
    let (mx, my) = pts.iter().fold((0.0, 0.0), |a, p| (a.0 + p.0, a.1 + p.1));
    let (mx, my) = ((mx / n) as f32 as f64, (my / n) as f32 as f64);
    let g = lstsq::<5>(pts.iter().map(|&(x, y)| {
        let (px, py) = (x - mx, y - my);
        ([-px * px, -py * py, -px * py, px, py], 10000.0)
    }))?;
    let c = solve([[2.0 * g[0], g[2]], [g[2], 2.0 * g[1]]], [g[3], g[4]])?;
    let q = lstsq::<3>(pts.iter().map(|&(x, y)| {
        let (px, py) = (x - mx - c[0], y - my - c[1]);
        ([px * px, py * py, px * py], 1.0)
    }))?;
    // q0 x^2 + q1 y^2 + q2 xy = 1: eigen-decompose [[q0, q2/2], [q2/2, q1]].
    let (a, b, h) = (q[0], q[1], q[2] / 2.0);
    let tr = 0.5 * (a + b);
    let det = ((a - b) * 0.5).hypot(h);
    let (l1, l2) = (tr - det, tr + det);
    if l1 <= 0.0 || l2 <= 0.0 {
        return None;
    }
    // Eigenvector of l1 (the smaller eigenvalue = the longer axis).
    let ang = if h.abs() > 1e-15 { (l1 - a).atan2(h) } else if a <= b { 0.0 } else { std::f64::consts::FRAC_PI_2 };
    Some(Ellipse {
        cx: mx + c[0],
        cy: my + c[1],
        a: 2.0 / l1.sqrt(),
        b: 2.0 / l2.sqrt(),
        angle_deg: ang.to_degrees(),
    })
}

impl Ellipse {
    /// Approximate radial residual (px) of a point, as feat2.ellipse_resid.
    pub fn resid(&self, x: f64, y: f64) -> f64 {
        let t = self.angle_deg.to_radians();
        let (dx, dy) = (x - self.cx, y - self.cy);
        let u = dx * t.cos() + dy * t.sin();
        let v = -dx * t.sin() + dy * t.cos();
        let rr = (u / (self.a / 2.0)).hypot(v / (self.b / 2.0));
        (rr - 1.0) * u.hypot(v) / rr.max(1e-6)
    }

    /// A fast inside test for the ellipse grown to full axes (a + grow, b + grow).
    pub fn inside_test(&self, grow: f64) -> impl Fn(f64, f64) -> bool + use<> {
        let t = self.angle_deg.to_radians();
        let (c, s) = (t.cos(), t.sin());
        let (ia, ib) = (2.0 / (self.a + grow), 2.0 / (self.b + grow));
        let (cx, cy) = (self.cx, self.cy);
        move |x, y| {
            let (dx, dy) = (x - cx, y - cy);
            let (u, v) = ((dx * c + dy * s) * ia, (-dx * s + dy * c) * ib);
            u * u + v * v <= 1.0
        }
    }

    /// Whether a point is inside the ellipse grown to full axes (a + grow, b + grow).
    pub fn contains_grown(&self, x: f64, y: f64, grow: f64) -> bool {
        let t = self.angle_deg.to_radians();
        let (dx, dy) = (x - self.cx, y - self.cy);
        let u = dx * t.cos() + dy * t.sin();
        let v = -dx * t.sin() + dy * t.cos();
        let (ra, rb) = ((self.a + grow) / 2.0, (self.b + grow) / 2.0);
        (u / ra).powi(2) + (v / rb).powi(2) <= 1.0
    }
}

/// A quadratic `c2 t^2 + c1 t + c0` with `t = x - xm` (centred for conditioning), as from `np.polyfit(x, y, 2)`.
#[derive(Clone, Copy, Debug)]
pub struct Quad {
    pub c: [f64; 3],
    pub xm: f64,
}

impl Quad {
    pub fn at(&self, x: f64) -> f64 {
        let t = x - self.xm;
        (self.c[0] * t + self.c[1]) * t + self.c[2]
    }

    /// Least-squares quadratic through the points with `keep[i]`.
    pub fn fit(xs: &[f64], ys: &[f64], keep: &[bool]) -> Option<Quad> {
        let n = keep.iter().filter(|&&k| k).count();
        if n < 3 {
            return None;
        }
        let xm = xs.iter().zip(keep).filter(|(_, k)| **k).map(|(x, _)| x).sum::<f64>() / n as f64;
        let c = lstsq::<3>(xs.iter().zip(ys).zip(keep).filter(|(_, k)| **k).map(|((&x, &y), _)| {
            let t = x - xm;
            ([t * t, t, 1.0], y)
        }))?;
        Some(Quad { c, xm })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn gaussian_kernels_match_opencv() {
        // cv2.getGaussianKernel(7, 1.0) * 256 rounded, centre adjusted: [1, 14, 62, 102, 62, 14, 1].
        assert_eq!(gauss_kernel_fixed(1.0), vec![1, 14, 62, 102, 62, 14, 1]);
        let k = gauss_kernel_fixed(1.5);
        assert_eq!(k.len(), 11);
        assert_eq!(k.iter().sum::<u32>(), 256);
    }

    /// Reference values from OpenCV 5.0 (cv2) on the same synthetic image: (arange(160000) % 251) as 400x400 u8.
    #[test]
    fn matches_opencv_on_a_test_image() {
        let img: Vec<u8> = (0..W * H).map(|i| (i % 251) as u8).collect();
        let mut out = Vec::new();
        let mut gb = GaussBufs::default();
        gauss_u8(&img, W, H, &gauss_kernel_fixed(1.0), Rect::full(W, H), &mut gb, &mut out);
        assert_eq!(out.iter().map(|&v| v as u64).sum::<u64>(), 19996729);
        assert_eq!(&out[..5], &[80, 80, 81, 82, 83]);
        assert_eq!(&out[200 * W + 100..200 * W + 105], &[106, 107, 108, 109, 110]);
        gauss_u8(&img, W, H, &gauss_kernel_fixed(1.5), Rect::full(W, H), &mut gb, &mut out);
        // A sub-rectangle gives the same pixels as the full image.
        let mut part = vec![0u8; W * H];
        gauss_u8(&img, W, H, &gauss_kernel_fixed(1.5), Rect { x0: 3, y0: 190, x1: 397, y1: 210 }, &mut gb, &mut part);
        assert!((190..210).all(|y| part[y * W + 3..y * W + 397] == out[y * W + 3..y * W + 397]));
        assert_eq!(out.iter().map(|&v| v as u64).sum::<u64>(), 19997305);
        assert_eq!(&out[..5], &[91, 91, 92, 93, 94]);
        assert_eq!(&out[200 * W + 100..200 * W + 105], &[123, 124, 125, 126, 127]);
        open9(&img, W, H, &mut MorphBufs::default(), &mut out);
        assert_eq!(out.iter().map(|&v| v as u64).sum::<u64>(), 4222820);
        assert_eq!(&out[..5], &[0, 1, 2, 3, 4]);
        assert_eq!(&out[399 * W + 395..], &[18, 18, 18, 18, 18]);
        let s = [sample(&img, W, H, 10.3, 5.51), sample(&img, W, H, 399.7, 7.0), sample(&img, W, H, -2.2, 3.3)];
        assert!((s[0] - 78.29004).abs() < 1e-3 && (s[1] - 187.0).abs() < 1e-3 && (s[2] - 165.4).abs() < 1e-3, "{s:?}");
    }

    #[test]
    fn opening_removes_small_bright_spots() {
        let (w, h) = (40, 30);
        let mut img = vec![50u8; w * h];
        img[15 * w + 20] = 250; // a 1-pixel glint
        for y in 5..20 {
            for x in 5..20 {
                img[y * w + x] = 200; // a 15x15 bright square survives
            }
        }
        let mut out = Vec::new();
        open9(&img, w, h, &mut MorphBufs::default(), &mut out);
        assert_eq!(out[15 * w + 20], 50);
        assert_eq!(out[12 * w + 12], 200);
    }

    #[test]
    fn fits_a_rotated_ellipse() {
        let (cx, cy, a, b, t) = (200.0f64, 150.0f64, 30.0f64, 18.0f64, 0.4f64);
        let pts: Vec<(f64, f64)> = (0..40)
            .map(|i| {
                let s = i as f64 / 40.0 * std::f64::consts::TAU;
                let (u, v) = (a * s.cos(), b * s.sin());
                (cx + u * t.cos() - v * t.sin(), cy + u * t.sin() + v * t.cos())
            })
            .collect();
        let e = fit_ellipse(&pts).unwrap();
        assert!((e.cx - cx).abs() < 1e-3 && (e.cy - cy).abs() < 1e-3, "{e:?}");
        assert!((e.a - 2.0 * a).abs() < 1e-2 && (e.b - 2.0 * b).abs() < 1e-2, "{e:?}");
        assert!(e.resid(cx + a * t.cos(), cy + a * t.sin()).abs() < 1e-3);
    }

    #[test]
    fn percentiles_match_numpy() {
        let mut hist = [0u32; 256];
        for v in [1u8, 2, 3, 4, 10] {
            hist[v as usize] += 1;
        }
        assert_eq!(percentile_hist(&hist, 50.0), 3.0);
        assert!((percentile_hist(&hist, 90.0) - 7.6).abs() < 1e-12); // np.percentile([1,2,3,4,10], 90)
        let mut v = vec![1.0, 2.0, 3.0, 4.0, 10.0];
        assert!((percentile(&mut v, 90.0) - 7.6).abs() < 1e-12);
    }

    #[test]
    fn components_are_four_connected() {
        // Two diagonal pixels are separate components under 4-connectivity.
        let mask = [true, false, false, true];
        let mut out = Vec::new();
        components(&mask, 2, 2, &mut CcBufs::default(), &mut out);
        assert_eq!(out.len(), 2);
        let mask = [true, true, false, true];
        components(&mask, 2, 2, &mut CcBufs::default(), &mut out);
        assert_eq!((out.len(), out[0].area), (1, 3));
    }
}
