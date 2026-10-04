"""Overlay contact sheets: one tile per protocol step per eye (both sessions), and a failure-case sheet."""
import numpy as np, cv2, sys
from io2 import *
from feat2 import extract, occluder_x
from viz2 import draw
from tab import build, RREF


def det_at(s, e, k, xm):
    F = frames(s, e)
    prev, pp = None, None
    for j in range(max(0, k - 15), k + 1):
        img = np.ascontiguousarray(canon(F[j], e))
        f, d = extract(img, xm, prev, RREF[e], pp)
        if f['ok_pupil']:
            prev, pp = (f['pupil_cx'], f['pupil_cy']), d['p']
    return img, f, d


def grid(tiles, cols):
    while len(tiles) % cols:
        tiles.append(np.zeros_like(tiles[0]))
    return np.vstack([np.hstack(tiles[j:j + cols]) for j in range(0, len(tiles), cols)])


def legend(w):
    im = np.zeros((22, w, 3), np.uint8)
    items = [('pupil', (0, 255, 0)), ('iris (pupil-shaped, concentric)', (255, 160, 0)), ('iris top', (255, 0, 255)),
             ('lid skin line', (0, 140, 255)), ('lid margin', (0, 255, 255)), ('lower lid', (255, 0, 200)), ('glint', (0, 0, 255))]
    x = 5
    for n, c in items:
        cv2.rectangle(im, (x, 6), (x + 14, 16), c, -1); cv2.putText(im, n, (x + 18, 16), 0, 0.42, (255, 255, 255), 1)
        x += 30 + 8 * len(n)
    return im


def step_sheet(s, out, frac=0.6):
    ft = frame_times(s)
    xm = {e: occluder_x(canon(frames(s, e)[::200].mean(0), e)) for e in 'LR'}
    tiles = []
    for i, lab, rep, t0, t1 in segments(s):
        for e in 'LR':
            t = ft[e][:len(frames(s, e))]
            k = int(np.nanargmin(np.abs(t - (t0 + frac * (t1 - t0)))))
            img, f, d = det_at(s, e, k, xm[e])
            tiles.append(draw(img, d, f, f's{s} {lab}{rep} {e} #{k}', crop=(175, 60, 350, 290), scale=1.3))
    g = grid(tiles, 6)
    g = np.vstack([legend(g.shape[1]), g])
    cv2.imwrite(out, g)


def failure_sheet(out):
    picks = []
    for s in (1, 2):
        T = build(s)
        for e in 'LR':
            d = T[e]
            openseg = np.isin(d['label'], ['normal', 'widen', 'look_up', 'bright', 'dark'])
            cands = {
                'pupil lost (eye open)': openseg & (d['ok_pupil'] == 0) & (d['box_mean'] < 80),
                'lid skin jump': openseg & (np.abs(d['skin_up'] - d['skin_up_f']) > 0.12),
                'lower lid jump': openseg & (np.abs(d['lo'] - d['lo_f']) > 0.12),
                'squint: pupil lost': (d['label'] == 'squint') & (d['ok_pupil'] == 0),
                'iris radius off >25%': openseg & (np.abs(d['iris_R'] / RREF[e] - 1) > 0.25),
            }
            for name, m in cands.items():
                idx = np.nonzero(m)[0]
                if len(idx):
                    k = int(idx[len(idx) // 2])
                    picks.append((s, e, k, name, int(m.sum()), int(openseg.sum() if 'squint' not in name else (d['label'] == 'squint').sum())))
    tiles = []
    for s, e, k, name, n, N in picks:
        xm = occluder_x(canon(frames(s, e)[::200].mean(0), e))
        img, f, d = det_at(s, e, k, xm)
        tiles.append(draw(img, d, f, f's{s} {e}#{k} {name} ({n}/{N})', crop=(175, 60, 350, 290), scale=1.3))
    cv2.imwrite(out, np.vstack([legend(6 * tiles[0].shape[1]), grid(tiles, 6)]))
    return picks


if __name__ == '__main__':
    tag = sys.argv[1]
    for s in (1, 2):
        step_sheet(s, f'overlay_steps_s{s}_{tag}.png')
    for p in failure_sheet(f'overlay_failures_{tag}.png'):
        print(p)
