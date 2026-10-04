import numpy as np, sys
import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
from tab import build, COLORS
out = sys.argv[1]
LABS = ['close', 'normal', 'widen', 'squint', 'look_up', 'look_down', 'bright', 'dark']
FE = [('skin_up_f', 'lid skin line above pupil / R  (WIDEN)'), ('up_f', 'lid margin above pupil / R'), ('ap_f', 'aperture / R'),
      ('lo_f', 'lower lid below pupil / R'), ('box_mean_f', 'grey where the iris should be (CLOSED)'), ('band_f', 'grey just below the lid'),
      ('pd_n_f', 'pupil diameter / iris radius'), ('v_open', 'Valve openness')]
T = {s: build(s) for s in (1, 2)}
fig, ax = plt.subplots(len(FE), 2, figsize=(16, 3.0 * len(FE)))
for j, e in enumerate('LR'):
    for i, (k, name) in enumerate(FE):
        a = ax[i, j]
        data, pos, cols = [], [], []
        for li, L in enumerate(LABS):
            for s in (1, 2):
                d = T[s][e]
                x = d[k][d['label'] == L]
                data.append(x[np.isfinite(x)] if np.isfinite(x).any() else np.array([np.nan]))
                pos.append(li * 3 + (s - 1) * 1.0)
                cols.append(COLORS[L])
        bp = a.boxplot(data, positions=pos, widths=0.8, patch_artist=True, showfliers=False, whis=(5, 95))
        for p, c in zip(bp['boxes'], cols):
            p.set_facecolor(c); p.set_edgecolor('k')
        a.set_xticks([li * 3 + 0.5 for li in range(len(LABS))]); a.set_xticklabels(LABS, fontsize=8)
        a.set_title(f'{e} eye: {name}  (left box = s1, right = s2; whiskers P5-P95)', fontsize=9); a.grid(alpha=0.3, axis='y')
plt.tight_layout(); plt.savefig(out, dpi=70)
