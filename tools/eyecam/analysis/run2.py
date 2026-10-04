"""Run feat2 over whole sessions (both eyes), in parallel chunks. Writes feats_s{s}[_variant].csv.

usage: run2.py <session> [nproc] [variant]; variant = base | tophat | tophat_band"""
import numpy as np, sys, time, csv, os
from multiprocessing import Pool
from io2 import frames, canon, frame_times
import feat2
from feat2 import extract, occluder_x, FEATS
import cv2, json
cv2.setNumThreads(1)

VARIANTS = {'base': {'lower_open': 9, 'lower_band': None}, 'tophat': {'lower_open': 15, 'lower_band': None},
            'tophat_band': {'lower_open': 15, 'lower_band': 8}}


def estimate_R(s, e, xm):
    """Unsupervised iris radius for the search windows: median of per-frame nasal-limbus fits on every 25th frame."""
    F = frames(s, e)
    rs = []
    for k in range(0, len(F), 25):
        f, d = extract(np.ascontiguousarray(canon(F[k], e)), xm)
        if f['ok_pupil'] and np.isfinite(f['iris_R']) and f['iris_n'] >= 4:
            rs.append(f['iris_R'])
    return float(np.median(rs))


def work(args):
    s, e, a, b, xm, R_ref, var = args
    feat2.CFG.update(VARIANTS[var])
    F = frames(s, e)
    prev, out, prev_p, prev_lower = None, [], None, None
    t0 = time.perf_counter()
    for k in range(a, b):
        img = np.ascontiguousarray(canon(F[k], e))
        f, d = extract(img, xm, prev, R_ref, prev_p, prev_lower)
        if f['ok_pupil']:
            prev, prev_p = (f['pupil_cx'], f['pupil_cy']), d['p']
        lo = d['lids'].get('lower') if d.get('lids') else None
        prev_lower = lo['coef'] if lo is not None else None
        out.append((k, f))
    return s, e, out, (time.perf_counter() - t0) / max(1, b - a)


if __name__ == '__main__':
    s = int(sys.argv[1])
    var = sys.argv[3] if len(sys.argv) > 3 else 'base'
    jobs, info = [], {}
    for e in 'LR':
        F = frames(s, e)
        xm = occluder_x(canon(F[::200].mean(0), e))
        R = estimate_R(s, e, xm)
        info[e] = dict(xmax=xm, R_search=R)
        n = len(F)
        jobs += [(s, e, a, min(n, a + 400), xm, R, var) for a in range(0, n, 400)]
    json.dump(info, open(f'runinfo_s{s}.json', 'w'))
    print('session', s, info)
    t0 = time.time()
    with Pool(int(sys.argv[2]) if len(sys.argv) > 2 else 8) as pool:
        res = pool.map(work, jobs)
    ft = frame_times(s)
    per = [r[3] for r in res]
    print('session', s, 'wall', round(time.time() - t0, 1), 's; per-frame ms (single core) median', round(1e3 * np.median(per), 2))
    with open(f'feats_s{s}' + ('' if var == 'base' else '_' + var) + '.csv', 'w', newline='') as fh:
        w = csv.writer(fh)
        w.writerow(['session', 'eye', 'eye_index', 't_cam'] + FEATS)
        for s_, e, out, _ in res:
            for k, f in out:
                w.writerow([s_, e, k, f'{ft[e][k]:.6f}'] + [('' if (isinstance(f[c], float) and not np.isfinite(f[c])) else (f'{f[c]:.4f}' if isinstance(f[c], float) else f[c])) for c in FEATS])
