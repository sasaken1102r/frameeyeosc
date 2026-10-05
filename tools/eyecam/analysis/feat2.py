"""Per-eye, per-frame classical feature extractor for the eyecam protocol recordings.

Works on upright frames (right eye V-flipped), no undistortion (1-2 px in the eye region).
Pipeline: glint-free grey opening -> pupil blob -> ray-refined pupil ellipse -> iris (limbus rays,
pupil-shaped ellipse, concentric) -> upper/lower lid margins (DP edge path + robust parabola) -> features.
"""
import numpy as np, cv2

K_OPEN = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (9, 9))
# lower-lid options: 'lower_open' = opening size used for the lower-lid gradient (removes reflections on the tear film /
# lid margin up to that size, i.e. a top-hat fill); 'lower_band' = +-px band around the previous frame's lower-lid curve
CFG = {'lower_open': 9, 'lower_band': None}
NRAY = 64
TH = np.linspace(0, 2 * np.pi, NRAY, endpoint=False)
COS, SIN = np.cos(TH), np.sin(TH)


def occluder_x(img):
    """x where the black lens-edge band on the temporal (large-x) side starts (from a frame or a mean image)."""
    cm = img[60:340].mean(0)
    xs = np.where(cm[280:] < 25)[0]
    return 280 + (int(xs[0]) if len(xs) else 120) - 3


def sample(img, xs, ys):
    """Bilinear sampling (float32 image)."""
    return cv2.remap(img, xs.astype(np.float32), ys.astype(np.float32), cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)


def find_pupil_blob(op, roi, prev):
    """Dark compact blob in the opened image. Returns (cx, cy, r_eq, level) or None."""
    x0, y0, x1, y1 = roi
    sub = op[y0:y1, x0:x1]
    if prev is not None:  # search near the previous pupil first
        px, py = prev
        wx0, wy0 = max(0, int(px - 70 - x0)), max(0, int(py - 70 - y0))
        win = sub[wy0:wy0 + 140, wx0:wx0 + 140]
        lo = float(np.percentile(win, 0.5))
    else:
        lo = float(np.percentile(sub, 0.5))
    best = None
    for dth in (14, 22, 32):
        m = (sub < lo + dth).astype(np.uint8)
        n, lab, st, cen = cv2.connectedComponentsWithStats(m, connectivity=4)
        for i in range(1, n):
            x, y, w, h, a = st[i]
            if a < 120 or a > 5500 or x == 0 or y == 0 or y + h >= sub.shape[0]:
                continue
            if w > 2.6 * h or h > 2.6 * w:
                continue
            fill = a / float(w * h)
            if fill < 0.5:
                continue
            c = (cen[i][0] + x0, cen[i][1] + y0)
            d = 0.0 if prev is None else np.hypot(c[0] - prev[0], c[1] - prev[1])
            sc = a * fill / (1 + max(0.0, d - 20) ** 2 / 400)
            if best is None or sc > best[0]:
                best = (sc, c, np.sqrt(a / np.pi), lo)
        if best is not None and best[2] > 9:
            break
    return None if best is None else best[1:]


def fit_ellipse_robust(pts):
    """fitEllipse with one pass of residual rejection. Returns ((cx,cy),(A,B),ang), mask."""
    if len(pts) < 6:
        return None, None
    e = cv2.fitEllipse(pts.astype(np.float32))
    for _ in range(2):
        r = ellipse_resid(e, pts)
        s = max(0.7, 1.4826 * np.median(np.abs(r)))
        keep = np.abs(r) < 2.5 * s
        if keep.sum() < 6 or keep.all():
            break
        e = cv2.fitEllipse(pts[keep].astype(np.float32))
        pts = pts[keep]
    keep = np.abs(ellipse_resid(e, pts)) < 3
    return e, keep


def ellipse_resid(e, pts):
    """Approximate radial residual (px) of points to ellipse e."""
    (cx, cy), (A, B), ang = e
    t = np.deg2rad(ang)
    dx, dy = pts[:, 0] - cx, pts[:, 1] - cy
    u = dx * np.cos(t) + dy * np.sin(t)
    v = -dx * np.sin(t) + dy * np.cos(t)
    rr = np.hypot(u / (A / 2), v / (B / 2))
    return (rr - 1) * np.hypot(u, v) / np.maximum(rr, 1e-6)


def refine_pupil(opf, c, r0, lvl, xmax):
    """Cast rays from blob centre, find mid-level crossing (pupil->iris). Occluded rays rejected."""
    cx, cy = c
    rs = np.arange(0.4 * r0, 2.0 * r0 + 6, 0.5)
    X = cx + np.outer(COS, rs)
    Y = cy + np.outer(SIN, rs)
    P = sample(opf, X, Y)  # NRAY x len(rs)
    inner = np.median(P[:, rs < 0.6 * r0])
    outer = np.median(P[:, (rs > 1.25 * r0) & (rs < 1.7 * r0)], axis=1)
    pts, okr = [], np.zeros(NRAY, bool)
    for k in range(NRAY):
        if outer[k] - inner < 14 or X[k, -1] >= xmax:  # dark outside (lashes/lid/vignette) or occluder
            if X[k, -1] >= xmax and (cx + COS[k] * r0) < xmax - 2:
                pass
            continue
        th = inner + 0.5 * (outer[k] - inner)
        above = np.nonzero(P[k] > th)[0]
        if len(above) == 0 or above[0] == 0:
            continue
        j = above[0]
        f = (th - P[k, j - 1]) / max(P[k, j] - P[k, j - 1], 1e-3)
        r = rs[j - 1] + f * (rs[j] - rs[j - 1])
        if r < 0.5 * r0 or r > 1.9 * r0:
            continue
        okr[k] = True
        pts.append((cx + COS[k] * r, cy + SIN[k] * r))
    pts = np.array(pts)
    if len(pts) < 10:
        return None
    e, keep = fit_ellipse_robust(pts)
    if e is None:
        return None
    (ex, ey), (A, B), ang = e
    a, b = max(A, B) / 2, min(A, B) / 2
    if not (5 < b and a < 60 and b / a > 0.35):
        return None
    return dict(cx=ex, cy=ey, a=a, b=b, ang=ang, e=e, inner=float(inner), vis=okr.mean(), pts=pts)


def iris_fit(opf, p, xmax):
    """Limbus along rays (left & right sides, top/bottom excluded), mapped into the pupil-ellipse
    frame (same axis ratio/orientation, concentric). Returns R (vertical-major semi-axis, px), points."""
    cx, cy = p['cx'], p['cy']
    (ex, ey), (A, B), ang = p['e']
    t = np.deg2rad(ang)
    ratio = min(A, B) / max(A, B)
    # unit vector of the major axis
    if A >= B:
        ua = np.array([np.cos(t), np.sin(t)])
    else:
        ua = np.array([-np.sin(t), np.cos(t)])
    ub = np.array([-ua[1], ua[0]])
    angs = np.deg2rad(np.r_[np.arange(-40, 41, 5), np.arange(140, 221, 5)])
    rs = np.arange(p['a'] * 1.25 + 3, 95, 0.75)
    res, pts = [], []
    for th in angs:
        d = np.array([np.cos(th), np.sin(th)])
        X, Y = cx + d[0] * rs, cy + d[1] * rs
        okx = (X < xmax - 3) & (X > 2)
        if okx.sum() < 12:
            continue
        prof = sample(opf, X[None, okx], Y[None, okx])[0]
        prof = np.convolve(prof, np.ones(3) / 3, 'same')
        g = prof[3:] - prof[:-3]
        nasal = np.cos(th) > 0  # rays to large x: the TEMPORAL side of both upright images (named nasal here)
        # large x: iris (dark) -> sclera (bright). small x (nasal): often dark shading, accept either sign but prefer +
        gg = g if nasal else np.where(g > 0, g, -0.8 * g)
        k = int(np.argmax(gg[2:-2])) + 2
        if gg[k] < (12 if nasal else 9):
            continue
        r = rs[okx][k + 1] + 0.375
        # normalised radius in the pupil-ellipse frame (expressed in major-axis px)
        v = d * r
        ra, rb = v @ ua, v @ ub
        rho = np.hypot(ra, rb / ratio)
        res.append((rho, nasal, gg[k]))
        pts.append((cx + v[0], cy + v[1]))
    if len(res) < 4:
        return np.nan, np.array(pts), 0
    rho = np.array([r[0] for r in res])
    nas = np.array([r[1] for r in res])
    if nas.sum() >= 4:  # large-x (temporal) limbus (iris -> bright sclera) is reliable; the nasal side often hits the canthus
        rho, pts = rho[nas], np.array(pts)[nas]
    med = np.median(rho)
    good = np.abs(rho - med) < 6
    R = float(np.median(rho[good])) if good.sum() >= 3 else np.nan
    return R, np.asarray(pts), int(good.sum())


def dp_path(cost, maxstep=1):
    """Min-cost left-to-right path through cost[y, x] with |dy|<=maxstep per column."""
    H, W = cost.shape
    acc = cost[:, 0].copy()
    back = np.zeros((H, W), np.int16)
    big = 1e9
    for x in range(1, W):
        cand = [acc]
        for s in range(1, maxstep + 1):
            up = np.r_[np.full(s, big), acc[:-s]]   # came from y-s
            dn = np.r_[acc[s:], np.full(s, big)]    # came from y+s
            cand += [up + 0.15 * s, dn + 0.15 * s]
        st = np.stack(cand)
        j = np.argmin(st, 0)
        acc = st[j, np.arange(H)] + cost[:, x]
        off = np.zeros(H, np.int16)
        for idx in range(1, 2 * maxstep + 1):
            s = (idx + 1) // 2
            off[j == idx] = -s if idx % 2 == 1 else s
        back[:, x] = off
    ys = np.zeros(W, np.int32)
    ys[-1] = int(np.argmin(acc))
    for x in range(W - 1, 0, -1):
        ys[x - 1] = ys[x] + back[ys[x], x]
    return ys


def robust_parabola(xs, ys, w):
    """Quadratic fit with MAD rejection; returns coef, inlier mask."""
    for _ in range(4):
        if w.sum() < 8:
            return None, w
        c = np.polyfit(xs[w], ys[w], 2)
        r = ys - np.polyval(c, xs)
        s = max(1.0, 1.4826 * np.median(np.abs(r[w])))
        w2 = w & (np.abs(r) < 2.5 * s)
        if (w2 == w).all():
            break
        w = w2
    if w.sum() < 8:
        return None, w
    return np.polyfit(xs[w], ys[w], 2), w


def lids(smf, gy, p, R, xmax, gy_low=None, prev_lower=None):
    """Upper lid: strongest bright(skin)->dark edge going down (gy<0) = where the lid skin ends ('skin line'),
    then the margin = the dark->bright step at the bottom of the thin dark band below it (lid underside/lash roots
    -> eyeball), searched within 3..16 px. Lower lid: dark margin line -> bright lid skin (gy>0).
    Pupil (dilated ellipse) masked; columns span the iris."""
    cx, cy, a, b = p['cx'], p['cy'], p['a'], p['b']
    xa, xb = int(max(186, cx - 1.1 * R)), int(min(xmax - 3, cx + 1.45 * R))
    xs = np.arange(xa, xb)
    out = {'upper': None, 'lower': None, 'margin': None}
    if len(xs) < 20:
        return out
    (ex, ey), (A, B), ang = p['e']
    for which in ('upper', 'lower'):
        if which == 'upper':
            ya, yb = int(max(4, cy - 2.1 * R)), int(min(396, cy + 0.45 * R))
            e = np.clip(-gy[ya:yb, xa:xb], 0, None).copy()
        else:
            ya, yb = int(cy + 0.25 * R), int(min(396, cy + 1.9 * R))
            e = np.clip((gy if gy_low is None else gy_low)[ya:yb, xa:xb], 0, None).copy()
            if prev_lower is not None and CFG['lower_band'] and e.shape[0] >= 6:
                yy = np.arange(ya, yb)[:, None]
                yp = np.polyval(prev_lower, xs)[None, :]
                e[np.abs(yy - yp) > CFG['lower_band']] = 0
        if e.shape[0] < 6:
            continue
        pm = np.zeros(e.shape, np.uint8)
        cv2.ellipse(pm, ((ex - xa, ey - ya), (A + 8, B + 8), ang), 1, -1)
        e[pm > 0] = 0
        nrm = np.percentile(e, 99) + 1e-3
        cost = -np.minimum(e / nrm, 1.0)
        ys = dp_path(cost, 1) + ya
        strength = e[ys - ya, xs - xa]
        good = strength > 0.2 * nrm
        coef, w = robust_parabola(xs.astype(float), ys.astype(float), good)
        if coef is None:
            continue
        out[which] = dict(xs=xs, ys=ys, good=w, coef=coef, y_at=float(np.polyval(coef, cx)),
                          frac=float(w.mean()), grad=float(np.median(strength[w])) if w.any() else 0.0)
    U = out['upper']
    if U is not None:
        # margin refinement: positive step below the skin line
        ysk = np.polyval(U['coef'], xs)
        my, mok = np.zeros(len(xs)), np.zeros(len(xs), bool)
        for j, x in enumerate(xs):
            y0 = int(round(ysk[j])) + 3
            y1 = min(395, y0 + 14)
            if y1 <= y0 + 2:
                continue
            if abs(x - ex) < A / 2 + 4 and y1 > ey - B / 2 - 3:  # over the pupil: no eyeball step
                continue
            col = gy[y0:y1, x]
            k = int(np.argmax(col))
            if col[k] > max(5.0, 0.15 * U['grad']):
                my[j], mok[j] = y0 + k, True
        # the band below the skin line has ~constant width; estimate it where the eyeball step is clear
        # (mostly the sclera) and shift the skin line by its median
        off = my - ysk
        if mok.sum() >= 6:
            o = np.median(off[mok])
            inl = mok & (np.abs(off - o) < 3)
            if inl.sum() >= 6:
                o = float(np.median(off[inl]))
                out['margin'] = dict(xs=xs, ys=my.astype(int), good=inl, coef=U['coef'] + np.array([0, 0, o]),
                                     y_at=U['y_at'] + o, off=o, frac=float(inl.mean()))
    return out


MARGIN_OFF = 7.0  # median margin - skin line offset (px), measured on session 1

FEATS = ['ok_pupil', 'pupil_cx', 'pupil_cy', 'pupil_a', 'pupil_b', 'pupil_ang', 'pupil_vis', 'pupil_inner', 'n_glints',
         'iris_R', 'iris_n', 'upper_skin_y', 'margin_ok', 'margin_off', 'upper_y', 'lower_y', 'upper_frac', 'lower_frac', 'upper_grad', 'lower_grad', 'aperture',
         'aperture_n', 'upper_to_pupil_n', 'lower_to_pupil_n', 'upper_iris_gap_n', 'sclera_above_px', 'sclera_band_int',
         'dark_frac', 'roi_mean', 'box_mean', 'box_p20']


def box_stats(op, cx, cy, R, xmax):
    """Grey statistics of the box where the iris should be (closed lid = bright skin, squint = dark lashes/iris)."""
    xa, xb = int(max(186, cx - 0.9 * R)), int(min(xmax - 3, cx + 0.9 * R))
    ya, yb = int(max(0, cy - 0.45 * R)), int(min(400, cy + 0.45 * R))
    if xb - xa < 5 or yb - ya < 5:
        return np.nan, np.nan
    b = op[ya:yb, xa:xb]
    return float(b.mean()), float(np.percentile(b, 20))


def grad_images(img, op):
    """Vertical gradients for the lids (and a separately opened one for the lower lid if configured)."""
    smf = cv2.GaussianBlur(op, (0, 0), 1.5).astype(np.float32)
    gy = np.zeros_like(smf)
    gy[2:-2] = smf[4:] - smf[:-4]
    gy_low = None
    if CFG['lower_open'] > 9:
        k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (CFG['lower_open'], CFG['lower_open']))
        sl = cv2.GaussianBlur(cv2.morphologyEx(img, cv2.MORPH_OPEN, k), (0, 0), 1.5).astype(np.float32)
        gy_low = np.zeros_like(sl)
        gy_low[2:-2] = sl[4:] - sl[:-4]
    return smf, gy, gy_low


def extract(img, xmax, prev=None, R_ref=None, prev_p=None, prev_lower=None):
    """img: upright uint8 400x400. prev = last pupil centre (search prior); prev_p = last good pupil dict
    (used when the pupil is not found, to still measure lids / closedness). Returns (features, detection)."""
    f = {k: np.nan for k in FEATS}
    roi = (186, 70, int(xmax), 320)
    op = cv2.morphologyEx(img, cv2.MORPH_OPEN, K_OPEN)
    opf = cv2.GaussianBlur(op, (0, 0), 1.0).astype(np.float32)
    x0, y0, x1, y1 = roi
    f['roi_mean'] = float(op[110:290, 200:x1].mean())
    blob = find_pupil_blob(opf, roi, prev)
    det = dict(p=None, iris_R=np.nan, lids={'upper': None, 'lower': None}, iris_pts=None, glints=[])
    p = None
    if blob is not None:
        c, r0, lo = blob
        p = refine_pupil(opf, c, r0, lo, xmax)
    f['ok_pupil'] = int(p is not None)
    f['dark_frac'] = float((op[110:290, 200:x1] < 30).mean())
    if p is None:
        if prev_p is None:
            return f, det
        # no pupil (closed, or squint with lashes over the pupil): lids/closedness at the last pupil geometry
        q = prev_p
        Rn = R_ref if R_ref else 2.0 * q['a']
        f['box_mean'], f['box_p20'] = box_stats(op, q['cx'], q['cy'], Rn, xmax)
        smf, gy, gy_low = grad_images(img, op)
        L = lids(smf, gy, q, Rn, xmax, gy_low, prev_lower)
        det['lids'], det['Rn'] = L, Rn
        U, Lo = L['upper'], L['lower']
        if U:
            f['upper_skin_y'] = U['y_at']
            f['upper_y'] = U['y_at'] + (L['margin']['off'] if L['margin'] is not None else MARGIN_OFF)
        if Lo:
            f['lower_y'] = Lo['y_at']
        if U and Lo:
            f['aperture'] = f['lower_y'] - f['upper_y']
            f['aperture_n'] = f['aperture'] / Rn
        return f, det
    det['p'] = p
    f.update(pupil_cx=p['cx'], pupil_cy=p['cy'], pupil_a=p['a'], pupil_b=p['b'], pupil_ang=p['ang'], pupil_vis=p['vis'],
             pupil_inner=p['inner'])
    # glints: bright tophat spots near the pupil
    gx0, gy0 = int(max(0, p['cx'] - 2.5 * p['a'])), int(max(0, p['cy'] - 2.5 * p['a']))
    gx1, gy1 = int(min(xmax, p['cx'] + 2.5 * p['a'])), int(min(400, p['cy'] + 2.5 * p['a']))
    th = cv2.subtract(img[gy0:gy1, gx0:gx1], op[gy0:gy1, gx0:gx1])
    n, lab, st, cen = cv2.connectedComponentsWithStats((th > 70).astype(np.uint8))
    det['glints'] = [(cen[i][0] + gx0, cen[i][1] + gy0) for i in range(1, n) if 2 <= st[i, 4] <= 120]
    f['n_glints'] = len(det['glints'])
    R, ipts, nI = iris_fit(opf, p, xmax)
    f['iris_R'], f['iris_n'] = R, nI
    det['iris_R'], det['iris_pts'] = R, ipts
    Rn = R_ref if R_ref else (R if np.isfinite(R) else 2.0 * p['a'])
    smf, gy, gy_low = grad_images(img, op)
    L = lids(smf, gy, p, Rn, xmax, gy_low, prev_lower)
    det['lids'] = L
    det['Rn'] = Rn
    U, Lo, M = L['upper'], L['lower'], L['margin']
    if U:
        f.update(upper_skin_y=U['y_at'], upper_frac=U['frac'], upper_grad=U['grad'], margin_ok=int(M is not None), margin_off=M['off'] if M is not None else np.nan)
        # margin = refined line when found, else skin line + typical band width
        U = dict(U, y_at=M['y_at'] if M is not None else U['y_at'] + MARGIN_OFF)
        f['upper_y'] = U['y_at']
        f['upper_to_pupil_n'] = (p['cy'] - U['y_at']) / Rn
        iris_top = p['cy'] - Rn
        f['upper_iris_gap_n'] = (iris_top - U['y_at']) / Rn
        f['sclera_above_px'] = max(0.0, iris_top - U['y_at'])
        # intensity of the band just below the upper margin over the iris columns (bright when sclera shows)
        ya, yb = int(U['y_at'] + 2), int(U['y_at'] + 2 + 0.25 * Rn)
        xa, xb = int(p['cx'] - 0.6 * Rn), int(min(xmax - 3, p['cx'] + 0.6 * Rn))
        if yb > ya and xb > xa:
            f['sclera_band_int'] = float(op[ya:yb, xa:xb].mean())
    if Lo:
        f.update(lower_y=Lo['y_at'], lower_frac=Lo['frac'], lower_grad=Lo['grad'])
        f['lower_to_pupil_n'] = (Lo['y_at'] - p['cy']) / Rn
    if U and Lo:
        f['aperture'] = Lo['y_at'] - U['y_at']
        f['aperture_n'] = f['aperture'] / Rn
    f['box_mean'], f['box_p20'] = box_stats(op, p['cx'], p['cy'], Rn, xmax)
    return f, det
