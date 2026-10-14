# fec.log first bench read: is the rung overhead pair oversized?

**Status:** a one-session bench read taken when fec.log landed, under the 2026-09-16 ladder's 1.0/0.5 overhead pair. The shipped ladder is now 0.5/0.25 on every rung (gs/bundle/maburgs.default.toml). The fec.log format and the FEC EPISODES report are in docs/observability.md ("fec.log").

Extracted from docs/observability.md during the 2026-10-11 docs tidy.

The FEC EPISODES section of `flightreport.py` is meant as input to a
*static* retune of `[[link.ladder]]`'s `overhead_base`/`overhead_enh`: a
single lost agg-6 aggregate at ov 1.0 / w 32 models to ~0.5, so the
shipped (at the time) 1.0/0.5 pair was the first thing a flight's max/p99
would judge.

Bench 2026-09-16 (session 0088, 80 s at rung 5, ausniff clean): base
episodes are one lost aggregate each — `m` p50/max 11/16 against `r`
≈ 41, `ov_req` p50/max 0.40/0.46, zero would-fail at 0.50; enh (ov 0.5)
`m` 12–16 against `r` ≈ 21, `ov_req` 0.38; ~0.75 episodes/s, the known
~1 lost agg-6/s.

⚠ The first ~18 s after a maburgs restart log a block of rung-0 episodes
with `m` ≈ 200 and ~170 abandoned, `stale` 0 — the same boot warm-up loss
that pollutes the rung-0 residual EWMA (`link.rungs[0]`), not a rung-0
verdict; drop them by time, or read only rungs the ladder actually held.
