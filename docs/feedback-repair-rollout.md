# Feedback repair — rollout

**Status 2026-10-07: phase 1 (shadow mode) logs on every flight; phase 2 (the
turnaround bench) has flown twice (results below); phase 3 (the listen
window) is built and host-tested, not yet flown.** Nothing below phase 3
exists yet. An independent take on the same problem, gilankpam's
`fec-nack`, is compared at the end of this page. The design this rolls out is
devourer's `docs/fpv-link-architecture.md` (branch
`claude/wifi-fpv-link-architecture-1bms9l` of `notsudogood/devourer`); this
page restates it in mabur's terms and tracks the phases.

## The design in one screen

The idea is software acknowledgement for the video downlink, without giving
up broadcast and FEC:

- **Downlink stays as it is:** broadcast, SVC-T base + enh, per-layer
  sliding-window RLC, SBI salvage, no-ack A-MPDU. The FEC overhead comes
  down to a *floor* sized for one lost aggregate per frame, not for fades.
- **A scheduled listen window after every burst.** The drone reserves
  ~1.5–2 ms after its burst and probe and budgets it in the bitrate policy,
  anchored to the chip TSF. The GS then sends into a known gap instead of a
  predicted one, which replaces `RcfSlotter`'s guesswork (`docs/tx-rx-timing.md`
  gaps #1, #2 and #5).
- **One status frame per AU period, inside that window.** It carries the
  RCF fields plus, per layer, "short by `k` over seqs `[a, b]`", repeated
  until filled. It is cumulative, so a lost status frame costs one period,
  not one `feedback_ms`.
- **The drone answers with `k + 1` fresh repair symbols over `[a, b]`**,
  queued ahead of the next AU, one rung lower. These are repairs, not
  retransmitted originals: any RLC repair spanning a missing seq substitutes
  for any missing source, so the request is a count, not a list. Loss the
  FEC already covered generates no request at all.
- **Confirmation** is the drone echoing the status seq in its next burst
  header. No hardware ACK is used anywhere: it is FEC-blind, its retries
  walk the rate down, and its ACK window is an 8-bit µs register (~30 km
  ceiling, ~12 km at devourer's 128 µs default) that fails silently past
  range.
- **The ladder keeps scoring pre-repair loss** and gains repair demand as a
  demote input, so repairs never hide a failing rung.

The number every later phase needs is how many repair symbols a layer is
short at a burst end. That is `SwDecoder::deficit()`: unknown live seqs
minus pending independent rows. It is exact — `rows_` is kept in echelon
form over unknown seqs only, so it reads 0 exactly when nothing is missing.

## Phases, each gated on a measurement

1. **Shadow mode, GS only — built (this page, below).** Log the per-burst
   shortfall on real flights. **Kill criterion:** if lost episodes are mostly
   outage-shaped (large, long-growing), the ladder and IDR already do the
   useful work; stop here.
2. **Turnaround bench — built, flown once (below).** Time status-on-air → repair-on-air
   at a passive witness (`tsfl`) with the drone's queue loaded. Add per-packet
   TX queue selection so a repair can overtake queued video. **Kill criterion
   (120 fps target):** p99 well above ~8 ms (~16 ms at 60 fps).
3. **Listen window + per-AU status frame**, FEC unchanged — built (below). A/B uplink
   delivery, `link.rcf_slot` timeouts and video loss (`ausniff.py`) against
   `RcfSlotter`.
4. **Repair loop on today's FEC:**
   - GS: request from `deficit()`, and hold an incomplete AU ≥ one round trip.
   - Drone: a retained-symbol ring ≥ RTT + one burst (`fec.window` 32 is
     about a third of a 30 KB AU), a "k repairs over `[a, b]`" envelope (the
     wire window is u8), and a front-of-queue lane.
   - Gates: `ausniff.py`, the `lat` segments, `aucadence.py`.
5. **Lower the overhead floor in steps** (1.0 → 0.5 → 0.25 at the mcs5
   rung), each step A/B'd on the same gates plus `fec.log` and `arq.log`.
   **Reminder when this phase starts (recorded 2026-10-09):** investigate
   per-slice H.265 encode/decode to overlap the `fec` first-body → AU-complete
   window (9.8 ms p50, base 12.6 / enh 6.2 —
   `docs/latency-budget-findings-2026-08-31.md`). What is known so far:
   - *Ground:* gehee/fpvOS decodes a picture as its slices arrive — an MPP
     patch (`br-external/package/rockchip-mpp/0002`) plus a kernel patch
     (`board/vrxpro/linux-patches/0003`, rkvdec2 stream mode, Rockchip BSP
     5.10). The author's RK3568 numbers: last slice → frame 2.0 ms vs 5.1 ms
     whole-picture; no independent or RK3566 measurement. Its patch 0001
     (h265d: refuse a slice whose PPS names a missing SPS) is a standalone
     crash fix worth taking regardless.
   - *Air (SSC338Q/Star6E):* libmi_venc exports `MI_VENC_SetH265SliceSplit`
     and mi_venc.ko has a slice-done IRQ. Slices quantize to 64-px CTU rows
     (1080p = 17 rows) and multi-slice disables re-encode. waybeam already
     wires it (`video0.sliceCount`, whole-AU output, for spatial
     concealment); a realtime VPE→VENC ring is impossible on i6e (waybeam
     `documentation/REALTIME_PIPELINE_INVESTIGATION.md`). Whether `GetStream`
     returns slices before the frame ends is **unverified**; a forum member
     recalls that it does (OpenIPC forum, 2026-10-09: "IIRC yes — the problem
     has always been the receiver side; most decoders wait for all parts of
     the frame"). Recollection, not measurement — the first experiment below
     settles it.
   - *So the blockers are on the ground:* the decoder (what the fpvOS patches
     address) and mabur itself — the GS's FEC releases a frame only once the
     whole AU has arrived, and the player hands MPP the whole AU. Slice-level
     decode needs both to release in-order source as it arrives.
   - *mabur today:* the drone loads the SliceSplit symbol but never calls it;
     the GS player feeds MPP one whole AU per `decode_put_packet`
     (IMMEDIATE_OUT on).
   - *First experiment:* `sliceCount=4` on the SSC338Q, logging per
     `GetStream` the `packNum`, `packetInfo[].sliceId`, `endFrame` and
     timestamps. Slices spread over several ms → worth pursuing (the FEC must
     then also release in-order source before the AU completes). All in one
     pack → the gain is capped at ~3 ms of decode + airtime overlap, ranking
     below the vsync-locked regulator and the pair-policy items.
   - *RK3566 vs RK3568 (checked 2026-10-09):* the decoder is the same block.
     The Rockchip 5.10 BSP builds `rk3566.dtsi` as `#include "rk3568.dtsi"`
     plus `/delete-node/`s that are all I/O (PCIe 3.0, `sata0`, `gmac0`,
     `lvds1`, `combphy0`) and touch no codec node, clock or OPP; mainline
     shares the codec nodes in `rk356x-base.dtsi`. What can still differ from
     the author's numbers: **(1) the kernel** — the GS (runcam_wifilink /
     radxa_zero3 builds) runs Radxa's Rockchip BSP **6.1.84**, not 5.10, so
     the fpvOS kernel patch needs a port of unknown effort; **(2) DRAM** — the
     GS boots `rk3566_ddr_1056MHz` (the BSP's rk3566.dtsi sets the same
     1056 freq), and the decoder streams references from DRAM, so absolute
     times may run slower than on a faster-clocked RK3568 board (the author's
     DDR speed is not stated); **(3) CPU** — the RK3566 drops the 1.99 GHz
     OPP (MPP parsing only, small).
   - *What it could buy (estimate from the budget above + the author's one
     measurement, not measured here):* `enc` (~7 ms), first-body → AU
     complete (~8–10 ms in 2026-10 flights) and `dec` (~6–9 ms) run in
     series today, ~22–25 ms per frame; with 4 slices they overlap. Decoder
     side alone (whole-AU from the drone, slices decoded as they arrive):
     **~3–6 ms** (the author's 5.1 → 2.0 ms). Encoder side too (slices leave
     the drone as they are encoded): up to **~5 ms more**, depending on how
     much of `enc` is encode rather than the frame-based VPE ahead of it and
     on `GetStream` really returning slices early. Together **~5–10 ms off
     the p50**, ~10 % of the ~75–85 ms glass-to-glass; large frames (IDR,
     scene changes) gain most, which trims some size-driven jitter. It does
     **not** fix the 80–90 ms spikes — those are FEC waits for lost symbols,
     and the decoder emits whole pictures, so a frame with a missing slice
     still waits (repair, phase 4, is that fix; the two compound). Gains
     reach the screen only once the vsync regulator re-centres on the earlier
     arrivals. Costs: a few % coding efficiency for 4 slices (no prediction
     across slice boundaries), the SSC338Q drops re-encode with multi-slice
     (bigger overshoot bursts), and work on both ends — drone per-slice
     ring/FEC packing; GS in-order slice release, per-slice player feed, and
     the BSP 6.1 decoder port.
6. **Ladder coupling** (repair demand as a demote input).

## Phase 1 as built

GS only, no wire change, no config key: it rides `debug_log.enable` like
`fec.log`. It pairs with any drone the unmodified GS pairs with.

- **`SwDecoder::deficit()` / `UepDecoder::deficit(sid)`** (`common/`): the
  shortfall above.
- **`maburgs::ArqShadow`** (`gs/src/arq_shadow.h`): pure logic, fed
  arrival-order events. A burst ends at:
  - the other layer's first body (base and enh AUs alternate);
  - the trailing probe body;
  - a same-layer body after an 8 ms RX-stamp gap (enh shed);
  - 15 ms of silence on the processing clock.

  The decoder is sampled 4 ms after the end. A body stamped inside that
  settle window is the other card's copy of the tail, not a new burst, and a
  probe copy stamped before the current burst began is ignored. Runs of
  short burst ends become episodes.
- **`arq.log`** (`gs/src/arq_log.h`, format in `docs/observability.md`): `E`
  episode rows and `S` per-layer burst counts.
- **Hooks in `maburgs`:**
  - `Aggregator::set_video_hook` sees each CRC-clean video body before it is
    decoded.
  - The probe sink ends the burst.
  - The loop ticks and drains every iteration.
  - `reset_continuity()` resets the tracker.
- **`maburgs --dry-run`** runs the same tracker over a replay and prints
  `arq_shadow <sid>: bursts= short= episodes= lost= peak= open_deficit=` on
  stderr. It is order-only, because the replay clock is synthetic.
  `tests/integration/run_gs_e2e.sh` pins that a clean replay of the fixture
  is never short and a seeded 40%-loss replay leaves the base layer short.
- **`tools/flightreport.py`** has an ARQ SHADOW section; `tools/session.py`
  resolves `arq.log`.

### Flying it

1. Deploy `maburgs` from this branch (`tools/build-arm64.sh`,
   `docs/deploy.md`), or flash a GS image built from it. The shipped bundle
   now has `[debug_log] enable = true`. A GS that keeps an older
   `/config/maburgs.toml` keeps that file's value, so check it there. The
   drone needs no change, but must be built from the same mabur commit (no
   wire change here, but a mismatched pair across other commits can be).
2. Fly the flights you care about. Range and obstruction are what decide
   this, not the bench.
3. Run `python3 tools/flightreport.py /media/dvr/log/NNNN`. Its ARQ SHADOW
   section reports, per layer:
   - **short-burst share and would-request rate:** the uplink cost, to set
     against today's 10–20 RCF/s;
   - **peak shortfall in aggregates;**
   - **episodes resolved in-band**, with their fix delay — the latency a
     repair at an 8 ms round trip would have saved;
   - **lost episodes,** split into burst-shaped (≤ 2 aggregates, stopped
     growing within 50 ms: one repair round could have saved them) and
     outage-shaped.

### Read it with these caveats

- **`dur_ms` is quantized to same-layer burst ends,** ~33 ms at 60 AU/s
  alternating. A resolved episode's fix delay is an upper bound, in steps of
  the layer period.
- **Watch for false positives.** A sample that caught a tail still in flight
  reads short and resolves at the next sample with nothing lost. That is
  indistinguishable from a genuine one-period in-band fix. The settle window
  and the late-copy rule guard against it; unit tests pin them, but they are
  not flight-validated.
- **`saveable` is a heuristic, not a proof.** In a live loop the request and
  its repairs cross the same channel, and can be lost too.
- **The reader's constants are assumptions:** aggregate size 6 bodies
  (`ampdu.max_num`), 8 ms round trip, 50 ms outage threshold, 10 s summaries
  (`tools/flightreport.py`).
- **Boot warm-up** can log large early episodes (the same debris `fec.log`
  shows in the first ~18 s after a `maburgs` restart). Drop them by time;
  `stale` catches rung-change debris.
- **Nothing is transmitted,** so this cannot say whether the uplink half
  works. That is phases 2–3.

## Phase 1 results (2026-10-03, two indoor flights)

Two flights in and around a brick-and-wood house with a garage, about
2.5 minutes of video each, signal −24 to −48 dBm outside the garage and down
to −83 dBm inside it. Read `flightreport.py`'s ARQ SHADOW section with one
correction: it drops every episode that straddled a rung change (`stale` >
0), which here hid most of the garage losses; the counts below include them.

- **Outside the garage, every lost episode was repairable-shaped:** 6 of 6
  across both flights, all on the enhance layer, all about one aggregate or
  less and not growing — one damaged frame each, about two a minute. Each
  matched a damaged or missing AU in `au.log` within ~60 ms.
- **Inside the garage, most were outages:** 14 of 20 on the second flight
  grew past two aggregates or kept growing past 50 ms, ending in multi-second
  shortfalls with the drone on the floor at −83 dBm. Repair cannot fix those;
  how fast the ladder falls, and how low its floor is, decides them.
- **The uplink survived the worst case:** the drone heard 10–14 GS frames/s
  at −83 dBm (~20/s normally), never zero. Telemetry resolution is 1 s, so a
  ~100 ms gap cannot be ruled out.
- **Request cost** would have been 0.4–4 requests/s against ~20 RCF/s.
- **Latency spikes** (a frame ≥ 70 ms, 38 one-second windows over both
  flights) sat at demotes (16), FEC-fixed shortfalls (6), lost frames (4),
  promotes (2) and nothing logged (10). The waits a repair round would cap
  are the in-band fixes, 15 ms typical and 30–70 ms at worst.

The adversarial counterpart: two flights, indoors, short. The stop condition
(losses mostly outage-shaped) is met inside the garage and not outside it,
so the repair loop's case rests on range flights, where marginal signal is
the common case rather than a garage visit. Separately, the bottom rung
matters more than repair for fades like these: the shipped ladder is all
40 MHz, and `docs/bw40-sweep-findings-2026-09-23.md` measured 20/0 reaching
4–6 dB further than 40/0.

## Phase 2 as built: the turnaround bench

What a repair round costs end to end, measured before any repair protocol
exists: the GS sends a ping, the drone answers on a chosen hardware TX
queue, and a GS card that did not send the ping times both on air.

- **devourer `TxMode::hw_queue`** (`docs/aggregation.md` there): per-packet
  hardware queue on Jaguar3 — BK, BE, VI, VO, Mgmt or High — through a
  private radiotap TX_FLAGS field. A frame in a higher-priority queue can air
  ahead of video already waiting in the chip; it cannot pass frames still in
  the USB pipe ahead of it (one bulk-OUT endpoint for every queue). Which
  queue actually overtakes the video queue is exactly what this bench
  measures.
- **Wire:** `T_TA_PING` (GS → drone: seq, lane, reply shape) and
  `T_TA_PONG` (drone → GS: the ping's seq and lane, frame idx/n, the drone's
  hold time and queue state, padded to the requested size). New types inside
  `RC_VERSION` 11; the GS pings only a drone whose DISC_ACK carries
  `CAP_TURNAROUND`.
- **Drone (`drone/src/ta_responder.h`):** the RX callback stamps and
  enqueues; a dedicated thread answers through the same direct send every
  control frame uses (never the video pool), at control robustness, on the
  ping's lane. Lane 0 is the default queue — the one control frames and
  video share today. Rate-limited to 50 pings/s, quiet during calibration.
- **GS:** `[turnaround]` in `maburgs.toml` (off by default). Pings leave at
  jittered random times, not in the RCF slot, so they sample every queue
  state; lanes interleave round robin. Sightings go to `ta.log`
  (`docs/observability.md`).
- **`flightreport.py` TURNAROUND:** per lane, replies received, on-air
  turnaround p50/p90/p99, host round trip, drone hold, loaded vs idle, per
  rung, and the gate.

### Running it

1. Flash both ends from the same build (the drone must advertise
   `CAP_TURNAROUND`; an older drone is simply never pinged).
2. Add to `/config/maburgs.toml` on the GS (an existing file never picks up
   new bundle sections) and restart maburgs:

   ```toml
   [turnaround]
   rate_hz = 10
   lanes   = [0, 4, 5]   # today's queue, voice, management
   frames  = 1
   bytes   = 64
   ```

3. Run video at a steady rung for a few minutes — on the bench is fine, the
   queue is loaded either way — then fly. `frames = 6`, `bytes = 1400`
   shapes the reply like a one-aggregate repair burst.
4. `python3 tools/flightreport.py /media/dvr/log/NNNN`.

### Read it with these caveats

- **The witness needs both GS cards on the home channel.** While the scout
  has the second card elsewhere, pings get host round trips only.
- **Unanswered pings are a result, not a bug.** The expectation was that a
  ping landing while the drone transmits is lost (half-duplex), and that
  rate is what phase 3's listen window has to beat. The first flight did not
  confirm that cause (results below): know the rate, not yet the reason.
- **The pings cost a little video.** Each is ~0.1 ms of GS transmit outside
  the RCF slot, and the other card is blanked for it (the self-blanking
  `RcfSlotter` exists to avoid). At 10 Hz that is ~0.1% of airtime.
- **Lane 0 is the video's queue:** the MGMT queue on singles rungs, the
  A-MPDU TID queue on aggregating ones (`ampdu.min_mcs_20/_40`; shipped:
  aggregation from mcs2 at 40 MHz, mcs4 at 20 MHz). So **on singles rungs
  lane 5 (Mgmt) is the video's queue too** and only lane 4 (VO) can overtake
  it there. The per-rung split separates the cases.
- **On-air turnaround includes the drone's RX USB path,** which a repair
  would pay too; "outside the drone's hold" is that plus the TX path, queue
  and air.
- **The GS drops replies it cannot parse into `X` rows.** The first
  phase-2 build dropped every pong (the GS RX body keeps the 4-byte FCS and
  the pong's CRC is at its end; fixed in 495f7da), which read as "the drone
  never answered". A TURNAROUND section with an `X` warning is a parser or
  wire mismatch, not a link result.

## Phase 2 results (2026-10-06, first flight)

One flight, ~2.3 minutes, the drone in the GS's room or one room away,
signal −20 to −50 dBm, 85% of the time at mcs4/40. `rate_hz` 10, lanes
0/4/5, one 64-byte reply per ping. 1 187 pings; ~350 replies per lane timed
on air by the witness card.

| lane (queue) | answered | on air p50 / p90 / p99 / max | gate |
|---|---|---|---|
| 0 (video's) | 370/395 | 2.8 / 7.4 / 11.8 / 13.1 ms | 60 fps only |
| 4 (VO) | 370/396 | 2.2 / 2.7 / 6.0 / 10.4 ms | PASS |
| 5 (Mgmt) | 365/396 | 1.9 / 2.4 / 4.4 / 12.5 ms | PASS |

- **The drone is not the cost:** ping → reply handed to the radio took
  0.12 ms typical, < 2 ms worst, on every lane.
- **Lane 0's tail is inside the chip.** All 53 lane-0 replies over 6 ms had
  the drone's TxQueue and USB pool empty and no air-clock backlog: the reply
  waited behind video already handed to the chip, which no host-side queue
  metric sees. VO and Mgmt skip that wait — the reason devourer's
  `TxMode::hw_queue` exists, now measured.
- **The latency spikes are the losses a repair targets.** 13 of the 15
  one-second `lat` windows with a frame ≥ 65 ms sat on an `arq.log` + `fec.log`
  episode (the worst: 90 ms at 106 s, 85 ms at 53 s); the other two (66–67 ms)
  were the top of the vsync-beat sawtooth. As in phase 1, every lost episode
  (7) was repairable-shaped and none outage-shaped.
- **7% of pings went unanswered** (82), and the cause is open. The witness
  heard 75 of them on air, and answered pings reached both GS cards 97% of
  the time, so the misses are on the uplink or inside the drone. They are
  spread evenly in time and identical across both GS TX cards and all three
  lanes. Pings inside a video receive window (`au.log` `t_first`..`t_complete`)
  went unanswered at the same rate as the rest (52% of each), so the
  half-duplex explanation is not supported at that resolution. The drone's
  responder counters print only at a clean maburd exit.

The adversarial counterpart, and why the gate is only half closed: **the
queue was almost never loaded** (air backlog ≥ 2 ms on 1–2 replies per
lane) — close range at mcs4 has headroom to spare, so the case the gate is
about, far away at mcs0–2 with long bursts, is unmeasured. The few low-rung
samples hint the floor rises there (VO p99 9.1 ms at mcs0, Mgmt 12.5 ms at
mcs1, n = 5–7 each — not a measurement), and all four VO/Mgmt replies over
6 ms fell at 25–36 s while the link climbed mcs0 → mcs4. Each p99 rests on
~4 replies. A 64-byte reply is not a repair: `frames = 3`, `bytes = 1400`
shapes it like one. Next flight: that shape, far enough to sit at mcs0–2.

## Phase 3 as built: the listen window

The drone keeps the air quiet for a few ms after every burst, and the GS sends
one short status into each of those gaps — instead of RcfSlotter predicting
where the drone's idle will be and timing out into a random blast when there
is none (`docs/tx-rx-timing.md` gaps #1 and #5). FEC, the ladder and the RCF
contents are unchanged; the status carries each layer's shortfall
(`SwDecoder::deficit()`) for phase 4, and the drone only records it.

- **Wire** (`common/include/mabur/rc_proto.h`): `T_STATUS` (GS → drone: seq,
  what marked the burst end, the AU's frame id, `listen_ms`, per-layer
  deficit) and `T_LWSTAT` (drone → GS once a second: statuses heard, a
  histogram of where they landed against their AU's gap, the gap's cost).
  New types inside `RC_VERSION` 11 like the turnaround pair, fixed-length with
  the CRC at a fixed offset (a body that still carries its FCS parses), and
  the GS sends statuses only to a drone whose DISC_ACK carries `CAP_LISTEN` —
  so either end can be flashed first.
- **Drone** (`drone/src/listen_window.h`): after the hot thread books an AU
  and its probe, the AirClock's `free_at` is the burst's modelled end on air;
  the gap runs from there for `listen_ms`, keyed by the AU's frame id. The
  next AU's first body — and any late repair of the last one, which would
  otherwise air right where the status is aimed — is stamped `not_before_us`;
  the TX writer waits for it (a held body always starts its own batch), and
  the clock reserves the gap so the model stays honest. Control, MSP,
  telemetry and pong sends that would land in a gap wait too. The gap exists
  only while statuses keep arriving (0.5 s timeout) and never exceeds 10 ms,
  so a GS that stops asking costs no video. Each status is timed against its
  own AU's gap on the RX thread.
- **GS** (`gs/src/listen_burst.h`): one burst end per AU — its first probe
  copy from either card, else the learned deadline if the probe is lost, else
  the completion when no probe is commanded — and one `T_STATUS` per burst
  end, right behind whatever RCF/DISC the slotter just released. In this mode
  the slotter stops predicting: every burst end releases, and grace widens to
  `ms − 2`.
- **`[listen]` in `maburgs.toml`** (off by default): `ms` the gap,
  `ab_s` > 0 alternates on/off every `ab_s` seconds so one flight carries
  both arms of the A/B. Exported as `link.listen` (the drone's latest
  T_LWSTAT under `link.listen.drone`).
- **`flightreport.py` LISTEN WINDOW**: per arm (gap on / off), RCF delivery
  and slot timeouts, statuses sent and heard, where they landed, the gap's
  cost (AUs held and for how long), and the video side (truncated + dropped
  AUs, abandoned symbols, pre-FEC loss, e2e latency from `lat.log`). Rows
  within 1 s of a switch belong to neither arm.

### Running it

Flash both ends from the same build, then add to `/config/maburgs.toml` on
the GS (an existing file never picks up new bundle sections) and restart
maburgs:

```toml
[listen]
ms   = 4     # start here; the histogram says whether it is too short
ab_s = 30    # on 30 s, off 30 s, on... -- both arms in one flight

[turnaround]
rate_hz = 0  # off: its replies would land in the gap and skew both benches
```

Fly the usual route, near and far, for long enough that each arm gets a few
minutes; then `python3 tools/flightreport.py /media/dvr/log/NNNN`.

### Read it with these caveats

- **"Where it landed" is relative to a model.** The gap starts at the
  AirClock's modelled end of the burst, and the arrival stamp is the drone's
  RX callback (its own USB RX latency included). A histogram centred late
  means either the GS is slow or the model ends bursts early — the same
  number either way, and the one that sizes `ms`.
- **The gap costs latency when a burst overruns.** The bitrate budget (0.6)
  leaves ~6.7 ms of idle per AU on paper and 4–6 ms measured
  (`docs/tx-rx-timing.md` §3.1), so a 4 ms gap mostly costs nothing; after a
  burst that runs long (IDR, scene change) the next AU waits up to `ms`. The
  LISTEN WINDOW section counts those holds, and `dq` in the GS's lat segments
  shows them per AU.
- **Statuses cost uplink air:** ~60 short frames/s at MCS0, ~0.2 ms each,
  each one blinding both GS cards for ~180 µs — harmless only if they land in
  the gap, which is the thing being measured.
- **Status delivery is a per-second ratio,** the drone's report against the
  GS's count, not a per-frame ledger.
- **Not yet flown.** Host tests pin the wire, the drone's gap and timing
  logic, the GS's burst-end detection, the export and the report.

## Compared: gilankpam's `fec-nack` (2026-10-05/06)

Upstream built and flew a software NACK on branch `fec-nack`
(`docs/fec-nack.md` there; RC_VERSION 15). It is kept as a comparison, not
merged: two independent designs on the same hardware are worth more than
one. Where they differ:

| | this rollout | `fec-nack` |
|---|---|---|
| request | a *count* of repairs short over `[a, b]` (phase 4) | a *list* of missing source seqs (≤ 128 per frame) |
| answer | fresh RLC repair symbols — any one fills any hole | the original source symbols from a 150 ms ring |
| uplink timing | a scheduled listen window after each burst (phase 3) | sent directly, mid-burst; the RCF slot filled at p50 44 ms in its spike |
| drone queue | a hardware queue that overtakes video (phase 2) | front of TxQueue, same hardware queue as video |
| scope | both layers; the goal is a lower FEC floor (phase 5) | base layer only; FEC and ladder unchanged ("option A") |
| pacing | one cumulative status frame per AU period, repeated until filled (phase 3) | adaptive settle 4–24 ms, 2 tries, 12 ms minimum lead, a drone air bucket |

Its bench (`docs/fec-nack-bench-findings-2026-10-06.md` there), pinned
mcs2/40 under a co-channel jammer: base truncated + dropped AUs 12 → 0,
fill p90 10–16 ms, `air_pct` unchanged; but 35% of requests wasted (FEC got
there first), uplink delivery 79% (96% without the jammer), and **at rung 0
no request was ever filled** (0 of 467). Two of those meet phase 2 head on:
its retx bodies wait behind video inside the chip exactly like lane 0 above
(the transport share of its fill time is about lane 0's p90), and rung 0 is
where in-chip video drains slowest — untested, but the place a VO lane
should show. Its 79–96% uplink delivery is the same order as the 93% above.

**For a like-for-like comparison** phases 3–4 report what its sideport
reports: base truncated + dropped AUs vs a control arm, fill ms (first
request → hole filled) p50/p90, wasted %, uplink delivery %, `air_pct`, and
fills per rung — rung 0 above all.


## Side experiment: pin the GS CPU governor (latency, not repair)

Recorded 2026-10-09; not part of the phased rollout, and not yet flown.

The GS image (Radxa BSP 6.1 kernel, `board/radxa/zero3/linux-fragment` in
sbc-groundstations) boots with `CONFIG_CPU_FREQ_DEFAULT_GOV_ONDEMAND=y`:
the RK3566's cores idle low and climb only after the governor samples the
load. maburgs' work arrives in 60 Hz bursts (drain, FEC, reassembly, MPP
parsing, the player), so the clock may still be ramping when a frame needs
it — a little delay and jitter per frame. The `performance` governor keeps
the cores at the RK3566's top in-spec OPP, 1.8 GHz, the same thing the
player already does for the GPU's devfreq under colortrans
(`docs/colortrans.md`). Whether ondemand actually costs anything here is
unmeasured.

Check what the GS runs, then switch for one flight (lasts until reboot;
the governor is built as a module):

```sh
ssh root@10.18.0.1 'p=/sys/devices/system/cpu/cpufreq/policy0; cat $p/scaling_governor $p/scaling_available_governors $p/scaling_cur_freq; for d in /sys/class/devfreq/*; do echo "$d $(cat $d/governor) $(cat $d/cur_freq)"; done'
ssh root@10.18.0.1 'modprobe cpufreq_performance 2>/dev/null; echo performance > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor; cat /sys/devices/system/cpu/cpufreq/policy0/scaling_governor'
```

Fly it as its own flight, not inside another A/B, and compare the `lat`
segments (`fec`, `dec`, `reg`, `dsp`, e2e p50/p99) against a flight on
ondemand over the same route; a difference under a couple of ms is inside
flight-to-flight noise. If it helps, the durable form is an init script or
`CONFIG_CPU_FREQ_DEFAULT_GOV_PERFORMANCE` in the GS build.

Rejected alongside it: **overclocking the RK3566.** The 1.99 GHz OPP exists
for the RK3568 and Rockchip's BSP deliberately deletes it for the RK3566
(`/delete-node/ opp-1992000000` in its `rk3566.dtsi`; mainline stops at
1.8 GHz). Re-adding it is a device-tree edit, but it runs the part past its
spec (higher voltage, unbinned silicon, heat that can trigger throttling in a
small enclosure) for ~10 % CPU clock — and the CPU does not decode video; the
hardware decoder has its own clock. If decode time itself becomes the target,
the bigger lever is DRAM (the GS runs it at 1056 MHz; the decoder streams
reference frames from it), which is also out of spec and board-dependent.
