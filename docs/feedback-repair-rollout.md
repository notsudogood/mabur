# Feedback repair — rollout

**Status 2026-10-04: phase 1 (shadow mode) has flown twice, indoors;
phase 2 (the turnaround bench) is built and host-tested, not yet run on
hardware.** Nothing below phase 2 exists yet. The design this rolls out is
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
2. **Turnaround bench — built (below).** Time status-on-air → repair-on-air
   at a passive witness (`tsfl`) with the drone's queue loaded. Add per-packet
   TX queue selection so a repair can overtake queued video. **Kill criterion
   (120 fps target):** p99 well above ~8 ms (~16 ms at 60 fps).
3. **Listen window + per-AU status frame**, FEC unchanged. A/B uplink
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
- **Unanswered pings are a result, not a bug.** A ping that lands while the
  drone is transmitting is lost (half-duplex); that rate is what phase 3's
  listen window has to beat.
- **The pings cost a little video.** Each is ~0.1 ms of GS transmit outside
  the RCF slot, and the other card is blanked for it (the self-blanking
  `RcfSlotter` exists to avoid). At 10 Hz that is ~0.1% of airtime.
- **Lane 0 is the video's queue:** the MGMT queue on singles rungs, the
  A-MPDU TID queue on aggregating ones (`ampdu.min_mcs_20/_40`). The per-rung
  split separates the two.
- **On-air turnaround includes the drone's RX USB path,** which a repair
  would pay too; "outside the drone's hold" is that plus the TX path, queue
  and air.
- **Not yet flown, not yet run on hardware.** Host tests pin the wire, the
  responder, the pinger, `ta.log` and the report.

