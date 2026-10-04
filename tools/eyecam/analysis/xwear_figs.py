import json, sys, numpy as np, csv
import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
from xwear import load, derive, sel, med, SESSIONS, WEAR
tag, R = sys.argv[1], sys.argv[2]
res = json.load(open('xwear_base.json'))
sh = res['shifts']
# --- wear shifts
P = [('R', 'iris radius (px)'), ('pupil_cy', 'pupil y, normal (px)'), ('pupil_cx', 'pupil x, normal (px)'),
     ('skin_up', 'lid skin line above pupil / R (normal)'), ('widen_step', 'widen step / R'), ('ap_n', 'aperture / R (normal)'),
     ('box_open', 'iris-box grey, open'), ('box_closed', 'iris-box grey, closed'), ('pd_bright', 'pupil/iris, bright'), ('pd_dark', 'pupil/iris, dark')]
fig, ax = plt.subplots(2, 5, figsize=(20, 7))
for a, (k, name) in zip(ax.flat, P):
    for e, c, off in (('L', '#1f77b4', -0.1), ('R', '#d62728', 0.1)):
        xs = [x['session'] + off for x in sh if x['eye'] == e]
        a.plot(xs, [x[k] for x in sh if x['eye'] == e], 'o-', color=c, label=e)
    a.set_xticks(SESSIONS); a.set_xticklabels([f's{s}\n{WEAR[s]}' for s in SESSIONS]); a.set_title(name, fontsize=9); a.grid(alpha=.3)
ax[0, 0].legend()
fig.suptitle('wear-to-wear shifts (s1/s2 = one wear A; B, C, D = HMD taken off and on)')
plt.tight_layout(); plt.savefig(f'{R}/eyecam_xwear_shifts_{tag}.png', dpi=75)
# --- conditions
rows = res['cond']
conds = [('same_wear', 'gated', 'ema35', 'same wear (s1<->s2)'), ('fixed', 'gated', 'ema35', 'fixed (other wear)'),
         ('block', 'gated', 'ema35', 'per-wear 15 s block'), ('block', 'gated', 'soft', 'block + soft map'),
         ('block', 'gated', 'mindur', 'block + 100 ms min'), ('auto', 'gated', 'ema35', 'auto (no prompts)')]
M = [('wide_tpr', 'EyeWide>0.5 in widen (TPR)'), ('wide_fpr', 'EyeWide>0.5 in neutral (FPR)'), ('acc_close', 'close -> closed'),
     ('acc_squint', 'squint -> squint'), ('acc_look_down', 'look_down -> open'), ('pupil_auc', 'pupil AUROC dark>bright')]
fig, ax = plt.subplots(1, len(M), figsize=(22, 4.5))
for a, (k, name) in zip(ax, M):
    for i, (c, cm, f, lab) in enumerate(conds):
        v = np.array([x[k] for x in rows if x['cond'] == c and x['closed_mode'] == cm and x['filt'] == f], float)
        a.boxplot([v[np.isfinite(v)]], positions=[i], widths=0.6, showfliers=True, whis=(0, 100))
        a.plot(np.full(len(v), i) + np.random.uniform(-0.15, 0.15, len(v)), v, '.', alpha=.4, color='k')
    a.set_xticks(range(len(conds))); a.set_xticklabels([c[3] for c in conds], rotation=40, ha='right', fontsize=8)
    a.set_title(name, fontsize=9); a.grid(alpha=.3, axis='y')
fig.suptitle('calibration conditions, test = steps after the first widen block; each dot = one (reference, test session, eye)')
plt.tight_layout(); plt.savefig(f'{R}/eyecam_xwear_conditions_{tag}.png', dpi=75)
# --- features CSV for all 5 sessions (normalised with each session's own normal-frame iris radius)
cols = ['session', 'wear', 'eye', 'eye_index', 't_cam', 'label', 'step', 'ok_pupil', 'pupil_cx', 'pupil_cy', 'pupil_a', 'pupil_b',
        'iris_R', 'iris_n', 'upper_skin_y', 'upper_y', 'lower_y', 'box_mean', 'skin_up', 'ap', 'lo', 'pd_n', 'skin_up_f', 'ap_f',
        'lo_f', 'pd_n_f', 'box_f', 'v_open', 'v_pitch']
with open(f'{R}/eyecam_features_5sess_{tag}.csv', 'w', newline='') as fh:
    w = csv.writer(fh); w.writerow(cols)
    for s in SESSIONS:
        D = load(s)
        for e in 'LR':
            d = D[e]
            Rn = med(d['iris_R'][sel(d, ['normal']) & (d['ok_pupil'] > 0) & (d['iris_n'] >= 4)])
            derive(d, Rn)
            n = len(d['t_cam'])
            for i in range(n):
                row = [s, WEAR[s], e]
                for c in cols[3:]:
                    v = d[c][i]
                    if c in ('label',):
                        row.append(v)
                    elif isinstance(v, (float, np.floating)):
                        row.append('' if not np.isfinite(v) else (f'{v:.6f}' if c == 't_cam' else f'{v:.4f}'))
                    else:
                        row.append(v)
                w.writerow(row)
print('ok')
