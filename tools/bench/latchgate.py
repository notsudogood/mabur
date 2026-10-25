#!/usr/bin/env python3
"""latchgate.py -- the release thread's bench gate: missed vsync latches and
release-thread lateness over a time window, read straight from a maburplay
debug-log session directory (frame.log + reg.log; docs/observability.md).

A "miss" is a frame.log row whose kernel latch ran a vblank late:
`t_flip - t_commit > 0.75 x the panel period` (frame.log's own `t_flip -
t_commit` is "the kernel's latch", docs/observability.md "frame.log"). A
commit made on time latches about one lead after it; one that missed its
vblank latches a period later than that, so three quarters of a period
splits the two at any lead short of half a period. The period is the
median `t_flip` interval between consecutive frame.log rows in the window
(the panel's period while the source keeps up with it; a frame dropped or
repeated now and then does not move a median), clamped to 4-50 ms, and
16.667 ms when the window has fewer than two rows. A source well below the
panel rate reads its own frame interval here, not the panel's: pass
`--miss-ms` then. A row with `t_commit == 0` (the presenter parked the
frame instead of committing it) has no latch to measure and is never a
miss. `dsp`/`e2e` are frame.log's own
per-frame segment columns (seg[6]/seg[7]) -- NOT reg.log tokens: the
regulator line carries no `dsp=`/`e2e=` field, only `rlate=`/`hand=` for
the release thread's own lateness and hand-over, so this reads dsp/e2e as
true percentiles over the window's frames and rlate/hand as the max of
each 1 Hz reg.log line's own already-aggregated p99/max (a window p99
recomputed from raw per-frame samples is not recoverable from reg.log).

Exclusions. Two kinds of event disturb presentation for a few frames
without saying anything about the release thread, and reg.log counts both
cumulatively:
  flush     `flushes=` (every output park: a ring flush, a decode-watchdog
            reset or recreate, a display hotplug, shutdown) or `disconts=`
            (an encoder pts discontinuity, a drone restart);
  recstart  `recstarts=` (a recorder start).
A rise between two consecutive reg.log lines carrying the counter means
the event happened somewhere in that gap; the EARLIER line's stamp is
taken as its time (the conservative bound: it widens the span rather than
narrowing it). The event's span then covers
  - misses: the first `--skip-after-flush N` frame.log rows at or after
    that stamp (they still count in `frames N`), and
  - pend growth: the growth between those two reg.log lines -- pend is a
    1 Hz counter, so the gap the event fell in is its finest grain.
Nothing outside a span is excluded: a miss past the N rows, or pend growth
in a later gap, counts. What was excluded is reported apart, as
`excluded: flush M pend P, recstart M pend P` (a row or gap in both kinds
of span counts once, as flush). An event already reflected in the very
first reg.log line of a session cannot be detected (nothing to compare it
against) and goes unexcluded; a counter that goes backwards is a player
restart, not an event.

`reg.log`'s `rlate=`/`hand=`/`flushes=`/`recstarts=` tokens arrived with
reglog 3; `# reglog 2` rows (and reglog-2-only "off" lines) lack them,
print `-` for rlate and hand, and exclude on `disconts=` alone. Reglog 4
appends `burn_in=`/`burn_enc=`/`burn_drop=` (the burned DVR's cumulative
counters); this gate parses them but does not gate on them.
`frame.log` needs the `t_commit`/`pbits` columns (framelog 3) that misses
are computed from; an older frame.log is refused as unreadable.

Usage: python3 tools/bench/latchgate.py <session-dir> --from <mono_s>
         --to <mono_s> [--skip-after-flush 24] [--miss-ms <ms>]
  <session-dir>       a debug-log session directory (tools/session.py);
                       must have both frame.log and reg.log
  --from/--to          the analysis window, CLOCK_MONOTONIC seconds --
                       the same clock frame.log's `t_flip` and reg.log's
                       row stamp are already in (no unit conversion, no
                       relative-to-first-row rebasing)
  --skip-after-flush   frame.log rows after a detected flush or recorder
                       start to exclude from the miss count (default 24)
  --miss-ms            the miss threshold in ms, overriding 0.75 x the
                       estimated panel period

All latency figures in the printed line (pend/pdrop excepted, which are
cumulative counts) are microseconds, frame.log/reg.log's native unit.

`pend`/`pdrop`'s `+P`/`+D` are reset-tolerant growth across the window's
reg.log rows, not last-minus-first: a player restart resets both
counters to 0 mid-window, and a plain subtraction would read that as a
negative delta that cancels real misses out of the exit-code sum (the
same convention as tools/flightreport.py's `_counter_growth` -- a value
going backwards contributes its post-restart value, never a negative
one).

Exit code 1 when `misses + pend-delta > 0`, both net of the exclusions
(`pdrop` is reported whole and does not itself trip the gate), 0
otherwise, 2 if the session directory or its logs can't be read.

(tests/test_latchgate.py)
"""
import argparse
import bisect
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import session as _session  # tools/session.py

MISS_FRACTION = 0.75           # of the panel period
DEFAULT_PERIOD_US = 16667      # fewer than two rows to estimate from
PERIOD_CLAMP_US = (4000, 50000)


def growth_step(prev, v):
    """One step of a reset-tolerant cumulative counter (see
    counter_growth)."""
    return v if v < prev else v - prev


def counter_growth(vals):
    """Reset-tolerant delta across a cumulative counter's values in a
    window, same convention as tools/flightreport.py's _counter_growth: a
    player restart (the counter going backwards, e.g. pend 10 -> 0) is not
    a negative delta, it is a reset, and the post-restart value is the
    growth since it (0, then 0 -> 2 contributes +2). A plain monotonic
    counter still just sums to last-first. Needs at least 2 values to mean
    anything; the caller is responsible for that check."""
    total = 0
    prev = None
    for v in vals:
        if prev is not None:
            total += growth_step(prev, v)
        prev = v
    return total


def pct(values, q):
    if not values:
        return None
    s = sorted(values)
    return s[min(len(s) - 1, int(q * len(s)))]


def parse_frame_log(path):
    """(version, rows); rows = [{'t_flip','t_commit','dsp','e2e'}], sorted
    by t_flip. Rows shorter than framelog-3's 22 columns are skipped (an
    older row has no t_commit/pbits to compute a miss from)."""
    version = None
    rows = []
    with open(path) as f:
        for line in f:
            if line.startswith('# framelog '):
                try:
                    version = int(line[len('# framelog '):].split()[0])
                except (ValueError, IndexError):
                    pass
                continue
            if line.startswith('#') or not line.strip():
                continue
            tok = line.split()
            if len(tok) < 22:
                continue
            try:
                rows.append({'t_flip': int(tok[0]), 'dsp': int(tok[17]),
                             'e2e': int(tok[18]), 't_commit': int(tok[20])})
            except ValueError:
                continue
    rows.sort(key=lambda r: r['t_flip'])
    return version, rows


COUNTERS = ('pend', 'pdrop', 'disconts', 'flushes', 'recstarts',
            'burn_in', 'burn_enc', 'burn_drop')


def parse_reg_log(path):
    """(version, rows); rows = [{'t', 'pend'?, 'pdrop'?, 'disconts'?,
    'flushes'?, 'recstarts'?, 'burn_in'?, 'burn_enc'?, 'burn_drop'?,
    'rlate_p99'?, 'rlate_max'?, 'hand_p99'?, 'hand_max'?}], sorted by t. A
    key is present only when the line carried it (regulator disabled -> no
    pend/pdrop/disconts; reglog 2 -> no rlate/hand/flushes/recstarts;
    reglog <4 -> no burn_in/burn_enc/burn_drop)."""
    version = None
    rows = []
    with open(path) as f:
        for line in f:
            if line.startswith('# reglog '):
                try:
                    version = int(line[len('# reglog '):].split()[0])
                except (ValueError, IndexError):
                    pass
                continue
            if line.startswith('#') or not line.strip():
                continue
            tok = line.split()
            if len(tok) < 2 or tok[1] != 'regulator:':
                continue
            try:
                t = int(tok[0])
            except ValueError:
                continue
            kv = dict(t.split('=', 1) for t in tok[2:] if '=' in t)
            row = {'t': t}
            for key in COUNTERS:
                if key in kv:
                    try:
                        row[key] = int(kv[key])
                    except ValueError:
                        pass
            if 'rlate' in kv and 'hand' in kv:
                try:
                    rp99, rmax = (int(x) for x in kv['rlate'].split('/'))
                    hp99, hmax = (int(x) for x in kv['hand'].split('/'))
                    row['rlate_p99'], row['rlate_max'] = rp99, rmax
                    row['hand_p99'], row['hand_max'] = hp99, hmax
                except ValueError:
                    pass
            rows.append(row)
    rows.sort(key=lambda r: r['t'])
    return version, rows


def event_gaps(reg_rows, keys):
    """[(t_a, t_b)] for every rise of any counter in `keys` between two
    consecutive reg.log rows carrying it: the event happened in (t_a,
    t_b], and t_a -- the EARLIER row's stamp -- is taken as its time (the
    conservative proxy -- see module docstring)."""
    out = set()
    for key in keys:
        prev = None
        for r in reg_rows:
            if key not in r:
                continue
            if prev is not None and r[key] > prev[key]:
                out.add((prev['t'], r['t']))
            prev = r
    return sorted(out)


def skip_indices(frame_rows, gaps, n):
    """Indices into `frame_rows` (already sorted by t_flip) that fall in
    the first `n` rows at-or-after each gap's start."""
    if n <= 0 or not gaps:
        return set()
    flips = [r['t_flip'] for r in frame_rows]
    out = set()
    for t_a, _ in gaps:
        i = bisect.bisect_left(flips, t_a)
        out.update(range(i, min(len(frame_rows), i + n)))
    return out


def in_gaps(t0, t1, gaps):
    """True when the reg.log step (t0, t1] overlaps one of `gaps`."""
    return any(t0 < t_b and t1 > t_a for t_a, t_b in gaps)


def panel_period_us(rows):
    """Median t_flip interval between consecutive rows (see the module
    docstring), clamped; DEFAULT_PERIOD_US with fewer than two rows."""
    iv = [b['t_flip'] - a['t_flip'] for a, b in zip(rows, rows[1:])]
    if not iv:
        return DEFAULT_PERIOD_US
    iv.sort()
    med = iv[len(iv) // 2] if len(iv) % 2 else (iv[len(iv) // 2 - 1] + iv[len(iv) // 2]) / 2
    return min(max(med, PERIOD_CLAMP_US[0]), PERIOD_CLAMP_US[1])


def analyze(session_dir, t0_s, t1_s, skip_after_flush=24, miss_ms=None):
    sess = _session.resolve(session_dir)
    if sess.frame is None or sess.reg is None:
        raise LookupError(f"{session_dir}: missing frame.log or reg.log")
    fversion, frame_rows = parse_frame_log(sess.frame)
    rversion, reg_rows = parse_reg_log(sess.reg)
    if fversion is None or fversion < 3:
        raise ValueError(
            f"{sess.frame}: needs '# framelog 3' or later (has {fversion}); "
            "misses are computed from the t_commit/pbits columns framelog 3 adds")
    if rversion is None:
        raise ValueError(f"{sess.reg}: no '# reglog N' marker")

    lo, hi = t0_s * 1e6, t1_s * 1e6
    flush_gaps = event_gaps(reg_rows, ('flushes', 'disconts'))
    rec_gaps = event_gaps(reg_rows, ('recstarts',))
    skip_flush = skip_indices(frame_rows, flush_gaps, skip_after_flush)
    skip_rec = skip_indices(frame_rows, rec_gaps, skip_after_flush)

    win = [(i, r) for i, r in enumerate(frame_rows) if lo <= r['t_flip'] <= hi]
    miss_us = (miss_ms * 1000.0 if miss_ms is not None
               else MISS_FRACTION * panel_period_us([r for _, r in win]))
    misses = ex_flush_m = ex_rec_m = 0
    for i, r in win:
        if r['t_commit'] == 0 or r['t_flip'] - r['t_commit'] <= miss_us:
            continue
        if i in skip_flush:
            ex_flush_m += 1
        elif i in skip_rec:
            ex_rec_m += 1
        else:
            misses += 1
    dsp_vals = [r['dsp'] for _, r in win]
    e2e_vals = [r['e2e'] for _, r in win]

    win_reg = [r for r in reg_rows if lo <= r['t'] <= hi]

    def delta(key):
        vals = [r[key] for r in win_reg if key in r]
        return counter_growth(vals) if len(vals) >= 2 else 0

    # pend growth step by step, each step charged to the gate or to the
    # exclusion whose gap it overlaps.
    pend = ex_flush_p = ex_rec_p = 0
    prev = None
    for r in win_reg:
        if 'pend' not in r:
            continue
        if prev is not None:
            g = growth_step(prev['pend'], r['pend'])
            if in_gaps(prev['t'], r['t'], flush_gaps):
                ex_flush_p += g
            elif in_gaps(prev['t'], r['t'], rec_gaps):
                ex_rec_p += g
            else:
                pend += g
        prev = r

    rl = [r for r in win_reg if 'rlate_p99' in r]
    return {
        'frames': len(win), 'misses': misses, 'miss_us': miss_us,
        'pend_delta': pend, 'pdrop_delta': delta('pdrop'),
        'excluded': {'flush': (ex_flush_m, ex_flush_p), 'recstart': (ex_rec_m, ex_rec_p)},
        'rlate': (max(r['rlate_p99'] for r in rl), max(r['rlate_max'] for r in rl)) if rl else None,
        'hand': (max(r['hand_p99'] for r in rl), max(r['hand_max'] for r in rl)) if rl else None,
        'dsp_p50': pct(dsp_vals, .5),
        'e2e_p50': pct(e2e_vals, .5), 'e2e_p99': pct(e2e_vals, .99),
    }


def fmt(v):
    return '-' if v is None else str(v)


def fmt_pair(p):
    return '-' if p is None else f"{p[0]}/{p[1]}"


def format_line(r):
    return (f"frames {r['frames']}  misses {r['misses']} "
            f"(late-latch rows, flip-commit > {r['miss_us'] / 1000.0:.1f} ms)  "
            f"pend +{r['pend_delta']}  pdrop +{r['pdrop_delta']}  "
            f"rlate {fmt_pair(r['rlate'])}  hand {fmt_pair(r['hand'])}  "
            f"dsp {fmt(r['dsp_p50'])}  e2e {fmt(r['e2e_p50'])}/{fmt(r['e2e_p99'])}  "
            f"excluded: flush {r['excluded']['flush'][0]} pend {r['excluded']['flush'][1]}, "
            f"recstart {r['excluded']['recstart'][0]} pend {r['excluded']['recstart'][1]}")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.split('\n\n')[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    ap.add_argument('session_dir')
    ap.add_argument('--from', dest='t0', type=float, required=True)
    ap.add_argument('--to', dest='t1', type=float, required=True)
    ap.add_argument('--skip-after-flush', type=int, default=24)
    ap.add_argument('--miss-ms', type=float, default=None)
    args = ap.parse_args()

    try:
        r = analyze(args.session_dir, args.t0, args.t1, args.skip_after_flush, args.miss_ms)
    except (OSError, LookupError, ValueError) as e:
        print(f"latchgate: {e}", file=sys.stderr)
        sys.exit(2)

    print(format_line(r))
    sys.exit(1 if (r['misses'] + r['pend_delta']) > 0 else 0)


if __name__ == '__main__':
    main()
