"""Per-frame table: features + derived normalised features + Valve (nearest sample) + labels."""
import numpy as np
from io2 import *
from ana0 import load

RREF = {'L': 63.5, 'R': 53.5}  # px; io2's anatomical eyes (the right eye's camera was called L before)
COLORS = {'lead_in': '#dddddd', 'close': '#555555', 'normal': '#ffffff', 'widen': '#ff9f40', 'squint': '#9b59b6',
          'look_up': '#3fa7d6', 'look_down': '#1d5d8c', 'bright': '#ffe066', 'dark': '#8c8c8c'}


def medfilt(x, k=5):
    """NaN-aware running median (centered)."""
    n = len(x); h = k // 2
    pad = np.r_[np.full(h, np.nan), x, np.full(h, np.nan)]
    W = np.lib.stride_tricks.sliding_window_view(pad, k)
    with np.errstate(all='ignore'):
        import warnings; warnings.simplefilter('ignore')
        return np.nanmedian(W, 1)


def build(s, delay=0.8):
    D = load(s)
    V = valve(s)
    tv = V['sample_time']
    out = {}
    for e in 'LR':
        d = dict(D[e])
        t = d['t_cam']
        lab, step = label_times(s, t, delay)
        d['label'], d['step'] = lab, step
        j = np.clip(np.searchsorted(tv, t), 1, len(tv) - 1)
        j = np.where(np.abs(tv[j - 1] - t) < np.abs(tv[j] - t), j - 1, j)
        ve = 'l' if e == 'L' else 'r'
        d['v_open'] = V[f'open_{ve}'][j]
        d['v_pitch'] = np.degrees(np.arcsin(np.clip(V[f'gaze_{ve}_y'][j], -1, 1)))
        d['v_yaw'] = np.degrees(np.arctan2(V[f'gaze_{ve}_x'][j], -V[f'gaze_{ve}_z'][j]))
        d['v_pty'] = V['extra_1' if e == 'L' else 'extra_3'][j]
        d['v_ptx'] = V['extra_0' if e == 'L' else 'extra_2'][j]
        open_ = d['ok_pupil'] > 0
        R = RREF[e]  # per-eye anatomical constant (session medians: L 53.8/53.1, R 64.0/63.0)
        d['R_ref'] = R
        cy = d['pupil_cy']
        d['up'] = (cy - d['upper_y']) / R
        d['gap'] = d['up'] - 1
        d['skin_up'] = (cy - d['upper_skin_y']) / R
        d['ap'] = (d['lower_y'] - d['upper_y']) / R
        d['lo'] = (d['lower_y'] - cy) / R
        d['pd_px'] = 2 * d['pupil_a']
        d['pd_n'] = 2 * d['pupil_a'] / R
        d['band'] = d['sclera_band_int']
        for k in ['up', 'skin_up', 'ap', 'lo', 'pd_px', 'pd_n', 'band', 'pupil_cy', 'pupil_vis', 'box_mean']:
            d[k + '_f'] = medfilt(d[k], 5)
        out[e] = d
    return out
