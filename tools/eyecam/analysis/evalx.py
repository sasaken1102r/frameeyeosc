"""Cross-session evaluation (tune on one session, test on the other) of widen / squint / close / pupil signals,
compared with Valve's openness and pupil-point extras. Prints tables and writes results JSON."""
import numpy as np, json, sys
from tab import build

NEUTRAL = ['lead_in', 'normal', 'look_up', 'look_down', 'bright', 'dark']


def auroc(pos, neg):
    """P(score_pos > score_neg), ties 0.5 (Mann-Whitney)."""
    pos, neg = pos[np.isfinite(pos)], neg[np.isfinite(neg)]
    if len(pos) == 0 or len(neg) == 0:
        return np.nan
    allv = np.r_[pos, neg]
    order = allv.argsort(kind='mergesort')
    ranks = np.empty(len(allv))
    sv = allv[order]
    i = 0
    while i < len(sv):  # average ranks for ties
        j = i
        while j + 1 < len(sv) and sv[j + 1] == sv[i]:
            j += 1
        ranks[order[i:j + 1]] = (i + j) / 2 + 1
        i = j + 1
    rp = ranks[:len(pos)].sum()
    return (rp - len(pos) * (len(pos) + 1) / 2) / (len(pos) * len(neg))


def cohen_d(a, b):
    a, b = a[np.isfinite(a)], b[np.isfinite(b)]
    s = np.sqrt(((len(a) - 1) * a.var(ddof=1) + (len(b) - 1) * b.var(ddof=1)) / (len(a) + len(b) - 2))
    return (a.mean() - b.mean()) / s


def fit_poly(x, y, deg=2):
    m = np.isfinite(x) & np.isfinite(y)
    return np.polyfit(x[m], y[m], deg)


def sel(d, labs):
    return np.isin(d['label'], labs)


def ffill(x):
    """Forward-fill NaNs (last valid value)."""
    idx = np.where(np.isfinite(x), np.arange(len(x)), 0)
    np.maximum.accumulate(idx, out=idx)
    y = x[idx]
    return y


def comp(cal, key, x, pitch):
    """Remove the neutral-expression dependence on gaze pitch (quadratic, pitch clamped to the calibrated range)."""
    c, lo, hi = cal[key]
    return x - np.polyval(c, np.clip(pitch, lo, hi))


def add_comp(d, cal):
    """Pitch-compensated features. *_cv: Valve gaze pitch; *_c: pupil y in the image (last valid value)."""
    cy = ffill(d['pupil_cy_f'])
    for k, base in (('up', 'up_f'), ('skin', 'skin_up_f'), ('ap', 'ap_f'), ('band', 'band_f')):
        d[k + '_cv'] = comp(cal, k + '_vp', d[base], d['v_pitch'])
        d[k + '_c'] = comp(cal, k + '_cy', d[base], cy)
    d['ap_any'] = d['ap_f']


def calibrate(d):
    """Per-user calibration from a tuning session: neutral-expression regressions on pitch."""
    m = sel(d, NEUTRAL) & (d['ok_pupil'] > 0)
    cal = {}
    for k, base in (('up', 'up_f'), ('skin', 'skin_up_f'), ('ap', 'ap_f'), ('band', 'band_f')):
        for pk, px in (('vp', d['v_pitch']), ('cy', d['pupil_cy_f'])):
            ok = m & np.isfinite(px) & np.isfinite(d[base])
            cal[f'{k}_{pk}'] = (np.polyfit(px[ok], d[base][ok], 2), float(np.percentile(px[ok], 1)), float(np.percentile(px[ok], 99)))
    add_comp(d, cal)
    d['cal'] = cal
    return cal


def best_threshold(pos, neg):
    """Threshold maximising balanced accuracy (pos > thr)."""
    pos, neg = pos[np.isfinite(pos)], neg[np.isfinite(neg)]
    c = np.unique(np.quantile(np.r_[pos, neg], np.linspace(0.01, 0.99, 197)))
    ba = [(np.mean(pos > t) + np.mean(neg <= t)) / 2 for t in c]
    return float(c[int(np.argmax(ba))])


def run(tune, test, out):
    A, B = build(tune), build(test)
    res = {}
    for e in ('L', 'R'):
        a, b = A[e], B[e]
        cal = calibrate(a)
        add_comp(b, cal)
    # binocular average (widen is bilateral)
    for D in (A, B):
        n = min(len(D['L']['t_cam']), len(D['R']['t_cam']))
        LR = {}
        for k in ['up_f', 'up_c', 'up_cv', 'skin_up_f', 'skin_c', 'skin_cv', 'ap_f', 'ap_c', 'ap_cv', 'ap_any', 'band_f',
                  'band_cv', 'v_open', 'v_pty', 'pd_n_f', 'pd_px_f', 'lo_f', 'ok_pupil', 'box_mean', 'box_mean_f']:
            LR[k] = np.nanmean(np.stack([D['L'][k][:n], D['R'][k][:n]]), 0) if k in D['L'] else None
        LR['label'] = D['L']['label'][:n]
        LR['t_cam'] = D['L']['t_cam'][:n]
        D['LR'] = LR
    # ---- WIDEN ----
    wfeats = ['up_f', 'up_cv', 'skin_up_f', 'skin_c', 'skin_cv', 'ap_f', 'ap_cv', 'band_f', 'band_cv', 'v_open', 'v_pty']
    W = {}
    for e in ('L', 'R', 'LR'):
        a, b = A[e], B[e]
        W[e] = {}
        for k in wfeats:
            if b.get(k) is None:
                continue
            x = b[k]
            wid, nor = x[sel(b, ['widen'])], x[sel(b, ['normal'])]
            neu = x[sel(b, NEUTRAL)]
            thr = best_threshold(a[k][sel(a, ['widen'])], a[k][sel(a, NEUTRAL)])
            W[e][k] = dict(auc_wn=auroc(wid, nor), d_wn=cohen_d(wid, nor), auc_wup=auroc(wid, x[sel(b, ['look_up'])]),
                           auc_wbright=auroc(wid, x[sel(b, ['bright', 'dark'])]), auc_wneu=auroc(wid, neu),
                           tpr=float(np.nanmean(wid > thr)), fpr_neu=float(np.mean(neu[np.isfinite(neu)] > thr)),
                           fpr_up=float(np.nanmean(x[sel(b, ['look_up'])] > thr)),
                           med_n=float(np.nanmedian(nor)), med_w=float(np.nanmedian(wid)),
                           med_up=float(np.nanmedian(x[sel(b, ['look_up'])])), med_dn=float(np.nanmedian(x[sel(b, ['look_down'])])),
                           valid_w=float(np.mean(np.isfinite(wid))))
    res['widen'] = W
    # ---- SQUINT / CLOSE ---- (3 classes, aperture with fallback when the pupil is lost; Valve openness for comparison)
    Q = {}
    for e in ('L', 'R', 'LR'):
        a, b = A[e], B[e]
        # closed detector: the box where the iris should be turns into bright lid skin (tuned close vs squint)
        tb = best_threshold(a['box_mean_f'][sel(a, ['close'])], a['box_mean_f'][sel(a, ['squint', 'normal', 'look_down'])])
        for D in (a, b):
            D['combo'] = np.where(D['box_mean_f'] > tb, -9.0, D['ap_cv'])
        Q[e] = {'t_box': tb}
        for k, sign in (('ap_any', 1), ('ap_cv', 1), ('combo', 1), ('lo_f', 1), ('v_open', 1)):
            if b.get(k) is None:
                continue
            ka = a[k].copy(); kb = b[k].copy()
            # close vs squint threshold and squint vs normal threshold (tuned on A)
            t1 = best_threshold(ka[sel(a, ['squint'])], ka[sel(a, ['close'])])
            t2 = best_threshold(ka[sel(a, ['normal', 'widen'])], ka[sel(a, ['squint'])])
            conf = {}
            for L in ('close', 'squint', 'normal', 'widen', 'look_down', 'look_up'):
                x = kb[sel(b, [L])]
                n = len(x)
                xf = np.where(np.isfinite(x), x, -9)  # missing measurement -> treat as closed
                conf[L] = [float(np.mean(xf <= t1)), float(np.mean((xf > t1) & (xf <= t2))), float(np.mean(xf > t2))]
            Q[e][k] = dict(t_close=t1, t_squint=t2, conf=conf,
                           auc_sq_norm=auroc(-kb[sel(b, ['squint'])], -kb[sel(b, ['normal'])]),
                           auc_sq_down=auroc(-kb[sel(b, ['squint'])], -kb[sel(b, ['look_down'])]),
                           auc_cl_sq=auroc(-np.where(np.isfinite(kb), kb, -9)[sel(b, ['close'])],
                                           -np.where(np.isfinite(kb), kb, -9)[sel(b, ['squint'])]),
                           med={L: float(np.nanmedian(kb[sel(b, [L])])) for L in ('close', 'squint', 'normal', 'widen', 'look_down', 'look_up')})
    res['squint'] = Q
    # ---- PUPIL ----
    P = {}
    for e in ('L', 'R', 'LR'):
        b = B[e]
        P[e] = {}
        for k in ('pd_px_f', 'pd_n_f'):
            if b.get(k) is None:
                continue
            x = b[k]
            br, dk, no = x[sel(b, ['bright'])], x[sel(b, ['dark'])], x[sel(b, ['normal'])]
            P[e][k] = dict(bright=float(np.nanmedian(br)), dark=float(np.nanmedian(dk)), normal=float(np.nanmedian(no)),
                           auc=auroc(dk, br), d=cohen_d(dk, br))
    res['pupil'] = P
    json.dump(res, open(out, 'w'), indent=1, default=float)
    return res, A, B


def fmt_widen(res, tag):
    lines = [f'### WIDEN ({tag})', '| eye | feature | AUROC w/n | d w/n | AUROC w/look_up | AUROC w/bright+dark | AUROC w/all-neutral | TPR | FPR neutral | FPR look_up | med normal | med widen | med look_up | med look_down |',
             '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|']
    for e, F in res['widen'].items():
        for k, v in F.items():
            lines.append(f"| {e} | {k} | {v['auc_wn']:.3f} | {v['d_wn']:.2f} | {v['auc_wup']:.3f} | {v['auc_wbright']:.3f} | {v['auc_wneu']:.3f} | {v['tpr']:.2f} | {v['fpr_neu']:.2f} | {v['fpr_up']:.2f} | {v['med_n']:.3f} | {v['med_w']:.3f} | {v['med_up']:.3f} | {v['med_dn']:.3f} |")
    return '\n'.join(lines)


def fmt_squint(res, tag):
    lines = [f'### SQUINT/CLOSE ({tag}) rows: true segment; cols: predicted [closed, squint, open]',
             '| eye | feature | t_close | t_squint | AUROC sq<normal | AUROC sq<look_down | AUROC close<squint | close | squint | normal | widen | look_down | look_up |',
             '|---|---|---|---|---|---|---|---|---|---|---|---|---|']
    for e, F in res['squint'].items():
        for k, v in F.items():
            if k == 't_box':
                lines.append(f'| {e} | closed if box grey > {v:.1f} | | | | | | | | | | | |')
                continue
            c = v['conf']
            cs = ' | '.join('/'.join(f'{p:.2f}' for p in c[L]) for L in ('close', 'squint', 'normal', 'widen', 'look_down', 'look_up'))
            lines.append(f"| {e} | {k} | {v['t_close']:.2f} | {v['t_squint']:.2f} | {v['auc_sq_norm']:.3f} | {v['auc_sq_down']:.3f} | {v['auc_cl_sq']:.3f} | {cs} |")
    return '\n'.join(lines)


def fmt_pupil(res, tag):
    lines = [f'### PUPIL ({tag})', '| eye | feature | normal | bright | dark | AUROC dark>bright | d |', '|---|---|---|---|---|---|---|']
    for e, F in res['pupil'].items():
        for k, v in F.items():
            lines.append(f"| {e} | {k} | {v['normal']:.3f} | {v['bright']:.3f} | {v['dark']:.3f} | {v['auc']:.3f} | {v['d']:.2f} |")
    return '\n'.join(lines)


if __name__ == '__main__':
    txt = []
    for tune, test in ((1, 2), (2, 1)):
        res, A, B = run(tune, test, f'eval_tune{tune}_test{test}.json')
        tag = f'tune s{tune} -> test s{test}'
        txt += [fmt_widen(res, tag), '', fmt_squint(res, tag), '', fmt_pupil(res, tag), '']
    open('eval_tables.md', 'w', encoding='utf-8').write('\n'.join(txt))
    print('\n'.join(txt))
