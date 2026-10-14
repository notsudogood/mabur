# In-flight channel change spike — findings 2026-09-14

**Status:** spike answered — the sideport sensors separate 802.11 contention, non-802.11 energy and fade (devourer's stock INTERFERENCE verdict does not); one card can run the verdict but every scout dwell costs video. Built as the in-flight hop. Current: docs/inflight-channel-hop.md.

Bench spike answering three questions before any in-flight channel
migration design: (1) can the GS tell interference from a fade, (2) where
would it jump and how would it scan, (3) does it work with one GS card.
Nothing was built. The runner and analysis scripts were throwaway; the
data is in GS session `/media/dvr/log/0078` (`flight.jsonl`, `scan.log`)
and the condition boundaries are listed at the end.

Setup: drone on the desk, two-card GS, rung 5 (MCS 5) at rest, sideport
sampled from `flight.jsonl`, `ausniff.py` alongside every window. The
interferers were a spare RTL8822EU on the PC next to the GS driven by
`tools/bench/benchjam.sh` (txdemo, QoS-Data, 6 Mbit/s legacy, 1000 or
1500 B) and an analog VTX on E2 (5685 MHz). The fade was the drone
carried out of the room. For the DJI O4 run the home channel was moved
to 165 on both ends (O4 at 5825 MHz, 20 MHz).

## Per-condition sensor table

Medians over the 60 s window unless noted. `fa`, `cca`, `own`, `foreign`
are the 1 Hz per-card energy record (`cards[i].energy`), `loss` is the
per-card `loss_pct`, `pre-FEC` is `link.pre_fec_loss`. RSSI/SNR/EVM are
the `s0` video class, card 1.

| condition | link ch | rung | fps | pre-FEC med / p90 | card1 loss | foreign/s | fa/s | crc fails | RSSI | SNR | EVM | LinkHealth (card1) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| baseline | 120 | 5 | 59.5 | 0 / 1.1 % | 0 | 0 | 4 | +12 | −61 | 29 | −28 | HEALTHY 100 % |
| jam 240 pps on **136** (80 MHz off) | 120 | 3–5 | 59.4 | 0 / 1.8 % | 7.5 % | 0 | 209 | +6095 | −59 | 28 | −22 | MARGINAL 76 %, INTERFERENCE 8 % |
| jam flat-out on **136** (80 MHz off) | 120 | 3–5 | 59.4 | 1.1 / 3.2 % | 7 % | 0 | 299 | +8418 | −60 | 23 | −21 | INTERFERENCE 47 % |
| jam 240 pps on **120** (co-channel) | 120 | **0** | 59.5 | 4.3 / 8.5 % | 3.8 % | 237 | 2 | +15 | −55 | 33 | −11 | MARGINAL 91 % |
| jam flat-out on **120** (co-channel) | 120 | **0** | **36** | 29 / 57 % | 24 % | 314 | 1 | +27 | −54 | 33 | −11 | MARGINAL 91 % |
| baseline on 136 (scan off) | 136 | 5 | 59.7 | 0 / 1.1 % | 0 | 0 | 2 | +6 | −58 | 30 | −29 | HEALTHY 100 % |
| analog VTX E2 next to GS | 136 | 0 | **0.2** | 99.5 / 100 % | 97 % | 0 | 250–2650 (10 reads) | +390 | — | — | — | NO_SIGNAL 72 % |
| fade (drone out of the room) | 136 | 0–2 (med 1) | 59.5 | 0 / 1.0 % | 1.3 % (card0 12 %) | 0 | 10 | +629 | **−86** | **10** | −9 | **WEAK 100 %** |
| baseline on 165 (after a 10 s burst cleared) | 165 | 5 | 60 | 0 / 1 % | 0–1 % | 0 | 0–5 | — | −54 | 32 | −25 | HEALTHY |
| DJI O4 on 5825, 20 MHz, next to GS | 165 | **0** | **10.7** | 85 / 96 % | 89 % (card0 98 %) | 0 | **388** (card0 744) | +7766 | −50 | 27 | −7 | INTERFERENCE 55 % (card0 100 %) |

ausniff: 0 frame gaps on every condition except the two flat-out jams
(2–3 gaps off-channel, 742 gaps and 35.8 fps co-channel), the analog
cliff (1824 gaps) and the O4 run (2989 gaps, 11.0 fps, 647 of 659 AUs
incomplete).

## Q1 — interference vs fade

**Yes, the sensors already on the sideport separate the three cases, but
not the way devourer's stock classifier does.**

- **Fade**: RSSI −61 → −86/−90 dBm and SNR 30 → 7–10 dB on both cards, FA
  and foreign stay at rest, CRC fails rise moderately. devourer's
  `classify_link_health` said WEAK on every sample. Unambiguous.
- **Co-channel 802.11** (another network, the realistic case): RSSI and
  SNR did NOT fall (they rose: −61 → −55, 29 → 33 — the decoded frames
  carry the interferer's energy). FA stayed at 0–2 and CRC fails at rest,
  because the interferer's frames decode fine. The tells were `foreign`
  frames (0 → 237–314/s), CCA busy, per-card loss and the recovered-symbol
  rate (2–5x). devourer's INTERFERENCE verdict, gated on FA > 300, **never
  fired**; it is a raised-floor detector, not a contention detector.
- **Analog / non-802.11 energy**: FA 250–2650/s with cca ≈ fa and zero
  foreign frames. This is the case LinkHealth's INTERFERENCE verdict was
  calibrated for, and it would have fired — on the reads that happened
  (see the gap below).
- **DJI O4 co-channel** (the realistic non-802.11 case): same family as
  analog but graded — the link stayed in SESSION the whole minute at rung
  0 and 5–30 fps. FA held at 500–930/s on card 0 and 170–850/s on card 1
  (cca ≈ fa, own 14–50/s against 2480 at rest), foreign 0, CRC fails
  130/s. RSSI on the frames that did decode ROSE to −48 (O4 energy inside
  the PPDU) and SNR still read 27–33 dB: **RSSI and SNR say nothing
  here**. LinkHealth said INTERFERENCE on 100 % of card-0 samples and 55 %
  of card-1 samples (the 300 FA gate straddles card 1's swing). This is
  the case where a hop is the ONLY fix: the ladder at rung 0 delivered
  11 fps.
- **Off-channel near-field blocking** (jammer 80 MHz away, 1 m from the
  GS): FA 200–300, CRC fails 100/s, EVM −22, RSSI flat. Looks like a
  raised floor. Not a flight case unless another pilot's VTX is next to
  the GS, but it is what a bench jammer produces when the channel is
  wrong — check `link.channel` before believing a jam run.

Two confounds a mabur classifier must handle:

1. **EVM is op-point dependent, not an interference tell.** Under
   co-channel jamming EVM read −11 dB only because the ladder had gone to
   rung 0, whose resting EVM is −10.9 (the rung store's per-rung EWMA).
   Any EVM signal has to be a delta against that rung's own EWMA.
2. **The FEC hides loss from a link-level delivery trigger.** Post-
   diversity `pre_fec_loss` stayed under 5 % in 97 % of windows while card
   1 alone lost 7 % median. chanmig's leg 1 (`ActivePolicy.loss_impair`
   0.05 on delivery) would almost never fire if fed link loss; feed it
   per-card loss or `streams[i].recovered` rate instead.

**The ladder is today's interference response, and it is the wrong one.**
At 240 pps co-channel it went straight to rung 0 (16 → ~2 Mbit/s) with
zero counted fade demotes and video still at 59.5 fps; flat-out it could
not save the link at all. A hop would have restored rung 5. The
fade-vs-interference verdict is precisely the input that lets the ladder
say "don't demote, hop".

Recommended per-window rule for mabur (thresholds to be calibrated from
these runs, not from devourer's):

```
weak        = rssi both cards < R_weak  AND snr < S_weak         -> fade, ladder owns it
contended   = foreign/s > F_min  OR  (cca - own) high             -> 802.11 neighbour, hop
raised      = fa/s > FA_min  AND  foreign ~ 0                     -> non-802.11 energy, hop
impaired    = per-card loss > L  OR  recovered rate > k x rest    -> gate; nothing fires without it
```

## Q2 — where to jump, how to scan

devourer chanmig's leg 2 is a **second adapter** dwelling a candidate
plan (`ScanPlan`/`SurveyRecord`), folded by `EvidenceStore` and scored by
`RecommendEngine`: a candidate must be fresh (< 60 s), observed for ≥ 3
rounds and ≥ 3 s per bin, under 20 % occupancy, not bursty, and better
than the current channel by a margin. mabur's boot scan already
implements that dwell loop (`gs/src/channel_scout.cpp`, `channel_ranker`)
but freezes at first ack and never dwells in SESSION.

In flight the scout costs card 1's diversity for every 250 ms dwell
(single-card pre-FEC misses are ~0.75 % vs the two-card shadowing of 92 %
of them, flight 0046/0050). Options, cheapest first:

1. **Scout on demand.** Only start dwelling once the Q1 rule says
   contended/raised. The link is already bad, so the diversity cost is
   paid when it is cheapest. Boot-scan ranking (`K` line) is the stale
   prior to order the candidates.
2. **Continuous slow scout** (chanmig's model): card 1 dwells one
   candidate every N seconds. Pays diversity every N s for evidence that
   is fresh when needed. Cost per dwell is unmeasured (the boot scan does
   not dwell in SESSION; needs a debug knob to measure ausniff gaps).
3. **Blind jump** to the next-best boot-scan channel with a
   verify-or-rollback window (chanmig's responder already models
   `rollback_ms`, `verify_ms`, recovery dwell). No scout at all.

The move itself exists: DISC proposal → drone ack-on-old-channel → 5 ms
TX drain → `FastRetune` → `move_confirm_ms`. The boot-time bench measured
a one-card move at 60.2 fps with one gap (`docs/channel-select.md`).
What is missing is a mid-session proposal path: today the pick is frozen
at first ack and `ChannelPlan` only re-proposes the frozen pick.

**Energy sampling is gated on SESSION** (`energy_period_ms` A records):
during the analog cliff only 10 reads happened in 60 s and none while
the link was down. An in-flight design has to read energy in every link
state, or it is blind exactly when it must decide.

## Q3 — one card

Leg 1 (the verdict on the active channel) works on one card: the sensors
above are per card. Leg 2 does not: chanmig's doc is explicit ("two
adapters, two processes"), and on one card every dwell is a 250 ms video
gap plus the send gate (`scoutgate`). A one-card in-flight design
degrades to option 3 above: blind jump to the boot ranking's next
channel, verify, roll back. The boot scan's one-card interleave already
shows the cost (a 880 ms scout cycle against a 2 s `move_confirm_ms`).

## Bench caveats

- `benchjam.sh` defaults to mabur's canonical SA (txdemo's default), so
  its frames are invisible to `foreign` and pollute `own`. Export
  `DEVOURER_TX_SA=02:11:22:33:44:55` (done for the co-channel runs).
- Off-channel, the jammer's frames never decoded at the GS (foreign 0,
  CRC fails 80/s of 240 sent): near-field blocking, not contention.
- The analog VTX at full power next to the GS is an on/off cliff. A
  graded run needs pit mode or distance.
- The session directory must be chosen by highest index, not mtime
  (`0077` had a newer mtime than the live `0078`), and `flight.jsonl` in
  `0078` stopped at t_ms 4151515 when the analog outage rotated the
  session; the fade window was sliced by `t_ms` from the newest dir.

## Condition boundaries (GS uptime s, flight.jsonl t_ms)

```
baseline      2270.75–2334.69   jam250 (ch136)  2345.60–2419.78
jam1000 (136) 2435.69–2507.13   jam250sa (136)  2787.35–2858.66
jam250c120    3075.29–3146.61   jam1000c120     3162.62–3234.26
baseline136   3720.42–3782.28   analogE2        3815.05–3877.32
fade          4302.13–4363.91 (session 0079)
baseline165   4782.49–4850.08 (session 0080)   o4_5825  4931.80–4993.25 (session 0080)
```

GS config during the 136 runs: `radio.scan.enable = false`, backup at
`/etc/maburgs.toml.pre-pin136`. For the O4 run home was 165 on both ends
(`*.toml.pre-o4` backups on both devices).

## Follow-up spike: 5 ms in-session dwells (same day)

Question: can the spare GS card scout candidates while the link is up
with a dwell short enough to be free, and does a 5 ms observe see
anything? Throwaway code on branch `spike-fastdwell` (a thread on the
spare card: retune → discard read → observe → read → retune back, every
step timed on the GS). Link on 136 at rung 5, candidates 120/149/165, 3
and ~8 dwells/s.

**Dwell cost on the GS (RK3566, RTL8822EU), 429 dwells:**

| step | median | p90 |
|---|---|---|
| FastRetune to candidate | 3.9 ms | 5.0 |
| discard read (`GetRxEnergy(false)`) | 6.5 ms | 7.8 |
| observe | 5.1 ms | 5.1 |
| read | 6.8 ms | 8.0 |
| FastRetune back | 4.0 ms | 5.3 |
| **total** | **26 ms** | 29 |

The reads dominate: `GetRxEnergy(false)` is ~11 USB transfers (five
register reads plus three read-modify-write counter resets). A read
trimmed to the OFDM FA/CCA pair plus one reset would put the dwell at
~15–19 ms; the retunes (4 ms each, above devourer's 0.6–3.5 ms doc figure)
are then the floor. 9 ms is not reachable on this USB path.

**Video cost:** none measurable at rung 5 — ausniff 59.7 fps, 0 gaps, 0
drops at 3 and at ~8 dwells/s; link pre-FEC loss p90 < 1 %. Card 1's own
loss counter shows the absence (0.8 % → 14.7 % median at ~8/s) and card 0
covers it. Untested where card 0 is marginal (flight / attenuation).

**Uplink cost:** when the dwell card is also the TX card, RCF sends during
a dwell leave on the candidate channel: drone RCF rx 19.4 → 18.4/s at
3/s and 17.5/s at ~8/s. `tx_card = 0` did not fully pin (the selector
still chose card 1 for 20–30 % of samples). The design must scout on the
non-TX card dynamically or gate sends during a dwell.

**What a 5 ms visit sees (per visit, FA / CCA / foreign frames):**

| candidate | condition | FA>0 visits | FA med / max | foreign |
|---|---|---|---|---|
| 120 | clean | 20 / 143 | 0 / 4 | 0 |
| 120 | benchjam 240 pps co-channel | 6 / 25 | 0 / 2 | **72 in 25 visits (≈3/visit, max 4)** |
| 149 | neighbourhood Wi-Fi (boot scan worst 649) | 93 / 143 | 3 / 16 | 13 |
| 165 | clean | 2 / 28 | 0 / 1 | 0 |
| 165 | **DJI O4 at 5825** | **143 / 143** | **13 / 31** (min 2) | 0 |

So a single 5 ms visit separates an O4 from a clean channel with no
overlap at all, an 802.11 neighbour shows as ~3 decoded foreign frames
per visit, and a busy real-world channel (149) is caught in about two of
three visits — summing the last 3–5 visits per candidate is enough for a
ranking. Verdict: viable. Rotating three candidates at one dwell each per
second costs the spare card ~8 % of its time and nothing visible on video
at rung 5.

### Single USB transfer latency on the GS (same day)

Probe: 16 raw vendor control reads of `REG_TSFTR` (one transfer each,
`libusb_control_transfer` on the frontend's handle, no devourer lock) and
16 `ReadTsf()` calls (three reads each under `_reg_mu`), every 3 s, with
the link in SESSION and again with the drone stopped (GS beaconing).

| | in session | idle (beaconing) |
|---|---|---|
| raw single read, median | 250 µs | 320 µs |
| raw single read, min / typical max | 120–200 / 350–750 µs | 200–250 / 500–650 µs |
| `ReadTsf` (3 reads), median | 800 µs | 780 µs |
| `ReadTsf` worst seen | 17.6 ms (one scheduling stall) | 1.4 ms |

So one synchronous control transfer costs ~250 µs on the RK3566 and the
RX bulk flood does not slow it (idle was, if anything, slower). That
re-counts the dwell: `GetRxEnergy(false)` at 6.5 ms is ~26 transfers, not
the ~12 I estimated from the source, and `FastRetune` at 4 ms is ~16.
The bus time of a high-speed control transfer is a fraction of 250 µs;
the rest is the per-transfer host turnaround (submit, completion IRQ,
wakeup). Two levers, both in devourer's Jaguar3 code: fewer transfers
(a scout read of just OFDM FA/CCA plus one counter reset is ~4–6), and
async submission so a dwell's transfers queue back to back instead of
paying the turnaround each (the InitWrite async write queue is the
precedent). Planning numbers: dwell 26 ms today, ~14 ms with the trimmed
read alone, ~10 ms with batching; 250 µs × transfers is the floor.
