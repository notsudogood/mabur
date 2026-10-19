# Next build backlog

What the operator has agreed goes into the next GS (and drone) build, in
order. Each item names its evidence. Remove an item when it ships.

## 0. BLOCKER for Hop on at 80 MHz: drone stops hearing the GS after hops (found 2026-10-18, gs8 + d8)

Evidence (bench, d8 + gs8, defaults: width 80, channels [36, 132], Hop on):
the drone took two GS hop orders (`retune 36 -> 132 (hop)`, then
`132 -> 36 (hop)`), so it decoded RCFs at both ends of a FastRetune. Then
the GS ordered epoch 4 to 132, logged `verify_pass` there, and stayed in
session (state=0) while the drone sat in RENDEZVOUS for minutes: its
telemetry read `radio.rx own=0 foreign=64..174 crcfail=0`, `rcf age 65535`
-- its receiver works, but not one GS frame. GS kept receiving video and
telemetry. Pinned (GS `channel = 36`, drone `channels = [36]`, no hops):
`state=2` steady, lp=0, encoder ~8 Mb/s. So the 80 MHz link itself works;
the hop path breaks it.

Prime suspect: devourer FastRetune at 80 MHz on the 8822E, never exercised
before (the 2026-10-17 bench and linkbench stayed on one channel;
tests/run_hop_validation.sh is 20 MHz). The fit: after a fast hop back to
36 the drone believes it is on 36, but if its radio is left partly tuned
to the 132 block (or the RX side is), the GS hears the drone on 132
(hence verify_pass) while the drone hears nothing (own=0), and the epoch-4
order never lands, so no `retune 36 -> 132` is logged. Not proven.

Test before fixing (bench, 5 min): GS `channel = 132` (pinned), drone
`channels = [36, 132]`: the drone parks on 36, the GS finds it there and
orders ONE hop to 132. Check drone `state` / GS `drone.radio.rx.own` after
it. Then devourer `ReadTunedCentral()` on the drone after each hop (expect
42 / 138) -- needs a debug verb or a log line in retune_now_.

Likely fix if confirmed (small, both ends): at 80 MHz use the full
`SetMonitorChannel` for a channel move instead of `FastRetune` (drone
`retune_now_`; GS `RadioFrontend::retune` while a card is wide). Costs
~90 ms per hop instead of ~2-5 ms; hops are rare. The devourer fix (fast
path at 80) can follow.

Until fixed: fly 80 MHz with Hop off (pinned channel).

Diagnosis build (d9): maburd logs `radio at central N, want M` and the
FastRetune time after every retune, re-reads it every 5 s (`rfc=` /
`rf_bad=` on the stats line, a `rf check: ... MISMATCH` line on a new wrong
value), and the GS logs each card's real centre with `MABUR_HOP_DEBUG=1`.
The test steps are in the gs8 + d8 test doc's "Hop and channel checks".
Hops into or out of an 8822E spur block (149/100/116/52 at 80 MHz) always
take devourer's full path, so a 132 <-> 149 hop is the control: if that
works while 36 <-> 132 fails, the fast path is the fault.

Ruled out by the operator (2026-10-18): switching the DRONE's RF width
around a hop (hop at 40, widen after). Each 40 <-> 80 switch is a full
retune, a pause on every ladder step in and out of 80. The drone stays at
its configured width; the fix has to make the 80 MHz hop itself sound.
Revisited 2026-10-19: acceptable once the width switch is fast (item 14,
which hops only at 40 and so makes this item moot); still ruled out with
the full-path switch. Until item 14 lands, candidates in order: (a) the missing piece of the lean hop at 80, once the
readout names it; (b) the chip firmware's own channel switch (H2C 0x1D)
at 80 -- devourer gates it to 20/40 today, the vendor driver's 80 MHz
support is unchecked; (c) the full path at 80 MHz hops only (never on
ladder steps), costing what the 132 <-> 149 test measures, once per hop.

## 1. GS: spread the per-packet work off the core thread

Evidence (bench 2026-10-12, d4 drone + gs4 GS, GS pinned at MCS5/40 with
the flight FEC pair 0.5/0.25, drone `max_kbps` 35000, low power off):
GS `fec` p50 swung 16–540 ms and climbed steadily on each channel until a
hop flushed it. Per-thread CPU over 30 s (6000 ticks = one core):

| side | busiest threads (% of one core) | cores busy |
|---|---|---|
| drone, 4 USB senders | mbr-hot 25, mbr-fecw 19, venc 8, txw 8, usb 4 × 7 | cpu0 83, cpu1 35 |
| drone, 8 USB senders | same; usb 8 × 3.7 | cpu0 83, cpu1 38 |
| GS `maburgs` | **one thread 95–97**, two at 17, rest ≤ 5 | 59–69 / 46–53 / 48–52 / 45–49 |
| GS `maburplay` | 41.5 (+ mpp 7.6) | — |

The drone has headroom and more USB senders changed nothing; one GS thread
(the core loop, `gs/src/main.cpp` `while (!g_stop)`) is saturated while
three of the RK3566's four cores sit half idle. Flight 2026-10-12 showed the
same signature milder (`fec` 35–44 ms at 35 Mb/s, 9 ms at 25) because the
ladder kept dropping to the 25 Mb/s rung. Caveat: one bench session, two
readings per point, and the hot thread was not yet identified by TID.

Why it is one thread: both cards' RX threads push every raw body into
`BodyQueue`; the core loop runs `sbi_unpack` (sub-block CRC16 + split +
copies) on EVERY copy, and only then does `SwDecoder::add_symbol` drop the
second card's copy as known — the per-byte work is done twice, serially.

Plan (GS only, no wire or drone change):
1. Profile the core path on the host replay (`maburgs --dry-run --in`, two virtual
   cards, ~35 Mb/s) to rank unpack/CRC vs FEC vs assembly.
2. Split 1: run `sbi_unpack` on each card's own RX thread and hand the core
   pre-checked symbols; the duplicate becomes a set lookup.
3. Only if needed: FEC repair on a worker (as the drone's `FecWorker`), then
   frame assembly/output on its own thread behind a queue.

Bench gate: the GS CPU probe at 35 Mb/s shows the busiest thread well under
80 %, `fec` flat, no hops. Until it ships, fly MCS5/40 at 25–30 Mb/s.

Status (2026-10-12): the host replay profile put ~80 % of the core path's
instructions in `gf::lincomb` reducing repairs that taught nothing (fully
known windows, and the second card's copy), and most of the rest in
`crc16_ccitt`. Committed: the repair skip (6e5e297; replay user time
3.22 -> 1.14 s, AU output byte-identical) and split 1 (bdb5142). The host GF
path is scalar where the GS's is NEON, so neither share transfers as is: a
GS-side profile of old / fix / split1 static aarch64 builds
(`tools/bench/pcsample`, `symbolize_bin.py`) decides whether step 3 is
needed. No GS image until that bench says so.

GS bench 2026-10-13 on efficient-link-nack-gs5 (cabcf43), 35 Mb/s MCS5/40:
the core thread fell from 95-97 % (pegged, backlog growing) to 84-85 %,
the two RX threads rose 17 -> 31 % each (split 1 doing its job), and with
a still scene `fec` held 9-10 ms p50 at 35 Mb/s (the 25 Mb/s level). The
same 85 % with motion in front of the camera (83.7 vs 84.6 %): GS load
follows bitrate, not scene content. pcsample of the gs5-equivalent build
(30 s, 25k samples): memory alloc/free ~23 %, std::map tree ops ~23 %
(SwDecoder known_/rows_, FrameStream chunk maps), kernel ~14 %, FEC
decoder logic 7 %, S1LossWindow::sample 6 % (re-sums its deque per
call), memcpy 4.5 %, slice_assembler find_codes 3.5 % (byte-wise
start-code scan). Next step if more GS headroom is wanted (50 Mb/s at
80 MHz needs it): ring-indexed symbol storage instead of std::map +
per-symbol vectors, pooled body buffers, running sums in S1LossWindow,
memchr-based start-code scan.

Status 2026-10-13 (3b1d509, GS image gs6): that list is done, except
pooled body buffers. Changes:
- Ring-indexed symbol store in SwDecoder.
- Fragments moved, not copied, decoder -> FrameStream, with a chunk table
  in place of the per-fragment std::map.
- An O(1) repair-dedup index with an exact fallback.
- S1LossWindow running sums.
- memchr start-code scan.

Results on the host replay (20 s at 35 Mb/s, 2 cards):
- AU output and stats are byte-identical to gs5 at 0/7/15 % drop.
- Core-thread instructions (excluding the RX-thread unpack): 2.29 G ->
  1.39 G (-39 %).
- The aarch64 static build gives the same output under qemu.

Counter-evidence: x86 instruction counts, not RK3566 time. The bench
gate is the GS CPU probe at 35 Mb/s on gs6. If the 84-85 % falls in
proportion it would land near 55 %, but allocator and cache effects can
move that either way.

Item 4 (80 MHz, 50 Mb/s) depends on this: at 50 Mb/s the gs5 core thread
would need ~120 % of a core.

## 2. maburcal: stop calibrating rates to full power

**Done in gs8 / d8 (2026-10-17), not yet bench-checked:** changes 1-3
below. A no-dip row goes out undetermined and the drone keeps it at its
pre-calibration power (from `power_mode = "none"`: the 8822E stock shape,
`drone/src/power_plan.h`); the pass bar is 99 % with 50 coarse frames per
cell (coarse ~43 s, was ~30 s); the noise-cell rule forgives at most 2 of
50 lost. Change 4 (EVM) is not done: the sweep frames still carry none.
Counterpart: at a 99 % bar a few frames of random loss end a run early, so
walls can come out low (under-power, the safe direction). And a calibrated
table leaves the VHT (80 MHz) rates at MCS7's level -- see docs/bw80.md.

Evidence (flight 2026-10-12, d4 drone + gs4 GS, calibrated on the bench the
same morning): the calibration wrote `rate_walls_rel = [63, 63, 63, 63, 40,
21, 23, 17]` and switched `power_mode` to `"offset"`. In flight the GS
measured EVM −10.5 dB at MCS1/2 and −13.5 dB at MCS3, flat across SNR 25–45
dB on both cards (the transmitter distorting, not the receivers overloading).
Flight 0027 at default power (`power_mode = "none"`) had −16.5 / −18.0 /
−19.5 dB at the same rates. MCS3 test packets lost 10 % (15 of 26 probe
episodes failed), so the ladder never left MCS2/20. Rolled back with
`/etc/mabur.toml.pre-cal`.

Cause, in `gs/src/cal_analysis.h`: a power level passes at 90 % delivered
(`CalThresholds::deliver_pct`), so a rate losing ~10 % at full power reads
as "no dip" and parks at the rail (`kRailRel`). The 2026-10-10 noise-cell
rule (forgive one 75–89 % coarse cell when the next two are back at 90 %)
also helps a row climb through the band where compression starts.

Changes:
1. A rate with no dip keeps its pre-calibration power instead of moving to
   the rail.
2. Raise the pass bar to what the ladder's probe gate accepts (~99 %), with
   enough frames per cell to measure it (longer run than 75 s).
3. Revert or narrow the noise-cell rule so it cannot forgive cells on the
   way up.
4. Check whether sweep frames now carry EVM (the flight probe bodies do;
   `docs/calibration.md` says the sweep frames carried none when it was
   tried) and reject any level where EVM falls sharply.

Bench-check before shipping: re-run on the same pair and compare EVM per
rate against `power_mode = "none"`.

## 3. Drone: CPU headroom for 40+ Mb/s

Evidence (bench 2026-10-12, Test A: 35 Mb/s at MCS5/40, SoC at its stock
800 MHz): cpu0 83 % busy, cpu1 35–40 %. cpu0 carries everything but the
producer: mbr-fecw 19, mbr-usb 4 × 7.3 (8 senders: 8 × 3.7, the same
total), venc-enc 8, mbr-txw 8, maburd 8, mbr-msp 3, the rest kernel/SDK;
cpu1 carries mbr-hot alone (25). Extrapolating the per-Mb/s part puts cpu0
at 100 % near 42–45 Mb/s -- an estimate, not measured. The operator keeps
the SoC at 800 MHz (no overclock), so the levers are placement and code.

Levers, cheapest first:
1. Rebalance the cores. The placement (`drone/src/main.cpp` kHotCore /
   kRestCore, dq-spike findings §19, 2026-09-01) predates the 2026-09-22
   frame-end-join deletion that took mbr-hot from ~70 % to ~20–25 % of its
   core; its own comment says to revisit it. Move mbr-fecw (or two USB
   senders) to cpu1: on paper both cores land near 60 % at 35 Mb/s. Bench
   it first with `taskset` by thread name, no build, watching drone `dq`
   and GS `fec` for the producer-preemption cost §19 measured.
   Since dac43df (drone d5) it is config, not taskset: `fec.worker_core =
   "hot"` and/or `radio.tx_threads_hot = 2` in /etc/mabur.toml, restart
   maburd; the startup line in /tmp/mabur.log prints the placement. Both
   default to the old policy.
2. Fewer USB transfers. The 8822E parses at most 3 descriptors per bulk
   transfer (devourer `TxAggPlan.h`, HalMAC BLK_DESC_NUM), and with
   `fec.feed_batch = 1` the senders often carry fewer. Read the batch mix
   from the drone's `usb_urb` gauge (`/tmp/mabur.log`), then A/B
   `feed_batch = 3`. Counterpart: 7a4faec went to `feed_batch = 1` because
   grouped bodies make bigger loss units (a lost aggregate took 12–16 enh
   symbols, past what 0.25 overhead repairs).
3. Cheaper repair building: `gf::lincomb` runs ~9.4 ns/B on the A7
   (bitrate-ceiling findings, lever 2); a two-rows-per-pass NEON kernel is
   the next term for mbr-fecw. Drone only, wire unchanged.
4. Less FEC on the top rung (the trim the flight logs are deciding): fewer
   repairs to build, fewer bodies to send, less air.
5. Check which core takes the USB controller's interrupt
   (`/proc/interrupts`); if cpu0, move it with `smp_affinity`.
6. Last: an async bulk-OUT data path for Jaguar3 in devourer (one thread,
   N transfers in flight) instead of N blocking senders. Larger change;
   devourer keeps J3 sends synchronous on purpose (bring-up NAK backoff),
   so data path only, after bring-up, with a bench A/B.

Status 2026-10-13 (2ed0441, drone image d6):
- Code cut on the hot thread (cpu1): memchr start-code scan and
  slicing-by-8 CRC16. Host dry-run: -59 % of its non-FEC instructions;
  the scans use 8x fewer on real encoder frames; same output.
- Three placement/idle knobs, all defaulting to the old behaviour:
  `fec.worker_core`, `radio.tx_threads_hot` and `fec.worker_spin`.
  `fec.worker_spin` is new lever 7: at 35 Mb/s repairs arrive ~70 us
  apart mid-frame against a ~50-100 us spin budget, so mbr-fecw's 19 % on
  cpu0 is probably mostly spinning (an estimate from rates, not a
  measurement). `worker_spin = 0` trades the spin for a futex wake per
  repair.

Counter-evidence: host instruction counts, not A7 time. The hot-thread
cut lands on cpu1, which already had room, so on its own it lowers
latency (dq) more than it frees cpu0. cpu0 only gains through the knobs:
bench each one against the drone CPU probe.

Bench 2026-10-13 (d6 + gs6, 35 Mb/s, MCS5/40, ch 64, 8 USB senders):

| layout | cpu0 | cpu1 | txq | e2e p50 | fec p50 |
|---|---|---|---|---|---|
| defaults | 81.0 | 28.0 | 4-5 ms | 39-42 | 7-8 |
| worker_spin = 0 | 80.7 | 26.1 | - | - | - |
| worker_core = "hot" | 72.1 | 41.5 | - | - | - |
| tx_threads_hot = 2 | 75.5 | 34.5 | - | - | - |
| worker_core hot + tx_threads_hot = 4 | 60.0 | 61.9 | 7-17 ms, rising | 38-41 | 8-9 |

- d6's code cut works: mbr-hot 25 -> 17.2 %.
- The spin estimate was wrong: with worker_spin = 0, mbr-fecw stays at 18 %.
  It is real work, not waiting.
- Moving work to cpu1 also makes mbr-txw and maburd use more (8 -> 11,
  6.5 -> 10.9), and total CPU rises (avg 54.5 -> 61 %).
- The balanced layout costs the TX queue: with motion in front of the
  camera, txq jumps to 15-17 ms and e2e to 70-80 ms, with no loss. At
  defaults, txq holds 4-6 ms and e2e 37-38 / 54 under the same motion.
  Big motion frames hit the video thread and the senders on cpu1 at the
  same moment, so keep USB senders off cpu1. Defaults stay.
- For 40+ Mb/s, try worker_core = "hot" alone; its txq is unmeasured.
- GS gs6: busiest maburgs thread 61.7 % (gs5 84-85, gs4 95-97).

Counter-evidence: 30 s readings, one bench session, 4-6 status lines per
layout.

Bench gate: step test 35 / 40 / 45 / 50 Mb/s at MCS5/40 with item 1 in,
drone and GS CPU probes, GS `fec` flat. 50 Mb/s also needs item 4 for the
air: at 40 MHz it is ~85 % of what MCS5 delivers.

## 4. 80 MHz top rung

**Built in gs8 / d8 (2026-10-17), unflown:** MCS3/80 and MCS4/80 rungs,
GS cards widen only for them (`docs/bw80.md`). Bench:
`docs/findings/2026-10-17-bw80-bench.md`.

Plan, constraints and go/no-go measurements: `docs/bw80-research-2026-10-12.md`.
First step is `linkbench --bw 80` plus a bench drone build carrying it; flight
code only after the bench says go. Blocked on items 1 and 3: the GS core
thread already saturates at 35 Mb/s, and the drone's cpu0 is the next wall.

## 5. Raw-recording `.srt` that walksnail-osd-tool renders

walksnail-osd-tool parses only Walksnail's exact subtitle line
(`Signal:N CH:N FlightTime:N SBat:N.NV GBat:N.NV Delay:Nms Bitrate:N.NMbps
Distance:Nm`, `backend/src/srt/frame.rs` upstream) and silently skips
anything else, so our `.srt` showed nothing. Write `record-NNNN.srt` in that
format (Signal 0–4 from link quality, CH = our channel, FlightTime =
seconds since the recording started, Delay = latency, Bitrate = bitrate;
SBat, GBat and Distance 0 — the GS has no battery or GPS feed) and keep
today's detailed line (MCS/width, RSSI, loss, VTX temp) in
`record-NNNN.link.srt` for VLC. GS only (`gs/player/src/osd_recorder.*`).

Related, not a code item: the tool also mis-renders our recordings'
frame rate (it decodes at the rate of the file's start, 30 fps while
disarmed). The workaround is converting to constant 60 fps with ffmpeg
before loading.

## 6. Gilang's RTL8812CU 40 MHz TX fix, for a Wyvern drone build (untested)

The Wyvern air unit's RTL8812CU (8822C die) stalls on 40 MHz injection.
Gilang's fix is in gilankpam/devourer master (`7c10751`, `ccb45ce`,
`3153975`, `3919a35`; write-up `docs/experiments/8822c-ht40-tx-stall.md`):
DISDATAFB=1 on the 8822C's 40/80 MHz frames, and a DIG/IGI floor (0x26) on
the C die while tuned wide. Drone-side only: the GS's 8812EU cards are
untouched, and his doc says a ground station never arms it.

- Does not cherry-pick cleanly onto our devourer branch (his master carries
  other Jaguar3 TX changes ours lacks; the first commit conflicts in
  `src/jaguar3/RtlJaguar3Device.cpp`). Port the two pieces by hand.
- His caveat: the IGI floor costs the CU an estimated 4–8 dB of idle-channel
  RX sensitivity while tuned wide (unmeasured). On the drone that is the
  uplink (RCF and NACK reception).
- Untested by us; needs a Wyvern bench run at 40 MHz before a flight.

## 7. Auto channel mode: stop costing the link at high rates

Evidence: on the 2026-10-13 bench (gs5, 35 Mb/s, MCS5/40) the channel was
pinned (`channel = 64`) and 4.5 min stayed clean -- `fec` 9-10 ms p50, drone
`txq` 4-11 ms, loss 0-0.9 %, EVM -25 to -27 dB -- including movement in
front of the camera. The operator saw the movement-related latency only
with `channel = "auto"`. Not yet A/B'd under equal conditions.

Two auto-only behaviours plausibly cause it:
1. **False hops.** The sustained-loss rule (`[hop.verdict] sustained_*`,
   0.5 % mean over 10 s at SNR >= 20 dB) fires on the link's own loss at
   35 Mb/s: flight 2026-10-12 hopped 112 -> 64 at 122.8 s with zero
   foreign traffic, own pre-FEC loss 0.34 % mean at 35 Mb/s against
   0.04-0.09 % at 25. FEC repaired all of it. Each hop costs ~130 ms of
   video, and the drone's `txq` read 70 ms at a bench hop.
2. **In-session scouting.** Two-card auto mode starts the in-flight scout
   (`channel_core.cpp` step_drains_), which takes a card off the link
   channel for ~7.5-8 ms dwells (flight 2026-10-12: 172 + 364 dwells in
   ~5 min). The cards are not equal: card 1 lost ~3x what card 0 did over
   that session (mean 2.6 % vs 0.9 %, p90 7.3 vs 4.2), so a dwell by the
   better card leaves the link on the worse one.

Changes:
1. The sustained-loss verdict counts only loss that cost video (residual
   after FEC / abandoned), not loss the repair data covered.
2. Pause the in-session scout while on the fast rungs (or whenever the
   link's FEC margin is thin); scout freely at low rates and while the link
   is down.
3. When it does scout, send the card with the worse recent loss / SNR and
   keep the better one on the link.

Bench gate: auto vs pinned at 35 Mb/s, 3 min each, same setup: `fec` p99,
loss, hop count and dwell count must match within noise. GS only.

## 8. Drone: onboard recorder off cpu0 (pinned 2026-10-17)

Question from the operator: will recording on the drone fit beside 50 Mb/s
live video? Today the recorder threads (`mbr-rec`, `mbr-recdrain`) inherit
the main thread's cpu0 pin, the core the USB senders and FEC worker
already load to an estimated 80-85 % at 50 Mb/s (unmeasured with the
camera running). The re-encode is the SoC's hardware encoder; the CPU part
is the copy and the SD writes: after the 2026-09-26 drain rewrite ~2 % of
a core user time plus ~7 % sys in the writer, kernel SD writeback and
iowait (`docs/findings/2026-09-26-vtx-recorder-bench.md`, 40 Mb/s record).

Changes:
1. `record.core = "rest" | "hot"`, default `"rest"` (today's layout).
2. Both recorder threads at the lowest priority (SCHED_IDLE or nice 19)
   on whichever core, so the video packer always preempts them and a
   short CPU costs recorded frames, not the live link.

Risk: the 2026-10-13 bench (item 3's table) put the USB senders on cpu1
and txq went from 4-6 ms to 15-17 ms under motion, because big motion
frames hit the packer and the senders at once. The recorder is off the
live path and light, but it copies those same frames at the same moment,
and its SD writes run partly in the kernel where the packer cannot always
preempt them.

Bench gate: first the d8 test doc's optional step 4 (record to SD while
pinned at MCS4/80 50 Mb/s, drone cpuprobe): if cpu0 has room, this item
can wait. Otherwise A/B "rest" vs "hot" at 50 Mb/s with motion: cpu0,
cpu1, txq, e2e, and recorded-frame drops.

## 9. GS on-screen menu (agreed 2026-10-10 for a later build)

Operator's spec: model the look on the Caddx Ascent goggles menu, simple,
driven by the RunCam's 5-way stick. Camera items only change resolution and
frame rate, offering what the auto-detected sensor can do and greying out
the rest (IMX415: no 1080p120; IMX335: everything open). Everything
granular stays out of the menu and is set over ssh via the GS's open AP.

Design mock-up (v5, 2026-10-17, on the Ascent layout below):
https://claude.ai/artifact/5AtztQm49YgZR5PNwLD2CR

Operator decisions 2026-10-17:
- **Bandwidth: Normal / Wide** replaces the Ascent's Smooth / High Quality /
  Long Range presets. Normal stops the ladder at MCS4/40 (25 Mb/s); Wide
  re-enables the 80 MHz rungs (MCS3/80, MCS4/80, up to 50 Mb/s). Both keep
  automatic width switching (20/40, plus 80 on Wide). Implementation is
  cheap: Normal = the ladder without its 80 MHz rungs, as
  `webgs::drop_80mhz_rungs` already does for the web GS; cards then never
  widen (gs/src/width_policy.h).
- **Channel hopping switch**, default on. In v5 it sits on the Channel tab
  (where the Ascent has its Hop switch). On = `radio.channel = "auto"`
  (boot pick + in-flight hop); Off = pinned to the channel the operator
  presses.
- **Spectrum tab**, after tipoman9/Spectrum
  (https://github.com/tipoman9/Spectrum). Its `8812eu.py` reads the vendor
  kernel driver's `/proc/net/rtl88x2eu/<if>/chan_info` (quality,
  availability, utilization, Wi-Fi vs interference utilization) after a
  passive scan. devourer reads the same counters itself (busy airtime /
  `clm`, `nhm_env` for non-Wi-Fi energy, `ArmChannelBusy`), so the GS can
  sweep 36-165 with the spare card while the link flies on the other one,
  show Wi-Fi vs other signals per channel and a verdict per 80 MHz block
  (40 MHz pair on Normal), and add or remove a block from the channel set.

Reference: the Caddx Ascent VRX Quick Start Guide V1.0 (operator-supplied
PDF, 2026-10-17; not committed). Its screens differ from mock-up v4, which
used a left sidebar and an orange highlight:
- Top tab bar: Channel | Settings | Playback; the open tab is light grey.
  SD card icon + free space top right, a red dot while recording.
- Settings: a title row ("Settings <"), a row of sub-tabs (Camera |
  Display | Record Set | Device), then a 3-column grid of cells, each
  "Label  value" with up/down chevrons: the value changes in place, no
  sub-page. Selection is a white outline on dark grey cells over black.
- Top level holds the everyday items: Transmit Power, Resolution
  (720p / 1080p only), Frame Rate (60 fps), Bandwidth (Auto / 10 / 20),
  Standby Mode (lower power while disarmed: our [low_power]), Preference
  (Smooth / High Quality / Long Range presets), Battery Warning, Language.
  The sub-tabs hold the detail.
- Channel: one row of channel cells, each with coloured signal bars
  (green / yellow / orange) and its frequency range; the current one light
  grey, selection a white outline; "Hop" toggle and "Refresh" top right.
- Bottom bar on the live screen: VTX volts, VRX volts, bitrate, latency,
  distance, a status message, channel + signal bars.
- 5-way stick scrolls and confirms; Back returns; Record starts/stops.

Open questions from the mock-up (the guide answers 1 and leans on 2-4):
1. ~~Does it match the Ascent?~~ No: redo the mock-up on the layout above.
2. Which resolutions to list? The Ascent offers 720p and 1080p only for
   live video. Above 1080p the IMX415 drops to 30 fps and the live picture
   lagged badly on the bench, so 1440p and 4K probably stay off.
3. Should the drone SD copy follow the live size and frame rate? The Ascent
   gives the recording its own resolution under Record Set.
4. Genlock's name: the Ascent's "Smooth" is a link preset, which our
   Bandwidth switch now replaces; genlock stays out of the menu (ssh).

Decided on v5 (2026-10-17):
5. Hop stays on the Channel tab only.
6. Spectrum keeps its own tab, and its results colour the Channel tab's
   signal bars (same thresholds and colours as its verdicts: Clear green,
   Fair yellow, Busy red), so a busy 36 shows red there; the breakdown
   stays on Spectrum.
7. Channel boxes show whole 80 MHz blocks on Normal too: other pilots may
   fly 80 MHz alongside, and their signal covers the whole block.
8. **Spectrum steers the hop.** Today the in-flight hop ranks
   `radio.channels` members from the spare card's in-flight dwells
   (gs/src/hop_ranker.h: energy, decoded foreign frames, NHM busy) and holds
   when no candidate qualifies. Add the last Spectrum scan as prior
   evidence per member (aged out, then overtaken by in-flight visits),
   weight Wi-Fi occupancy above other energy (routers stay put), and never
   hop to a member the evidence rates Busy: hold instead. Goal: indoors the
   link does not drop onto 36-48, where most routers sit. Counterpart: the
   scan is taken where the GS stands, which suits the downlink it receives,
   but up high the drone hears more routers than the GS does; the in-flight
   dwells are what catch that, and backlog item 7 pauses them on the fast
   rungs.

## 10. 10 MHz bottom rung, for penetration (agreed 2026-10-17, later)

A new lowest rung below MCS0/20: MCS0 at 10 MHz narrowband, under both
Bandwidth settings, for getting through walls and trees at the edge.
devourer already does 5/10 MHz on the 8812EU (Jaguar3 narrowband,
`FastSetBandwidth` for a cheap 20 <-> 10 switch). Needs: a 10 MHz rung in the
ladder and profile byte, the GS cards following it (like the 80 MHz width
policy, in the other direction), and a bench check of reach vs MCS0/20.
Counterpart: half the airtime capacity of MCS0/20 (about 3 Mb/s nominal),
so the 500 kb/s floor still fits but re-sends get tighter; and 10 MHz is
more sensitive to frequency offset between the two radios.

## 11. GS: channel-health reads off the core thread (Gilang's stall fix, agreed 2026-10-18; MERGED 522c3fd6, ships with the next image)

Evidence (bench 2026-10-18, gs8 + d8 pinned at MCS4/80, test doc step 5):
the `au_tail` gauge (last body's RX stamp to the AU's close on the core
thread, 5 s windows) read a wait of 8 ms or more in 20 of 24 windows, worst
28.3 ms. With `hop.window_ms` 150 -> 2000 the operator reported the stall
gone (run B's numbers not recorded). Same signature as Gilang's
(gilankpam/mabur `demand-refresh`,
`docs/findings/2026-10-17-maburgs-core-stall.md`): every channel-health
window, `ChannelCore::step_hop_edge_and_window_` reads each card's NHM
busy, energy and re-arms NHM on the core thread, taking devourer's
`_reg_mu`, which the card's `coex_runtime_loop` holds 10-26 ms every 2 s.

Fix: his `WindowSampler` -- a per-card sampler thread publishing a
snapshot the core thread reads without blocking, plus a per-card
device-use lock ordering it against teardown, bursts and dwells
(`demand-refresh` e352ce81..b6493915, 2026-10-17), on his master since
2026-10-18 (PR #85, master 5d642c0d). Port by merging his master; our
`channel_core.{h,cpp}` and
`radio_frontend` have diverged (80 MHz width jobs), so expect conflicts
there. His numbers: 0.131 -> 0.019 stalls/s and queue-wait p99.9 11.7 ->
6.0 ms with the window at 2000 ms. *Against it:* the residual 0.02/s is
the USB I/O itself, which the sampler moves but does not remove; his
result is his build on a Radxa. Gate: the new image, same pin (MCS4/80)
and scene, shows at most ~3 of 24 five-second windows with an RX-queue wait
of 8 ms or more. The new image no longer prints the `au_tail` gauge
`stall.sh` reads (Gilang dropped it); the same wait is per AU in `au.log`
(aulog 7: column 17 `t_last`, 18 `t_drain`, so `$18 - $17`), so the test
doc's step 5 needs an au.log version of the script for that image.

## 12. Gilang's phase lock in place of our genlock (agreed 2026-10-18; MERGED 522c3fd6, ours retired, ships with the next image)

Evidence: his bench (Radxa, session 0148) unlocked e2e p50/p90/p99
40.6/48.1/51.0 ms -> locked steady 35.4/36.3/37.0; Orange Pi 33.2/35.7/37.4;
one flight (0150) 41.3/42.7/44.2 against 46-47 p50 unlocked, judder
seconds 9 % against 14-32 %. Ours, flown twice: phase error p50 1.6 ms,
within 2 ms 60-77 % of steered seconds, and never measured as e2e, so the
two are not compared like for like. *Against his:* never-shown frames did
not improve (0.34/s against 0.33), one flight, and his bench 35.4 ms is
his latency allowance, not the lock.

What it brings: capture-time placement once locked (frames released by
capture time, so arrival jitter stops moving the display), slope-fit
acquisition (locked in 7-16 s), wait/acq/lock/oor states, and the drone's
boot sensor trim (`sensor_cal`, already on his master da0bc70 since
2026-10-14). Cost: `RC_VERSION` 16 (`Rcf::snr_off`) is a flag day for the
drone, the GS and the hosted web GS together; conflicts with our genlock in
`frame_regulator`, maburplay `main.cpp` and the drone's sensor writer (our
`star6e_controls_apply_sensor_mfps` / `genlock_trim.h` against his boot trim
plus offset). Merging his master also brings his Orange Pi GS work
(v4l2req decode, colortrans CTM). On his master since 2026-10-18 (PR #85,
master 5d642c0d), merged into ours as 522c3fd6 (31 files resolved), and our
genlock removed in the commit after it: T_GENLOCK/CAP_GENLOCK off the wire,
the drone's sensor_mfps path and debug verb gone, old `[genlock]` /
`display.genlock` keys still load (one stderr line) so a kept config boots.
Flag day: RC_VERSION 16, so the next GS and drone images go out together.
Also arrives with the merge: demand refresh (`venc.gop_s` 0, the GS asks
for an IDR on damage; our record-start keyframe ask gives way to his
`refresh dvr`), reg.log/frame.log, and `display.chain_budget` 0 /
`vsync_lead_ms` 5. Gate: on the bench, `reg.log` shows `lock=lock` within
~20 s of video and stays there; flightreport's PHASE LOCK section reads it.
Flash both ends to fresh defaults, as for gs8/d8 (drone `sysupgrade -n`, GS
config files deleted): the code defaults keep `phase_lock`, `sched_cap_ms`
and demand refresh OFF, so a config kept from gs8/d8 (which has none of
those keys, and `gop_s` 0.5) would run without them.

## 13. Player OSD shows 80 MHz rungs as "/80" (done in code, ships with the next GS image)

Bench 2026-10-18: the bar's MCS cell read "4/40" while MCS4 flew at 80 MHz
(the width was clamped to 20..40). Fixed in 0c27b8d5 with a test; display
only, the link itself was at 80.

## 14. Drone steps its own width 80 <-> 40 with a fast switch; hops only at 40 (agreed 2026-10-19)

Why: the drone radio sits at 80 for the whole session, so its receiver
hears the GS's 20 MHz control frames worse than it would at 40: ~1-1.5 dB
on the GS cards (bench 2026-10-17, up to ~3 dB on one card; the drone
itself not measured). That is the link whose 3 s silence drops the drone
into failsafe at MCS0/20. Video on the 20/40 rungs gains ~0: the GS cards
already sit at 40 there, and devourer's 8822E TX power has no tuned-width
term (TX quality from an 80-tuned radio is unmeasured). Operator garage
walk, 2026-10-19: the link fell to MCS0/20 where
Gilang's all-40 build holds MCS0/40. That is a 4-6 dB gap; 80 MHz explains
at most ~1.5 dB of it, so gate on the A/B first.

Gate: the gs8 + d8 test doc's "Drone width checks" W3/W4 (drone at 80 vs
40, same garage spot, failsafe seconds + time per rung). Build only if the
drone at 40 holds a higher low rung or spends less time in failsafe.

Parts:

a. **devourer: a fast 40 <-> 80 width switch on the 8822E** (same block),
   lean like `FastRetune`: RF18 centre + BW bits, RF 0x1a RXBB, BB
   BW/primary, TX DFIR, AGC table, BB reset, no recalibration. Or the chip
   firmware's channel switch (H2C 0x1D) with bw = 80, which devourer gates
   to 20/40 today. Validate: register parity against the full path (like
   `tests/hop_parity_check.sh` / `tests/fast_bw_parity.sh`), on-air dead
   air, and delivery at 80 after a fast widen vs a full tune (the skipped
   IQK is the risk). Estimate 2-5 ms typical: `FastRetune` dead air
   measured 2.75 ms median / 7.1 ms max (8822B), ~2.3 ms (8822C); mabur's
   GS cards timed `FastRetune` calls at 1.9-12 ms with one 44.7 ms. None
   of that is a width change, none is on the drone's host (SSC338Q, 76-80
   us per USB control transfer).
b. **Drone: home width 40**, widen to 80 only for the 80 rungs and 80
   probes, back to 40 after a hold (as the GS's 3 s). Switch right after a
   frame's burst ends, so the dead air lands in idle air: one frame delayed
   by up to the switch time, only when the ladder crosses 40/80.
   Coordination stays loose (a wide receiver hears narrow frames): going
   up, one GS card widens first (already does for probes), then the drone;
   going down, the drone first, the GS cards after their hold.
c. **Hops only at 40.** A hop ordered on an 80 rung steps to 40 first,
   hops, then the ladder climbs back. Item 0's 80 MHz hop path is then
   never used. 80 MHz exists only on 36 and 132 (devourer puts the 80 MHz
   primary on a block's lowest channel; 52/100/116/149 blocks carry an
   8822E spur and take the 65-91 ms full path), so on an escape pair
   (140+144, 44+48, 124+128, ...) the ladder tops out at MCS4/40.
d. **GS only, independent, no pause: cards narrow to 20 on the 20 rungs.**
   ~0.5-1 dB on video there (SNR readout -0.8 / -1.1 dB tuned 40 vs 20,
   near its ceiling; an earlier read 0.2-0.5 dB). The 20 -> 40 probe then
   widens one card first, as the 80 probe does.

Counterpart: none of it is built. Skipping recalibration may cost EVM at
80. The drone's switch may be slower than the GS's. Hops at 40 were benched
once (2026-09-15, before the 40/80 rungs) and never flown. With today's
full-path switch (65-91 ms on the GS cards) the plan is a 4-5 frame pause
per 40/80 crossing at 60 fps, which the operator ruled out.

## Proposed, not yet agreed

Gaps found while building gs8 / d8. Ask before scheduling.

- **VHT rates in the calibrated power table.** With `power_mode =
  "offset"`, devourer's custom table leaves every VHT rate at MCS7's level
  (`devourer/src/TxPower.h`), so the 80 MHz rungs fly 3-4 dB under stock.
  maburd warns at boot. Fix: extend `TxRateDiffsQdb` to VHT1SS MCS0-7
  (a devourer contract change across generations).
- **Channel 149 at 80 MHz.** The 149-161 block is non-DFS but carries an
  8812EU spur at 155 (notched). A 5-minute bench run on 149 before adding
  it to the default set `[36, 132]`.
- **A better-looking bottom rung.** MCS0/20 flies 1080p60 at 500 kb/s:
  robust but very blocky. A lower resolution or frame rate there would
  look better; costs a keyframe on every switch into or out of it.

