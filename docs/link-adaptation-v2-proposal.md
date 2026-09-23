# Link adaptation v2 — three-tier proposal

**Status: PARTLY IMPLEMENTED, NOTHING FLOWN.** Written 2026-09-15 from a
design discussion; implementation started 2026-09-16. Every number
attributed to a flight, bench session or source file is real; everything
else is derived from the shipped formulas or is an explicit assumption,
and is marked as such.

| piece | state |
|---|---|
| Ladder default was a stale flight ladder (§2.1 trap) | **fixed**, failsafe-only now |
| Observed-MCS scoring + `Following` adopt (§4, Fight B) | **landed**, `FollowCfg`, default ON |
| Tier 1 FEC-overhead policy (§3) | **landed**, `link.overhead`, default **OFF** (observe-only) |
| Tier 1 arming | comparison flight **flown 2026-09-23** (`ov_target` 0.50 vs commanded 1.00, 100% of samples) — but see `docs/rung5-standing-queue-findings-2026-09-23.md` §6: arming it as designed spends the saving on +33% video, not headroom, and rung 5's defect is the headroom |
| Tier 0 reflex (§3) | **not started** — blocked on the reciprocity measurement in §9.1 |
| Tier 2 wire (**RC_VERSION 10**, `kProbeStreamIdDn`) | **landed**, inert — GS never sets the byte yet |
| Tier 2 objective + arm logic | **landed**, `link.objective`, default **OFF** |
| Tier 2 ladder *decision* | **not wired** — `objective.act = true` is rejected at load |
| Fast restore (§4) | **landed**, `link.follow.restore`, default **OFF** — built on the hop's own `restore()`; `restore_target` exported either way |
| Fast-restore cycling guard (§9.3) | **landed** — a restore the drone undoes inside `restore_trial_ms` penalizes that rung, so the existing ledger backoff spaces retries |
| Metrics (§6) | **not started** — and §6's energy metric may be partly removed on `gilankpam/mabur` master, see §8a |
| Merge with `gilankpam/mabur` master (`ca3ad5d`) | **done** — RC_VERSION 10, 18-byte head; tier 1/2 now respect the hop's store blank |
| Tier 1's `min_ov` floor | **fixed** — 0.3 → 0.5, sourced from `fec.log`'s `ov_req`; rung 5 only, see §8a |
| §2.1's "flat overhead pair" framing | **corrected 2026-09-19** — master ships a two-step pair now; the finding and the worked example survive, see §2.1 |

⚠ **Updated 2026-09-23: this branch HAS now been on a device** — one
96.8 s bench session on the RunCam WiFilink pair, observe-only, written up
in `docs/rung5-standing-queue-findings-2026-09-23.md`. It did not validate
the tiers; it found that rung 5 runs with a ~220 ms standing queue and no
headroom, which changes what arming tier 1 is worth (§6 there). The host
gate passes (137/138; the one failure is environmental), and `gs_e2e` +
`gs_au_e2e` pass — so the drone→GS path has run end to end on RC_VERSION 9.
The `ausniff` and `aucadence` device gates are STILL NOT run, and
`aucadence` is the one tiers 1 and 2 both need before either is armed —
now with a specific question to answer, since mcs5 was the only rung with a
negative completion→probe offset in that session.

Building `maburgs` requires devourer at HEAD rather than the pinned
submodule, now for TWO independent reasons: `f3b76ea` predates both
`card_scan_usb.cpp`'s `DeviceProbe.h` and the hop scout's
`GetRxEnergyScout`. Note `gilankpam/mabur` master pins the SAME `f3b76ea`
while calling `GetRxEnergyScout`, so that master does not build against
its own pinned submodule either — the pin is stale in both lines and
everyone is building against the sibling `../devourer` checkout. Not
bumped here; that is a separate decision.

Read `docs/link-adaptation.md` for what actually ships today. Note when
reading the code that there are TWO ladders: the 6-rung struct default in
`gs/src/config.h:145` (a single failsafe mcs0 rung since the §2.1 trap was
fixed) is a fallback used only when the config omits `link.ladder`, and the
SHIPPED flight ladder in `gs/bundle/maburgs.default.toml` replaces it
wholesale — mcs 0-5, flat 1.0/0.5 on this branch, two-step
0.5/0.5/0.5/1/1/1 on `gilankpam/mabur` master (§2.1). `common/src/profile.cpp`'s `profile_table()` is a
third list and is NOT the ladder — it is reached only by tests and
`apply_max_range()`. Every number in this page is the flight ladder.

This page describes a replacement for the control structure, not for the
plumbing —
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
| **MCS** | 6 rungs, mcs 0/1/2/3/4/5, capped by `max_mcs = 5` (`gs/bundle/maburgs.default.toml:65-93`). Consecutive, so rung index == MCS here. Both streams ride the scored MCS (same-rate since 2026-08-30). |
| **FEC overhead** | **Fixed per rung, never adapted at runtime.** Flat 1.0 base / 0.5 enh on all six rungs on this branch; a two-step 0.5→1.0 base / 0.25→0.5 enh between rungs 2 and 3 on `gilankpam/mabur` master (§2.1). Either way it is a CONSTANT the RCF carries, not an actuator, so the controller's only lever is the rate. |
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

Take the shipped bitrate formula with an equal overhead pair, where `f₀`
cancels exactly (the shipped pair is NOT equal — see the caveat below —
but the equal case is where the result is legible):

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

**Caveat on the exact form.** `(1 − 2L)` above is the equal-protection
case (`ov_base == ov_enh`, where `f₀` cancels). The shipped pair is
unequal — 1.0 / 0.5 — so the general form is

```
kbps = 1000 · B · rate / [ f₀/(1 − 2L_base) + (1 − f₀)/(1 − 2L_enh) ]
```

which collapses to `rate · (1 − 2L)` when the two layers see the same
loss and carry the same overhead. Still monotone in `rate` and decreasing
in both losses, so the ranking logic is unchanged; only the constant
moves. Use the simple form for reasoning, the general one for code.

Where the ladder actually sits (`B` = 0.5, `f₀` = 0.60; no `L` term,
because today's overhead does not respond to loss at all). Both flown
pairs, since they differ — this branch's flat 1.0/0.5 and
`gilankpam/mabur` master's two-step (§2.1):

```
                  THIS BRANCH (flat)      MASTER (two-step)
rung/mcs  speed   video    step   bud_b    video    step   bud_b
   0      6.5Mb   1.81Mb          0.500    2.32Mb          0.333
   1     13.0Mb   3.61Mb  2.00x   0.500    4.64Mb  2.00x   0.333
   2     19.5Mb   5.42Mb  1.50x   0.500    6.96Mb  1.50x   0.333
   3     26.0Mb   7.22Mb  1.33x   0.500    7.22Mb  1.04x   0.500
   4     39.0Mb  10.83Mb  1.50x   0.500   10.83Mb  1.50x   0.500
   5     52.0Mb  14.44Mb  1.33x   0.500   14.44Mb  1.33x   0.500
```

On the flat pair every rung tolerates the same loss — base budget 50 %,
enh 33 % — and `down_util` 0.35 demotes above 17.5 % base pre-FEC loss,
identically on every rung.

Master's pair buys the low rungs **28 % more video** (2.32 vs 1.81 Mbps at
mcs0) by spending less of an already small rate on FEC, and that is a real
gain. But look at the `2 → 3` step: **1.04×**, against 1.33× in raw radio
rate. The extra FEC at rung 3 eats almost the entire rate gain, so in
video terms rungs 2 and 3 are nearly the same operating point at very
different protection levels. §2.1 picks that apart.

### 2.1 A demote buys no protection — the core defect

⚠ **Corrected 2026-09-19 for `gilankpam/mabur` master's ladder.** This
section was written against a ladder with a genuinely flat overhead pair
(1.0 base / 0.5 enh on all six rungs), which is still what THIS branch's
`gs/bundle/maburgs.default.toml` ships. Master has since moved to a
two-step pair, so the old heading ("the flat overhead pair") no longer
describes the flown config. **The finding itself survives intact, and the
worked example below is unchanged** — see "What master's ladder does and
does not change" at the end of this section for exactly why.

**The ladder trades away rate without ever buying protection.** Dropping
a rung cuts the video rate 1.33–2.0× and leaves the loss tolerance
*exactly where it was* — or, at one transition on master, makes it worse.
A demote is therefore close to a pure loss whenever the current rung's
`L` is still inside its budget, and the budget is generous.

**Worse: the pair is over-provisioned at precisely the loss level where it
gives up, and no choice of overhead fixes that.** `down_util` 0.35 scored
against a base budget of 0.5 fires the demote at `L` = 17.5 %, while
`ov_base` 1.0 is carrying enough repair for 50 % — a **2.86× margin**, not
the 2× the design is tuned around. That ratio is **structural**: the
demote fires at `down_util × B` and the budget is `B`, so the margin is
`1 / down_util` = 2.857 whatever the overhead is. Master's lower rungs
(`ov_base` 0.5, `B` = 0.333) fire at `L` = 11.7 % against a 33.3 % budget
— the same 2.86×. Re-tuning the ladder's overhead values cannot touch
this; only making overhead an actuator, or moving `down_util`, can.

So at the moment of the demote there is unspent FEC on air *and* the
controller is about to spend a third of the video rate to buy robustness
it already had.

Worked example, mcs4 at the instant `L` reaches the demote threshold
(`B` = 0.5, `f₀` = 0.60):

| | video | what it does |
|---|---|---|
| shipped, hold the rung | 10.83 Mbps | not an option the controller has |
| **shipped, demote to mcs3** | **7.22 Mbps** | what actually happens |
| tier 1: hold, `ov` → 0.538 | **12.68 Mbps** | 2× margin on the measured `L` |

Tier 1 holds mcs4 on **less** FEC than ships today (0.538 vs 1.0),
because 0.538 is what a 2× margin on 17.5 % loss actually costs — and
delivers **1.76× the video the demote gives**, and 1.17× what holding the
rung on the shipped pair would give. The current design cannot reach any
of that: overhead is not an actuator, so its only lever is the rate.

This is the single strongest argument for tier 1, and it is arithmetic on
shipped constants rather than a bet on the reflex or the probes.

Two caveats on the example. It assumes the enh layer sees the same `L` as
base, which UEP exists precisely because it does not — the real gain is
smaller and needs the general two-layer form above. And it assumes `L` is
stationary over the window, which during a fade it is not.

The rate steps themselves are fine — 2.0× at the very bottom, 1.33–1.5×
everywhere else — and mcs3 is already a rung, so there is nothing to fix
in the ladder's *spacing*. What is missing is its second dimension.

**What master's ladder does and does not change (2026-09-19).** Master
now ships a two-step pair instead of a flat one:

| rung | mcs | `ov_base` | budget `B` | demote at `L` |
|---|---|---|---|---|
| 0–2 | 0, 1, 2 | 0.5 | 0.333 | 11.7 % |
| 3–5 | 3, 4, 5 | 1.0 | 0.500 | 17.5 % |

(`overhead_enh` steps 0.25 → 0.5 alongside it. `down_util` is still 0.35.)

Three consequences. The first settles whether this section survives at
all; the second turned out to be the most interesting thing in it:

1. **The worked example above is untouched.** mcs4 is rung 4 and mcs3 is
   rung 3; both carry `ov_base` 1.0, so the demote the example prices
   still crosses no overhead step and still buys exactly zero extra
   tolerance. Every number in that table stands.
2. **The 3 → 2 demote is the sharpest instance of this section's argument,
   not a counterexample to it.** It gives up **3.6 % of the video**
   (7.22 → 6.96 Mbps) and **a third of the base budget** (0.500 → 0.333).
   Under the flat pair a demote was "expensive in rate, neutral on
   tolerance"; across this boundary it is "nearly free in rate, and it
   *reduces* tolerance". So in the two dimensions the controller actually
   reasons about — rate and budget — rung 3 dominates rung 2 outright.

   That demote can still be correct, because the point of a lower MCS is
   that the physical `L` falls too. But it now has a precise condition:
   it pays only if mcs2's pre-FEC loss is **more than a third lower** than
   mcs3's, enough to cover the budget it hands back. Nothing in the
   shipped controller checks that — `down_util` fires on the current
   rung's utilisation alone and never compares the two rungs. Checking it
   IS tier 2's `rate × (1 − 2L)` objective (§2), which is why this
   boundary is an argument for the proposal rather than against it.
3. **It is a rate optimization, not a protection trade — so it is
   orthogonal to this proposal rather than a partial fix for it.** Less
   FEC at low rungs is defensible on its own terms: mcs0–2 are
   intrinsically more robust, so the same physical channel needs less
   repair there, and spending less of the (already small) low-rung rate on
   FEC is a straight win. What it does not do is let the controller
   *exchange* rate for protection, which is the lever tier 1 adds.

An earlier revision of this note (and a chat claim on 2026-09-19) said
master had "partly addressed" the defect and that tier 1's headroom was
therefore smaller. That was wrong: "non-flat" was read off without
checking which rungs the step falls between. It sits BELOW the rungs the
worked example uses, so the headroom there is unchanged.

**Reconcile on merge.** This branch's bundle still ships the flat
1.0/0.5 pair on all six rungs; master's two-step pair is the newer and
better-reasoned choice, so take master's side of
`gs/bundle/maburgs.default.toml`'s ladder. Note that tier 1's `min_ov`
floor of 0.5 (see §8a) then exactly MEETS master's rungs 0–2, which ask
for 0.5. The floor only clamps downward, so with
`link.overhead.enable = true` tier 1 can still RAISE overhead on those
rungs when loss demands it — it just can never trade any of that 0.5 back
for rate, which is where the low-rung bitrate win would have come from.
Downward movement is available only at rungs 3–5. Per-rung floors are the
fix; `fec.log` covers rung 5 only so far.

✅ **A latent trap noted here is now fixed.** The struct default and the
shipped bundle used to disagree on both dimensions, so a GS config that
merely omitted `link.ladder` flew a materially different ladder, silently
and validly. `gs/src/config.h:145` is now a single failsafe rung
(`{{{0, 1.0, 0.5}}}`), which cannot promote anywhere: the failure mode is
a visibly crippled ~1.8 Mbps link instead of a silently aggressive one.

(mcs6/mcs7 are excluded by `max_mcs = 5`, "mcs5 is unholdable at range".
Raising that is a separate question, and
`docs/evm-sweep-findings-2026-08-10.md` shows mcs7 is noise-limited and
must stay linear.)

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

**Ceiling:** `link.ladder[].overhead_*` validates to [0.1, 2.0]
(`gs/src/config.cpp:274`), so the deepest expressible protection is
ov 2.0 = a 67 % loss budget. That is a config range, not a wire or FEC
limit, but tier 1 must respect it (or raise it deliberately).

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
| mcs1 | 3.70 % | 10.62 % | — | 14.3 % |
| mcs2 | 2.83 % | 5.43 % | 10.62 % | 18.9 % |
| mcs3 | 1.97 % | 3.70 % | 5.43 % | 11.1 % |
| mcs4 | 1.54 % | 2.83 % | 3.70 % | 8.1 % |

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
below**, not a constant. On the shipped ladder that ratio is 1.33× or
1.5× for every step except mcs1→mcs0 (2.0×), so the threshold is close to
uniform in the middle of the ladder and much higher at the bottom. Also gate on residual: if `resid`/`resid3` is nonzero while
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

## 5. Mental model for the FEC (why tier 1 is cheap)

Anyone arriving from a block-RS link (WFB-ng and friends) needs this
before tier 1 makes sense, because the usual vocabulary does not carry
over. Shipped geometry, `bundle/mabur.default.toml`:

```
symbol      = 332 B          window = 32 symbols
radio body  = 4 symbols (blocks_per_body) -> the window spans 8 bodies
window      = 10.4 KB of video ~ 7.8 ms ~ half a frame at the mcs4 rung
ov_base 1.0 -> one repair per source symbol  (half that layer's air)
ov_enh  0.5 -> one repair per two            (a third of it)
```

**Block RS** waits for k data packets, computes n−k parity, sends n, and
the receiver needs any k of them. Blocks are independent; the ratio is a
property of the block.

**mabur's sliding-window RLC** does none of that:

1. **Data never waits.** A symbol is sent the moment it is full,
   unmodified (the code is *systematic*). No block-formation latency.
2. **Repairs come out at a rate, not in a burst.** A credit counter gains
   `overhead` per sealed symbol and emits a repair each time it crosses
   1.0. This is why there is no "7/12" to speak of — you set a production
   *rate*, and it is a `double`.
3. **Every repair covers a different, overlapping window** of the last
   ≤32 symbols, carrying its own `window_start`, `window_len` and
   `repair_key` so the receiver can regenerate the coefficients.

The model in one line:

> **Each repair is one equation; each lost symbol is one unknown. You
> need as many covering equations as you have unknowns.**

One lost symbol is solved by the next repair covering it. Three losses in
a window need three independent covering repairs. Consequences worth
knowing:

- **Protection is spread over time, not bunched.** A symbol's repair
  trickles in over the following ~8 bodies, so an interference burst
  cannot take out all of one symbol's protection the way it can take out
  a block's whole parity run. That is the time diversity the encoder
  comments cite, and the window length is the knob for it.
- **No block boundary, so no hard cliff — but there is a deadline.** An
  unsolvable symbol stays pending and may become solvable as more repairs
  arrive; mabur eventually gives up at the abandonment horizon. That is
  what `syms_abandoned` counts, and it is the sliding-window equivalent
  of "the block failed" — there is no block to attach it to.
- **Not MDS, unlike RS.** Random coefficients occasionally produce a
  repair that is linearly dependent on ones already held (~1/256 each at
  GF(256), so under 1 % waste). Tier 1 should carry a little slack for it
  rather than running exactly at its target margin.

The payoff for tier 1: the receiver is never told the overhead. It
collects equations and solves. That is why `set_overhead()` is a live
knob with no handshake, no block boundary to wait for, and no encoder
write — hence no keyframe.

---

## 6. Metrics: what to add

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

## 7. Comparison summary

| | current | proposed |
|---|---|---|
| Decision authority | GS only | GS (tiers 1–2) + drone reflex (tier 0), with an explicit follow contract |
| Worst-case reaction | ~650–700 ms, unbounded if the RCF is lost | ~100–150 ms, delivery-free |
| FEC overhead | fixed per rung, never adapted — not an actuator at all (flat 1.0/0.5 here, two-step on master, §2.1) | continuously controlled from raw `pre_fec_loss` |
| What a demote buys | 1.33–2.0× less rate, **zero** extra loss tolerance | nothing, because FEC absorbs it first (§2.1) |
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

## 8. Suggested implementation order

Each step is independently valuable and independently revertable.

1. ✅ **GS observed-MCS scoring + `Following` state.** Self-contained, no
   drone change, unit-testable (`LadderController` is "pure decision logic
   — no clock, no I/O, no radio types"). Fixes a real latent bug today:
   the existing `apply_max_range()` failsafe *already* creates the
   mis-scoring of Fight B, and nothing handles it.
   *Landed: `gs/src/mcs_mode.h`, `FollowCfg` in `ladder_controller.h`.*
2. **Metrics as observe-only exports** (salvage density, FCS-vs-gap, EVM
   z-score, `fa + foreign`). Ship them to the sideport and
   `flightreport.py` and fly a few park sessions before any of them gates
   a decision.
3. ✅ **Tier 1 inner loop**, behind a config flag, bitrate decoupled and
   dead-banded from the start.
   *Landed: `gs/src/overhead_policy.h`, `link.overhead`, default OFF. The
   next step is a flight at `enable = false` comparing
   `link.ctl.ov_target` against the fixed 1.0/0.5 pair — arming it before
   that is arming a loop nobody has seen the inputs of.*
4. **Tier 0 reflex**, observe-only first: log what it *would* have done
   against recorded flights before arming it. This is the staging pattern
   the air-clock gate used (`shed_ms` 0 = observe-only) and it worked.
5. **Tier 2 restructure + armed −1 probe**, last, once §2's objective has
   been checked against real per-rung `L` data.

Gates, per CLAUDE.md: `tools/bench/ausniff.py` for anything touching
maburgs, plus `tools/bench/aucadence.py` for anything touching the
bitrate policy or UEP overhead — which tiers 1 and 2 both do.

---

## 8a. Collision with `gilankpam/mabur` master (found 2026-09-17)

This branch was built on `notsudogood/mabur` master (`f51f8e0`).
`gilankpam/mabur` master — **the remote both device images build from** — is
a sibling off that same base with ~45 commits on it, and two of them
collide with this work. Read this before merging in either direction.

### RC_VERSION 9 was already taken — reconciled at 10 (DONE)

`e65922a rc: RC_VERSION 9 — RCF hop_ch/hop_epoch, Telem channel/hop_epoch`
landed there on 2026-09-15, two days before this branch's own
`RC_VERSION 9`. They are incompatible and they claim the SAME BYTE:

| | their v9 | this branch's v9 |
|---|---|---|
| `RCF_HEAD_LEN` | 17 | 16 |
| byte 15 | `hop_ch` | `probe_profile_dn` |
| byte 16 | `hop_epoch` | (CRC) |
| `TELEM_LEN` | 89 (+channel/hop_epoch) | 87, untouched |

The head lengths differ, so a frame from one build fails the other's CRC
check rather than mis-parsing — the failure is loud, not silent. But the
version number is ambiguous, which is worse than either layout.

**Resolved as `RC_VERSION 10`, an 18-byte head**, keeping their fields at
15–16 (already flown) and appending this branch's at 17:

```
... byte 14 probe_profile | 15 hop_ch | 16 hop_epoch | 17 probe_profile_dn | crc16
```

Both sides also patched the SAME four `tests/integration/run_*_e2e.sh`
scripts that hand-pack the RCF head — theirs to 17 bytes with trailing
zeros, this branch's to 16. Those four conflict textually and must end at
18. Note they have `tools/…/gen_vectors.py` emitting the hop fields
(`fee3b18`); this branch regenerated `rc.json`'s goldens with an ad-hoc
Python packer instead. Use their generator for the v10 goldens.

### `fec.log` measures what tier 1's floor was guessing at

`ca3ad5d gs: fec.log — per-episode FEC loss gauge for sizing the rung
overhead pair` is not a competing controller. It is the **measurement tier 1
is missing**, and it says the tier 1 floor is set too low.

`SwDecoder` books a loss *episode* at horizon eviction — a run of source
seqs the channel never delivered directly, merged within one repair window,
with the distinct covering repairs received (two-card deduped), abandoned
vs recovered, and stale-below-watermark. `flightreport.py` then prints, per
(sid, mcs, ov), the overhead each non-stale episode **would have needed**:

```
ov_req = (sqrt(1+4c)-1)/2,   c = m·ov·(1+ov)/r
```

— at overhead `x` the same lost air carries `m(1+ov)/(1+x)` sources against
`r·x/ov` covering repairs, and the decoder needs repairs ≥ sources.

**That is a fundamentally better model than `OverheadPolicy`'s.** This
branch drives overhead from the MEAN pre-FEC loss rate
(`ov = mL/(1−mL)`, `margin` 2). But sliding-window FEC does not fail on a
rate — it fails on *coverage*: whether the repairs spanning a burst
outnumber the sources lost inside it. A 2 % mean loss spread evenly is
trivially covered; the same 2 % arriving as one 16-symbol burst inside a
32-symbol window may not be. `margin = 2` on the mean is a crude proxy for
burstiness; `ov_req` measures it directly, per episode.

The bench numbers (2026-09-16, GS deployed, ausniff 60.0 fps / fid_gaps 0)
make the consequence concrete. Rung-5 base: episodes are one lost agg-6
each, `m` 11/16 p50/max against `r ≈ 41`, **`ov_req` max 0.46**; enh at
ov 0.5 reads 0.38. Reproducing the formula gives 0.387 at m=11 and 0.515 at
m=16, bracketing their reported max.

Set against what this branch would command at those rungs:

| measured need (rung 5) | `OverheadPolicy` target at a sparse mean L |
|---|---|
| `ov_req` ≈ 0.39–0.52 | L=1 % → 0.02, L=5 % → 0.11, L=10 % → 0.25 |
| | all floored to `min_ov` = **0.30** |

So **`min_ov = 0.3` is below what the measured worst episodes need**, and at
sparse loss the policy's own target is far below that — the floor is the
only thing preventing much worse. `overhead_policy.h` says of that 0.3:
"a judgement call pending flight data on that variance, not a measurement."
This is that data, and it says the number is wrong.

Two ways to fix it, and the second is better:

1. Raise `min_ov` to the measured `ov_req` max. **DONE** — `min_ov` is 0.5,
   the first point on the `step` 0.1 grid at or above the measured 0.46.
   `OverheadPolicy::ov_req(m, r, ov)` now mirrors `flightreport.py`'s
   `fec_ov_req()` in C++ so the derivation sits next to the constant, and
   `test_overhead_policy` pins the floor against the measurement so it
   cannot drift back to a guess. Costs ~13 % of video rate wherever the
   floor binds (`kbps ∝ 1/(1+ov)`), which on a sparse link is most of the
   time — the conservative direction for a floor: too high costs bitrate,
   too low costs AUs.

   Two caveats carried in the code: only **rung 5** is measured (the
   same burst in time destroys fewer symbols at a lower MCS, which argues
   rung 5 is the worst case — reasoning, not data), and one floor serves
   both layers, so enh sits at the 0.5 it already flies.

   It also refines §2.1: base is ~2× over-provisioned (1.0 vs 0.46), enh
   only ~1.3× (0.5 vs 0.38) — so the bitrate win tier 1 chases is almost
   entirely on **base**, not the pair.
2. **Drive tier 1 from `ov_req` directly** — feed the episode gauge's
   high-percentile `ov_req` over a recent window instead of `mL/(1−mL)`.
   Still the better shape, and still not done: it replaces the margin
   heuristic with the quantity that actually decides recovery, and makes
   §9.5's RLC-rank-deficiency slack the only remaining fudge factor. The
   C++ `ov_req()` is the groundwork; nothing calls it on the hot path.

Either way the flown 1.0 base pair is confirmed over-provisioned by ~2× at
rung 5 — §2.1's argument, independently measured rather than derived.

### The hop design and this branch touch the same ladder surface

Read `docs/inflight-channel-hop.md` §4 ("Ladder interaction: restore the
pre-onset rung") on that master before merging. Three real interactions,
one of them a defect in tier 1.

**`LadderController::restore(rung, now)` IS §4's fast-restore.** Theirs
clamps the rung into range, sets `idx_` directly, clears all three
probation fields, `reset_windows()`, `mark_transition()`, and logs
`CtlReason::HopRestore` — bypassing the probe gate by construction, which
is the ~3.1 s/rung this branch wanted to skip. Two consequences:

- §4's fast restore should call `restore(pre_adopt_rung(), now)` with its
  own reason, not add a second mechanism.
- This branch's `Following` **adopt block is the same code inline**, minus
  the clamp. On merge it should BE `restore()` too, so there is one
  rung-override path rather than three.

**Tier 1 has no blank, and by the hop design's own argument it needs one.**
`blank_store(until_ms)` suspends the per-rung EWMA writes because "an
interferer's demoted operating point is real RF evidence for the CHANNEL
that just got abandoned, not for what the rung can do in general, and must
not poison the learned per-rung statistics." That reasoning applies
verbatim to `OverheadPolicy`, which is fed raw `health.pre_fec_loss` with
no blank at all — so through an interference episode tier 1 would size
overhead to the jammed channel and then carry that sizing onto the new one.

The mechanism is concrete, not hypothetical. `VrxController::restore_rung()`
calls `sync_op_()`, which is `cur_op_ = op_from_rung(ctrl_.op())` — it
rebuilds the WHOLE operating point from the rung's config, overhead pair
included. `apply_overhead_policy()` then overwrites `cur_op_.overhead_*`
on the next `step()`. So a hop order's intent to restore the pre-onset
operating point is half-defeated once tier 1 is armed: the rung sticks,
the overhead does not, and what replaces it was measured on the channel
being abandoned.

Fix: gate `OverheadPolicy::feed()` on the same `blank_store_until_ms_`
deadline — hold the commanded overhead rather than feeding, exactly as the
store holds its EWMAs. That needs an accessor; the deadline is private
today. Note the blank starts at the first `interfered` window
(`hop_blank.h`'s `hop_store_blank_until()`), not at the order, so the
hold covers the detection windows too.

**Tier 2 self-disarms under interference — by accident.** `want_probe()`
requires `residual_clean` (both residual losses zero), and interference
drives residual nonzero, so the down probe stops arming. That is the right
behaviour reached for the wrong reason, and it is only half of it:
`should_demote()` still scores `hi` from the contaminated `pre_fec_loss`.
Harmless while `act = false`; load-bearing the moment it is armed. Both
should key on the hop verdict explicitly rather than inferring it from
residual.

That matters because the hop design deliberately **runs the ladder
unfrozen during detection** — "a demote or two, each an IDR" is accepted
and priced in, and `restore()` overwrites it when the hop lands. An
objective-driven demote is not the same animal: it is a COMPARISON in
which both rungs' scores were measured on the interfered channel, so the
"priced in" argument does not extend to it. `objective.act` wants gating
off inside the blank.

**Encouraging sign:** the hop design gates its blank on `hop.enable`
specifically so that observe-only flights are not silently different from
pre-branch recordings, citing `docs/data-provenance.md`. That is the same
staging discipline this branch used for `link.overhead` and
`link.objective`, so the two designs are compatible in philosophy as well
as in code.

### Smaller collisions to expect

- `tools/flightreport.py`: they add `FEC EPISODES` and `HOP`; this branch
  adds `LINK-ADAPTATION V2`. Textual conflict in `main()` and the section
  order. `tools/session.py` learns `fec.log` and `scan.log` on their side.
- `gs/src/ladder_controller.*`: covered above. Also note `CtlReason` grows
  on both sides (`HopRestore` theirs, `Follow` here) and both branches
  touch `mark_transition()`/`reset_windows()`; `f4b4987` split
  `blank_store`'s s3 gate from the s3 demote decisions.
- **The flown ladder's overhead pair diverged (found 2026-09-19, after the
  `ca3ad5d` merge).** Master's `gs/bundle/maburgs.default.toml` now steps
  `overhead_base` 0.5 → 1.0 and `overhead_enh` 0.25 → 0.5 between rungs 2
  and 3; this branch still ships the flat 1.0/0.5. Take MASTER's side on
  merge — it is the better-reasoned choice (low MCS is intrinsically
  robust, so it needs less repair) and it is what the hardware flies. Two
  knock-ons: §2.1's heading was wrong and is corrected there, and tier 1's
  `min_ov` 0.5 floor BINDS at rungs 0–2 under master's pair, so an armed
  `link.overhead` can only move rungs 3–5 until per-rung floors exist.
- Master-era GS configs carry two keys this branch rejects, and both are
  fatal at load: `link.probe.clean_bodies` (master renamed this branch's
  `clean_ms`, and changed the UNIT — bodies, not milliseconds) and the
  player's `[colortrans]` section. `maburgs` then crash-loops at 2 s;
  `maburplay` exits 2, which `S97maburplay` treats as terminal — a
  permanently black screen. See `docs/deploy.md`.
- `10b22b3` **removes `radio.scan.energy_period_ms` and the 1 Hz A
  records.** §6's metric 4 cites `cards[i].energy` (`fa`/`cca`/`igi`) as an
  available signal; on that master it is at least partly gone. Re-check
  before building on it.
- `tests/test_flightreport.py` and `tests/CMakeLists.txt` are touched by
  both.

---

## 9. Open questions and untested premises

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

   **Partly answered, and by the host fixture rather than a recording.**
   One failure mode turned out not to need park data at all: a drone parked
   on a rate floor it will not leave (its own `apply_max_range()` clamp, or
   a thermal/power limit) plus a clean GS-side `u` cycles *unconditionally*
   — restore, drone re-asserts its rung, GS re-adopts, repeat every
   `restore_clean_ms`. Measured on `tests/test_ladder_controller.cpp`'s
   fixture: **31 restores in the 20 s after one adopt**, one IDR each, which
   is the deleted 2026-09-01 FEC→bitrate loop's keyframe storm arriving by a
   new road.

   The guard reuses machinery rather than adding a retry timer: a restore
   the drone undoes within `restore_trial_ms` (3000, mirroring
   `probation_ms`) calls `penalize_rung()` on the restored rung, and the
   fast-restore block consults `is_penalized()` exactly as the promote block
   does. The ledger's existing `penalty_base_ms` doubling to
   `penalty_max_ms` then supplies the backoff. Same fixture: **3 restores in
   20 s**, spaced 6 s then 11 s.

   `follow_restore_rejected` / `follow_restore_penalized` are on the
   sideport, so a park recording can now distinguish the two failure modes
   this item conflates — a link that has not really recovered (rejected
   stays 0, the restores simply do not stick) from a drone-side floor
   (rejected climbs). **Still open:** the genuine restore→re-dip case at
   pillar spacing, which needs the recording. Probe-gating the restore
   remains the fallback if the ledger's backoff proves too coarse.
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
