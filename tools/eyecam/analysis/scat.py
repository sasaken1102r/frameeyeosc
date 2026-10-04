import numpy as np
import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
from tab import build, COLORS
fig, ax = plt.subplots(2, 4, figsize=(20, 9))
for s in (1, 2):
    T = build(s)
    for j, e in enumerate('LR'):
        d = T[e]
        for i, (k, xk) in enumerate([('skin_up_f', 'pupil_cy_f'), ('skin_up_f', 'v_pitch')]):
            a = ax[j, i + 2 * (s - 1)]
            for L in ['normal', 'lead_in', 'look_up', 'look_down', 'bright', 'dark', 'widen', 'squint']:
                m = d['label'] == L
                a.scatter(d[xk][m], d[k][m], s=2, c=COLORS[L] if L not in ('normal', 'lead_in') else 'k', label=L, alpha=0.5)
            a.set_title(f's{s} {e} {k} vs {xk}'); a.grid(alpha=.3)
ax[0, 0].legend(markerscale=5)
plt.tight_layout(); plt.savefig('scat.png', dpi=60)
