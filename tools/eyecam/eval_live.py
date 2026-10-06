"""eyecam のライブエンジン（Rust）の出力を、Python 試作の xwear.py と同じ指標で評価する。

    python eval_live.py DIR     # DIR に test_r{ref}_t{test}.csv（eyecam-rec --replay ... --calib-block 4 の出力）

テストに使うのは、各セッションの最初のブロック（lead_in, close, normal, widen）より後の段（step >= 4）だけ。
xwear の ('block', 'gated', 'soft') と同じ条件: 15 秒のブロックで wear 校正、user 校正（視線と開きの関係・細めの深さ・
瞳孔の範囲）は別のかぶりの参照セッションから。eye_wide は生の値と、Python と同じ EMA 0.35 をかけた値の両方を出す。
フレームレートは t_cam の間隔から測る（--resample で落とした録画でも、回/分と EMA が時間で同じになる。EMA の 0.35 は 90 fps でのもの）。
"""

import csv
import glob
import os
import re
import sys

import numpy as np

NEUTRAL = ['normal', 'look_up', 'look_down', 'bright', 'dark']


def auroc(pos, neg):
    """P(pos > neg)、同点は 0.5（Mann-Whitney）。"""
    pos, neg = pos[np.isfinite(pos)], neg[np.isfinite(neg)]
    if len(pos) == 0 or len(neg) == 0:
        return np.nan
    allv = np.r_[pos, neg]
    order = allv.argsort(kind='mergesort')
    ranks = np.empty(len(allv))
    sv = allv[order]
    i = 0
    while i < len(sv):
        j = i
        while j + 1 < len(sv) and sv[j + 1] == sv[i]:
            j += 1
        ranks[order[i:j + 1]] = (i + j) / 2 + 1
        i = j + 1
    return (ranks[:len(pos)].sum() - len(pos) * (len(pos) + 1) / 2) / (len(pos) * len(neg))


def ema(x, a):
    """NaN を飛ばす指数移動平均。"""
    y, acc = np.full(len(x), np.nan), np.nan
    for i, v in enumerate(x):
        if np.isfinite(v):
            acc = v if not np.isfinite(acc) else acc + a * (v - acc)
        y[i] = acc
    return y


def episodes(mask):
    """立ち上がりの回数。"""
    return int(np.sum(np.diff(np.r_[0, mask.astype(int)]) == 1))


def load(path):
    """目ごとの列の辞書。"""
    with open(path, newline='') as f:
        rows = list(csv.DictReader(f))
    out = {}
    for e in 'LR':
        rr = sorted([r for r in rows if r['eye'] == e], key=lambda r: int(r['eye_index']))
        d = {}
        for k in rr[0].keys():
            if k in ('eye', 'label'):
                d[k] = np.array([r[k] for r in rr], dtype=object)
            else:
                d[k] = np.array([float(r[k]) if r[k] != '' else np.nan for r in rr])
        out[e] = d
    return out


def med(x):
    x = x[np.isfinite(x)]
    return float(np.median(x)) if len(x) else np.nan


def fps_of(d):
    """t_cam の間隔の中央値から測ったフレームレート。"""
    dt = np.diff(d['t_cam'])
    dt = dt[(dt > 0) & (dt < 0.5)]
    return 1.0 / float(np.median(dt))


def evaluate(d):
    """1 本・片目の指標（xwear.evaluate と同じ定義）。"""
    fps = fps_of(d)
    T = d['step'] >= 4
    sel = lambda labs: T & np.isin(d['label'], labs)
    r = {}
    s = d['skin_up']
    wid, nor, neu = sel(['widen']), sel(['normal']), sel(NEUTRAL)
    r['auc_wn'] = auroc(s[wid], s[nor])
    r['auc_wneu'] = auroc(s[wid], s[neu])
    # 90 fps で 0.35 の EMA と同じ時定数
    a = 1.0 - (1.0 - 0.35) ** (90.0 / fps)
    for tag, W in (('raw', d['eye_wide']), ('ema35', ema(d['eye_wide'], a))):
        r[f'wide_tpr_{tag}'] = float(np.nanmean(W[wid] > 0.5))
        r[f'wide_fpr_{tag}'] = float(np.nanmean(W[neu] > 0.5))
        r[f'wide_fp_ep_min_{tag}'] = episodes((W > 0.5) & neu) / (neu.sum() / fps) * 60
    closed = d['closed'] > 0
    pred = np.where(closed, 0, np.where(d['eye_squint'] > 0.5, 1, 2))
    for lab, cls in (('close', 0), ('squint', 1), ('normal', 2), ('widen', 2), ('look_down', 2), ('look_up', 2)):
        m = sel([lab])
        r[f'acc_{lab}'] = float(np.mean(pred[m] == cls)) if m.any() else np.nan
    r['acc_3class_bal'] = float(np.nanmean([r['acc_close'], r['acc_squint'], r['acc_normal']]))
    r['closed_in_open'] = float(np.mean(closed[sel(['normal', 'widen', 'look_down', 'look_up'])]))
    r['closed_in_brightdark'] = float(np.mean(closed[sel(['bright', 'dark'])]))
    pd = d['pupil_ratio']
    br, dk = sel(['bright']), sel(['dark'])
    r['pupil_auc'] = auroc(pd[dk], pd[br])
    r['pupil_mm_bright'] = med(d['pupil_mm'][br])
    r['pupil_mm_dark'] = med(d['pupil_mm'][dk])
    r['dil_bright'] = med(d['pupil_dilation'][br])
    r['dil_dark'] = med(d['pupil_dilation'][dk])
    r['pupil_share_normal'] = float(np.mean(d['ok_pupil'][nor] > 0))
    r['wear_cal_share'] = float(np.mean(d['wear_cal'][T] > 0))
    r['fps'] = fps
    return r


def main():
    base = sys.argv[1]
    res = []
    for path in sorted(glob.glob(os.path.join(base, 'test_r*_t*.csv'))):
        m = re.search(r'test_r(\d)_t(\d)\.csv$', path)
        D = load(path)
        for e in 'LR':
            r = evaluate(D[e])
            r.update(ref=int(m.group(1)), test=int(m.group(2)), eye=e)
            res.append(r)
    keys = [k for k in res[0] if k not in ('ref', 'test', 'eye')]
    print(f'n = {len(res)} (ref x test x eye)')
    for k in keys:
        v = np.array([x[k] for x in res], float)
        print(f'  {k:24s} mean {np.nanmean(v):7.3f}  min {np.nanmin(v):7.3f}  max {np.nanmax(v):7.3f}')


if __name__ == '__main__':
    main()
