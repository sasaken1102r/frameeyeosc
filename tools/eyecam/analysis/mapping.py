"""Proposed VRCFT mapping (EyeWide / EyeSquint / EyeLid / pupil) with per-user calibration from one protocol session,
applied causally to the other session. Prints per-segment medians and false-trigger rates; saves a time-series figure."""
import numpy as np, sys, json
import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
from tab import build, COLORS, RREF
from evalx import calibrate, add_comp, ffill, sel, NEUTRAL
from io2 import cues

R_MM = 5.9  # assumed iris radius (HVID 11.8 mm); only scales pupil mm


def causal_med(x, k=5):
    """Causal running median over the last k frames (NaN-aware)."""
    out = np.full(len(x), np.nan)
    for i in range(len(x)):
        w = x[max(0, i - k + 1):i + 1]
        w = w[np.isfinite(w)]
        if len(w):
            out[i] = np.median(w)
    return out


def ema(x, alpha):
    y = np.full(len(x), np.nan)
    acc = np.nan
    for i, v in enumerate(x):
        if np.isfinite(v):
            acc = v if not np.isfinite(acc) else acc + alpha * (v - acc)
        y[i] = acc
    return y


def calib_params(a):
    """Per-user constants from the calibration session (a = per-eye table with comp features)."""
    neu = sel(a, NEUTRAL) & (a['ok_pupil'] > 0)
    P = dict(
        wide0=float(np.nanpercentile(a['skin_c'][neu], 90)),           # dead zone: neutral P90
        wide1=float(np.nanmedian(a['skin_c'][sel(a, ['widen'])])),       # widen median -> 1.0
        ap_open=float(np.nanmedian(a['ap_f'][sel(a, ['normal'])])),  # absolute normal aperture / R
        sq0=float(np.nanpercentile(a['ap_cv'][neu], 5)),                # squint starts below neutral P5
        sq1=float(np.nanmedian(a['ap_cv'][sel(a, ['squint'])])),         # squint median -> 1.0
        ap_closed=float(np.nanmedian(a['ap_any'][sel(a, ['close'])] - 0)),
        box_open=float(np.nanmedian(a['box_mean_f'][sel(a, ['normal'])])),
        box_closed=float(np.nanmedian(a['box_mean_f'][sel(a, ['close'])])),
        pd_min=float(np.nanpercentile(a['pd_n_f'][sel(a, ['bright'])], 5)),
        pd_max=float(np.nanpercentile(a['pd_n_f'][sel(a, ['dark'])], 95)),
    )
    P['box_thr'] = P['box_open'] + 0.35 * (P['box_closed'] - P['box_open'])  # closed-lid brightness varies between sessions
    return P


def outputs(b, P, fps=90.0):
    """Causal per-frame VRCFT outputs for one eye."""
    clamp = lambda v: np.clip(v, 0, 1)
    box = causal_med(b['box_mean'], 3)
    closed = box > P['box_thr']
    # widen/squint forced to 0 while closed and for 80 ms after re-opening (blink transients); lost frames hold
    hold = np.convolve(closed.astype(float), np.ones(int(0.08 * fps)), 'full')[:len(closed)] > 0
    skin = causal_med(b['skin_up'], 5)  # raw (unfiltered) feature -> causal median
    from evalx import comp
    skin_c = comp(b['cal'], 'skin_vp', skin, b['v_pitch'])
    ap = causal_med(b['ap'], 5)
    ap_c = comp(b['cal'], 'ap_vp', ap, b['v_pitch'])
    wide = clamp((skin_c - P['wide0']) / (P['wide1'] - P['wide0']))
    sq = clamp((P['sq0'] - ap_c) / (P['sq0'] - P['sq1']))
    wide = np.where(hold, 0.0, wide); sq = np.where(hold, 0.0, sq)
    for v in (wide, sq):  # hold the last value through blinks / lost frames
        if not np.isfinite(v[0]):
            v[0] = 0.0
    wide, sq = ffill(wide), ffill(sq)
    wide, sq = ema(wide, 0.35), ema(sq, 0.35)  # ~25 ms time constant at 90 Hz
    openv = clamp((ap_c + P['ap_open'] - P['ap_closed']) / (P['ap_open'] - P['ap_closed']))
    openv = np.where(closed, 0.0, openv)
    lid = np.where(closed, 0.0, 0.75 * np.nan_to_num(openv, nan=0.0) + 0.25 * np.nan_to_num(wide))
    lid = ema(lid, 0.5)
    pdn = causal_med(b['pd_n'], 9)
    pd_mm = ema(pdn * R_MM, 0.2)
    dil = clamp((pdn - P['pd_min']) / (P['pd_max'] - P['pd_min']))
    return dict(EyeWide=wide, EyeSquint=sq, EyeLid=lid, closed=closed.astype(float), PupilDiameter_MM=pd_mm, PupilDilation=ema(dil, 0.2))


def main():
    rep = {}
    for tune, test in ((1, 2), (2, 1)):
        A, B = build(tune), build(test)
        figrows = []
        rep[f'{tune}->{test}'] = {}
        for e in 'LR':
            calibrate(A[e]); add_comp(B[e], A[e]['cal']); B[e]['cal'] = A[e]['cal']
            P = calib_params(A[e])
            O = outputs(B[e], P)
            b = B[e]
            r = {'params': P}
            for L in ['close', 'normal', 'widen', 'squint', 'look_up', 'look_down', 'bright', 'dark']:
                m = sel(b, [L])
                r[L] = {k: float(np.nanmedian(v[m])) for k, v in O.items()}
                r[L]['valve_open'] = float(np.nanmedian(b['v_open'][m]))
            neu = sel(b, ['normal', 'look_up', 'look_down', 'bright', 'dark'])
            r['false'] = dict(wide_gt_0p5_neutral=float(np.nanmean(O['EyeWide'][neu] > 0.5)),
                              squint_gt_0p5_neutral=float(np.nanmean(O['EyeSquint'][neu] > 0.5)),
                              squint_gt_0p5_lookdown=float(np.nanmean(O['EyeSquint'][sel(b, ['look_down'])] > 0.5)),
                              valve_lt_0p6_lookdown=float(np.nanmean(b['v_open'][sel(b, ['look_down'])] < 0.6)),
                              wide_gt_0p5_widen=float(np.nanmean(O['EyeWide'][sel(b, ['widen'])] > 0.5)),
                              squint_gt_0p5_squint=float(np.nanmean(O['EyeSquint'][sel(b, ['squint'])] > 0.5)),
                              closed_in_close=float(np.nanmean(O['closed'][sel(b, ['close'])])),
                              closed_in_open=float(np.nanmean(O['closed'][sel(b, ['normal', 'widen', 'squint', 'look_down', 'look_up'])])))
            rep[f'{tune}->{test}'][e] = r
            figrows.append((e, b, O))
        # figure for this test session
        t00 = cues(test)[0][1]
        fig, ax = plt.subplots(6, 1, figsize=(16, 12), sharex=True)
        keys = [('EyeWide', 'EyeWide'), ('EyeSquint', 'EyeSquint'), ('EyeLid', 'EyeLid (0.75 = normal, 1 = full widen)'),
                ('valve', 'Valve openness'), ('PupilDiameter_MM', 'pupil diameter (mm, iris 11.8 mm assumed)'), ('closed', 'closed detector')]
        for a, (k, name) in zip(ax, keys):
            for lab, t, sec in cues(test):
                if lab != 'end':
                    a.axvspan(t - t00, t + sec - t00, color=COLORS.get(lab, 'w'), alpha=0.35, lw=0)
            for e, b, O in figrows:
                y = b['v_open'] if k == 'valve' else O[k]
                a.plot(b['t_cam'] - t00, y, '#1f77b4' if e == 'L' else '#d62728', lw=0.8, label=e)
            a.set_ylabel(name, fontsize=8); a.grid(alpha=0.3)
        for lab, t, sec in cues(test):
            if lab != 'end':
                ax[0].text(t - t00 + 0.1, 1.02, lab, fontsize=7)
        ax[0].legend(loc='upper right'); ax[-1].set_xlabel('time (s)')
        fig.suptitle(f'proposed VRCFT outputs, calibrated on session {tune}, applied causally to session {test}')
        plt.tight_layout(); plt.savefig(f'vrcft_outputs_cal{tune}_test{test}.png', dpi=80)
    json.dump(rep, open('mapping_report.json', 'w'), indent=1)
    for k, v in rep.items():
        for e, r in v.items():
            print(k, e, 'false/true rates', {a: round(b, 3) for a, b in r['false'].items()})
            for L in ['close', 'normal', 'widen', 'squint', 'look_up', 'look_down', 'bright', 'dark']:
                print('   ', f'{L:9}', {a: round(b, 2) for a, b in r[L].items()})


if __name__ == '__main__':
    main()
