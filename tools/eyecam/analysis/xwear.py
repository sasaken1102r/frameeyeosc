"""Cross-wear evaluation over 5 sessions / 4 wears: fixed calibration vs short per-wear calibration (+ unsupervised
'auto' baseline), wear-to-wear parameter shifts, and before/after of the proposed fixes.

usage: xwear.py [variant]   (variant of the feature files: base | tophat | tophat_band)
"""
import numpy as np, csv, json, sys
from io2 import WEAR, label_times, valve, segments
from tab import medfilt
from evalx import auroc

SESSIONS = [1, 2, 3, 4, 5]
NEUTRAL = ['normal', 'look_up', 'look_down', 'bright', 'dark']
BLOCK_STEPS = (0, 1, 2, 3)  # lead_in, close1, normal1, widen1 (~15 s)
R_MM = 5.9
FPS = 90.0


def load(s, variant='base'):
    """Per-eye arrays from feats_s{s}[_variant].csv + labels + nearest Valve sample."""
    fn = f'feats_s{s}.csv' if variant == 'base' else f'feats_s{s}_{variant}.csv'
    with open(fn) as fh:
        r = csv.DictReader(fh)
        rows = list(r)
    cols = [c for c in r.fieldnames if c != 'eye']
    V = valve(s)
    tv = V['sample_time']
    out = {}
    for e in 'LR':
        rr = sorted([x for x in rows if x['eye'] == e], key=lambda x: int(x['eye_index']))
        d = {c: np.array([float(x[c]) if x[c] != '' else np.nan for x in rr]) for c in cols}
        t = d['t_cam']
        d['label'], d['step'] = label_times(s, t, 0.8)
        j = np.clip(np.searchsorted(tv, t), 1, len(tv) - 1)
        j = np.where(np.abs(tv[j - 1] - t) < np.abs(tv[j] - t), j - 1, j)
        ve = e.lower()
        d['v_open'] = V[f'open_{ve}'][j]
        d['v_pitch'] = np.degrees(np.arcsin(np.clip(V[f'gaze_{ve}_y'][j], -1, 1)))
        d['box_f'] = medfilt(d['box_mean'], 5)
        d['session'] = s
        out[e] = d
    return out


def derive(d, R):
    """Normalised lid/pupil features for iris radius R (px)."""
    cy = d['pupil_cy']
    x = {'skin_up': (cy - d['upper_skin_y']) / R, 'ap': (d['lower_y'] - d['upper_y']) / R,
         'lo': (d['lower_y'] - cy) / R, 'pd_n': 2 * d['pupil_a'] / R}
    for k, v in x.items():
        d[k] = v
        d[k + '_f'] = medfilt(v, 5)


def sel(d, labs, steps=None):
    m = np.isin(d['label'], labs)
    if steps is not None:
        m &= np.isin(d['step'], steps)
    return m


def test_mask(d):
    """Frames used for testing in every condition: everything after the first widen block (step >= 4)."""
    return d['step'] >= 4


def med(x):
    x = x[np.isfinite(x)]
    return float(np.median(x)) if len(x) else np.nan


# ---------------------------------------------------------------- calibration
def calib_full(d):
    """Full-protocol calibration on one session (all steps)."""
    ok = d['ok_pupil'] > 0
    R = med(d['iris_R'][sel(d, ['normal']) & ok & (d['iris_n'] >= 4)])
    derive(d, R)
    P = dict(R=R)
    P.update(levels(d, None))
    # pitch dependence of the aperture (deviation from the value at the normal pitch), quadratic, clamped
    m = sel(d, ['lead_in'] + NEUTRAL) & ok & np.isfinite(d['v_pitch']) & np.isfinite(d['ap_f'])
    c = np.polyfit(d['v_pitch'][m], d['ap_f'][m], 2)
    p0 = med(d['v_pitch'][sel(d, ['normal'])])
    P['ap_pitch'] = (c.tolist(), float(np.percentile(d['v_pitch'][m], 1)), float(np.percentile(d['v_pitch'][m], 99)), p0)
    P['ap_sq'] = med(d['ap_f'][sel(d, ['squint'])])
    P['f_sq'] = (P['ap_sq'] - P['ap_cl']) / (P['ap_n'] - P['ap_cl'])
    P['pd_min'] = float(np.nanpercentile(d['pd_n_f'][sel(d, ['bright'])], 5))
    P['pd_max'] = float(np.nanpercentile(d['pd_n_f'][sel(d, ['dark'])], 95))
    return P


def levels(d, steps):
    """Levels measurable from normal / widen / close frames (optionally restricted to some steps)."""
    ok = d['ok_pupil'] > 0
    n, w, c = sel(d, ['normal'], steps), sel(d, ['widen'], steps), sel(d, ['close'], steps)
    return dict(b_n=med(d['skin_up_f'][n & ok]), b_w=med(d['skin_up_f'][w & ok]), ap_n=med(d['ap_f'][n & ok]),
                ap_cl=med(d['ap_f'][c]), box_o=med(d['box_f'][n]), box_c=med(d['box_f'][c]))


def calib_block(d, ref):
    """Short per-wear calibration: only the first ~15 s (close1, normal1, widen1) of this session.
    Shape parameters that the block cannot provide (aperture-vs-pitch curve, relative squint depth, pupil range in
    iris units) are taken from the reference full calibration."""
    ok = d['ok_pupil'] > 0
    blk = np.isin(d['step'], BLOCK_STEPS)
    R = med(d['iris_R'][blk & np.isin(d['label'], ['normal', 'widen']) & ok & (d['iris_n'] >= 4)])
    derive(d, R)
    P = dict(R=R)
    P.update(levels(d, BLOCK_STEPS))
    P['ap_pitch'] = ref['ap_pitch'][:3] + (med(d['v_pitch'][sel(d, ['normal'], BLOCK_STEPS)]),)
    P['f_sq'] = ref['f_sq']
    P['ap_sq'] = P['ap_cl'] + ref['f_sq'] * (P['ap_n'] - P['ap_cl'])
    P['pd_min'], P['pd_max'] = ref['pd_min'], ref['pd_max']
    return P


def calib_auto(d, ref):
    """No prompts: R, skin baseline and open-eye box brightness from long-run medians of this session's own frames
    (pupil visible, pitch within the reference's normal +-15 deg); widen step, squint depth, closed-box ratio from ref."""
    ok = d['ok_pupil'] > 0
    R = med(d['iris_R'][ok & (d['iris_n'] >= 4)])
    derive(d, R)
    pn = ref['ap_pitch'][3]
    m = ok & (np.abs(d['v_pitch'] - pn) < 15)
    P = dict(R=R, b_n=med(d['skin_up_f'][m]), ap_n=med(d['ap_f'][m]), box_o=med(d['box_f'][m]))
    P['b_w'] = P['b_n'] + (ref['b_w'] - ref['b_n'])
    P['ap_cl'] = P['ap_n'] - (ref['ap_n'] - ref['ap_cl'])
    P['box_c'] = P['box_o'] * ref['box_c'] / ref['box_o']
    P['ap_pitch'] = ref['ap_pitch'][:3] + (med(d['v_pitch'][m]),)
    P['f_sq'] = ref['f_sq']
    P['ap_sq'] = P['ap_cl'] + ref['f_sq'] * (P['ap_n'] - P['ap_cl'])
    P['pd_min'], P['pd_max'] = ref['pd_min'], ref['pd_max']
    return P


# ---------------------------------------------------------------- decision rules
def ap_comp(d, P):
    c, lo, hi, p0 = P['ap_pitch']
    p = np.clip(d['v_pitch'], lo, hi)
    return d['ap_f'] - (np.polyval(c, p) - np.polyval(c, np.clip(p0, lo, hi)))


def closed_mask(d, P, mode='rel'):
    """rel: open + 0.35 (closed - open) from the calibration; abs: midpoint of the calibration's open/closed medians;
    gated: box > 1.3 x long-run open level (median while the pupil is visible, no calibration) AND no pupil for 3 frames."""
    if mode == 'gated':
        nop = np.convolve((d['ok_pupil'] == 0).astype(float), np.ones(3), 'full')[:len(d['ok_pupil'])] >= 3
        return (d['box_f'] > 1.3 * med(d['box_f'][d['ok_pupil'] > 0])) & nop
    if mode == 'rel':
        thr = P['box_o'] + 0.35 * (P['box_c'] - P['box_o'])
    else:
        thr = 0.5 * (P['box_o'] + P['box_c'])
    return d['box_f'] > thr


def causal_run_med(x, k):
    out = np.full(len(x), np.nan)
    for i in range(len(x)):
        w = x[max(0, i - k + 1):i + 1]
        w = w[np.isfinite(w)]
        if len(w):
            out[i] = np.median(w)
    return out


def ema(x, a):
    y, acc = np.full(len(x), np.nan), np.nan
    for i, v in enumerate(x):
        if np.isfinite(v):
            acc = v if not np.isfinite(acc) else acc + a * (v - acc)
        y[i] = acc
    return y


def eye_wide(d, P, closed, filt='ema35'):
    """Causal EyeWide in 0..1. filt: 'ema35' (5-frame median + EMA 0.35, as in the first report),
    'ema15' (slower EMA), 'mindur' (ema35, but output only after > 0.5 has held for 100 ms)."""
    x = causal_run_med(d['skin_up'], 5)
    step = P['b_w'] - P['b_n']
    if filt == 'soft':  # 0.5 at half the calibrated widen step, 1.0 at 3/4 of it
        w = np.clip((x - (P['b_n'] + 0.25 * step)) / (0.5 * step), 0, 1)
    else:  # 0.5 at 65% of the step, 1.0 at the calibrated widen level
        w = np.clip((x - (P['b_n'] + 0.3 * step)) / (0.7 * step), 0, 1)
    hold = np.convolve(closed.astype(float), np.ones(int(0.08 * FPS)), 'full')[:len(closed)] > 0
    w = np.where(hold, 0.0, w)
    w = np.where(np.isfinite(w), w, np.nan)
    if not np.isfinite(w[0]):
        w[0] = 0.0
    idx = np.where(np.isfinite(w), np.arange(len(w)), 0)
    np.maximum.accumulate(idx, out=idx)
    w = w[idx]
    if filt == 'ema15':
        return ema(w, 0.15)
    w = ema(w, 0.35)
    if filt == 'mindur':
        n = int(0.1 * FPS)
        above = w > 0.5
        run = np.zeros(len(w), int)
        for i in range(len(w)):
            run[i] = run[i - 1] + 1 if (above[i] and i) else int(above[i])
        gate = np.zeros(len(w), bool)
        on = False
        for i in range(len(w)):  # latch on after n frames above, off when it drops below 0.3
            if not on and run[i] >= n:
                on = True
            elif on and w[i] < 0.3:
                on = False
            gate[i] = on
        w = np.where(gate, w, np.minimum(w, 0.49))
    return w


def episodes(mask):
    """Number of rising edges."""
    m = mask.astype(int)
    return int(np.sum(np.diff(np.r_[0, m]) == 1))


def evaluate(d, P, closed_mode='rel', filt='ema35'):
    """Metrics on the test frames of one eye of one session under calibration P."""
    T = test_mask(d)
    r = {}
    closed = closed_mask(d, P, closed_mode)
    # WIDEN: calibration-free ranking + calibrated, thresholded output
    s = d['skin_up_f']
    wid, nor, neu = T & sel(d, ['widen']), T & sel(d, ['normal']), T & sel(d, NEUTRAL)
    r['auc_wn'] = auroc(s[wid], s[nor])
    r['auc_wneu'] = auroc(s[wid], s[neu])
    r['auc_wup'] = auroc(s[wid], s[T & sel(d, ['look_up'])])
    W = eye_wide(d, P, closed, filt)
    r['wide_tpr'] = float(np.nanmean(W[wid] > 0.5))
    r['wide_fpr'] = float(np.nanmean(W[neu] > 0.5))
    r['wide_med_w'] = med(W[wid]); r['wide_med_n'] = med(W[nor])
    neu_sec = neu.sum() / FPS
    r['wide_fp_episodes_per_min'] = episodes((W > 0.5) & neu) / neu_sec * 60
    # widen onset latency: cue -> EyeWide > 0.5, for the widen cues inside the test range
    lat = []
    for i, lab, rep, t0, t1 in segments(d['session'], 0.0):
        if lab == 'widen' and i >= 4:
            m = (d['t_cam'] >= t0) & (d['t_cam'] < t1)
            hit = np.nonzero(W[m] > 0.5)[0]
            lat.append(float(d['t_cam'][m][hit[0]] - t0) if len(hit) else np.nan)
    r['wide_onset_s'] = med(np.array(lat))
    # SQUINT / CLOSE: 3 classes
    apc = ap_comp(d, P)
    thr_sq = 0.5 * (P['ap_n'] + P['ap_sq'])
    pred = np.where(closed, 0, np.where(np.isfinite(apc) & (apc < thr_sq), 1, 2))
    pred = np.where(~closed & ~np.isfinite(apc), 1, pred)  # no lids measured while not closed: call it squint
    for lab, cls in (('close', 0), ('squint', 1), ('normal', 2), ('widen', 2), ('look_down', 2), ('look_up', 2)):
        m = T & sel(d, [lab])
        r[f'acc_{lab}'] = float(np.mean(pred[m] == cls)) if m.any() else np.nan
    r['acc_3class_bal'] = float(np.nanmean([r['acc_close'], r['acc_squint'], r['acc_normal']]))
    r['closed_in_brightdark'] = float(np.mean(closed[T & sel(d, ['bright', 'dark'])]))
    r['closed_in_open'] = float(np.mean(closed[T & sel(d, ['normal', 'widen', 'look_down', 'look_up'])]))
    # Valve for reference (same frames)
    vo = d['v_open']
    r['valve_close'] = float(np.mean(vo[T & sel(d, ['close'])] < 0.2))
    r['valve_lookdown_lt06'] = float(np.mean(vo[T & sel(d, ['look_down'])] < 0.6))
    # PUPIL
    pd_ = d['pd_n_f']
    br, dk = T & sel(d, ['bright']), T & sel(d, ['dark'])
    r['pupil_auc'] = auroc(pd_[dk], pd_[br])
    r['pupil_mm_bright'] = med(pd_[br]) * R_MM
    r['pupil_mm_dark'] = med(pd_[dk]) * R_MM
    dil = np.clip((pd_ - P['pd_min']) / (P['pd_max'] - P['pd_min']), 0, 1)
    r['dil_bright'] = med(dil[br]); r['dil_dark'] = med(dil[dk])
    return r


def shifts(D):
    """Per-session wear parameters (R from that session's own normal frames)."""
    rows = []
    for s in SESSIONS:
        for e in 'LR':
            d = D[s][e]
            ok = d['ok_pupil'] > 0
            n = sel(d, ['normal']) & ok
            R = med(d['iris_R'][n & (d['iris_n'] >= 4)])
            derive(d, R)
            rows.append(dict(session=s, wear=WEAR[s], eye=e, R=R, pupil_cx=med(d['pupil_cx'][n]), pupil_cy=med(d['pupil_cy'][n]),
                             skin_y_px=med(d['upper_skin_y'][n]), lower_y_px=med(d['lower_y'][n]),
                             skin_up=med(d['skin_up_f'][n]), widen_step=med(d['skin_up_f'][sel(d, ['widen']) & ok]) - med(d['skin_up_f'][n]),
                             ap_n=med(d['ap_f'][n]), lo_n=med(d['lo_f'][n]), ap_sq=med(d['ap_f'][sel(d, ['squint'])]),
                             ap_cl=med(d['ap_f'][sel(d, ['close'])]),
                             box_open=med(d['box_f'][sel(d, ['normal'])]), box_closed=med(d['box_f'][sel(d, ['close'])]),
                             pd_bright=med(d['pd_n_f'][sel(d, ['bright'])]), pd_dark=med(d['pd_n_f'][sel(d, ['dark'])]),
                             pd_normal=med(d['pd_n_f'][n]), v_pitch_normal=med(d['v_pitch'][sel(d, ['normal'])]),
                             v_open_lookdown=med(d['v_open'][sel(d, ['look_down'])]),
                             pupil_ok_squint=float(np.mean(ok[sel(d, ['squint'])]))))
    return rows


def run(variant='base'):
    D = {s: load(s, variant) for s in SESSIONS}
    res = {'shifts': shifts(D), 'cond': []}
    refs = {1: 'A', 3: 'B', 4: 'C', 5: 'D'}  # one full-calibration session per wear
    for rs, rw in refs.items():
        for e in 'LR':
            ref = calib_full(D[rs][e])
            for ts in SESSIONS:
                if WEAR[ts] == rw:
                    continue
                d = D[ts][e]
                for cond, P in (('fixed', ref), ('block', None), ('auto', None)):
                    if cond == 'block':
                        P = calib_block(d, ref)
                    elif cond == 'auto':
                        P = calib_auto(d, ref)
                    else:
                        derive(d, ref['R'])  # fixed: the reference wear's iris radius too
                    for closed_mode in ('rel', 'abs', 'gated'):
                        for filt in (('ema35', 'ema15', 'mindur', 'soft') if closed_mode == 'gated' else ('ema35',)):
                            r = evaluate(d, P, closed_mode, filt)
                            r.update(ref_session=rs, ref_wear=rw, test_session=ts, test_wear=WEAR[ts], eye=e, cond=cond,
                                     closed_mode=closed_mode, filt=filt, R_used=P['R'])
                            res['cond'].append(r)
    # same-wear reference: s1 <-> s2 (the first report's setting), fixed calibration
    for a, b in ((1, 2), (2, 1)):
        for e in 'LR':
            ref = calib_full(D[a][e])
            d = D[b][e]
            derive(d, ref['R'])
            r = evaluate(d, ref, 'gated')
            r.update(ref_session=a, ref_wear='A', test_session=b, test_wear='A', eye=e, cond='same_wear', closed_mode='gated',
                     filt='ema35', R_used=ref['R'])
            res['cond'].append(r)
    json.dump(res, open(f'xwear_{variant}.json', 'w'), indent=1, default=float)
    return res


if __name__ == '__main__':
    run(sys.argv[1] if len(sys.argv) > 1 else 'base')
