# Does the phase lock work on the Orange Pi GS, and how does it compare with the Radxa?

**Status:** bench record; the phase lock runs on the Orange Pi unchanged.
Reference pages: `docs/phase-lock.md`, `docs/opi-zero2w.md`. Same protocol
as `2026-10-18-phase-lock-bench.md` (the Radxa run, session 0148).

## Setup

Bench Orange Pi Zero 2W (H618, one RTL8812EU card) on an image built from
master, with `maburgs` and `maburplay` swapped for the stage-7 glibc builds
of the phase-lock head (`tools/build-arm64.sh`, `out/arm64/maburgs-opi` and
`maburplay-opi`, both glibc-dynamic). Bench drone unchanged from the Radxa
run (RC_VERSION 16 build). Session 0000 (the board's session numbering
starts over on a fresh image), rung 4 (mcs4/40, ov 0.50), static scene,
GS pinned to channel 144 for the run.

The player config is the bundle's with the Orange Pi deltas
(`backend = "v4l2req"`, `dvr.mode = "raw"`, no `[input.rec]`): Cedrus
streamed decode, `vsync_lead_ms` 5, `chain_budget` 0, 1920x1080 at
60.000 Hz.

- **Arm U (unlocked):** `phase_lock = false`, `sched_cap_ms = 0`. 308 s.
- **Arm L (locked):** `phase_lock = true`, `phase_margin_ms = 4`,
  `sched_cap_ms = 16`, the bundle's values. maburplay restarted at the
  arm's start, so the window includes acquisition. 409 s.

Judder = `lat.log` seconds with p99 ≥ p50 + 10 ms. Never-shown = `au.log`
AUs with no `frame.log` row. Holes = `au.log` pts steps over 25 ms or a fid
gap.

## Result

| | Orange Pi | Radxa (0148) |
|---|---|---|
| U e2e p50 / p90 / p99 (ms) | 38.8 / 45.7 / 47.6 | 40.6 / 48.1 / 51.0 |
| U sawtooth | 31 → 47 (~100 s beat) | 33 → 51 |
| U judder s / never-shown /s | 1.0 % / 0.006 | 1 % / 0.003 |
| L e2e p50 / p90 / p99 (ms) | **33.2 / 35.7 / 37.4** | 35.4 / 36.3 / 37.0 (steady) |
| L judder s / never-shown /s | **0 % / 0.005** | 0 % / 0.003 |
| L acquire, share locked | 7 s, 98 % | 16 s, 97 % |
| L \|lerr\| locked p50 / p90 / max (ms) | 0.17 / 0.54 / 1.05 | 0.19 / 0.65 / 1.14 |
| L offset changes, centre | 57 /min, +11.6 mfps | 57 /min, +11.5 mfps |
| pts holes (both arms) | 0 | 0 |

- **Holding:** the lock never unlocked. In the locked window the
  regulator booked 0 chained frames, 0 cuts, 0 replaced and 0 pdrops. Hold
  in the natural slot p10/p50/p90 7.9 / 10.0 / 12.1 ms, the same in every
  50 s bucket.
- **The e2e drift is the link, not the lock.** e2e p50 went from 36 to
  32 ms over arm L (hence the wider p90). The hold stayed at 10.0 ms; the
  transit shrank (`fec` p50 7.1 → 4.0 ms, `air` 2.0 → 1.1 ms) and the
  allowance followed it (`alw` 9.5 → 7.5 ms), which moves the capture slot
  closer to the release. The Radxa run's transit was steady and so was its
  e2e.
- **Decode:** `dec` p50/p99 3.6 / 6.1 ms unlocked, 3.7 / 6.0 locked.
- **GS tail** (`t_complete − t_last`): p50 0.28-0.29 ms, p99 1.7 (U) /
  2.4 (L) ms.
- **ausniff**, both arms: 60.5 fps, 0 frame-id gaps, 0 resyncs, no
  incomplete AUs.

## Answer

Yes. With no Orange Pi-specific change, the lock acquires faster than on
the Radxa (7 s against 16 s), holds the same phase error and the same
offset cadence, and gives 33.2 ms p50 at 0 % judder seconds. The unlocked
stream is also ~2 ms lower than the Radxa's across the beat. The image the
board came with had no link key and no `/media/dvr` mount; both were added
by hand for the run, which the image build should carry.
