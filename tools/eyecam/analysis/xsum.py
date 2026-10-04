import json, sys, numpy as np
r = json.load(open(sys.argv[1]))['cond']
KS = ['auc_wn', 'auc_wneu', 'auc_wup', 'wide_tpr', 'wide_fpr', 'wide_fp_episodes_per_min', 'wide_onset_s', 'wide_med_w',
      'acc_close', 'acc_squint', 'acc_normal', 'acc_widen', 'acc_look_down', 'acc_look_up', 'acc_3class_bal',
      'closed_in_open', 'closed_in_brightdark', 'valve_close', 'valve_lookdown_lt06', 'pupil_auc', 'pupil_mm_bright', 'pupil_mm_dark', 'dil_bright', 'dil_dark']
def agg(rows, title):
    print(f'## {title}  (n={len(rows)})')
    for k in KS:
        v = np.array([x[k] for x in rows], float)
        print(f'  {k:26s} mean {np.nanmean(v):7.3f}  min {np.nanmin(v):7.3f}  max {np.nanmax(v):7.3f}')
groups = {}
for x in r:
    groups.setdefault((x['cond'], x['closed_mode'], x['filt']), []).append(x)
for key in sorted(groups):
    agg(groups[key], key)
