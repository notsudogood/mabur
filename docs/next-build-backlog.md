# Next build backlog

What the operator has agreed goes into the next GS (and drone) build, in
order. Each item names its evidence. Remove an item when it ships.

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

Bench gate: step test 35 / 40 / 45 / 50 Mb/s at MCS5/40 with item 1 in,
drone and GS CPU probes, GS `fec` flat. 50 Mb/s also needs item 4 for the
air: at 40 MHz it is ~85 % of what MCS5 delivers.

## 4. 80 MHz top rung

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
