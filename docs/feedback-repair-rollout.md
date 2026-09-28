# Feedback repair — rollout

**Status 2026-09-28: phase 1 (shadow mode) is built and host-tested. It has
not flown.** Nothing below phase 1 exists yet. The design this rolls out is
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
2. **devourer turnaround bench.** Time status-on-air → repair-on-air at a
   passive witness (`tsfl`) with the drone's queue loaded. Add per-packet TX
   queue selection so a repair can overtake queued video. **Kill criterion
   (120 fps target):** p99 well above ~8 ms.
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
