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

The feedback timeout is gone. What remains is an ordinary loss-driven
single-rung demote — the ladder behaving as designed, rather than the
control plane starving and bottoming out.

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
standing queue, no timeouts, motion absorbed. The defect this page opened
with is closed.

Still outstanding before any of this ships:

1. **`tools/bench/aucadence.py` has still never run on a device**, and
   CLAUDE.md gates exactly this class of change (UEP overhead + bitrate
   policy) on it. Run 1 also flagged mcs5 as the only rung with a negative
   base−enh completion offset.
2. **Range.** Every number here is 36–40 dB SNR on a bench. The one ov 0.6
   failure is the leading indicator to watch.
3. `superframe_p_pct` was never needed — at 14.7 Mbps the headroom absorbs
   a 52.8 kB peak unaided. It stays the lever if the bitrate is pushed back
   up.
