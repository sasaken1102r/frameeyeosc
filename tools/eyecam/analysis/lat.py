"""Response latency (cue -> 50% of step) and frame noise for the proposed signals; Valve for comparison."""
import numpy as np
from io2 import cues
from tab import build

def step_time(t, x, tc, pre, post_win=(1.5, 4.5), lvl=0.5, sign=None):
    """Time after cue tc at which x crosses lvl between pre-level and post plateau."""
    p0 = np.nanmedian(x[(t > tc - pre) & (t < tc)])
    p1 = np.nanmedian(x[(t > tc + post_win[0]) & (t < tc + post_win[1])])
    thr = p0 + lvl * (p1 - p0)
    m = (t >= tc) & (t < tc + post_win[1])
    xs, ts = x[m], t[m]
    up = p1 > p0
    hit = np.nonzero((xs > thr) if up else (xs < thr))[0]
    return (ts[hit[0]] - tc if len(hit) else np.nan), p0, p1

for s in (1, 2):
    T = build(s)
    C = cues(s)
    for e in 'LR':
        d = T[e]; t = d['t_cam']
        x = d['skin_up']  # unfiltered
        out = []
        for i, (lab, tc, sec) in enumerate(C):
            if lab == 'widen':
                out.append(('widen on', *step_time(t, d['skin_up_f'], tc, 1.0)))
                out.append(('widen off (close cue)', *step_time(t, d['box_mean_f'], tc + sec, 1.0, (1.0, 1.8))))
                out.append(('valve open at close', *step_time(t, d['v_open'], tc + sec, 1.0, (1.0, 1.8))))
            if lab == 'close':
                out.append(('box closed', *step_time(t, d['box_mean_f'], tc, 1.0, (1.0, 1.8))))
            if lab == 'bright' and i > 0:
                out.append(('pupil constrict (bright)', *step_time(t, d['pd_n_f'], tc, 1.0, (2.0, 4.0))))
            if lab == 'dark':
                out.append(('pupil dilate 50% (dark)', *step_time(t, d['pd_n_f'], tc, 1.0, (6.5, 7.9))))
            if lab == 'squint':
                out.append(('squint ap', *step_time(t, d['ap_f'], tc, 1.0)))
        print(f's{s} {e}: ' + '; '.join(f'{n} {lt:.2f}s ({a:.2f}->{b:.2f})' for n, lt, a, b in out))
        # noise: normal frames, unfiltered and 5-frame median
        m = (d['label'] == 'normal') & np.isfinite(x)
        dx = np.diff(x)[m[1:] & m[:-1]]
        jit = np.nanstd(dx) / np.sqrt(2)
        # within-segment SD after median filter (per normal segment, detrended by segment median)
        sds = []
        for st in np.unique(d['step'][d['label'] == 'normal']):
            mm = d['step'] == st
            sds.append(np.nanstd(d['skin_up_f'][mm]))
        wid = np.nanmedian(d['skin_up_f'][d['label'] == 'widen']) - np.nanmedian(d['skin_up_f'][d['label'] == 'normal'])
        pj = np.diff(d['pd_n'])[(d['label'][1:] == 'bright') & (d['label'][:-1] == 'bright')]
        print(f'   skin_up jitter(frame) {jit:.4f} /R = {jit*{"L":63.5,"R":53.5}[e]:.2f}px; normal-seg SD (5-med) {np.mean(sds):.3f}; widen step {wid:.3f}; SNR frame {wid/jit:.1f}, seg {wid/np.mean(sds):.1f}; pupil_n jitter {np.nanstd(pj)/np.sqrt(2):.4f}')
