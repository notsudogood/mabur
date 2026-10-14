# Probe per AU: why, costs, and the 2026-09-16 bench

**Status:** shipped — the drone trails every video AU with a probe (60/s at 60 fps SVC-T), the gate counts its streak in bodies (`link.probe.clean_bodies` 90, `max_util` 0.05); current mechanism → docs/link-adaptation.md "Probe stream".

Extracted from docs/link-adaptation.md during the 2026-10-11 docs tidy.

**Why.** The gate is a zero-loss run test: a streak of n bodies passes
with probability (1−p)^n at candidate-rung PPDU loss p, so its resolution
is a body count. The 2026-09-06 flights held 90 bodies (3 s at 30/s) and
18 % of probed promotes still failed inside 2 s, all after a 90/90-clean
streak — 90 bodies can only reject ≈3.3 % PPDU loss at 95 % (rule of
three) and rung 5 fails below 1 %. The goal here is not more resolution
but the SAME resolution in half the wall time: the promote latency is
bounded by `probe.clean_bodies` and `link.clean_ms` together (block 6
requires both streaks), so both came down.

**What changed.**
- The drone trails EVERY video AU with a probe, base and enh
  (`probe_follows()` in `drone/src/probe_source.h`: any video sid while a
  probe is commanded, never a shed layer's AU). 60 fps SVC-T ⇒ 60
  probes/s; 30 fps ⇒ 30/s; enh shed ⇒ base still probes, so the gate no
  longer drops to NoInfo whenever the enh layer is absent (30 fps with
  SVC-T off used to mean no probe at all, and NoInfo holds every promote).
- `ProbeTrack::on_au(sid, fid)` books one expectation per video AU
  (`sid < UepEncoder::kNumStreams`); base and enh share one frame-id
  counter, so the off-profile cancel path is unchanged.
- The RcfSlotter treats a base completion like an enh one while a probe is
  commanded (`probe_follows = true`): the release is the probe's arrival,
  or the learned tail deadline if it is lost. Releasing at the base
  completion would put the RCF blast exactly on the base-tail probe — the
  2.5× GS-inflicted probe loss of 2026-09-04 again.
- `link.probe.clean_ms` → `link.probe.clean_bodies` (default 90; the old
  key fails boot). `LinkHealth::probe_bodies_total` (ProbeTrack union
  `expected_blocks / bpb`) feeds `ProbeGate::streak_bodies`, counted from
  the streak's first Clean sample. Sideport `link.probe.streak_bodies`
  replaces `streak_ms`; maburtop prints `54b`.
- The loss quantum halves: one lost body over the 500 ms window is
  1/30 / 0.333 = **0.1** at 60/s (0.2 at 30/s). `max_util` 0.15 → **0.05**
  in the bundle config so it still means "zero loss allowed" at any frame
  rate; 0.1–0.199 would now allow one loss, the setting flight 20 showed
  fails in ~1 s.
- Bundle config: `link.clean_ms` 3000 → 1500, `link.probe.clean_bodies`
  90, `link.probe.max_util` 0.05. Promote latency ≈ 1.5 s + RCF lag at
  60 fps, unchanged confidence.

**Costs.** Probe airtime doubles: ≈0.8 % of air at rung 4 (probe at mcs5),
≈2.8 % at rung 0 (probe at mcs1), where the budget already runs tight —
the bench "nil cost" of 2026-09-04 was measured at mcs5 only. Twice as
many bodies are in flight across each RCF lag, so `off_profile` per rung
change should read ~2–12, not 2–6. Mismatched pair: new drone + old GS
reads arrived > expected (loss clamps to 0, blind gate); old drone + new
GS reads 50 % loss and never promotes — deploy both ends together, no
wire bytes change. What this does NOT fix: the probe is one short PPDU
while video flies as agg6 aggregates ~6× longer on air, so it
underestimates per-aggregate loss; that bias is the remaining fast-fail
lever (SNR floor at the candidate rung, or `rung_offset 2`).

**Bench 2026-09-16 (deployed both ends, commit 61f19e7).** Session
`/media/dvr/log/0104` (the ctl.log's second `ctllog` header is the new
pair; the first two climbs in that file are the boot-time old binaries —
both boards had been power-cycled ~10 min before the deploy).
- Cold climb 0→5, all `promote_probed`, **1.85 s/rung** (was ~3.1–3.5):
  `noinfo` at the rung change, `clean` at +0.30–0.36 s, promote +1.5 s
  later = 90 bodies at 60/s. Repeated on a second adaptive restart.
- `ausniff` 60.5 fps, 0 fid gaps, 908/908 (twice); `aucadence` +2.78 ms
  (inside the 2.4–2.9 band).
- Pinned mcs4 / probe mcs4, 254 s, 15 255 probe bodies at **59.9/s**
  (7628 base-tail, 7627 enh-tail, joined to au.log by fid): probe body
  loss **0.052 % on base tails, 0.066 % on enh tails**, no partial
  bodies, vs the enh stream's own 0.17 % symbol loss — the base-tail
  probe is not blasted by the RCF slotter.
- `off_profile` 10 over one 5-rung climb (2/change, inside 2–12).
- Rung-0 air: pinned mcs0 with the probe at mcs1 (60/s) vs no probe —
  drone `air_backlog_max_ms` ≤ 6, `air_shed` never, `txq` depth ≤ 2.
  ⚠ The sideport `link.air_pct` (60.1 vs 60.4 %, n 100 each) sums only
  streams 0/1, so it cannot see the probe's share; analytically the
  probe costs ≈ 60 × 0.91 ms = **5.5 % of air at rung 0** (2.7 % before),
  ≈ 1.6 % at rung 4.

