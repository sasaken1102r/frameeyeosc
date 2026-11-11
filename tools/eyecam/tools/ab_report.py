"""Before/after report over `eyecam-rec --replay` CSVs (standard library only).

    python3 -I ab_report.py [--group NAME] --pair REC_DIR BEFORE.csv AFTER.csv [--pair ...] [--blinks L:59.9,R:70.9]
                            [--json OUT.json]

Each --pair is one recording replayed twice (before / after, same options otherwise); --group starts a new set of
pairs (totals per set). The measures follow tools/eyecam/README.md ("枠の外の瞳" section):
  - Valve close: Valve's openness under 0.3 for 3 samples or more; caught when the camera says closed within 0.15 s
    of it.
  - short close: a closed run under 0.4 s; "with Valve" when Valve's openness is under 0.5 within 0.15 s of it.
  - false close: closed while Valve >= 0.6, runs of 0.3 s or more (README: frames whose nearest Valve sample is >= 0.6).
    Valve lags the camera by about 0.2 s and misses some real blinks, so a second count takes whole closed runs of
    0.3 s or more and calls them false only when Valve stays >= 0.6 from 0.1 s before to 0.4 s after the run.
  - false half-shut: eye_lid < 0.5 (not closed) while Valve >= 0.6, share of those Valve-open frames.
  - pupil share: frames with the pupil, all frames and in normal steps.
  - cue recordings: closed share in close steps, eye_wide > 0.5 share in widen steps (recall) and in neutral steps
    (false), eye_squint > 0.5 in squint steps, the widen step b_w - b_n (median).
  - per step: closed share, the box ratio over the closed-eye rule's line (box_ratio against the long-run open level,
    and box_rule_ratio against the line in use when written), gate rejections, small pupils kept, pupil share, pupil
    size against the normal steps', eye_lid < 0.5 while not closed.
  - dropped pupils (--drop-pupil replays): drop bursts with a closed frame during them or within 0.1 s after.
  - --blinks E:T: whether eye E reads closed within 0.3 s of t_rel T (cue time), for blinks Valve may miss.
The step labels are the CSV's (cue steps without their first 0.8 s).
"""
import csv
import json
import os
import sys

NEUTRAL = ('normal', 'look_up', 'look_down', 'bright', 'dark')


def num(s):
    """Float of a CSV cell, NaN when empty."""
    try:
        return float(s)
    except (TypeError, ValueError):
        return float('nan')


def fin(x):
    """x is a finite number."""
    return x == x and abs(x) != float('inf')


def med(v):
    """Median of the finite values (NaN when none)."""
    v = sorted(x for x in v if fin(x))
    if not v:
        return float('nan')
    n = len(v)
    return v[n // 2] if n % 2 else 0.5 * (v[n // 2 - 1] + v[n // 2])


def share(flags):
    """Share of true values (NaN when empty)."""
    flags = list(flags)
    return sum(1 for x in flags if x) / len(flags) if flags else float('nan')


def load_csv(path):
    """Rows per eye, sorted by camera time."""
    eyes = {'L': [], 'R': []}
    with open(path, newline='') as fh:
        for r in csv.DictReader(fh):
            d = {k: (v if k in ('eye', 'label') else num(v)) for k, v in r.items()}
            eyes[r['eye']].append(d)
    for e in eyes:
        eyes[e].sort(key=lambda d: d['t_cam'])
    return eyes


def load_valve(rec):
    """Valve's sample times and openness per eye."""
    t, o = [], {'L': [], 'R': []}
    with open(os.path.join(rec, 'valve.csv'), newline='') as fh:
        for r in csv.DictReader(fh):
            t.append(num(r['sample_time']))
            o['L'].append(num(r['open_l']))
            o['R'].append(num(r['open_r']))
    return t, o


def load_t0(rec):
    """t_raw of the first cue (t_rel = t - t0 + its t_rel), or None without cues."""
    path = os.path.join(rec, 'cues.csv')
    if not os.path.exists(path):
        return None
    with open(path, newline='') as fh:
        rows = list(csv.DictReader(fh))
    return (num(rows[0]['t_raw']) - num(rows[0]['t_rel'])) if rows else None


def runs(rows, flag):
    """Runs of consecutive frames where flag(row): (first index, last index)."""
    out, start = [], None
    for i, d in enumerate(rows):
        if flag(d):
            if start is None:
                start = i
        elif start is not None:
            out.append((start, i - 1))
            start = None
    if start is not None:
        out.append((start, len(rows) - 1))
    return out


def run_len(rows, a, b):
    """Seconds a run of frames a..b covers (one frame interval past its last frame)."""
    dt = rows[b]['t_cam'] - rows[b - 1]['t_cam'] if b > 0 else 1.0 / 90
    return rows[b]['t_cam'] - rows[a]['t_cam'] + dt


def valve_closes(valve, eye):
    """Valve closes: openness < 0.3 for 3 samples or more, as (start, end) times."""
    t, o = valve[0], valve[1][eye]
    out, start, n = [], None, 0
    for i, v in enumerate(o):
        if fin(v) and v < 0.3:
            if start is None:
                start, n = i, 0
            n += 1
        else:
            if start is not None and n >= 3:
                out.append((t[start], t[i - 1]))
            start = None
    if start is not None and n >= 3:
        out.append((t[start], t[-1]))
    return out


def valve_min(valve, eye, a, b):
    """Valve's lowest openness between times a and b (NaN without samples)."""
    t, o = valve[0], valve[1][eye]
    lo = bisect_left(t, a)
    v = [o[i] for i in range(lo, len(t)) if t[i] <= b and fin(o[i])]
    return min(v) if v else float('nan')


def bisect_left(t, x):
    """First index with t[i] >= x (t sorted)."""
    lo, hi = 0, len(t)
    while lo < hi:
        m = (lo + hi) // 2
        if t[m] < x:
            lo = m + 1
        else:
            hi = m
    return lo


def eye_metrics(rows, valve, eye, t0, blinks):
    """All measures for one eye of one replay."""
    m = {}
    closed = lambda d: d['closed'] == 1
    cl_runs = runs(rows, closed)
    times = [d['t_cam'] for d in rows]
    # Valve closes caught
    vc = valve_closes(valve, eye)
    caught = 0
    for a, b in vc:
        i, j = bisect_left(times, a - 0.15), bisect_left(times, b + 0.15)
        caught += any(rows[k]['closed'] == 1 for k in range(i, min(j + 1, len(rows))))
    m['valve_closes'] = len(vc)
    m['valve_caught'] = caught
    # Short closes
    short = [(a, b) for a, b in cl_runs if run_len(rows, a, b) < 0.4]
    m['short_closes'] = len(short)
    m['short_closes_valve'] = sum(
        1 for a, b in short if valve_min(valve, eye, rows[a]['t_cam'] - 0.15, rows[b]['t_cam'] + 0.15) < 0.5)
    # False closes, README rule: closed while the nearest Valve sample >= 0.6, runs >= 0.3 s
    fr = [(a, b) for a, b in runs(rows, lambda d: closed(d) and fin(d['v_open']) and d['v_open'] >= 0.6)
          if run_len(rows, a, b) >= 0.3]
    m['false_close_n'] = len(fr)
    m['false_close_s'] = sum(run_len(rows, a, b) for a, b in fr)
    m['false_close_max_s'] = max((run_len(rows, a, b) for a, b in fr), default=0.0)
    # False closes, windowed: whole closed runs >= 0.3 s with Valve >= 0.6 from 0.1 s before to 0.4 s after
    fw = [(a, b) for a, b in cl_runs if run_len(rows, a, b) >= 0.3
          and valve_min(valve, eye, rows[a]['t_cam'] - 0.1, rows[b]['t_cam'] + 0.4) >= 0.6]
    m['false_close_win_n'] = len(fw)
    m['false_close_win_s'] = sum(run_len(rows, a, b) for a, b in fw)
    # False half-shut
    vo = [d for d in rows if fin(d['v_open']) and d['v_open'] >= 0.6]
    m['half_shut_frames'] = sum(1 for d in vo if not closed(d) and fin(d['eye_lid']) and d['eye_lid'] < 0.5)
    m['half_shut_share'] = m['half_shut_frames'] / len(vo) if vo else float('nan')
    m['closed_valve_open_frames'] = sum(1 for d in vo if closed(d))
    # Pupil share
    m['frames'] = len(rows)
    m['pupil_share'] = share(d['ok_pupil'] == 1 for d in rows)
    normal = [d for d in rows if d['label'] == 'normal']
    m['pupil_share_normal'] = share(d['ok_pupil'] == 1 for d in normal)
    # Cue steps
    by = {}
    for d in rows:
        if d['label']:
            by.setdefault(d['label'], []).append(d)
    if by:
        m['close_acc'] = share(closed(d) for d in by.get('close', []))
        m['widen_recall'] = share(fin(d['eye_wide']) and d['eye_wide'] > 0.5 for d in by.get('widen', []))
        neu = [d for lab in NEUTRAL for d in by.get(lab, [])]
        m['widen_false'] = share(fin(d['eye_wide']) and d['eye_wide'] > 0.5 for d in neu)
        m['squint_acc'] = share(fin(d['eye_squint']) and d['eye_squint'] > 0.5 for d in by.get('squint', []))
        m['widen_step'] = med(d['b_w'] - d['b_n'] for d in rows)
    # Per step (cue step number), with the pupil size against the normal steps'
    has_rule = any(fin(d.get('box_rule_ratio', float('nan'))) for d in rows)
    a_norm = med(d['pupil_a'] for d in normal if d['ok_pupil'] == 1)
    steps = {}
    for d in rows:
        if d['label']:
            steps.setdefault(int(d['step']), []).append(d)
    m['steps'] = []
    for k in sorted(steps):
        s = steps[k]
        ratio = 'box_rule_ratio' if has_rule else 'box_ratio'
        m['steps'].append({
            'step': k, 'label': s[0]['label'], 'frames': len(s),
            'closed': share(closed(d) for d in s),
            'pupil': share(d['ok_pupil'] == 1 for d in s),
            'box_over': share(d.get('box_ratio', float('nan')) > 1.3 for d in s),
            'rule_over': share(d.get(ratio, float('nan')) > 1.3 for d in s),
            'rule_med': med(d.get(ratio, float('nan')) for d in s),
            'gated': share(d.get('gated', 0) == 1 for d in s),
            'small_kept': share(d.get('small_kept', 0) == 1 for d in s),
            'pupil_size': med(d['pupil_a'] for d in s if d['ok_pupil'] == 1) / a_norm if a_norm else float('nan'),
            'lid_lt_05': share(not closed(d) and fin(d['eye_lid']) and d['eye_lid'] < 0.5 for d in s),
        })
    # Dropped pupils: closed during or within 0.1 s after a drop burst
    drops = runs(rows, lambda d: d.get('dropped', 0) == 1)
    if drops:
        hit = 0
        for a, b in drops:
            j = bisect_left(times, rows[b]['t_cam'] + 0.1)
            hit += any(rows[k]['closed'] == 1 for k in range(a, min(j + 1, len(rows))))
        m['drop_bursts'] = len(drops)
        m['drop_bursts_closed'] = hit
    # Listed real blinks: closed within 0.3 s of the given t_rel
    if t0 is not None:
        m['blinks'] = []
        for e, tr in blinks:
            if e != eye:
                continue
            t = t0 + tr
            i, j = bisect_left(times, t - 0.3), bisect_left(times, t + 0.3)
            m['blinks'].append((tr, any(rows[k]['closed'] == 1 for k in range(i, min(j, len(rows))))))
    return m


SUM_KEYS = ('valve_closes', 'valve_caught', 'short_closes', 'short_closes_valve', 'false_close_n', 'false_close_s',
            'false_close_win_n', 'false_close_win_s', 'half_shut_frames', 'closed_valve_open_frames', 'frames')


def fmt(v):
    """Compact number."""
    if isinstance(v, float):
        if not fin(v):
            return '-'
        return f'{v:.3f}' if abs(v) < 100 else f'{v:.1f}'
    return str(v)


def main(argv):
    pairs, group, blinks, out_json = [], 'all', [], None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == '--group':
            group = argv[i + 1]
            i += 2
        elif a == '--pair':
            pairs.append((group, argv[i + 1], argv[i + 2], argv[i + 3]))
            i += 4
        elif a == '--blinks':
            for item in argv[i + 1].split(','):
                e, t = item.split(':')
                blinks.append((e, float(t)))
            i += 2
        elif a == '--json':
            out_json = argv[i + 1]
            i += 2
        else:
            sys.exit(__doc__)
    report = []
    for grp, rec, before, after in pairs:
        valve = load_valve(rec)
        t0 = load_t0(rec)
        res = {'group': grp, 'rec': os.path.basename(rec.rstrip('/')), 'eyes': {}}
        for side, path in (('before', before), ('after', after)):
            rows = load_csv(path)
            for e in ('L', 'R'):
                res['eyes'].setdefault(e, {})[side] = eye_metrics(rows[e], valve, e, t0, blinks)
        report.append(res)
    if out_json:
        with open(out_json, 'w') as fh:
            json.dump(report, fh, indent=1)
    show = ('valve_caught', 'valve_closes', 'short_closes_valve', 'short_closes', 'false_close_n', 'false_close_s',
            'false_close_win_n', 'false_close_win_s', 'half_shut_share', 'pupil_share', 'pupil_share_normal',
            'close_acc', 'widen_recall', 'widen_false', 'squint_acc', 'widen_step', 'drop_bursts_closed',
            'drop_bursts')
    for res in report:
        print(f"== {res['group']} / {res['rec']}")
        for e in ('L', 'R'):
            b, a = res['eyes'][e]['before'], res['eyes'][e]['after']
            cells = [f'{k}={fmt(b[k])}->{fmt(a[k])}' for k in show if k in b or k in a]
            print(f'  {e}: ' + ' '.join(c.replace('=-->-', '=-') for c in cells if '=-->-' not in c))
            for tr, ok in a.get('blinks', []):
                okb = dict(b.get('blinks', [])).get(tr)
                print(f'     blink t_rel {tr}: caught {okb} -> {ok}')
    # Per-step tables (recordings with cues)
    for res in report:
        for e in ('L', 'R'):
            b, a = res['eyes'][e]['before'], res['eyes'][e]['after']
            if not a['steps']:
                continue
            print(f"-- steps {res['rec']} {e} (before -> after): closed, rule_over (box ratio > 1.3 on the rule's box), "
                  f"rule_med, box_over (full box), gated, small_kept, pupil, pupil_size, lid<0.5")
            sb = {s['step']: s for s in b['steps']}
            for s in a['steps']:
                o = sb.get(s['step'], {})
                cols = ('closed', 'rule_over', 'rule_med', 'box_over', 'gated', 'small_kept', 'pupil', 'pupil_size',
                        'lid_lt_05')
                print(f"   {s['step']:2d} {s['label']:10s} " + ' '.join(
                    f"{c}={fmt(o.get(c, float('nan')))}->{fmt(s[c])}" for c in cols))
    # Totals per group
    groups = []
    for res in report:
        if res['group'] not in groups:
            groups.append(res['group'])
    for g in groups:
        tot = {side: {k: 0.0 for k in SUM_KEYS} for side in ('before', 'after')}
        pmin = {side: 1.0 for side in ('before', 'after')}
        for res in report:
            if res['group'] != g:
                continue
            for e in ('L', 'R'):
                for side in ('before', 'after'):
                    m = res['eyes'][e][side]
                    for k in SUM_KEYS:
                        tot[side][k] += m[k]
                    pmin[side] = min(pmin[side], m['pupil_share'])
        b, a = tot['before'], tot['after']
        print(f'== total {g}: ' + ' '.join(f'{k}={fmt(b[k])}->{fmt(a[k])}' for k in SUM_KEYS)
              + f" pupil_share_min={fmt(pmin['before'])}->{fmt(pmin['after'])}")


if __name__ == '__main__':
    main(sys.argv[1:])
