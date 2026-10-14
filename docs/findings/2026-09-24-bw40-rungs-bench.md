# 40 MHz rungs — bench 2026-09-24

**Status:** bw40-rungs deployed and bench-passed (steps 2-7, no flight); the boot-scan "deaf receiver" was a jammer-init timing artefact; the drone TX wedge was fixed in devourer 1123d4b; the "hop split" open item was later resolved by the no-home channel set. The ladder has since gone all-40. Current → docs/bw40.md, docs/channel-select.md.

Extracted from docs/bw40.md during the 2026-10-11 docs tidy.

## Bench 2026-09-24 (Task 14 of the plan; steps 2-7, no flight)

Both ends deployed from branch `bw40-rungs` 57e1707 (`*.pre-bw40` binaries
and configs kept on both devices; drone config = the device's tuned file with
the three renamed/added keys, GS config = `gs/bundle/maburgs.default.toml`
with `debug_log.enable = true`). Sideport medians over 20-60 s samples;
`hear` = `drone.rcf.rx_pps` / GS card `tx_pps`. Drone `low_power` was
disabled for the throughput rows and restored afterwards.

**Step 2, drone at 40 / GS at 20 (20-only ladder):** linked at 20/5 and full
rate; RCF hear 97.3 % (gate 85 %; the pre-deploy figure at the same rung was
93 %); `ausniff` 60 s 0 gaps / 0 incomplete / 0 resyncs.

**Step 3, no_agg cost (pinned 20/4, agg6, GS still at 20):** probes on
(60 canaries/s, each its own PPDU) vs off — drone `sent_pps` 1955 vs 1900,
enc 12.7 vs 12.7 Mb/s, air 54.5 vs 54.0 %, jitter 4.2 vs 4.4 ms, pre-FEC
loss 0 vs 0. The cost is +60 standalone PPDUs/s (~3 % of PPDUs) and nothing
else measurable at 54 % air. Note `link.probe.enable` does NOT stop the
canaries under a static pin — the probe follows `link.probe.pin_mcs` there.

**Step 4, GS at 40 (132+136), 20-only ladder, pinned per rung, 20-30 s each:**

| rung | hear % | enc Mb/s | air % | pre-FEC loss max | jitter ms | ausniff |
|---|---|---|---|---|---|---|
| 20/0 | 83 | 2.5 | 67 | 2.0 % (one sample) | 8.0 | clean |
| 20/1 | 73.5 / 78.6 | 4.9 | 64 | 0.8 % | 5.4 / 8.3 | clean |
| 20/2 | 73 | 6.8 | 60 | 0.8 % | 9.1 | clean |
| 20/3 | 74 | 8.9 | 57 | 0.4 % | 10.2 | clean |
| 20/4 | 97 | 12.8 | 54 | 0 | 3.7 | clean |

The low-rung hear rate is partly the GS width: same binary, same pin 20/1,
GS `radio.width` 20 → 82.4 / 82.2 %, GS at 40 → 73.5 / 78.6 %, with identical
uplink RSSI (−56 / −59 dBm) and SNR, so it is deferral, not power. Not a
collapse, but the uplink direction is 4-9 points worse at 40 on the singles
rungs. The GS logs `card 1 width 20 -> 40 MHz` once at the freeze (card 0 is
InitWritten at 40).

**Step 5, the 40 rungs (pinned, 60 s each):** 40/3: hear 97 %, enc 17.5 Mb/s
(cmd 17.6 — the airtime budget, not the cap), air 53.5 %, loss max 0.5 %,
jitter 3.1 ms, `aucadence` +0.60 ms. 40/4: hear 97 %, enc 23.5 Mb/s (cmd
24000 = the cap), air 48 %, loss max 0.7 %, jitter 4.9 ms, `aucadence`
+1.68 ms, sideport `lat` fec p50 4.4-8.5 ms / enc 7.4 / air 1.6 ms. Both
`ausniff` 0/0/0. Drone stderr: `A-MPDU ON at mcs3/40 (min_mcs_40 2)`.
Free ladder: 0 → 6 in ~7 s (six probed promotes ~1.7 s apart), 274 of 300
samples at 40/4 in the first minute, `ctllog 12` header carries
`40:3/50:25,40:4/50:25`; `ausniff` 60 s: 0 gaps, 2 incomplete (the climb).
Drone SoC: 83-84 °C at pinned 20/0-20/2 full rate, 58-62 °C at 40/4.

**In-session retune cost (40 MHz dwells from 132+136, n=146 each):** 144
`to_us` med 1.3 ms (p90 2.2, max 21.8) / `back_us` 0.7; 128 1.3 / 0.7;
40 3.3 (p90 4.3, max 5.5) / 2.6. All fast-path; the candidate list stands.

**Step 6, boot scan with a dirty secondary — PASS on the rerun (evening);
the first-day failure was a jammer-timing artefact.** Drone off so the scan
runs, host 8822EU `benchjam` 6M/1000 B on 140.

First runs (jammer started 4 s before the GS):

| run | 132 | 136 | 140 | 144 | 36 | 40 | 124 | 128 | pick |
|---|---|---|---|---|---|---|---|---|---|
| jam 140 heavy | 717 | 0 | 0 | 2 | 0 | 602 | 6 | 1141 | 144 (140+144) |
| no jam | 703 | 2 | 0 | 2 | 0 | 694 | 1 | 1081 | 144 |
| jam 36 heavy | 734 | 0.5 | 0 | 1.5 | 0 | 815 | 5.5 | 1194 | 144 |

(`cca_ofdm` median per 275 ms boot dwell.) The jammed half read idle, and
the day's conclusion was "the 8822EU receiver is deaf for ~20-25 s after
bring-up". **That was wrong.** On this host the dongle's full init takes
**37.6 s** (`txdemo.first_tx_submit` in the jam log: ~12 k control transfers
at ~3 ms each, with FW download, RF tables, RFK and IQK each taking seconds),
so the jam went on the air ~26 s into the scan, which is exactly where the
per-round readings on 140 "woke" (25.8 s). The host `rxdemo` "deaf 20 s"
evidence was the same thing: its `rx.energy` thread starts before init,
and the counters go live at t=20.3 s, right after `table_rf_ab` and
`phydm_param_init` finish. Every devourer fix tried against the "deafness"
(full tune vs FastRetune, ctrl_batch, NHM windows, IGI, AGC) changed
nothing because nothing was broken. The same scans read 132/40/128 busy
from round 0, so the scout was never deaf.

Rerun (GS started only after the jam log shows `first_tx_submit`,
production binary, `radio.scan.enable` on in a /tmp config):

| run | 132 | 136 | 140 | 144 | 36 | 40 | 124 | 128 | pick |
|---|---|---|---|---|---|---|---|---|---|
| jam 140 (on air first) | 195 | 481 | **173 / 188 fr** | 422 | 118 | 384 | 189 | 203 / 180 fr | 128 (124+128) |
| no jam | 808 | 0 | 3 | 16.5 | 53 | 445 | 15.5 | 794 | 144 (140+144) |

Round 0 on 140 (3.7 s after the scout card's bring-up) already reads 174 CCA
/ 184 foreign frames. The 180 foreign frames on 128 are the host jammer
leaking (0 without it). 40, 128 and 132 are real ambient FA energy that the
GS antenna hears with no decodable frames.

**Scoring gap (open, predates bw40):** `ChannelRanker::busy` counts events
(`cca − own + fa + foreign`), not airtime. The jammed 140 (~188 × 1000 B at
6 Mb/s ≈ 95 % of a 275 ms dwell) scored 366, below the unjammed home 136
(1416, all short FA bursts). The pair pick still avoided it only because the
jam leaks into 144 (998) and the pair scores as its worse half. A clean-
adjacent long-frame user on one half could win the pick.

Jammer note: `benchjam` (`txdemo`) keeps carrier sense on and defers to the
drone's own frames, so an in-session jam next to a live link runs at ~50 fps
and proves nothing; use `linkbench-tx --no-cca --foreign-sa` for those
(575 fps). The boot-scan jams ran with the drone off and were fine.

**Step 7, co-channel jam on the op primary (144, CCA-free `linkbench-tx`).**
The first write-up said the verdict took ~43 s to reach `interfered` while the
ladder collapsed. Both were wrong: `linkbench-tx` needs ~38 s of host init
before its first frame (same artefact as step 6). Re-timed against the jam's
first TX second (evening, jam on 136): first `interfered` window 0.01 s after
onset, hop `order` at 0.16 s. The ladder held until the hop; the collapse
came afterwards, on channels the jam still reached.

**Drone TX wedge (root-caused and fixed in devourer, 2026-09-24).** Under the
jam the drone's TX died for good (`bulk_send EP 5 FAIL rc=-7` forever, RX
fine, only a `maburd` restart cleared it). Cause: Jaguar3 sends used sync
bulk-OUT with 20/50 ms timeouts; with the FIFO full from deferral a timeout
cancelled a transfer the chip had half-taken (`got 1536/4439`), and with 4
sender threads the next queued transfer streamed in as the rest of that
packet. Register state read live (`chipstate --no-claim`): TXDMA_STATUS
PAYLOAD_UDN, 17-19 of 1745 public pages free, MAC queues empty. Hopping is
not needed (pinned 40/4, hop off, wedged at ~22 s); it only made it likely by
restoring the top rung into a still-jammed neighbourhood. Resubmitting the
cancelled tail did not help. Fix (devourer 1123d4b, `src/BulkOutTimeout.h`):
a bulk-OUT longer than one USB packet is never given a timeout; control
frames (one packet) keep theirs. Bench on that build: 2 x 90 s pinned-rung
jams and a hop-under-jam run (8 retunes), no stall, no wedge.

**Open: hop split.** After `verify_fail` -> `withdraw` -> an order the drone
never heard, the drone falls back home (`move_unconfirmed`) while the GS stays
on 40/128 indefinitely, so the link is down until a GS restart (GS session
0207). Under a close jam every candidate also failed verify (leakage into
144/128, ambient false alarms on 40) and the hop ended back on the jammed
channel.

Not run: step 8 (flight). Bench artefacts of the day live in GS session
`/media/dvr/log/0196` (rejoined by every restart; the `scanlog 3` header
marks the new-binary portion).
