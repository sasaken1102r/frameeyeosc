import numpy as np, sys
import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
from io2 import *
from tab import build, COLORS
s = int(sys.argv[1]); out = sys.argv[2]
T = build(s, delay=0.0)
t00 = cues(s)[0][1]
rows = [('skin_up_f', 'lid skin line above pupil / R (WIDEN)'), ('up_f', 'lid margin above pupil / R'), ('ap_f', 'aperture / R'),
        ('lo_f', 'lower lid below pupil / R'), ('box_mean_f', 'grey at iris box (CLOSED)'), ('pd_n_f', 'pupil diam / iris radius'),
        ('pupil_cy_f', 'pupil y (px)'), ('v_open', 'Valve openness'), ('v_pitch', 'Valve gaze pitch (deg)')]
fig, ax = plt.subplots(len(rows), 1, figsize=(16, 2.0 * len(rows)), sharex=True)
for a, (k, name) in zip(ax, rows):
    for lab, t, sec in cues(s):
        if lab == 'end': continue
        a.axvspan(t - t00, t + sec - t00, color=COLORS.get(lab, 'w'), alpha=0.35, lw=0)
    for e, c in (('L', '#1f77b4'), ('R', '#d62728')):
        d = T[e]
        a.plot(d['t_cam'] - t00, d[k], c, lw=0.7, label=e)
    a.set_ylabel(name, fontsize=8); a.grid(alpha=0.3)
ax[0].legend(loc='upper right')
for lab, t, sec in cues(s):
    if lab != 'end': ax[0].text(t - t00 + 0.1, ax[0].get_ylim()[1], lab, fontsize=7, va='top')
ax[-1].set_xlabel('time from protocol start (s); shading = cue segments (no reaction delay)')
fig.suptitle(f'session {s}: features vs cues (L blue, R red; 5-frame median)')
plt.tight_layout(); plt.savefig(out, dpi=80)
