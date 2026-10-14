# Airtime model history: AirBalancer, the UEP flatten, measured bitrate inputs, the parked 90 fps canary

**Status:** all superseded — both layers now ride the op's MCS with a per-rung `overhead_base`/`overhead_enh` pair, the bitrate formula has fixed inputs (`kShareBase` 0.60) and delivered rates, presets are decomposed into `venc.ref_*`/`intra_refresh_*` keys, and the probe follows every AU. Current model → docs/airtime-model.md.

Extracted from docs/airtime-model.md during the 2026-10-11 docs tidy. The page was written 2026-08-29 after the AU-completion-jitter investigation (EMA 21 → 5–8 ms), bench-measured on the prod pair at mcs5 / 10 Mbit/s; the sections below are its dated history, verbatim.

## The 2-stream split and the AirBalancer (2026-08-29 → 2026-08-30)

**Later the same day (airtime-balance-uep), the link collapsed to 2
streams (BASE sid0 at `mcs−1`, ENH sid1) and the single `air_bytes`
formula above became per-stream and measurement-anchored rather than
nominal, because the naive `len·(1+ov)` model measured a reproducible
~2× gain error at large frames (SBI/frag framing + repair quantization
it doesn't account for — spike 2,
`docs/findings/2026-08-29-airtime-balance-spike.md`). The split had a
structural limit: at 2:1 rate ratios (the mcs1 rung) the balancer's
rails could not reach air balance — 3.3 ms/pair structural residual —
which motivated a same-rate counter-study (flat vs asymmetric overhead
per rung, static and under motion,
`docs/findings/2026-08-30-same-rate-uep.md`). The drone's `AirBalancer`
tracked the true per-stream multiplier from what actually got
serialized, `m_s = emit_s / len_s` (`emit_s`/`len_s` are α=1/16 EWMAs of
emitted body bytes and frame-unit bytes, IDR frames excluded as 2–10×
outliers), and solved the per-stream air split around that measured
anchor:

    air_s(ov) = len_s · (m_s + (ov − ov_s_applied)) / rate_s

subject to `air_b = air_e` (balanced) and the budget invariant
`len_b·ov_b + len_e·ov_e = (len_b + len_e)·ov_cmd` (repair-byte-neutral:
the split redistributed, it did not add or remove overall FEC bytes).
`rate_b`/`rate_e` were `phy_rate(base_mcs)`/`phy_rate(profile_mcs)` from
the applied ladder, so an active ENH probe's rate flowed in
automatically.

⚠ **Superseded the next day (2026-08-30, same-rate-fixed-pairs).** Both
streams now ride the SAME scored mcs — the `mcs−1` base rate and the
`AirBalancer` solver described just above are both gone. UEP is
expressed only through FEC overhead, and that overhead is a fixed
**per-rung config pair** (`overhead_base`/`overhead_enh`, carried in the
v5 RCF, RC_VERSION 5) applied directly to UEP with no per-frame
redistribution. `AirFeed`, the deleted solver's measurement-only
successor, survived until 2026-09-01 publishing `share_base`/
`excess_base`/`excess_enh` into the bitrate blend; it is now deleted
too — see the bitrate paragraph below. GS-side, a single `budget()`/
`budget_for(rung)` no longer exists: `budget_base()`/
`budget_base_for(rung)` score sid 0 and `budget_enh_for(rung)` scores
sid 1 — see `docs/link-adaptation.md`.

## The enh-probe rate feed-in (2026-09-03 → 2026-09-04)

The encoder's commanded bitrate keeps the two-term shape — a single-rate
`kbps = rate(ladder[1]) · budget / (1 + ov)` is wrong in both directions
once base and enh carry different overhead:

    kbps = airtime_budget / [ f0·(1+ov_base)/rate_b + (1−f0)·(1+ov_enh)/rate_e ]

where `ov_base`/`ov_enh` are the commanded pair off the v5 RCF and `f0`
is a **fixed** base byte share (`kShareBase`, 0.60). **`rate_e` is the
base profile's rate even during an enh probe** (since 2026-09-03): the
probe slot flies the candidate mcs, but feeding that rate in raised the
command 11–15% on every probe entry at rungs 0–3 and lowered it on exit —
two `SetChnAttr` writes, two forced IDRs, on a pipe already at ~65–70%
air — and was the largest single source of >80 ms p50 seconds in
flight-0011 (23 of 48). The 2026-08-05 probe spec always said the probe
changes MCS only; the rate feed-in came with the 2-slot op (2bbaa3f)
expecting the AirBalancer to absorb it, and the balancer is gone.
⚠ SUPERSEDED 2026-09-04: there is no more "enh probe" slot to feed a
rate into at all — the probe-stream redesign (§6 below,
`docs/link-adaptation.md` "Probe stream") moved the candidate-MCS canary
onto its own SBI stream, so ENH always runs the op MCS and `rate_e` is
simply the op rate, no longer a special case. The mechanism this
paragraph describes is historical; the principle it states (a probe must
never change bitrate) is exactly why the probe stream is excluded from
this formula and from `run_bitrate_policy` altogether.

## Measured inputs removed from the bitrate formula (2026-09-01)

⚠ **Measured inputs removed 2026-09-01.** `f0` was AirFeed's live
`share_base`, and each term also carried a measured framing excess
`excess_s = m_s − (1 + ov_s_applied)`. That correction was real —
framing excess runs ~5% at large frames and grows as frames shrink —
but it closed a loop the encoder could not afford: lower rate → smaller
frames → proportionally more padding → higher excess → lower target,
re-evaluated on every RCF at 10–20 Hz against a 100 kbps rounding grid.
On Star6E every `MI_VENC_SetChnAttr` that changes the rate emits a
keyframe that bypasses `idr_rate_limit` (measured 2026-09-01: 15 real
changes → 15 IDRs, one frame later; 16 same-value writes → 0), and in
`flight-0000` an IDR cost a median 63.8 kB against a 21.4 kB base P
frame (38 ms of airtime at its rung, p95 92 ms) while **151 of 226
bitrate writes had no rung change behind them**. The blend was buying a
few percent of airtime accuracy for ~150 keyframes per 12-minute
flight. `f0 = 0.60` is the measured median of that flight (561
one-second windows, p50 0.601, sd 0.044); fixing it there costs
−2.7%/+1.0% of target on the worst pair in the flight (1.0/0.5) and
exactly nothing on an equal pair. The dropped framing excess is not
compensated in the formula — `airtime_budget` is the lever if the
resulting overshoot ever matters. Full derivation of the deleted
solver:
`docs/superpowers/specs/2026-08-29-airtime-balance-uep-design.md` §2;
current architecture and GS-side ladder/attribution consequences:
`docs/link-adaptation.md`; same-rate measurement basis:
`docs/findings/2026-08-30-same-rate-uep.md`.

## The 4-stream flat-refs overhead policy (2026-08-29)

`kUepRefOverhead` (common/include/mabur/uep_encoder.h) is
**{0.50, 0.50, 0.50, 0.50}**, scaled by `cmd_overhead/0.25` and clamped
[0.125, 2.0]. At the mcs5 rung (cmd 0.5) every stream carries 1.0×
overhead → equal air per byte on every class; at rung 0 (cmd 1.0)
everything clamps to 2.0× uniformly. It was {1.00, 0.75, 0.50, 0.25}
(devourer heritage) until 2026-08-29; the flatten happened in two
reviewed steps (s1–s3, then s0) because frame→stream routing is
whole-frame (`classify_frame`: IDR/param-sets → s0, TRAIL_R tid0 → s1,
TRAIL_N and tid≥2 → s3; s2 is unused by current 1:1 SVC-T structures),
so unequal refs = unequal air for equal frames.

Resilience was re-proven at the reduced protection with the loss-sim
rig, not assumed: 5% injected s1 loss → 0.03–0.10% post-FEC residual
(the OLD refs demoted two ladder rungs at just 2%); 5% injected s0 loss
→ zero video gaps and no IDR-request even fired, because rally's intra
stripes heal without IDRs. That last fact is *why* s0 could join the
flat ladder: the belt-and-braces 2.0× on IDRs was sized for a preset
world (ltr) where a lost IDR froze the picture.

**The RcAgent airtime estimator is now exact, not conservative.** It
budgets video air as `bitrate × (1 + eff1)`; under the old refs eff1
(1.5) overstated the true pair-average (~1.0) by ~25%, a hidden margin.
With flat refs the estimate matches reality, so the same 75% airtime cap
admits more commanded bitrate at a given rung, and the ladder's
utilization/`u` readings sit on a new scale (thresholds deliberately NOT
retuned — see the dated scale-break entry in data-provenance.md).

⚠ **Superseded later the same day.** The 4-stream flat-refs policy above
was itself replaced within the day by the 2-stream (BASE sid0/ENH sid1)
literal-overhead policy + `AirBalancer` described in §1 — `kUepRefOverhead`
and the s0..s3 routing this section describes no longer exist.
`link.streams` shrank from 4 entries to 2; every overhead-shaped sideport
value from before this second change is cmd-scale (half the actual air
overhead the same nominal number means after it) — see the dated
"overhead literal + 4→2 stream collapse" entry in data-provenance.md. This
section stays as the record of the flatten investigation that made the
collapse safe (it's what proved base/enh could carry equal, then reduced,
protection without a resilience regression).

## Encoder knobs as written then (presets, `min_iqp` 44, `superframe_p_pct` default 0)

- **Preset choice is the big lever.** `rally` (refPred) equalizes
  base/enhance payloads and spreads intra cost (2 s GOP + 150 ms
  stripes → IDRs only ~1.7× a P frame). `ltr:1` is structurally 1.8:1
  (TRAIL_R references 2-back; windowed CBR won't equalize) and measures
  ~11–12 ms EMA even on flat refs — don't run it if jitter matters.
- **`venc.min_iqp`** (config, 0=leave firmware 12; also volatile via
  `:8301 /venc/set?min_iqp=N`, 1..51, with `max_iqp=N` beside it)
  programs `u32MinIQp`, the I-frame QP floor — **the one direct IDR
  SIZE bound star6e honours.** Bench 2026-09-06 (static 1080p, ROI off,
  `max_ipprop` 2, the 14400→10800 attr-change IDR): floor 12/30/36/42/48
  → IDR 11.6/11.4/5.0/2.6/1.4 kB, P frames unchanged at 23 kB, the
  `SetRcParam` write fires no IDR and the very next IDR honours it.
  `max_iqp` 48→51 did nothing at 1800 kbps. Motivation: the rung-0
  demote IDR (27–41 kB in flight 0356 = 90–140 ms on the wire at mcs0,
  every 75–140 ms over-budget episode) — half of it was `roi_qp_low`
  −24 landing in the keyframe (ROI now disabled in the drone config),
  the rest is the RC anchoring the GOP at a low I-QP. Cost: one soft
  keyframe per transition, refined by the following P frames at no
  extra bytes. Shipped 44 on 2026-09-06 as a first static guess, to be
  refined from the au.log IDR sizes after a flight; a per-rung floor
  (high at rungs 0–1, firmware at the top) is the follow-up if a single
  value proves too coarse on promotes.
- **`venc.superframe_p_pct`** (config, 0=off; also volatile via
  `:8301 /venc/set?superframe_p_pct=N`, 100..1000) programs
  `MI_VENC_SetSuperFrameCfg` REENCODE with a **P-frame** bit threshold of
  N % of the rung's per-frame budget (kbps×1024/(fps×8)), re-derived on
  every bitrate write; I stays unlimited unconditionally because an I
  threshold below the IDR size stalls the channel for good (fork probe
  2026-08-27, `../mabur-fork/mabur-stack-20260828`). That probe is the
  only hardware evidence so far: rung 2, 6000 B cap → P frames 11.1 →
  3.2 kB from the next frame, keyframe-free, 0 drops. **The RC re-plans
  well under the cap**, so this is a quality lever as much as a bound —
  the 2026-09-03 bench motivates it (scene-cut P frames at 2–6× budget,
  `docs/findings/2026-09-03-handover-venc-overshoot.md`); sweep before shipping
  non-zero. Ported 2026-09-03, default 0.

## Where the model could go next (as of 2026-08-29)

- **Below the floor**: headroom. At 2× the air-rate margin the
  serialization slope halves; that's a ladder policy question
  (utilization target per rung), not an encoder or FEC change.
- **Per-rung overhead shape**: the flat ladder is uniform per *stream*;
  the rung's `cmd_overhead` still scales all streams together and the
  clamp at 2.0 flattens rung 0 fully. If a future rung wants
  differentiated protection back, it must pay the jitter price knowingly
  — the arithmetic in §1 prices it (Δair × 0.85 µs/B, ×2 in the EMA).
- **The IDR budget**: with `max_ipprop` bounding the ratio and s0 flat,
  the remaining IDR cost is pure payload (~1.7×P). The only further
  reductions are quality trades (`qp_delta`, tighter ipprop toward the
  1.4 floor) — measured, available, currently not worth it.
- **Explored and parked (2026-08-29): a 90 fps canary layer.** Base
  60 fps guaranteed + 30 fps disposable TRAIL_N on s3 (`ltr:2`-style
  period at 90 fps) would raise the shed floor from 30 to 60 fps, and
  a lighter-FEC canary is the one legitimate future use of asymmetric
  overhead (pair it with source-priority TX scheduling — the decoder's
  systematic fast path already delivers sources immediately; only the
  TX-side source/repair interleaving couples parity to completion time).
  Parked because the GS display is 60 Hz: the visible 90 fps vanishes,
  and the permanent cost (≈1/3 less bits per displayed frame at equal
  rate, or ~1.3–1.5× the air) buys only a rarely-entered shed mode plus
  ~4 ms freshness. Revisit on a >60 Hz display, or if flight data shows
  the 30 fps shed floor actually hurting. Per-stream higher MCS for the
  canary was hardware-refuted as general policy (July uniform-PHY
  ruling: s3 at +1/+2 rungs saw 42% RF loss) — canary-tolerable, but
  the wall is sharp. Any alternating bitrate ratio r:1 prices as
  EMA ≈ 2·Δair/wire-rate; 1-in-N droppable patterns cost ~2/N of the
  every-other-frame damage.

## Probe stream air share, enh-only probe (2026-09-04)

The always-on probe stream (`docs/link-adaptation.md` "Probe stream")
adds one 1405 B body per ENH AU — 30 bodies/s at 30 fps — at the MCS of
rung `current + link.probe.rung_offset`. Air share = 30 × serialization
of 1405 B at that MCS: **≈ 1 % at mcs4-5, ≈ 2.7 % at mcs1** (lower MCS
serializes a fixed byte count more slowly). It is NOT budgeted by
`run_bitrate_policy` or the RcAgent airtime estimator — the probe
changes MCS only, never bitrate (§1's superseded "enh probe" note above
states the same principle for the mechanism this one replaced), so it
costs zero encoder writes and zero IDRs. At the top of the ladder (no
probe commanded, `probe_rung() == -1`) the cost is zero, not 1 %.
