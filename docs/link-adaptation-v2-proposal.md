# Link adaptation v2 — three-tier proposal

**Status: PROPOSAL. Nothing here is implemented, built or flown.** Written
2026-09-15 from a design discussion. Every number attributed to a flight,
bench session or source file is real; everything else is derived from the
shipped formulas or is an explicit assumption, and is marked as such.

Read `docs/link-adaptation.md` for what actually ships today. Note when
reading the code that there are TWO ladders: the 6-rung struct default in
`gs/src/config.h:100` (mcs 0/2/4/5/6/7, per-rung overhead) is a fallback
used only when the config omits `link.ladder`, and the SHIPPED flight
ladder in `gs/bundle/maburgs.default.toml` (mcs 0-5, flat 1.0/0.5)
replaces it wholesale. `common/src/profile.cpp`'s `profile_table()` is a
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
| **FEC overhead** | **Fixed, and FLAT across every rung**: `overhead_base` 1.0 / `overhead_enh` 0.5 on all six. Carried in the RCF, never adapted at runtime. So a rung is effectively just an MCS — the ladder has no FEC dimension at all today. |
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

Where the shipped ladder actually sits (`B` = 0.5, `f₀` = 0.60, the flat
1.0/0.5 pair, so no `L` term — today's overhead does not respond to loss
at all):

```
rung/mcs  speed    video    step up from prev
   0      6.5Mb   1.81Mb
   1     13.0Mb   3.61Mb    2.00x
   2     19.5Mb   5.42Mb    1.50x
   3     26.0Mb   7.22Mb    1.33x
   4     39.0Mb  10.83Mb    1.50x
   5     52.0Mb  14.44Mb    1.33x
```

Every rung tolerates the same loss — base budget 50 %, enh budget 33 % —
and `down_util` 0.35 demotes at a base pre-FEC loss above 17.5 %,
identically on every rung.

### 2.1 The flat overhead pair is the core defect

**The ladder trades away rate without ever buying protection.** Dropping
a rung cuts the video rate 1.33–2.0× and leaves the loss tolerance
*exactly where it was*, because the overhead pair does not vary by rung.
A demote is therefore close to a pure loss whenever the current rung's
`L` is still inside its budget — and the budget is generous, because it
is the same 50 % at every rung.

**Worse: the shipped pair is over-provisioned at precisely the loss level
where it gives up.** `down_util` 0.35 scored against a base budget of
0.5 fires the demote at `L` = 17.5 %, while `ov_base` 1.0 is carrying
enough repair for 50 % — a **2.86× margin**, not the 2× the design is
tuned around. So at the moment of the demote there is unspent FEC on air
*and* the controller is about to spend a third of the video rate to buy
robustness it already had.

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

⚠ **Latent trap while this is unimplemented.** The struct default
(`gs/src/config.h:100`) and the shipped bundle disagree on *both*
dimensions — mcs 0/2/4/5/6/7 with per-rung overhead 2.0…0.2, versus
mcs 0-5 with a flat 1.0/0.5 pair. A GS config that simply omits
`link.ladder` therefore flies a materially different ladder, silently and
validly. Worth reconciling independently of this proposal.

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
| FEC overhead | fixed, and FLAT across every rung (1.0/0.5) — not an actuator at all | continuously controlled from raw `pre_fec_loss` |
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

1. **GS observed-MCS scoring + `Following` state.** Self-contained, no
   drone change, unit-testable (`LadderController` is "pure decision logic
   — no clock, no I/O, no radio types"). Fixes a real latent bug today:
   the existing `apply_max_range()` failsafe *already* creates the
   mis-scoring of Fight B, and nothing handles it.
2. **Metrics as observe-only exports** (salvage density, FCS-vs-gap, EVM
   z-score, `fa + foreign`). Ship them to the sideport and
   `flightreport.py` and fly a few park sessions before any of them gates
   a decision.
3. **Tier 1 inner loop**, behind a config flag, bitrate decoupled and
   dead-banded from the start.
4. **Tier 0 reflex**, observe-only first: log what it *would* have done
   against recorded flights before arming it. This is the staging pattern
   the air-clock gate used (`shed_ms` 0 = observe-only) and it worked.
5. **Tier 2 restructure + armed −1 probe**, last, once §2's objective has
   been checked against real per-rung `L` data.

Gates, per CLAUDE.md: `tools/bench/ausniff.py` for anything touching
maburgs, plus `tools/bench/aucadence.py` for anything touching the
bitrate policy or UEP overhead — which tiers 1 and 2 both do.

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
