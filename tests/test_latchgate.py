#!/usr/bin/env python3
"""Test for tools/bench/latchgate.py: the release-thread bench gate reads a
synthetic frame.log/reg.log pair and reports missed vsync latches
(`t_flip - t_commit` past 0.75 of the panel period) and the regulator's
pend/pdrop delta over a --from/--to window.

Covers: a miss plus a pend delta (exit 1), a clean pair (exit 0), `#
reglog 2` tolerance (no rlate/hand -> `-`), `--skip-after-flush`
excluding a miss right after a rise in reg.log's cumulative `disconts=`
or `flushes=` while still counting one further out, a `recstarts=` rise
excluding misses and pend growth in its span only, the excluded counts
reported apart, the miss threshold following the panel period (with the
`--miss-ms` override), and `# reglog 4`'s `burn_in=`/`burn_enc=`/
`burn_drop=` tokens parsing through without tripping the gate.
"""
import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest

LATCHGATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..',
                          'tools', 'bench', 'latchgate.py')

# Loaded by path (not a package import) so parse_reg_log can be asserted on
# directly, next to the subprocess-level checks the rest of this file uses.
_spec = importlib.util.spec_from_file_location('latchgate', LATCHGATE)
latchgate = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(latchgate)


def frame_row(t_flip, t_commit, dsp=4000, e2e=9000):
    pts = t_flip % (1 << 32)
    t_submit = t_flip - 20000
    t_complete = t_flip - 18000
    t_decoded = t_flip - 10000
    t_release = t_flip - 8000
    # t_flip pts fid sid len flags slices t_submit t_complete t_decoded
    # t_release enc dq air fec dec reg dsp e2e exact t_commit pbits
    return (f"{t_flip} {pts} 1 0 1000 0x00 1 {t_submit} {t_complete} "
            f"{t_decoded} {t_release} 500 200 300 100 1000 200 {dsp} {e2e} 1 "
            f"{t_commit} 0x00")


def reg_row(t, pend=0, pdrop=0, disconts=0, rlate=None, hand=None, flushes=None,
            recstarts=None):
    base = (f"{t} regulator: held=1 late=0 replaced=0 disconts={disconts} "
            f"hold_ema=0.00ms present_jitter=0.00ms vsync=locked skips=0 "
            f"fallback=0 pend={pend} heals=0 pdrop={pdrop} chained=0 chain=0 "
            f"chain_max=0 chains=0 cuts=0 alw=2.0ms lslot=0 lock=off "
            f"lerr=0.00ms loff=0 lctr=0.0 present=60 acq=1/2 req=1/2 "
            f"ioctl=300/900")
    if rlate is not None:
        base += f" rlate={rlate[0]}/{rlate[1]} hand={hand[0]}/{hand[1]} rt=1"
    if flushes is not None:
        base += f" flushes={flushes} recstarts={recstarts}"
    return base


def write(d, name, text):
    with open(os.path.join(d, name), 'w') as f:
        f.write(text)


def run(d, t0=0, t1=2, skip=24, extra=()):
    return subprocess.run(
        [sys.executable, LATCHGATE, d, '--from', str(t0), '--to', str(t1),
         '--skip-after-flush', str(skip), *extra],
        capture_output=True, text=True)


def periodic_rows(n, period, misses, miss_latch, clean_latch=5000):
    """n frame.log rows `period` µs apart; rows in `misses` latch
    `miss_latch` µs after their commit, the rest `clean_latch`."""
    rows = []
    for i in range(n):
        t_flip = i * period
        rows.append(frame_row(t_flip, t_flip - (miss_latch if i in misses else clean_latch)))
    return rows


class LatchgateTest(unittest.TestCase):
    def test_miss_and_pend_delta_trip_the_gate(self):
        with tempfile.TemporaryDirectory() as d:
            rows = [
                frame_row(1_000_000, 995_000),       # 5 ms, clean
                frame_row(1_016_666, 1_000_666),     # 16 ms, a miss
                frame_row(1_033_332, 1_028_332),     # 5 ms, clean
            ]
            write(d, 'frame.log', '# framelog 3\n' + '\n'.join(rows) + '\n')
            write(d, 'reg.log', '# reglog 3\n'
                  + reg_row(1_000_000, pend=3, rlate=(100, 200), hand=(50, 100)) + '\n'
                  + reg_row(1_500_000, pend=4, rlate=(120, 250), hand=(60, 110)) + '\n')
            r = run(d)
        self.assertIn('misses 1', r.stdout, r.stdout)
        self.assertIn('pend +1', r.stdout, r.stdout)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)

    def test_clean_pair_exits_zero(self):
        with tempfile.TemporaryDirectory() as d:
            rows = [
                frame_row(1_000_000, 995_000),
                frame_row(1_016_666, 1_011_666),
                frame_row(1_033_332, 1_028_332),
            ]
            write(d, 'frame.log', '# framelog 3\n' + '\n'.join(rows) + '\n')
            write(d, 'reg.log', '# reglog 3\n'
                  + reg_row(1_000_000, pend=3, rlate=(100, 200), hand=(50, 100)) + '\n'
                  + reg_row(1_500_000, pend=3, rlate=(90, 180), hand=(45, 90)) + '\n')
            r = run(d)
        self.assertIn('misses 0', r.stdout, r.stdout)
        self.assertIn('pend +0', r.stdout, r.stdout)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_reglog_2_prints_dash_for_rlate_and_hand(self):
        with tempfile.TemporaryDirectory() as d:
            rows = [
                frame_row(1_000_000, 995_000),
                frame_row(1_016_666, 1_011_666),
            ]
            write(d, 'frame.log', '# framelog 3\n' + '\n'.join(rows) + '\n')
            write(d, 'reg.log', '# reglog 2\n'
                  + reg_row(1_000_000, pend=3) + '\n'
                  + reg_row(1_500_000, pend=3) + '\n')
            r = run(d)
        self.assertIn('rlate -', r.stdout, r.stdout)
        self.assertIn('hand -', r.stdout, r.stdout)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_skip_after_flush_excludes_only_the_near_miss(self):
        with tempfile.TemporaryDirectory() as d:
            rows = []
            for i in range(40):
                t_flip = i * 16667
                miss = i in (10, 30)
                t_commit = t_flip - (16000 if miss else 5000)
                rows.append(frame_row(t_flip, t_commit))
            write(d, 'frame.log', '# framelog 3\n' + '\n'.join(rows) + '\n')
            # disconts rises between the two reg.log lines: a discontinuity
            # happened somewhere in (0, 200000] us; the proxy anchors the
            # flush at the EARLIER line's stamp (t=0), so --skip-after-flush
            # 24 (the default) drops frame.log rows 0..23 -- row 10's miss
            # is in that span, row 30's is not.
            write(d, 'reg.log', '# reglog 3\n'
                  + reg_row(0, pend=0, disconts=0, rlate=(100, 200), hand=(50, 100)) + '\n'
                  + reg_row(200_000, pend=0, disconts=1, rlate=(100, 200), hand=(50, 100)) + '\n')
            r = run(d, t0=0, t1=1, skip=24)
        self.assertIn('frames 40', r.stdout, r.stdout)
        self.assertIn('misses 1', r.stdout, r.stdout)

    def test_flushes_rising_excludes_the_near_miss_and_that_gaps_pend(self):
        # flushes= rises between the lines at 0 and 200000 us (a ring
        # flush, a watchdog reset or a hotplug parked the output in that
        # gap): rows 0..23 after the earlier stamp lose their misses, and
        # the pend growth across that gap (+2) is excluded; the miss at row
        # 30 is outside the span and still counts, and so does the pend
        # growth of the next gap (+1).
        with tempfile.TemporaryDirectory() as d:
            write(d, 'frame.log', '# framelog 3\n'
                  + '\n'.join(periodic_rows(40, 16667, (10, 30), 16000)) + '\n')
            write(d, 'reg.log', '# reglog 3\n'
                  + reg_row(0, pend=0, rlate=(100, 200), hand=(50, 100),
                            flushes=0, recstarts=0) + '\n'
                  + reg_row(200_000, pend=2, rlate=(100, 200), hand=(50, 100),
                            flushes=1, recstarts=0) + '\n'
                  + reg_row(600_000, pend=3, rlate=(100, 200), hand=(50, 100),
                            flushes=1, recstarts=0) + '\n')
            r = run(d, t0=0, t1=1, skip=24)
        self.assertIn('frames 40', r.stdout, r.stdout)
        self.assertIn('misses 1', r.stdout, r.stdout)
        self.assertIn('pend +1', r.stdout, r.stdout)
        self.assertIn('excluded: flush 1 pend 2, recstart 0 pend 0', r.stdout, r.stdout)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)

    def test_recstarts_rising_excludes_misses_and_pend_in_its_span_only(self):
        # A recorder start between the lines at 0 and 300000 us: the miss
        # at row 5 (inside the 24 rows after the earlier stamp) and that
        # gap's pend growth (+3) are excluded and reported as such. The
        # miss at row 40 and the next gap's pend growth (+1) are outside
        # the span and still trip the gate.
        with tempfile.TemporaryDirectory() as d:
            write(d, 'frame.log', '# framelog 3\n'
                  + '\n'.join(periodic_rows(60, 16667, (5, 40), 16000)) + '\n')
            write(d, 'reg.log', '# reglog 3\n'
                  + reg_row(0, pend=0, rlate=(100, 200), hand=(50, 100),
                            flushes=0, recstarts=0) + '\n'
                  + reg_row(300_000, pend=3, rlate=(100, 200), hand=(50, 100),
                            flushes=0, recstarts=1) + '\n'
                  + reg_row(700_000, pend=4, rlate=(100, 200), hand=(50, 100),
                            flushes=0, recstarts=1) + '\n')
            r = run(d, t0=0, t1=1, skip=24)
        self.assertIn('misses 1', r.stdout, r.stdout)
        self.assertIn('pend +1', r.stdout, r.stdout)
        self.assertIn('excluded: flush 0 pend 0, recstart 1 pend 3', r.stdout, r.stdout)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)

    def test_an_exclusion_alone_passes_the_gate(self):
        # Everything bad in the window sits in a recorder start's span.
        with tempfile.TemporaryDirectory() as d:
            write(d, 'frame.log', '# framelog 3\n'
                  + '\n'.join(periodic_rows(40, 16667, (3,), 16000)) + '\n')
            write(d, 'reg.log', '# reglog 3\n'
                  + reg_row(0, pend=0, rlate=(100, 200), hand=(50, 100),
                            flushes=0, recstarts=0) + '\n'
                  + reg_row(300_000, pend=1, rlate=(100, 200), hand=(50, 100),
                            flushes=0, recstarts=1) + '\n')
            r = run(d, t0=0, t1=1, skip=24)
        self.assertIn('misses 0', r.stdout, r.stdout)
        self.assertIn('pend +0', r.stdout, r.stdout)
        self.assertIn('excluded: flush 0 pend 0, recstart 1 pend 1', r.stdout, r.stdout)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_miss_threshold_follows_a_100hz_panel(self):
        # 10 ms between flips: a latch past 0.75 of the period (7.5 ms) is
        # a miss, so the 8 ms row counts and the 5 ms rows do not.
        # --miss-ms overrides the estimate.
        with tempfile.TemporaryDirectory() as d:
            write(d, 'frame.log', '# framelog 3\n'
                  + '\n'.join(periodic_rows(30, 10000, (12,), 8000)) + '\n')
            write(d, 'reg.log', '# reglog 3\n'
                  + reg_row(0, rlate=(100, 200), hand=(50, 100)) + '\n'
                  + reg_row(200_000, rlate=(100, 200), hand=(50, 100)) + '\n')
            r = run(d, t0=0, t1=1)
            o = run(d, t0=0, t1=1, extra=('--miss-ms', '9'))
        self.assertIn('misses 1', r.stdout, r.stdout)
        self.assertIn('flip-commit > 7.5 ms', r.stdout, r.stdout)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn('misses 0', o.stdout, o.stdout)
        self.assertIn('flip-commit > 9.0 ms', o.stdout, o.stdout)
        self.assertEqual(o.returncode, 0, o.stdout + o.stderr)

    def test_missing_logs_exit_2(self):
        with tempfile.TemporaryDirectory() as d:
            r = run(d)
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)

    def test_restart_shaped_counter_never_goes_negative(self):
        # pend 10 -> 0 -> 2 is a player restart mid-window, not a drop of
        # 8: last-minus-first would read "pend +-8", which cancels a real
        # miss out of the misses+pend-delta exit-code sum. The growth is
        # 0 (10 -> 0, a reset) + 2 (0 -> 2) = +2, and the miss alone must
        # still trip the gate.
        with tempfile.TemporaryDirectory() as d:
            rows = [
                frame_row(1_000_000, 995_000),      # clean
                frame_row(1_016_666, 1_000_666),    # 16 ms, a miss
                frame_row(1_033_332, 1_028_332),    # clean
            ]
            write(d, 'frame.log', '# framelog 3\n' + '\n'.join(rows) + '\n')
            write(d, 'reg.log', '# reglog 3\n'
                  + reg_row(1_000_000, pend=10, pdrop=5, rlate=(100, 200), hand=(50, 100)) + '\n'
                  + reg_row(1_100_000, pend=0, pdrop=0, rlate=(100, 200), hand=(50, 100)) + '\n'
                  + reg_row(1_500_000, pend=2, pdrop=1, rlate=(120, 250), hand=(60, 110)) + '\n')
            r = run(d)
        self.assertIn('misses 1', r.stdout, r.stdout)
        self.assertIn('pend +2', r.stdout, r.stdout)
        self.assertIn('pdrop +1', r.stdout, r.stdout)
        self.assertNotIn('+-', r.stdout, r.stdout)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)

    def test_reglog_4_rows_parse(self):
        # reglog 4 appends burn_in=/burn_enc=/burn_drop= (the burned DVR's
        # cumulative counters); the gate parses them and still runs clean on
        # an otherwise-clean pair.
        with tempfile.TemporaryDirectory() as d:
            rows = [
                frame_row(1_000_000, 995_000),
                frame_row(1_016_666, 1_011_666),
            ]
            write(d, 'frame.log', '# framelog 3\n' + '\n'.join(rows) + '\n')
            write(d, 'reg.log', '# reglog 4\n'
                  + reg_row(1_000_000, rlate=(100, 200), hand=(50, 100))
                  + ' burn_in=1 burn_enc=1 burn_drop=0\n'
                  + reg_row(1_500_000, rlate=(100, 200), hand=(50, 100))
                  + ' burn_in=1 burn_enc=1 burn_drop=0\n')
            _, reg_rows = latchgate.parse_reg_log(os.path.join(d, 'reg.log'))
            r = run(d)
        self.assertEqual(reg_rows[0]['burn_drop'], 0)
        self.assertIn('misses 0', r.stdout, r.stdout)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == '__main__':
    unittest.main()
