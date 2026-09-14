# Multi-MCS link — feasibility study, 2026-09-14

Answers: *can the adaptive-bitrate ladder be replaced by a multi-MCS link
that carries one stream at the viewer's MCS and a second one MCS lower,
while probing op−1, op−2 and op+1?*

Scope: feasibility and cost only. Switching policy and bitrate derivation
are explicitly out of scope (the request defers both). Nothing here is
flown or benched — every number is either quoted from a dated findings doc
or computed from `common/src/profile.cpp`'s rate table, the **shipped**
ladder in `gs/bundle/maburgs.default.toml`, and `run_bitrate_policy`'s
formula, and is marked as such.

**Verdict in one paragraph.** The *mechanism* is almost entirely already
built: per-stream MCS is a solved problem on this link (radiotap cache
slot per stream, `drone/src/radio_tx.cpp`), the RCF already carries an
MCS-space probe command, and the GS's loss attribution is already keyed
per-sid with a per-sid expected MCS. What is not free is **air**. The two
video "streams" today are SVC-T temporal layers of one HEVC encode, not
two feeds, so "a second stream one MCS lower" means one of three very
different things, and they price orders of magnitude apart. The
probe half of the request is cheap and mostly a config/wire widening. The
"remove adaptive bitrate" half is the only part that is **not** feasible
as stated across the current ladder's 8:1 rate span — but it *is*
feasible across a narrow 2-rung window, and that narrow version is the
most interesting thing in this document, because it kills the
IDR-per-rung-change pathology that `docs/link-adaptation.md`'s congestion
shed and air clock sections, and `docs/airtime-model.md` §1, are all
working around from different directions.

---

## 1. What exists today (the baseline this would replace)

| piece | where | what it does |
|---|---|---|
| 2 video streams, BASE sid0 / ENH sid1 | `common/src/nal.cpp` `classify_frame` | **SVC-T temporal layers of ONE encoder channel.** ENH is TRAIL_N — non-referenced, droppable. BASE carries the IRAPs and every referenced picture. They are not independently decodable feeds; ENH alone is worthless, BASE alone is a complete 30 fps video. |
| same-rate rule | `common/src/profile.cpp` `ladder_from` | Both slots ride the scored MCS. UEP is expressed **only** as a per-rung FEC overhead pair. |
| the operating ladder | `gs/bundle/maburgs.default.toml` `[[link.ladder]]` | **Six rungs, mcs 0–5, one MCS per rung**, every rung `overhead_base` 1.0 / `overhead_enh` 0.5, `max_mcs` 5. `profile_table()` in `profile.cpp` is *not* this — it is the rendezvous bootstrap the drone applies from `Disc::init_profile`. Getting these two confused inverts the cost of several things below. |
| per-stream MCS mechanism | `drone/src/radio_tx.cpp` | `Cache{ std::array<LayerCache,3> }` — one prebuilt radiotap per slot, `stream_id → slot`, swapped atomically. Slot 2 is the probe's own MCS. **Per-body MCS already works.** |
| probe stream | sid 5, `common/include/mabur/probe_wire.h`, `gs/src/probe_track.h` | One 1405 B canary body after every ENH AU, at the MCS of rung `current + link.probe.rung_offset` (default 1). Scored as a promote gate. |
| probe command | `Rcf::probe_profile`, `common/include/mabur/rc_proto.h` | A full `encode_profile` byte — **MCS-space on the wire already**; only `VrxController` narrows it to "the next rung". `link.probe.pin_mcs` already bypasses that for the bench. |
| per-sid attribution | `gs/src/transition_edge.h`, `SwBoundary` in `common/include/mabur/sw_decoder.h` | `dec.mark_transition(sid, mcs, now)` — the decoder already tracks **a separate expected PHY rate per sid**, a leftover of the 2026-08-29 mcs−1 era that was never removed. |
| bitrate policy | `RcAgent::run_bitrate_policy` | `kbps = airtime_budget / [f0·(1+ov_b)/rate_b + (1−f0)·(1+ov_e)/rate_e]`, f0 = 0.60 fixed, budget 0.5, clamp [1000, 16000]. Pure function of the operating point. |

Two prior results bound everything below and must not be re-discovered:

- **2026-08-29 → 2026-08-30: the mcs−1 base rate was built, flown, and
  rolled back.** `docs/same-rate-uep-findings-2026-08-30.md`. The killer
  was air *alternation*: at the mcs1 rung the 2:1 base/enh rate ratio is
  mathematically unbalanceable under a repair-neutral budget, leaving a
  3.3 ms/pair structural residual that the jitter EMA doubles.
- **July 2026 uniform-PHY ruling** (comment at `profile.cpp:60`): SVC-T
  enhance traffic at +1/+2 rungs measured **20–42 % RF loss**. Sending
  *video* above the scored MCS is refuted. A *canary* above it is fine —
  that is exactly what the probe stream does today.

---

## 2. The request splits three ways

"One stream one MCS less than the one the user is viewing, and the other
is the one the user is viewing" reads three ways against what actually
exists. They are not variations — they are different projects.

### Reading A — UEP rate-split: BASE at op−1, ENH at op

BASE (the 30 fps decodable spine) drops one MCS; ENH (the 30 fps
droppable top-up that makes it 60) stays at op. "The stream the user is
viewing" = base+enh at 60 fps; "the stream one MCS lower" = the base
spine that survives when the link can't carry enh. Graceful degradation
is already what the player does: losing enh AUs yields 30 fps, no freeze
(`gs/player/src/main.cpp` treats sid0 as the join point; enh is
non-referenced).

**This is a git revert of the 2026-08-29 design**, and the reason it was
reverted is measured and still applies.

### Reading B — true simulcast: two independent encodes

Two complete, independently decodable video streams, the GS displaying
one and holding the other hot, switching instantly. This is what "switches
streams" most naturally means, and it is the only reading where a switch
costs no IDR.

**Nothing of this exists.** It needs a second VENC channel, a second
frame ring, a second UEP pipeline, and roughly a second stream's worth of
air.

### Reading C — thin hot-standby simulcast

Reading B with the second stream deliberately small (lower resolution
and/or a quarter of the bitrate) — a survivability feed, not a second
full-quality one. The interesting middle.

---

## 3. Reading A — feasible, cheap to build, expensive on air

### 3.1 Build cost: low

Every mechanism is present. `ladder_from` returns `max(m−1,0)` for slot 0
again (a ~3-line change), `RadioTx::set_ladder` already builds a distinct
radiotap per slot, `TransitionEdge` already computes the base spec via
`ladder_from(...)[0]` and marks the decoder with that MCS — that code was
written for exactly this and still reads `[0]` today. `ladder_spec_str`
still *prints* `BASE=MCS{m−1}` — the string was never updated after the
rollback. No wire change, no RC_VERSION bump.

### 3.2 Air cost: this is the whole question

On the shipped ladder (§1) one rung *is* one MCS, so **"one rung down"
and "one MCS down" are the same thing** here — a convenience the vendored
`profile_table()` does not have.

Moving 60 % of the bytes (`kShareBase`) down one MCS step multiplies
their serialization time by `rate(op)/rate(op−1)`. Computed from the
shipped ladder, rate table and `run_bitrate_policy` (budget 0.5, f0 0.60,
clamp [1000, 16000]):

| rung = MCS | rate step down | today's cmd | base at mcs−1 | Δ quality |
|---|---|---|---|---|
| 0 | — (floor) | 1806 kbps | 1806 | 0 % |
| 1 | 13.0 → 6.5 (**2.0×**) | 3611 | 2167 | **−40.0 %** |
| 2 | 19.5 → 13.0 (1.5×) | 5417 | 4062 | −25.0 % |
| 3 | 26.0 → 19.5 (1.33×) | 7222 | 5909 | −18.2 % |
| 4 | 39.0 → 26.0 (1.5×) | 10833 | 8125 | −25.0 % |
| 5 | 52.0 → 39.0 (1.33×) | 14444 | 11818 | −18.2 % |

That is the price **if bitrate is re-commanded to hold the air budget**.
Hold today's bitrate instead and the duty goes (per 33.3 ms base/enh
pair, nominal — before the ~5 % framing excess and the encoder's measured
up-to-40 % CBR overshoot, which together put a nominal 50 % on air at a
measured `link.air_pct` ~66 %):

| rung = MCS | today's duty | duty with base at mcs−1 | **added alternation** |
|---|---|---|---|
| 0 | 50 % | 50 % | 0 (no lower MCS) |
| 1 | 50 % | **83 %** | **+11.1 ms** |
| 2 | 50 % | 67 % | +5.6 ms |
| 3 | 50 % | 61 % | +3.7 ms |
| 4 | 50 % | 67 % | +5.6 ms |
| 5 | 50 % | 61 % | +3.7 ms |

Alternation is quoted as the **delta this change adds**, not an absolute:
the absolute base−enh air gap is scene-dependent (the 2026-08-30 sweep
found the encoder drifting *enh*-heavy at mcs3 — lenB 17.2 K vs lenE
28.8 K — which inverted the gap entirely), and it was measured at
+3.3 ms for the shipped 1.0/0.5 overhead pair. The delta above is
`airB × (rate ratio − 1)` and does not depend on the byte-share estimate
being right in absolute terms, only on base carrying the larger share.

Three things fall out:

1. **`docs/airtime-model.md` §1 prices alternation directly: the jitter
   EMA is ≈ 2× Δair.** So this adds roughly **+7 to +11 ms of jitter EMA
   at rungs 2–5, and +22 ms at rung 1**, on top of today's 5–8 ms floor.
   The 10–15 ms of the old-UEP-refs era was killed as unacceptable and
   the 21 ms `ltr:1` era with it. Rung 1 alone lands past both. This is
   the 2026-08-30 rollback reason restated at the current budget and the
   current ladder.
2. **The bottom rung cannot have a lower companion.** mcs0 has nothing
   below it, so the design degenerates to today's exactly where margin
   matters most — and rung 1, the lowest rung that *can* split, is the
   one where it costs the most (the 2:1 step, the same
   "mathematically unbalanceable" ratio that
   `docs/same-rate-uep-findings-2026-08-30.md` opens with).
3. **The cost is worst at the bottom and mildest at the top** (18 % at
   rungs 3 and 5, 40 % at rung 1) — the inverse of where the extra
   margin is wanted. Any serious version of Reading A should apply the
   split per-rung rather than universally.

### 3.3 What Reading A actually buys

A real, non-trivial gain: the base spine gets the link-budget margin of a
full MCS step. When the channel degrades past op's wall, ENH dies and
BASE keeps going — the operator sees 30 fps instead of a demote cascade.
That is a genuine resilience improvement and it is the honest core of the
user's intuition.

The cost is 18–40 % of picture quality at every rung that can split (or
the jitter above, or some split of the two), permanently, in exchange for
a better-behaved cliff. That is a product judgement, not an engineering
blocker. **Feasible. Measured once already. Rolled back for reasons that
still hold at the current budget.**

---

## 4. Reading B — true simulcast: not feasible at full rate

### 4.1 Encoder: the largest unknown in this document

The drone runs **one** VENC channel: `MI_VENC_CreateChn` is called once
(`drone/venc/star6e_pipeline.c:647`), bound `VIF → VPE port 0 → VENC`.
The venc frame ring (`drone/vendor/venc_frame_ring.h`) is a single
stream of slots with no stream id.

The door is open but unbuilt: the teardown path already knows about **VPE
port 1**, the "second-scaler tap" (`star6e_pipeline.c:114–120`), and
defensively disables it — so the SDK exposes a second scaled output on
this SoC. Whether an i6e/Star6E can run 1080p60 on channel 0 *and* a
second encode on channel 1 within its ISP/VENC/DDR budget is **not known
from this repo** and is not answerable from source. It needs a hardware
spike before anything else in Reading B is worth designing.

Adjacent hardware facts that set expectations: `I6_SYS_LINK_LOWLATENCY`
on the VPE→VENC bind is *rejected* by this SDK (2026-08-31), and
`u32MaxISize`/`u32MaxPSize` are dead on star6e. This chip's SDK surface
has repeatedly been narrower than the headers promise. Assume nothing.

### 4.2 Air: a second full-rate stream does not fit

At rung 5 (op mcs5), a co-equal second stream at mcs4 costs `52/39` =
1.33× the air per bit. Duty goes 0.5 → ~1.17. At rung 2 (op mcs2) with a
second stream at mcs1, 1.5× per bit → ~1.25. Both are over 100 % of the
air before framing excess, before the probe, and before leaving the
~6.7 ms/AU idle window that `RcfSlotter` needs to get an RCF to the drone
at all (`docs/tx-rx-timing.md` §3.1). **Full-rate simulcast is
arithmetically impossible on this link.**

### 4.3 Verdict

Not feasible as stated. Do not design it.

---

## 5. Reading C — thin hot-standby: feasible on air, blocked on the encoder

Size the standby at a quarter of the primary. At rung 5: primary
11 Mbps at mcs5 → 0.38 duty; standby 2.8 Mbps at mcs4 → 0.13 duty; total
~0.5 — today's budget, with a continuously-encoded, independently
decodable fallback already on air one MCS lower. The primary gives up
~23 % of today's 14444 kbps to pay for it.

This is the only reading that delivers what "switching streams" promises:
a switch that costs **zero encoder writes and zero IDRs**, because both
encodes are already running. Against a codebase where
`docs/airtime-model.md` §1 records *151 of 226 bitrate writes had no rung
change behind them*, each one a keyframe at 63.8 kB against a 21.4 kB P
frame, that is a large prize.

It is gated entirely on §4.1: no second encoder channel, no Reading C.
The air arithmetic is the easy half.

---

## 6. The probe half — feasible, cheap, but read §6.3 first

Requested: probe op−1, op−2 and op+1 alongside the video.

### 6.1 op+1 already ships

`link.probe.rung_offset` defaults to 1, and on the shipped ladder one
rung *is* one MCS — so **the op+1 probe already ships, exactly as
requested**. It is nominally in rung space
(`VrxController::build_rcf` looks up `cfg_.ladder.ladder[pr].mcs`), but
the wire field is a full `encode_profile` byte, so if the ladder is ever
re-spaced, MCS-space probing needs no wire change — only a change to how
the GS picks the number. `link.probe.pin_mcs` already does MCS-space
directly for the bench.

### 6.2 Down-probes: mechanism is cheap

Per extra probe stream, the work is:

| component | change |
|---|---|
| `RadioTx::Cache` | `std::array<LayerCache, 3>` → 5; `build_frame`'s `stream_id → slot` map |
| SBI | stream ids 6, 7 (the id is a `u8`; 4 = MSP, 5 = probe) |
| `Rcf` | 2 more profile bytes (or a small array) — **RC_VERSION 8 → 9, flag-day deploy** |
| `ProbeTrack` | today keeps counters for *one* commanded profile plus an `off_profile` count; becomes a 3-entry map. Its AU-keyed expectation model (`enh_fid`) generalises unchanged. |
| `RcfSlotter` | the probe-arrival release (`on_probe_tail`) must fire on the **last** probe of the group, and `tail_ub_ms` must cover all three bodies |
| `link.probe` config | `rung_offset` is currently `[1,7]` — down-probes need signed offsets or an explicit MCS list |
| in-repo consumers | `tools/maburtop.py`, `tools/flightreport.py`, `gs/player`'s OSD, `probelog N` marker — **same commit**, per CLAUDE.md |

Air cost, computed for the shipped 1405 B body at 30/s:

| probe MCS | per body | at 30/s |
|---|---|---|
| mcs0 | 1.73 ms | **5.19 %** |
| mcs1 | 0.86 | 2.59 % |
| mcs2 | 0.58 | 1.73 % |
| mcs3 | 0.43 | 1.30 % |
| mcs4 | 0.29 | 0.86 % |
| mcs5 | 0.22 | 0.65 % |

Worked cases: at rung 2 (op mcs2), probes at mcs1 + mcs0 + mcs3 =
**9.1 %** of air — of which the op+1 probe already ships, so the *new*
cost is 7.8 %. At rung 1 (op mcs1), mcs0 + (no mcs−2) + mcs2 = **6.9 %**,
new cost 5.2 %. At rung 5 there is no op+1 within `max_mcs`, and at rungs
0–1 there is no op−2: the full three-probe set only exists at rungs
2–4. Against a link measuring ~66 % `air_pct` at a nominal 50 %,
9 % is real money and it lands on the idle window the RCF slotter needs.
Mitigation: down-probes need not run at 30/s. At 10/s the cost thirds —
but `link.probe.min_syms` is 40 and the gate's quantum is 0.2 per lost
body, so a slower probe needs a proportionally longer window and reacts
proportionally slower. That trade is explicit and tunable, not a blocker.

Note also that the down-probes are the *expensive* ones (a fixed byte
count serializes slowest at the lowest rate) and, per §6.3, the least
informative per unit air.

### 6.3 The design caution: down-probes measure nothing until it's too late

On a channel where delivery is monotonic in MCS — which is the assumption
the entire ladder rests on — `P(loss | mcs−2) ≤ P(loss | mcs−1) ≤
P(loss | op)`. So while the op stream is clean, **both down-probes are
certainly clean and carry no information at all.** They only say anything
once the op stream is already failing.

That is not an argument against them; it is an argument about what they
are *for*. A down-probe is not a demote **trigger** — the live streams
already are that, measured, attributed, and fast. A down-probe is a
demote **depth estimator**, and that is worth building, because the
current cascade is a known pathology:

> flight-0011: the GS "stepped 5→4→3→2 in 450 ms with an IDR on every
> step into a queue that was already full" — `docs/link-adaptation.md`,
> "Drone congestion shed"

and

> the post-demote latency spike is a drain: 2–3 IDRs plus the encoder
> still producing the OLD rung's bitrate for ~0.4–1 s — *ibid.*, "Drone
> air clock"

Knowing, at the moment of the first demote, that mcs−1 is also lossy but
mcs−2 is clean, lets the ladder land in **one** step instead of three.
That is the strongest single argument in the whole request, and it does
not require Reading A, B or C — it can be built against today's link.

`docs/link-adaptation.md` already reserves the slot: *"the gate state at
`rung_offset 1` is a leading indicator for demotes but does not drive one
yet — the design's §10 keeps probe-driven demote explicitly out of
scope."* Down-probes are that v2, and `flightreport.py`'s probe-lead
report is the tuning input already in place.

---

## 7. "Remove the adaptive bitrate" — not as stated, yes in a narrow window

The ladder spans mcs0–mcs5: 6.5 → 52 Mbps, an **8:1** rate span. One
fixed bitrate cannot serve it. Sized for the floor (1806 kbps at rung 0),
mcs5 runs at 6 % duty — 88 % of the link's capacity thrown away. Sized
for the top (14444), rung 0 needs 400 % of the air. There is no fixed
bitrate that is not one of those two failures.

**But narrow the MCS window and it works.** Sizing the bitrate for the
window's floor and letting the duty fall as the MCS rises:

| window | fixed bitrate | duty across the window | top-rung quality vs today |
|---|---|---|---|
| mcs4–5 | 10833 kbps | 50 % → 37.5 % | −25 % |
| mcs3–5 | 7222 | 50 % → 33 % → 25 % | −50 % |
| mcs2–5 | 5417 | 50 % → 37.5 % → 25 % → 19 % | −62 % |

Within any of those windows an MCS change costs **no `SetChnAttr`, no
keyframe, no drain** — the encoder never hears about it.

The two-step mcs4–5 window is the sweet spot: 25 % of top-rung quality
for keyframe-free switching across the top of the range. Three steps
costs half the picture and is probably not worth it.

This is, I think, the actual shape of the idea worth pursuing, and it
composes with everything above:

- a **narrow MCS window** (2 steps) with a bitrate fixed for the
  window's floor, so switching MCS costs zero encoder writes;
- the **down-probes** of §6 to pick the landing rung in one step when the
  window itself has to move (a window move still costs a bitrate write,
  so you want to make as few as possible and land them correctly);
- and Reading A's **base-at-op−1** as an optional overlay if the extra
  spine margin is judged worth 18–25 % of quality at the top rungs.

The catch is that the ladder's dynamic range is the thing being spent.
mabur flies a 30 dB+ range today by moving an 8:1 bitrate along an 8:1
rate ladder. A narrow-window link gives up the far end of that. Whether
that is acceptable is a flight-envelope question — how much of a typical
flight is spent below rung 4? — and `flightreport.py` over the existing
recordings can answer it before a line of code is written. **That is the
cheapest next step in this entire document.**

---

## 8. Recommended order

1. **Free, today:** run `flightreport.py` over the existing flight corpus
   for rung-occupancy. If flights live at rungs 4–5, the narrow-window
   design in §7 is on. If they spend real time at rungs 0–1, it is dead
   and so is most of §3. Note `max_mcs` is already 5 with the comment
   "mcs5 is unholdable at range; 5 is the reach ceiling" — the answer may
   already be known to the operator.
2. **Cheap, high value:** down-probes as a **demote-depth estimator**
   (§6.2/§6.3). Self-contained, no encoder work, no reading of §2
   required, and it attacks a pathology three findings docs already
   document. Gate on `aucadence.py` per CLAUDE.md.
3. **Hardware spike, only if Reading C is wanted:** can Star6E run a
   second VENC channel off VPE port 1 alongside 1080p60? Unanswerable
   from source. Until this returns yes, Readings B and C are not designs.
4. **Reading A only with fresh bench data.** The 2026-08-30 rollback
   stands; re-opening it needs a new alternation/jitter sweep at budget
   0.5, not an argument.

## 9. Things this study does not know

- Whether the 8812EU's A-MPDU engine will aggregate across a rate change
  or split the aggregate. Today's split is per-burst (base AU and enh AU
  are separate bursts, probe is its own PPDU), so Reading A probably
  never mixes rates inside one aggregate — but "probably" is doing work
  there, and `third_party/devourer` is not checked out in this tree.
- Star6E's second-channel capacity (§4.1).
- Whether per-rate TX power walls hold up with three-to-five rates in
  regular rotation. `maburcal` already calibrates per-MCS walls
  (`radio.rate_walls_rel`, one per HT MCS) so the mechanism exists, but
  it has been exercised against one video rate at a time.
- Every jitter figure in §3.2 is computed from the model in
  `docs/airtime-model.md` §1, not measured. The model has been right
  before (it predicted the `ltr:1` and old-UEP EMAs), which is why it is
  quoted here — but a 16 ms prediction is a reason to bench, not a result.
