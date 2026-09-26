# Rung 5 is a standing queue with no headroom — first device flight (2026-09-23)

**First device run of the link-adaptation v2 branch** (`claude/loving-cori-33yjcg`,
`6cd8724`, RC_VERSION 10). Until this session
`docs/link-adaptation-v2-proposal.md`'s status table read "Nothing here has
been on a device"; it has now.

New hardware: the RunCam WiFilink pair (`docs/deploy.md` "The RunCam WiFilink
pair"), **not** the OpenIPC URLLC AIO + Radxa Zero 3W bench rig every earlier
finding came off. Both images built from this branch — drone
`ssc338q_fpv_openipc-urllc-aio`, GS `runcam_wifilink` — mabur `6cd8724`,
devourer `587b1ae`.

| | |
|---|---|
| session | one, 96.8 s, `debug_log.enable = true`, bench, close range |
| ladder | flat pair, `ladder=0/100:50,…,5/100:50`, `down_util=0.35`, `up_util=0.15` |
| tiers | `link.overhead` OFF, `link.objective` OFF, `link.follow.restore` OFF (all observe-only) |
| link | SNR 36.8–40.7 dB, RSSI −32…−38 dBm, `pre_fec_loss` 0.0, `foreign=0` |

Instruments: `tools/flightreport.py` over the session directory, plus ad-hoc
scripts over `au.log` + `lat.log` (session scratchpad only, not committed;
the tables below carry the numbers so they can be redone).

## TL;DR

1. **The whole latency excursion is one stage.** `lat.log` decomposes it:
   `enc=7  dq=1  air=1–10  dec=8–10  reg=5–11  dsp=5` are flat and healthy
   for the entire session at every rung. `fec` — the AU **arrival span**,
   not decode cost — is 6–11 ms at rungs 0–4 and 150–706 ms at rung 5.
   Nothing else moves.

2. **Rung 5 carries a ~220 ms standing queue at equilibrium, indefinitely.**
   With a static scene the plateau is stable for 30+ s: `fec` wanders
   137–255 ms with no trend, mean AU size flat at ~30.4 kB, `air_pct` ~50 %,
   loss 0.0. A pipeline with adequate throughput should sit near-empty.
   ~220 ms is ~13 frame periods permanently in flight. **This standing
   queue is the defect** — it consumes the headroom, so nothing is left to
   absorb jitter.

3. **Motion overflows it; the mean bitrate never changes.** Mean AU size is
   ~30 kB across the whole plateau. What breaks is `max_sz`:

   | t (s) | mean_sz | max_sz | fec (ms) | |
   |---|---|---|---|---|
   | 755–787 | ~30 400 | 32 500–36 600 | 137–255 | static |
   | 788 | 29 022 | **42 291** | 407 | hand enters frame |
   | 790 | 31 697 | 49 853 | 444 | |
   | 792 | 30 334 | **51 474** | 467 | |
   | 795 | 31 714 | 51 501 | 526 | demote fires |

   A ~40 % rise in *peak* frame size, at constant mean, is enough. This is
   `docs/airtime-model.md`'s "jitter ∝ frame size" with no margin left to
   absorb the jitter.

4. **The demote is a control-plane timeout, not loss.** `reason=timeout`
   (`feedback_timeout_ms = 1000`) with `u=0.0000` at the instant it fires.
   Three demotes: `5→0 timeout` ×2, `2→0 starved` ×1. Because fade was never
   the path, `link.fade.min_rung = 2` never applied — which is why it falls
   all the way to **rung 0** rather than stopping at 2. `air_pct` spikes
   **48 % → 385–418 %** across the collapse: instantaneous oversubscription
   starves the RCF slot, the GS stops hearing feedback, and the ladder
   bottoms out. Recovery is the ordinary climb (`restore = false`), ~3.1 s
   per rung, so the cycle repeats every ~40 s.

5. **Rung 5 pays 2× for repair it measurably never needed.** `fec.log` at
   mcs5: `ov_req` p50 **0.40**, max **0.46**, against **1.00** commanded.
   Tier 1 wanted 0.50 in **100 %** of samples at every rung 1–5 (`n=157` at
   rung 5). "Would fail at ov 0.50: 0 of 12". That wasted third of rung 5's
   airtime is exactly the margin a 51 kB frame needs.

6. **Arming tier 1 as designed would NOT fix this — now MEASURED, not
   predicted.** flightreport prices it as +33 % video at rung 5: the
   recovered airtime becomes bitrate, not slack. Run 2 below cut
   `overhead_base` 1.0 → 0.6 and watched the encoder reclaim **~85 % of the
   saving as video within seconds**. For this defect the freed airtime has
   to be *prevented* from becoming bitrate.

## What this is not

Each of these was a live hypothesis before the logs arrived; each is closed
by a measurement in this session:

| Hypothesis | Killed by |
|---|---|
| TX power baseline (`power_mode = "none"` leaving a stale chip offset) | SNR 36.8–40.7 dB, RSSI −32…−38 dBm, EVM −25.9 dB at rung 5 |
| Sensor stuck below 60 fps after a cold boot | 5753 AUs / 96.8 s = **59.4 AU/s**; rung 5 measures 58.9 AU/s; `enc` flat at 7 ms |
| RC size bounds reverting → oversized IDRs every GOP | ramp is not a 0.5 Hz sawtooth; `air` and `dec` never move |
| Channel / interference | `foreign=0`, `fa=1–2`, `pre_fec_loss` 0.0, `verdicts: healthy 76 / interfered 47` with no correlation to the demotes |
| First-boot-after-flash bootargs (`LX_MEM`/`mma_heap`) | the fault tracks **rung**, not boot |

MCS 5 is not failing on link quality. It fails with a 40 dB SNR and zero
loss, carrying its mean rate fine.

## Continuity

This is the same shape as `docs/air-clock-flight-findings-2026-09-06.md` §1
— "zero headroom by coincidence … a queue at 95 % load integrates every
burst, so a 3 % pricing error is a 2–3 × backlog error". That page found it
between the bitrate policy and the air-clock efficiency constant. This one
finds it between rung 5's bitrate target and what the rung actually flies,
on different hardware, with the overhead pair as the visible sink.

Corollary worth keeping: with a 220 ms standing backlog and
`air_clock.shed_ms = 25`, the enhance gate is permanently shed at rung 5.
The salvage counters show it — rung 5 `corrupt=75 salvaged=129 sub_fail=171`
against single digits at every other rung.

## Anomalies logged, not chased

- `follow`: `above_ignored=1`, which flightreport itself flags as "should be
  0: the drone only ever descends on its own authority".
- mcs5 is the **only** rung with a negative completion→probe offset
  (p50 −2.59 ms vs +0.86…+2.05 elsewhere, min −599 ms). That is the
  base−enh completion signature `tools/bench/aucadence.py` gates on, and
  `aucadence` has still not been run on a device.
- One truncated line in `flight.jsonl` (576 parsed, 1 unparseable) — a
  sideport datagram cut at ~8 kB. Cosmetic here; would matter to a consumer
  that does not skip bad lines.

## Next

1. `max_mcs = 4` is a working link today: rungs 0–4 measured clean, 38–54 ms
   e2e, 6–11 ms `fec`, no timeouts.
2. The open question is **where the 220 ms sits** at static equilibrium.
   ~50 % `air_pct` says throughput is adequate for the mean rate, so this is
   a pacing/queue-depth question, not a throughput one.
3. Peak-to-mean is the targeted knob: 51 kB against a 30 kB mean is 1.7×.
   `max_ipprop` and the encoder size bounds in `docs/airtime-model.md` exist
   for this.
4. `aucadence.py` before any change to the overhead pair, per CLAUDE.md —
   and note anomaly 2 above suggests it has something to say already.


## Run 2 — `overhead_base` 1.0 → 0.6 at rung 5 (same day)

Single-variable A/B on the same pair, same bench, same static scene.
`superframe_p_pct` left at 200. Change verified in the loaded config, not
just the file: `ctl.log` header reads
`ladder=0/100:50,…,4/100:50,5/60:50`, and `fec.log` mcs-5 base rows carry
`ov 0.60`.

### The mechanism is confirmed

| | run 1 (ov 1.0) | run 2 (ov 0.6) |
|---|---|---|
| rung 5 arrival span, first ~7 s | 212.1 ms | **22.7 ms** |
| rung 5 arrival span, later | 212 ms, stable | 133.3 ms, climbing |
| `fec` p50 (whole session) | 212 at rung 5 | **9 ms** |
| `e2e` p50 / p90 | 86 / 480 ms | **44 / 63 ms** |
| rungs 0–4 span | 8.8 ms | 8.7 ms (unchanged, as expected) |

Cutting rung 5's base overhead collapsed the standing queue by **9×**, from
212 ms to 22.7 ms. The bracket-vs-budget mismatch is the mechanism.

### The failure mode changed, qualitatively

| | run 1 | run 2 |
|---|---|---|
| demote reason | `timeout` ×2 (control-plane starvation) | `s3_residual` ×2 (measured loss) |
| demote depth | 5 **→ 0**, the floor | 5 **→ 4**, one rung |
| `air_pct` at the event | 48 % → **385–418 %** | no excursion |

No feedback timeout in this run. What remains is an ordinary loss-driven
single-rung demote — the ladder behaving as designed, rather than the
control plane starving and bottoming out. ⚠ **Rarer, not gone:** run 4
below logged one `5→0 timeout` at `ov_base` 0.6 in ~190 s at rung 5, a
different shape from run 1's (see "Run 4 — the `ov 0.6` timeout").

### Protection was not cut too close

`fec.log` mcs5 at ov 0.60: `ov_req` p50 **0.34**, p90 0.39, max **0.47**,
and *"would fail at ov 0.50: 0 of 19"*. 0.60 carries margin, and even 0.50
would have covered every episode in this session. Tier 1 still wants 0.50
in 100 % of rung-5 samples.

### Why it is incomplete: the encoder reclaimed the saving

| | video rate | on-air rate |
|---|---|---|
| run 1, ov 1.0 | 15.2 Mbps | 26.5 Mbps |
| run 2, ov 0.6, early | **16.3 Mbps** | 25.2 Mbps |
| run 2, ov 0.6, later | 16.4 Mbps | 25.4 Mbps |

Dropping `ov_base` 1.0 → 0.6 should have freed ~3.0 Mbps of on-air. Only
**1.3 Mbps** materialised, because the commanded video rate rose 15.2 →
16.3 Mbps — the bitrate policy sizes video from the rung's capacity *net of
the overhead bracket*, so cutting the bracket raises the video command by
construction. **The encoder ate ~85 % of the saving.**

⚠ **Corrected from the run-2 data itself.** This page first estimated the
allowance at ~19–21 Mbps from PHY 52–58 × `efficiency` 0.73 ×
`airtime_budget` 0.5. That is wrong: those two constants govern the
encoder's rate *command*, not what the medium carries. Measured instead
from the queue's own fill rate — span 111 → 173 ms over 4 s is a **1.55 %
deficit** against 25.2 Mbps offered — actual deliverable capacity at mcs5
on this pair is **≈ 24.8 Mbps**. Run 2 overshoots it by only ~0.4 Mbps,
which is why it fills slowly rather than not at all.

**Run 2 never reached equilibrium, and was static throughout.** AU size is
flat at ~34,450 B from the moment rung 5 is entered (t=95 s) — the encoder
settled immediately and never ramped — yet the span climbs monotonically
22.7 → 173 ms to the end of the log. At ~15 ms/s it reaches the ~270 ms
`frame_lookahead` ceiling about 6 s after the log ends, i.e. it parks
exactly where run 1 parked. **Run 2 bought time (3 s → ~15 s to fill), not
headroom.**

**The encoder did not "choose" to take the saving — it clipped.**
`encoder.bitrate_max_kbps = 16000` programs 16.384 Mbps and measured video
is 16.3–16.4: the command is pinned at the ceiling. In run 1 the policy
commanded 15.2 Mbps, under the cap. Cutting the bracket raised the command
until it hit the rail, which is why ~85 % of the saving vanished rather
than some arbitrary fraction.

This is the tier-1 caution in TL;DR §6 demonstrated on hardware: **overhead
relief does not become headroom unless something stops it becoming
bitrate.**

### Next

The remaining lever is the video rate itself, so the saving lands as slack.
Against the **measured** ~24.8 Mbps capacity, and the 1.55× bracket at
`ov_base` 0.6: `bitrate_max_kbps` 16000 → **14000** gives video ~14.0 and
on-air ~21.7 Mbps, ~12 % headroom. That is run 3 — and it must hold rung 5
static for **≥ 60 s**, the question run 2's ~10 s window could not answer:
does the span plateau, or only creep more slowly?

Unchanged from run 1: `dq` is 1 ms, `enc` 7.2 ms — the drone still queues
nothing and encodes fine. `frame_lookahead = 8` remains where the backlog
parks when there is one.

## Run 3 — `bitrate_max_kbps` 16000 → 14648, `ov_base` 0.6 held (same day)

The video-rate cap run 2 called for. Drone `bitrate_max_kbps` 14648
(= 15.00 Mbps, since star6e programs `kbps × 1024`), GS rung 5 `ov_base`
0.6 unchanged, `superframe_p_pct` still 200. Cap confirmed to bind: mean AU
34,400 → **30,866 B**, video 16.3 → **14.7 Mbps**, on-air **22.7 Mbps**
against the measured ~24.8 capacity — **~8.5 % headroom**.

### The standing queue is gone

Rung 5 held ~60 s. Span across 40 s of it, sampled every 3 s:

```
t_s   mean_sz  max_sz   span   e2e   fec
113     30820   44798   16.4    47    17
119     30725   31465   16.4    59    25
128     30921   34984   25.2    50    20
137     30898   31446   16.8    49    18
143     30805   52820   17.8    64    24   <- 52.8 kB peak frame, absorbed
152     33530   36528   23.1    51    19
```

**Flat. No trend over 40 s.** And the t=143 row is the point: a 52.8 kB peak
— larger than any frame in run 1 — passed through with the span unmoved.
That is what headroom buys.

### Three runs

| | run 1 | run 2 | run 3 |
|---|---|---|---|
| `ov_base` / `bitrate_max_kbps` | 1.0 / 16000 | 0.6 / 16000 | **0.6 / 14648** |
| video | 15.2 Mbps | 16.3 | **14.7** |
| on-air (capacity ≈ 24.8) | 26.5 | 25.2 | **22.7** |
| rung-5 span | 212 ms, parked | 22.7 → 173, climbing | **16–23 ms, flat** |
| `e2e` p50 | 86 ms | 44 | **50** |
| `e2e` p90 | 480 ms | 63 | **60** |
| `e2e` worst | **741 ms** | 173 | **101** |
| demotes off rung 5 | `timeout` ×2, to rung **0** | `s3_residual` ×2, to rung 4 | `residual` ×1, to rung 4 |

Worst-case latency fell **741 → 101 ms**, and the failure mode went from a
control-plane collapse to the floor, to one ordinary single-rung demote
that recovered in 3.7 s.

Note the video comparison that matters: stock `ov 1.0` could only have
carried ~12.6 Mbps *cleanly* (24.8 ÷ 1.75 with margin). Run 3 delivers
**14.7 Mbps with flat latency** — more usable video than the shipped
configuration could ever sustain, not less.

### The cut is not free — first failure at ov 0.6

`fec.log` mcs5 at ov 0.60, run 3: **`n=30 stale=1 failed=1`**, with
`ov_req` p50 0.38 / p90 0.39 / **p99 = max = 0.79**. *"would fail at ov
0.50: 1, 0.75: 1, 1.00: 0"*.

One episode needed 0.79 and failed at 0.60; it would have survived at 1.00.
That is the `residual` demote at t=108303 (`u=0.1477` — a 14.8 % pre-FEC
loss burst at 39 dB SNR, so a burst rather than a fade). Runs 1 and 2 had
zero mcs5 failures.

So the honest accounting: cutting rung 5's base overhead from 1.0 to 0.6
buys a flat queue and +2 Mbps of usable video, and costs roughly one failed
episode per minute of rung-5 flight **on a clean bench at 39 dB**. At range,
where `ov_req`'s tail is fatter, that rate will rise. 0.6 is a bench result,
not a flight-validated setting.

### Status

MCS 5 is usable: 14.7 Mbps, `e2e` p50 50 ms / p90 60 ms / worst 101 ms, no
standing queue, motion absorbed. The defect this page opened with is
closed. (No timeouts in this 60 s run; run 4 found them rarer, not gone.)

Still outstanding before any of this ships:

1. **`tools/bench/aucadence.py` has still never run on a device**, and
   CLAUDE.md gates exactly this class of change (UEP overhead + bitrate
   policy) on it. Run 1 also flagged mcs5 as the only rung with a negative
   base−enh completion offset.
2. **Range.** Every number here is 36–40 dB SNR on a bench. The one ov 0.6
   failure is the leading indicator to watch.
3. `superframe_p_pct` was not needed for the standing queue — at 14.7 Mbps
   the headroom absorbs a 52.8 kB peak unaided. Run 4 measured it as a
   working peak clipper (see below), but it did not move the spike tail.

## Run 4 — one 11-minute session: `ov 0.6` → `ov 0.5` → calibration (2026-09-24)

One `debug_log` session, 473–1137 s, three configurations separated by a GS
restart and a `maburcal start`:

| segment | time | GS rung 5 | notes |
|---|---|---|---|
| A | 473–679 s | `ov_base` 0.6 | `ctl.log` header `5/60:50` |
| — | 680–715 s | — | GS restart; header becomes `5/50:50` |
| B | 715–875 s | `ov_base` 0.5 | pre-calibration |
| — | 875–963 s | — | `maburcal start`: 88 s with no video |
| C | 963–1137 s | `ov_base` 0.5 | post-calibration, `power_mode = "offset"` |

Drone throughout: `bitrate_max_kbps` 14648 and — measured, not configured
from here, see next section — `superframe_p_pct` 140.

### `superframe_p_pct` 140 is a real peak clipper, at no mean-rate cost

Rung-5-sized AUs (> 20 kB), share of frames per 500 B bin around the
140 % threshold (31,249 B budget × 1.4 = **43,749 B**):

| bin | session 4, cap 140 (n = 24,807) | run 3, cap 200 (n = 3,497) |
|---|---|---|
| 40,000–42,499 | **0 frames** | 16 (4.6 ‰) |
| 42,500–42,999 | 12 (0.5 ‰) | 0 |
| 43,000–43,999 | **545 (22.0 ‰)** | 4 (1.1 ‰) |
| > 44,500 | 16 (0.6 ‰) | 10 (2.9 ‰) |
| p50 | 30,300 B (14.4 Mbps) | 30,845 B |
| max | 47,245 | 52,820 |

An empty band below the threshold and a spike just under it is the REENCODE
signature: frames that would have been 45–55 kB are re-encoded down to just
beneath the cap. The median is untouched. The operator had meant to revert
to 200 before this session; the bitstream says the drone ran 140 throughout.

⚠ **Correction.** Earlier in this investigation (chat, never in this page)
an unlogged OSD reading of ~10.6 Mbps at `superframe_p_pct` 140, plus the
2026-08-27 probe's ~53 % cap-to-mean ratio in `docs/airtime-model.md`, was
read as "the RC re-plans the whole stream to about half the cap, so any cap
that clips peaks halves the mean". The logged session contradicts that. The
probe's 6000 B cap sat *below* its 11.1 kB mean — a different regime, where
cutting the mean is the only thing a cap can do. With the cap above the
natural p99, only the tail moves. The 10.6 Mbps figure has no log behind it;
rung 4 runs ~10.9 Mbps.

**But it did not shorten the spike tail.** Segment B (cap 140, `ov` 0.5):
`e2e` high p90 93 ms, worst 123 ms. Run 3 (cap 200, `ov` 0.6): 90 ms, 101 ms.
Removing the > 44 kB frames left the tail where it was, so the residual
spikes are not driven by peak frame size.

This answers `docs/handover-venc-overshoot-2026-09-03.md`'s open question
and its "sweep before shipping non-zero": on star6e `superframe_p_pct` binds,
and at a cap above the natural p99 it clips P-frame peaks without a mean
cost. One bench session, one scene; not flight-validated.

### `ov_base` 0.5: no latency gain, a measurable protection cost

| rung 5, base layer | episodes | failed | `ov_req` max | would fail at 0.75 |
|---|---|---|---|---|
| `ov_base` 0.6 | 105 | 1 (~1 %) | 0.58 | 0 |
| `ov_base` 0.5 | 180 | 3 (~1.7 %) | 0.72 | 0 |

Latency at 0.5 matches run 3 at 0.6 (high p90 93 vs 90 ms). Segment A's
worse tail (high p90 115 ms, worst 780 ms) is dominated by the one event
below, not by the overhead. The extra failures are the extra 5→4 demotes the
operator saw. **Recommendation: stay at 0.6.**

### Run 4 — the `ov 0.6` timeout at 575 s

```
 t_s  air%  mean_sz  max_sz  span   e2e p50/hi   fec
 573    51    30393   43297    22     54/100      20
 574    35    30334   31136   574     65/780      30
 575   452    29956   43775   306    405/695     369   <- 5->0 timeout
 576    50     3188    4012     7     42/98        6
```

Not run 1's mechanism. There is no gradual build-up and no oversize frame;
the span jumps 22 → 574 ms in one second while `air_pct` *falls* to 35 %,
then surges to 452 % as the backlog releases. It reads as a ~0.5 s delivery
stall. Cause not determined from these logs — an off-channel dwell, a USB
stall and an RF burst would all look like this here. One event in ~190 s of
rung 5.

### Calibration made it worse: the GS split a receiver off the sweep channel

`maburcal start` ran at 875 s. From `scan.log`:

```
M 712517  all 136 -> 120  commit       session op channel 120 (home 136)
M 880404  0   120 -> 136  split_home   5 s into the run
M 943268  0   136 -> 120  reunite      after the sweep
```

The run takes the video link down by design, so after `split_after_ms`
(5 s) `ChannelPlan` did what it does on any loss and sent card 0 home.
`cal.log`'s coarse pass shows the result: **card 0 heard 43 of 216 cells**
and nothing from row 1 index 23 onward. The walls were measured on one
antenna.

What that produced:

- **Walls no single PA produces:** MCS0 +63, MCS1 −33, MCS2 +63, MCS3 23,
  MCS4 −9, MCS5 −4. `docs/calibration.md` treats a 3 dB MCS0/MCS1 spread
  as a reproducibility failure; this is 96 indices.
- **MCS0 parked at the +63 rail**, and `legacy_wall_rel` is set from MCS0
  by design, so control frames went out near full power. `ctrl`-class EVM
  **−17.1 → −10.2 dB** while RSSI rose −34.0 → −32.6 dBm: PA compression.
  Per-rung EVM tracks the walls exactly: rungs 0 and 2 (rail) read about
  −10 dB after the run, rungs 1 and 4 (−33, −9) read about −34 dB.
- **MCS2 failed its own verify pass:** 62 % delivery.

Effect at rung 5, same `ov_base` 0.5, before (B) against after (C):

| | B pre-cal | C post-cal |
|---|---|---|
| seconds with an `e2e` spike > 100 ms | 2 | **10** |
| worst `e2e` | 123 ms | **275 ms** |
| `5→0 timeout` | 0 | **2** |

Why it was never seen before: `ChannelPlan::tick()` already returns early
when `op == home` ("the split has nothing to split"). A bench whose session
channel is the home channel cannot hit this. The boot scan here committed
120 with home 136.

**Fixed in the same commit as this section** — the GS half of what the
drone already does. The drone latches a retune requested mid-sweep and
replays it on `cal_active`'s falling edge (`docs/channel-select.md`); the GS
had no such guard. Now every GS-initiated card move is gated on one
predicate, `CalSession::running()` (AwaitAck through Verify, wider than
`radio_silent()` because the link is down for the whole run):

- `ChannelPlan::set_calibrating()`: the split is **deferred, not
  excluded**. The loss timer keeps counting, but no card leaves `op`. This
  deliberately differs from a hop's window, which is excluded. A run ends
  with the drone in `RENDEZVOUS` every time and the drone replays its
  deferred move home on the falling edge, so the accumulated loss is real
  evidence of where the drone is — the split fires on the first tick after
  the run and meets it there.
- `hop_active(in_session, scout_joined, calibrating)`: the verdict engine,
  freshness burst and hop controller stand down for the run. `in_session`
  alone did not cover it: the session re-linked briefly mid-run (943 s,
  949 s) and scout dwells ran in exactly those windows.
- The periodic in-flight scout thread gates on the same predicate.

Tests: `test_channel_plan` (deferred for the whole run; fires on the first
tick after; never advanced by a short run; a mid-run re-link restarts the
clock; one-card holds its only radio), `test_cal_session` (`running()`
across AwaitAck → Sweep → Verify → Done, and after Failed and `abort()`),
`test_hop_burst_gate`. With the one-line guard removed,
`calibration_defers_the_split_for_the_whole_run` fails exactly as the
bench did. Host suite 143/148; the five failures are environmental in the
build container (no sibling `../devourer`, no `nix-shell` for ffprobe,
running as root). With `../devourer` supplied, `host_e2e`, `gs_e2e` and
`gs_au_e2e` pass; `player_e2e` passes up to its ffprobe step.

**Operator consequence:** any calibration run before this fix on a session
whose operating channel was not the home channel measured its walls on one
card. Roll it back (`cp /etc/mabur.toml.pre-cal /etc/mabur.toml`) and re-run
once this `maburgs` is deployed. GS binary only: no wire, config or
`RC_VERSION` change, no flag day.

### Still open

- ~~The residual spike tail at rung 5.~~ **Found, runs 5–7 below**: the
  hop block's freshness burst, not the periodic scout.
- The run-4 575 s stall.
- `aucadence.py` on a device; mcs5 remains the only rung with a negative
  completion→probe offset in every run.

## Runs 5–7 — the residual tail is the hop freshness burst (2026-09-26)

Setup for all three: channel 165 pinned as home on both ends
(`radio.scan.enable = false`), `ov_base` 0.6 at rung 5,
`bitrate_max_kbps` 14648, `superframe_p_pct` **200** (the shipped default — the `.pre-cal` restore brought it back, so NOT the 140 of run 4; confirmed on the drone after run 7), calibration rolled back
to `.pre-cal`, `hop.enable = false`, `hop.scout_when_disabled = false`,
`link.objective.enable = true` (tier 2 observe; never armed — rung-5 loss
never approached its 12.5 % threshold). Between runs 4 and 5 a field
flight on 165 and a comparison against a second, hardware-identical
WiFilink pair (another operator's flight, newer upstream build) had
established that the remaining tail is NOT offered load (the second pair
carries the same ~14.2 Mbps at 1.0/0.5 with a 15 ms `fec` stage and 0 of
140 rung-5 seconds over 100 ms), NOT signal level (flat ~30 % tail from
−60 to −40 dBm here, clean down to −10 dBm there) and NOT the drone TX
queue (5 ms wait, ≤ 1 ms air backlog in every run on both pairs).

**`scout_when_disabled = false` does not stop in-session dwells.** It
gates only the periodic scout thread. The core loop's freshness burst
(`hop_burst_due`, a back-to-back sweep of every candidate, run
synchronously on the core thread) keys on the verdict trigger, which the
shadow controller still latches on `interfered`/`impaired` windows. On
the home bench — `interfered` in 42–61 % of verdicts even on 165 — it
fired at its `dwell_period_ms` floor (334 ms minimum gap measured):

| run | bursts (`scan.log` D rows) | rung-5 s with `fec` max > 40 ms | > 100 ms seconds | worst second | `fec` max p90 |
|---|---|---|---|---|---|
| 5 (bench, default 333 ms) | 108 (324) | 74 % | 11 / 72 | 595 ms | 91 ms |
| 6 (bench, 60000 in the file, GS not restarted) | 120 (360) | 83 % | 13 / 64 | 758 ms | 78 ms |
| **7 (bench, 60000 loaded)** | **2 (6)** | **4 %** | **1 / 69** | **109 ms** | **36 ms** |
| second pair, flight | — | 6 % | 0 / 140 | 99 ms | 38 ms |

Run 7's channel was *busier*, not quieter (297 `interfered` verdicts vs
213/217). In runs 5–6, AUs whose first packet landed within 50 ms of a
burst had a > 40 ms arrival span 34–40 % of the time, against 14–15 % for
the same window 500 ms earlier. The second pair runs these bursts too,
without the effect; not investigated (different build).

Fix: `hop_burst_due` takes `dwells_allowed` = `hop.enable ||
hop.scout_when_disabled`, the scout thread's own start predicate, so
`scout_when_disabled = false` now means no in-session dwells at all.
`test_hop_burst_gate` covers it; removing the gate fails the new case.
GS binary only. Until that `maburgs` is deployed, `hop.dwell_period_ms =
60000` is the stand-in (it also slows the periodic scout, which is off
anyway with `scout_when_disabled = false`). With the hop *enabled* the
burst still blocks the core thread; moving it off that thread is open.

A "white flash" of the kind the operator has reported since the first run, caught
on camera in run 7 (OSD `lat 64/111`, the run's only > 100 ms second, at
~70 s): no FEC failure anywhere in the run, and the AUs in that second
jump from ~30 kB to 46–51 kB with 37–63 ms arrival spans — a real scene
change (the camera's exposure swinging towards the bright window), not
decoder concealment of lost data. At `superframe_p_pct` 200 the P-frame
cap sits at ~62 kB at this rate, so nothing clipped them; the operator
reports the same reaction to fast hand motion on the bench and to whipping
the quad in the field. Next: 120 (~37 kB; the 30–40 kB bucket arrived in
≤ 24 ms p90 in run 7) with a static/motion-alternating bench run.
