# Link adaptation v2 — three-tier proposal

**Status: PROPOSAL. Nothing here is implemented, built or flown.** Written
2026-09-15 from a design discussion. Every number attributed to a flight,
bench session or source file is real; everything else is derived from the
shipped formulas or is an explicit assumption, and is marked as such.

Read `docs/link-adaptation.md` for what actually ships today. This page
describes a replacement for its control structure, not for its plumbing —
the wire formats, FEC scheme, probe stream and attribution machinery are
all reused unchanged.

Motivating use case, which the current design is not built for: a
freestyle drone in an abandoned industrial park. LOS is interrupted every
few seconds by steel structure, dips are deep (15–20 dB) and short
(200–400 ms), and some are frequency-selective nulls rather than
broadband shadowing.

---

## 1. What ships today

| | current |
|---|---|
| **Who decides** | GS only. Every demote input is measured GS-side on received video; `T_TELEM` is display-only and the ladder never reads it. |
| **MCS** | 5 fixed rungs, mcs 0/1/2/4/5 (`common/src/profile.cpp:100`). Both streams ride the scored MCS (same-rate since 2026-08-30). |
| **FEC overhead** | **Fixed per rung from config** (2.0/1.5/1.0/0.5/0.2, base==enh), carried in the RCF. Never adapted at runtime. |
| **Bitrate** | Derived from overhead + rate against `encoder.airtime_budget` (0.5 shipped): `kbps = 1000·B / [f₀(1+ov_b)/rate_b + (1−f₀)(1+ov_e)/rate_e]`, `f₀ = 0.60` fixed (`drone/src/rc_agent.cpp:208`). |
| **Demote triggers** | s1 util, s1 residual, enh util, enh residual, fade — all transition-attributed, all GS-measured. Confirms 250/500 ms, 100 ms inside the 2.5 s fade regime. |
| **Promote** | Always-on +1 probe stream (SBI sid 5, one 1405 B body per enh AU), gated Clean/Lossy/NoInfo. ~3.1 s/rung measured on the bench climb. |
| **Decision metrics** | pre-FEC loss, residual loss, RSSI, SNR. EVM is **label-only and deliberately excluded** (`gs/src/ladder_controller.h:141`). `fa`/`cca`/`igi` feed channel selection only, never link adaptation. |
| **Actuation latency** | GS decides → RCF → drone applies: p50 48 ms, p90 66 ms, max 88 ms (measured, `docs/switch-loss-findings-2026-09-05.md`). Unbounded if the RCF is lost. |
| **Drone autonomy** | Three narrow mechanisms: congestion shed (TxQueue ≥ half cap), air-clock shed (`shed_ms` 25), and failsafe → `apply_max_range()` (mcs0 / ov 2.0 / enh shed) after `failsafe_ms` = 1000 ms of RCF silence. |

The gap: the fastest possible GS-side reaction is ~650–700 ms (500 ms
window + 100 ms in-regime confirm + delivery + apply), and it requires the
RCF to arrive — in exactly the conditions that lose RCFs. The only
drone-side rate authority is a 1000 ms binary cliff to mcs0.

---

## 2. The unified objective (the core result)

The shipped bitrate formula, with an equal overhead pair (which is what
every shipped rung has, so `f₀` cancels exactly), is:

```
kbps = 1000 · B · rate / (1 + ov)
```

Set the FEC budget to twice the measured pre-FEC loss `L` — i.e. run FEC
at 50 % utilisation, which is the margin the whole design is tuned around:

```
budget = ov/(1+ov) = 2L   ->   1 + ov = 1/(1 − 2L)
```

Substituting:

```
                kbps = 1000 · B · rate · (1 − 2L)
```

So **the entire operating-point decision is one objective: maximise
`rate × (1 − 2L)`**, where `L` is the pre-FEC loss measured at that rate.
A rung with `L ≥ 0.5` is not viable at any overhead. This is exact against
the shipped formula, not an approximation, and it subsumes both control
loops below — the inner loop sets `ov` from `L`, the outer loop picks the
`rate` that maximises the product.

Decision surface for the shipped ladder (kbps of video sustainable, at
`B` = 0.60):

```
        L=  0%     2%     5%     8%    10%    15%    20%    25%
r0 mcs0   3900   3744   3510   3276   3120   2730   2340   1950
r1 mcs1   7800   7488   7020   6552   6240   5460   4680   3900
r2 mcs2  11700  11232  10530   9828   9360   8190   7020   5850
r3 mcs4  23400  22464  21060  19656  18720  16380  14040  11700
r4 mcs5  31200  29952  28080  26208  24960  21840  18720  15600
```

**Consequence that reframes the whole design: demoting is almost never
the right move.** Read across a row versus the row below it. At the
mcs4→mcs2 step (2.0× rate ratio), mcs2 at *zero* loss scores 11700 — which
mcs4 only falls to at `L` = 25 %. So the link should ride mcs4 with
overhead climbing to 1.0 before dropping to mcs2 is even a tie. The
current design, which pins overhead per rung and demotes on loss, gives
away most of that headroom.

**Corollary — the ladder's MCS gaps are too wide.** The 2.0× steps
(mcs4→mcs2, mcs1→mcs0) are where demotion is worst-valued. Inserting mcs3
as a rung splits the mcs2↔mcs4 gap into 1.33× and 1.5× steps and makes
demotion useful much earlier. Cheap to try: it is one config row, no code.
(mcs6/mcs7 are a separate question — `docs/evm-sweep-findings-2026-08-10.md`
shows mcs7 is noise-limited and must stay linear.)

---

## 3. Architecture: three tiers

The current design has one control loop with one actuator (the rung) and
one decision-maker (the GS). The proposal splits it by **timescale**,
which is what the park use case demands:

```
tier 0   REFLEX      drone-side, blind, descend-only      ~100–150 ms
tier 1   INNER       FEC overhead, continuous             ~0.5–1 s
tier 2   OUTER       MCS / rung selection                 seconds
```

Each tier's actuator is independent, so they compose rather than
sequence. There is deliberately **no threshold handoff** between them — a
strict "inner loop escalates to outer loop" ordering would delay the MCS
drop behind the FEC ramp, which is exactly backwards for a fast fade.

### Tier 0 — the reflex (new)

**Problem it solves:** a demote command has to travel on the failing
link. GS-side authority cannot react to a hard cliff, by construction.

**Trigger (drone-local, zero round trip):** the drone already measures
RSSI and SNR on every received RCF — `uplink_track.on_rc_frame(pkt
.RxAtrib.rssi, pkt.RxAtrib.snr)` at `drone/src/main.cpp:1497` — and
currently throws it away into display-only telemetry. Run the same
dual-timescale EWMA the GS's `fade.predict` uses (fast tau 300 ms, slow
2 s rising / 20 s falling) on that signal, drone-side. Second trigger,
free: consecutive missed RCFs (2 misses at `feedback_ms` 50 = 100 ms).

*Assumption requiring validation:* channel reciprocity. Absolute levels
differ (drone omni + 1 chain vs GS patches + 2-card diversity, different
TX powers), but a shadowing event should appear on both directions. Only
the **delta** is used, never the absolute. This has not been measured on
this hardware and is the proposal's largest untested premise.

**Action:** graded descent, not the current binary cliff.

| condition | action |
|---|---|
| Δ RSSI ≳ 10 dB below slow baseline, or 2 RCFs missed | drop 1 rung, shed enh |
| ≳ 18 dB, or 4 RCFs missed | drop toward the floor |
| `failsafe_ms` total silence | existing `apply_max_range()` |

**Strictly descend-only.** The drone never climbs blind. A wrong blind
downshift costs bitrate; a wrong blind upshift costs the link.

**Release on evidence, not a timer** — mirror the GS's latched predictive
trigger, which "releases only on an *observed* recovery". Plus a max-hold
ceiling so a stuck reflex cannot strand the link low.

### Tier 1 — the inner loop (new actuator, existing hook)

**What changes:** FEC overhead stops being a per-rung config constant and
becomes the continuously controlled variable, targeting `budget ≈ 2L`
(50 % utilisation).

**Why this is cheap:** the FEC is a **systematic sliding-window RLC over
GF(256)** — "the sole FEC scheme; block RS is retired"
(`common/include/mabur/sw_encoder.h:15`). Each repair symbol carries its own
`window_start`, `window_len` and `repair_key` in its 14-byte header, so
there is no fixed (k, n) the two ends must agree on. Three consequences,
all verified in the source:

1. `SwEncoder::set_overhead()` — "Takes effect immediately (no block
   boundary to wait for)". `UepEncoder::set_overhead` already fans it out
   per layer (`common/src/uep_encoder.cpp:100`).
2. The decoder's only config-matched field is `symbol_size`
   (`common/src/sw_decoder.cpp:198`, rejected into `symbols_bad_cfg`).
   Overhead is never checked; `window_len` is read per repair from the
   header with only a `kResetSpan` sanity bound. **So overhead can change
   mid-stream with no coordination and no wire change.** Symbol size
   cannot, and must stay config-pinned.
3. Overhead is a `double` with fractional credit accumulation, so it is
   continuous. There is no `n/12` quantisation — that framing came from
   block-RS links (e.g. WFB-ng) and does not apply here.

**Two traps, both drawn from things this repo already got wrong once:**

- **Drive the loop from raw `pre_fec_loss`, never from `u`.** Today
  `u_ = h.pre_fec_loss / budget_base()` (`gs/src/ladder_controller.cpp:310`)
  and `budget = ov/(1+ov)`. If `ov` becomes the actuator, it sits in the
  denominator of its own error signal: raising it lowers `u` with no
  change in the link. Use `u` only as the actuator's readout.
- **Separate FEC changes from bitrate pushes.** `set_overhead()` touches no
  encoder verb, but re-deriving the bitrate calls `MI_VENC_SetChnAttr`,
  and on Star6E every rate-changing write emits an IDR that bypasses
  `idr_rate_limit` (measured 2026-09-01: 15 real changes → 15 IDRs, 16
  same-value writes → 0), at a median 63.8 kB vs a 21.4 kB base P frame.
  The closed loop that did this was **deleted** on 2026-09-01 for
  producing ~150 keyframes per 12-minute flight. So: run overhead
  continuously, push bitrate only on quantised steps behind a dead band
  and a minimum write interval, and keep `f₀` fixed at 0.60 rather than
  measuring it.

### Tier 2 — the outer loop (restructured)

**What changes:** instead of demoting on a loss threshold, pick the rung
that maximises `rate × (1 − 2L)` from §2. `L` per rung comes from:

| rung | source of `L` |
|---|---|
| current | the op stream itself (already measured) |
| +1 | the existing always-on +1 probe |
| −1 | a **conditionally armed** −1 probe (below) |
| anything else | `RungStore` per-rung EWMA history — already kept (`u`, `resid`, `evm_db`, `n`, `age_s`, `dwell_s`, `visits`, `exits_bad`), currently observe-only |

**The +1 probe stays exactly as shipped.** It is validated (every promote
`promote_probed` on the bench climb) and costs nothing measurable: bench
flight-0018 read 59.28 % air with it on vs 59.35 % off.

**The −1 probe is armed, not always-on.** Probe airtime scales with the
inverse of the probe's rate, so downward probes are expensive:

| op rung | +1 probe | −1 probe | −2 probe | all three |
|---|---|---|---|---|
| r1 (mcs1) | 3.7 % | 10.6 % | — | 14.3 % |
| r2 (mcs2) | 2.0 % | 5.4 % | 10.6 % | 18.0 % |
| r3 (mcs4) | 1.5 % | 3.7 % | 5.4 % | 10.7 % |

(Derived: 1405 B fixed body, 60/s, `bytes·8/rate` + ~40 µs PPDU overhead.)

An always-on 3-probe scheme costs 10–18 % air on a pipe whose budget is
0.5–0.6 — it would have to become a term in the bitrate formula and would
directly cost video. Worse, a −1/−2 probe on a *healthy* link is
informationless: at 240 blocks/s, time to observe a single loss event is
0.4 s at `L`=1e-2 but **42 s** at 1e-4 and ~7 min at 1e-5, and ~10× that
for an actual estimate. Parked far from the error cliff it reports "clean"
on every timescale a fade lives on.

Arming it changes the regime, which is the whole point: once the inner
loop has pushed overhead deep, the main stream is carrying real loss, and
X−1 (one 1.33–2.0× rate step up in margin) sits where its PER is
measurable rather than at the floor.

**Arm condition:** overhead deep enough that demotion is arguable — and
from §2 that threshold is **a function of the rate ratio to the rung
below**, not a constant. At a 2.0× step you need `L` ≈ 25 % before the
lower rung ties even at zero loss; at the 1.33× mcs5→mcs4 step it pays far
earlier. Also gate on residual: if `resid`/`resid3` is nonzero while
overhead is deep, FEC is already failing — demote now, do not spend 2–3 s
probing.

**Window:** the −1 probe needs a **longer window than the +1 probe** —
2–3 s, not 500 ms. At `L` ≈ 1 % a 500 ms window sees ~120 blocks against a
1-block quantum, i.e. a 0/1/2 reading. It informs a demote *target*, not
an instant demote, so it can afford the latency.

---

## 4. The anti-fight contract (required, not optional)

With tier 0 in place, the drone can change rate without being told. Two
distinct fights follow, and the fast one is entirely drone-side.

### Fight A — the next RCF clobbers the reflex (~20 Hz)

The drone applies every valid RCF. Reflex drops → an RCF arrives 50 ms
later still commanding the old rung → drone climbs back → reflex fires
again. A 20 Hz thrash, before the GS knows anything.

**Fix, drone-local, no wire change:** a **reflex floor latch**. The drone
records a floor rung and clamps any RCF-commanded op to at most that
floor for the hold's duration, while still honouring everything else in
the RCF (overhead pair, probe profile, shed flags). Precedent is adjacent:
`failsafe_shed_` is "held sticky … until an RCF/DISC takes the agent back
to LINKED" specifically so a later recompute "can't silently clobber it"
(`drone/src/rc_agent.cpp:59-64`).

### Fight B — the GS mis-scores and cascades (seconds)

The GS scores `u_ = h.pre_fec_loss / budget_base()`, and `budget_base()`
indexes `cfg_.ladder[idx_]` — the rung the GS **commanded**. Worked
example, blind drop from r3 (ov 0.50) to r0 (ov 2.00):

- GS believes budget = 0.50/1.50 = 0.333
- drone is actually sending ov 2.0 → real budget = 0.667
- `u_` reads **2× too high** → demote → fade regime opens → 100 ms
  confirms → cascade, with a `SetChnAttr` and a ~63.8 kB IDR per step

It converges to the same rung by accident, having paid several IDRs on a
link already in trouble.

**Fix — the GS must observe rather than assume.** The information is
already on every frame and needs no protocol: `RxBody::mcs` comes off the
RX PHY descriptor (`gs/src/radio_frontend.cpp:245-248`) and is already fed
to the decoder as the attribution generation boundary
(`gs/src/aggregator.cpp:289`). Decisively, it is **"valid independent of
the body CRC"** (`common/include/mabur/node.h`) — so it still reads during
the fade, when bodies are failing FCS. Nothing else competes: an SBI
header field needs the body to arrive *and* its header to parse, and
telemetry is 1 Hz and display-only.

Three changes, all GS-side:

1. **Carry observed MCS into `LinkHealth`** — a per-sid **mode** over a
   short window (mode, not mean: it is categorical, and 255 = unknown must
   be excluded, same discipline as the EVM zero-sentinel rule).
2. **Score against where the drone actually is.**
   `ladder_controller.cpp:289` is one line — `measured_rung_ = idx_;` — and
   `measured_rung()` exists for exactly this class of bug already ("the
   loss is filed against the rung the link demoted TO, and the rung that
   actually caused it never appears"). Resolve it from the observed MCS,
   then score `u_` against `budget_base_for(measured_rung_)`.
3. **A `Following` state.** When the observed rung sits below the
   commanded one persistently, the controller adopts the observed rung,
   **suspends the promote path**, books no demote and no rung penalty
   (nothing was tried and failed — same reasoning as the probe gate's
   `Lossy → hold, no penalty`), remembers the pre-reflex rung, and
   suppresses `on_tick`'s timeout drop (`:606`) so the GS does not
   panic-drop on top of a reflex it is already following.

**Disambiguating the RCF-lag tail.** A *commanded* rung change also makes
observed ≠ commanded for p50 48 / p90 66 / max 88 ms. `ProbeTrack` already
solved this: lag-tail bodies carrying a different profile are booked
`off_profile` and are "not scored", not "lost". Same rule — observed ==
*previously* commanded inside the tail window is lag; matching neither
current nor previous command is drone-initiated; require a few frames'
confirm. Free bonus: mcs3/6/7 are off-ladder and unambiguously
drone-chosen.

### Fast restore — where the park problem is actually won

The cold climb is ~3.1 s/rung through the probe gate, so recovering from
the floor to r3 takes 9–12 s. With an obstruction every few seconds, a
reflex without a fast restore makes the link **worse than no reflex** —
this is the proposal's single most important design constraint.

The GS already knows the rung it commanded before adopting, so **no wire
field is needed to carry a restore target.** On a short clean confirm,
restore directly to the remembered rung, bypassing the rung-by-rung
climb. Guard it: once only, never above the remembered rung, and only
after the drone's hold has released — then hand authority back.

---

## 5. Metrics: what to add

Everything in tier A below is already counted and merely not a decision
input.

**A — available now, plumbed, orthogonal to PER**

1. **Sub-block salvage density** — the best one. `rx.keep_corrupted = true`
   is on (and devourer only honoured it on Jaguar3 from 2026-09-08, which
   is why per-card `crc_fail` read 0 for the project's whole history), so
   FCS-failed PPDUs still reach the decoder and CRC-clean sub-blocks get
   salvaged. `subblocks_salvaged`, `bodies_corrupt` and especially
   `arr_salvage_only` give a **graded** corruption measure at a far higher
   event rate than whole-body loss — which directly attacks the
   statistical-resolution problem that limits every probe. A
   "distance to the cliff" signal that moves before PER does.
2. **FCS-fail rate vs seq-gap rate** (`crc_fail/frames` vs
   `seq_expected/seq_received`) — separates *heard-but-corrupt*
   (SNR/EVM-limited, graceful, MCS downshift helps) from *not-heard-at-all*
   (deep fade, collision, self-blanking; MCS downshift may not help). In
   the park case this also separates broadband shadowing from a
   frequency-selective null, which want opposite responses: a null leaves
   RSSI near-flat while killing subcarriers, and FEC helps more than MCS.
3. **EVM z-score against the rung's own baseline.** The sweep is emphatic:
   never threshold raw EVM (−24 dB is "healthy at mcs4" and "6 dB degraded
   at mcs7" simultaneously) — use deviation from that rung's baseline, and
   `RungStore` already accumulates `evm_db` *and* `evm_sd_db` per rung. So
   `(evm_now − rung.evm_db)/rung.evm_sd_db` is ready-made. It also yields a
   specific diagnostic: **EVM worsening while RSSI/SNR holds or rises = PA
   compression**, where downshifting MCS is the wrong answer.
4. **The energy block** (`fa_ofdm`, `foreign`, `igi` in `cards[i].energy`)
   — the interference axis, independent of PER, already plumbed by the
   channel-select work. PER up *with* `fa`/`foreign` up = interference
   (a collision kills any MCS; FEC or a channel change is the answer); PER
   up with `fa` flat and RSSI falling = range (MCS downshift is right).
   **The most valuable addition for deciding which tier should act.**
   Documented caveats: own-beacon leak contaminates it, and `cca − own`
   clamps to 0 under A-MPDU, so on an active link the usable score is
   `fa + foreign` only.
5. **Per-card divergence** — per-card counters already sit beside the union
   "purely for the *which card collapsed at range* question". Spread
   between them flags antenna-pattern loss, fixable by attitude rather
   than by downshifting.

**B — in hardware, not plumbed**

6. **Absolute noise floor.** `abs_noise_floor_dbm` exists in `ScoutEnergy`
   and `dev_cfg.rx.abs_noise_floor` is on, but it needs `with_nhm=true`,
   costing ~2 ms + ~10 ms USB, explicitly kept off the 60 fps RX path.
   RSSI alone cannot separate "signal weaker" from "noise louder" — which
   is precisely the MCS-vs-FEC question. Sample at 1 Hz on one card. The
   NHM histogram bins exist in devourer's `RxEnergy` but only `nhm_valid`
   is carried into mabur's subset.

**Not worth adding:** anything ACK- or retry-derived. There is no ACK, no
retry, and `tuning.disable_cca = true` — it is pure injection, so
minstrel-style success EWMAs have no source. Also not goodput: it is
supply-limited by the encoder, so it measures the bitrate policy, not the
link.

---

## 6. Comparison summary

| | current | proposed |
|---|---|---|
| Decision authority | GS only | GS (tiers 1–2) + drone reflex (tier 0), with an explicit follow contract |
| Worst-case reaction | ~650–700 ms, unbounded if the RCF is lost | ~100–150 ms, delivery-free |
| FEC overhead | fixed per rung, config | continuously controlled from raw `pre_fec_loss` |
| Bitrate | re-derived on every rung change | quantised steps, dead band, decoupled from FEC changes |
| Rung choice | loss threshold → demote | maximise `rate × (1 − 2L)` |
| Downward information | none (demote is blind to what's below) | conditionally armed −1 probe + `RungStore` history |
| Recovery from the floor | 9–12 s (~3.1 s/rung, probe-gated) | direct restore to the remembered rung |
| Drone rate autonomy | binary cliff to mcs0 after 1000 ms silence | graded, RSSI-triggered, latched, descend-only |
| GS response to drone autonomy | none — mis-scores and cascades | observes `RxBody::mcs`, adopts, suspends promote |
| Metrics driving decisions | pre-FEC loss, residual, RSSI, SNR | + salvage density, FCS-vs-gap split, EVM z-score, `fa`/`foreign` |
| Wire changes required | — | **none for the minimum** |

That last row is the main practical point: the reflex latch is local, and
the GS learns the drone's rate from the PHY descriptor. No RC_VERSION
bump, no flag day, benchable without a two-device deploy.

---

## 7. Suggested implementation order

Each step is independently valuable and independently revertable.

1. **GS observed-MCS scoring + `Following` state.** Self-contained, no
   drone change, unit-testable (`LadderController` is "pure decision logic
   — no clock, no I/O, no radio types"). Fixes a real latent bug today:
   the existing `apply_max_range()` failsafe *already* creates the
   mis-scoring of Fight B, and nothing handles it.
2. **Add mcs3 as a ladder rung.** One config row, no code. Tests §2's
   claim that the 2.0× gaps are the problem.
3. **Metrics as observe-only exports** (salvage density, FCS-vs-gap, EVM
   z-score, `fa + foreign`). Ship them to the sideport and
   `flightreport.py` and fly a few park sessions before any of them gates
   a decision.
4. **Tier 1 inner loop**, behind a config flag, bitrate decoupled and
   dead-banded from the start.
5. **Tier 0 reflex**, observe-only first: log what it *would* have done
   against recorded flights before arming it. This is the staging pattern
   the air-clock gate used (`shed_ms` 0 = observe-only) and it worked.
6. **Tier 2 restructure + armed −1 probe**, last, once §2's objective has
   been checked against real per-rung `L` data.

Gates, per CLAUDE.md: `tools/bench/ausniff.py` for anything touching
maburgs, plus `tools/bench/aucadence.py` for anything touching the
bitrate policy or UEP overhead — which tiers 1 and 2 both do.

---

## 8. Open questions and untested premises

1. **Uplink/downlink reciprocity** (tier 0's trigger). Unmeasured on this
   hardware. Falsifiable cheaply: log drone uplink RSSI alongside the GS's
   downlink RSSI through a few obstruction events and correlate the
   deltas. **Do this before building tier 0** — the whole tier rests on it.
2. **Whether `L` at X−1 is genuinely measurable when X is deep.** §3
   argues it from waterfall steepness, but the PER-vs-SNR curves for this
   chip at these rungs have not been measured. The `RungStore` history
   already being collected can answer it retrospectively.
3. **Fast-restore stability.** Restoring directly to the remembered rung
   after a 200–400 ms dip risks a restore→re-dip→reflex cycle at pillar
   spacing. Needs a real park recording to tune, and may need the restore
   itself to be probe-gated after the first few cycles.
4. **Whether a genuine fast fade even matches the model.** Every recorded
   fade to date is ramp-type under ~0.45 dB/s (26 loss-driven demote
   episodes, flights 0017/0018); "a genuine FAST fade (obstruction,
   multipath null) has still never been recorded against this trigger."
   The park case is asserted from the use case, not from data. A recording
   session there is the cheapest next step in this whole document.
5. **RLC rank deficiency.** Unlike RS, a random linear code can emit
   linearly dependent repairs (~1/256 per symbol at GF(256), so sub-1 %
   effective waste). Harmless today, but a tier 1 loop running FEC close
   to its target margin should carry a little slack for it.
