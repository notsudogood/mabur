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

6. **Arming tier 1 as designed would NOT fix this.** flightreport prices it
   as **+33 % video** at rung 5 — the recovered airtime becomes bitrate, not
   slack, restoring the zero-headroom condition at a higher rate. The
   standing queue would return. For this defect the freed airtime has to
   become headroom.

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
