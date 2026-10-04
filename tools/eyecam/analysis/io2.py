"""Loading for eyecam protocol recordings (memmap raw frames, timing, cues, valve)."""
import os, csv, numpy as np, cv2

ROOT = os.environ.get('EYECAM_DATA', os.path.expanduser('~/eyecam'))
SESS = {1: 'rec_2026-10-03_23-50-19', 2: 'rec_2026-10-03_23-51-59',
        3: 'rec_2026-10-04_00-34-28', 4: 'rec_2026-10-04_00-38-45', 5: 'rec_2026-10-04_00-41-34'}
# HMD wear (taken fully off and on between wears); sessions 1 and 2 share one wear
WEAR = {1: 'A', 2: 'A', 3: 'B', 4: 'C', 5: 'D'}
# sessions whose eye_L/eye_R files hold the other camera (eyecam-rec numbered the cameras by picture position and
# got it backwards: meta slot_camera=1,1,1,1,0,0,0,0, frames upside down after the usual flip, iris radii swapped)
SWAPPED = {5}


def file_eye(s, eye):
    """Which raw file / frames.csv label holds this anatomical eye."""
    return {'L': 'R', 'R': 'L'}[eye] if s in SWAPPED else eye


def sdir(s):
    return os.path.join(ROOT, SESS[s])


def frames(s, eye):
    """memmap (N,400,400) uint8 of raw frames for eye 'L'/'R'."""
    p = os.path.join(sdir(s), f'eye_{file_eye(s, eye)}.raw')
    n = os.path.getsize(p) // 160000
    return np.memmap(p, np.uint8, 'r', shape=(n, 400, 400))


def canon(img, eye):
    """Raw -> upright: right eye flipped vertically."""
    return img[::-1] if eye == 'R' else img


def read_csv(path):
    with open(path, newline='') as f:
        r = csv.reader(f)
        h = next(r)
        rows = [x for x in r]
    return h, rows


def frame_times(s):
    """dict eye -> array of t_cam indexed by eye_index."""
    h, rows = read_csv(os.path.join(sdir(s), 'frames.csv'))
    ie, ii, it = h.index('eye'), h.index('eye_index'), h.index('t_cam')
    out = {}
    for e in 'LR':
        rr = [r for r in rows if r[ie] == file_eye(s, e)]
        t = np.full(max(int(r[ii]) for r in rr) + 1, np.nan)
        for r in rr:
            t[int(r[ii])] = float(r[it])
        out[e] = t
    return out


def cues(s):
    h, rows = read_csv(os.path.join(sdir(s), 'cues.csv'))
    return [(r[3], float(r[1]), float(r[4])) for r in rows]  # label, t_raw start, seconds


def valve(s):
    h, rows = read_csv(os.path.join(sdir(s), 'valve.csv'))
    a = np.array([[float(v) if v not in ('', 'nan') else np.nan for v in r] for r in rows])
    return {k: a[:, i] for i, k in enumerate(h)}


def segments(s, delay=0.8, tail=0.0):
    """List of (step_index, label, rep, t0, t1) with reaction delay trimmed from the start."""
    c = cues(s)
    out, reps = [], {}
    for i, (lab, t, sec) in enumerate(c):
        if lab == 'end':
            break
        reps[lab] = reps.get(lab, 0) + 1
        out.append((i, lab, reps[lab], t + delay, t + sec - tail))
    return out


def label_times(s, t, delay=0.8):
    """Per-time label ('' outside trimmed segments) and step index."""
    lab = np.array([''] * len(t), dtype=object)
    step = np.full(len(t), -1)
    for i, l, rep, t0, t1 in segments(s, delay):
        m = (t >= t0) & (t < t1)
        lab[m] = l
        step[m] = i
    return lab, step
