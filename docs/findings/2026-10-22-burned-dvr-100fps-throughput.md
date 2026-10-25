# Burned DVR at 100 fps: flights 0209/0210 and the stage throughput bench

**Status:** current limitation, documented in `docs/colortrans.md` ("Known
limitations"). At a 100 Hz panel the burned recorder cannot record every
vblank, and its colour stage costs live view ~4 ms; `dvr.burned.every = 2`
is the working setting.

Branch `burned-glass` (head 065ee1a6), Radxa GS, `dvr.burned.size = ""`,
burned bitrate 20000 kbps, `dvr.burned.every = 1`, `dvr.target = "both"`,
`colortrans.enable = true` unless stated, `vsync_lead_ms = 5`. Record
toggled mid-flight with the button (flights) or `kill -USR1` (bench).

## Flights

| | 0209 | 0210 |
|---|---|---|
| drone | 1728x972@100 | 1920x1080@60 |
| panel | goggles 1920x1080@100.000 | 1920x1080@60 |
| recording | 303.7–390.7 s (87 s) | 223.4–355.5 s (132 s) |

Per-frame segments from `frame.log`, p50 (ms), rows while the phase lock
held, before / during / after the recording:

| | 0209 (100 fps) | 0210 (60 fps) |
|---|---|---|
| e2e | 33.9 / 37.5 / 32.8 | 38.0 / 37.5 / 36.9 |
| e2e p99 | 35.5 / 40.1 / 43.4 | 39.3 / 53.4 / 53.2 |
| dec | 2.3 / 3.7 / 2.3 | 2.6 / 3.1 / 2.6 |
| dec p99 | 3.7 / 7.7 / 3.6 | 4.0 / 5.5 / 4.2 |
| fec | 4.6 / 5.4 / 4.5 | 7.4 / 7.7 / 7.4 |
| enc, air, dq, dsp | flat | flat |

- At 100 fps the cost steps exactly at the record edges (5 s medians: `dec`
  2.3 → 3.7 ms at the press, 3.8 → 2.4 at the stop; `fec` +~1 ms with
  `air` flat, so maburgs slows under the GS load, not the link). The
  regulator's allowance follows the wider transit tail, which is how the
  decode spread reaches e2e.
- At 60 fps the recording costs nothing measurable at p50 (`dec` +0.5 ms).
  The p99 rows in 0210 are link-loss bursts, present outside the
  recording too.
- 0209's recorder at `every = 1`: `burn_in` +100/s, `burn_enc` +75–81/s,
  `burn_drop` +19–24/s for the whole recording (1825 of 8670, 21 %),
  steady from the first second on: a throughput ceiling, not bursts.
  0210: 31 drops, all in the first second (the GPU stage's first frame).
- 0 `pend`/`pdrop` while flying in either session, recording or not.

### Phase lock in the two flights

- The sensor rate followed the panel to ~0.01 fps at both rates (pts step
  vs flip step, 20 s buckets).
- |lerr| while locked, p50: 0.3 ms at 60 fps; 0.6–0.7 ms at 100 fps
  outside the recording, 1.66 ms during it (the offset reached the +200
  mfps clamp, one 10 s unlock at 356 s).
- Every unlock except the drone restart in 0210 (pts restarted at ~5 s
  after 66 s of silence) follows a step in the regulator's allowance: a
  loss burst (`lslot` +8..13) pins `alw` at the 16 ms cap for the 600-frame
  window, the target moves 4–6 ms, the lock reads it as phase error and
  drops to `acq` for 6–10 s with capture placement off (`chained` +50..98/s,
  `reg` +5 ms); the step back down when the burst ages out can unlock it
  again (0210 at 422 s). 0209's 356 s unlock was `alw` 13 → 10 ms about
  6 s after a rung change. This is the open allowance-step item from
  `2026-10-19-configurable-fps-bench.md`, seen in flight.
- `PhaseLock::kSlopeMsPerSecPerMfps` is 1/60 (the physics at 60 fps); at
  100 fps the true value is 1/100, so the slope-to-offset estimate is 0.6×
  and the proportional loop's time constant 3.3 s instead of 2 s. The
  first acquire took 23 s at 100 fps against 10–17 s at 60 — consistent
  with that, not proven by it.

## Bench: which stage caps the recorder

Drone 1728x972@100, GS desktop monitor at 2560x1440@99.946 (no goggles on
the bench; the monitor has no 1080p@100 mode, so the scanout carries more
memory traffic than in flight). 30 s idle, 90 s recording, `BurnRecorder:
stopped` line for the per-stage means (`ct_ms` = GPU draw + RGA on the ct
thread, `enc_ms` = the `encode()` call on the recorder thread), lat lines
averaged per second.

| run | encoded / in | ct_ms mean | enc_ms mean | e2e p50 idle → rec | dec p50 idle → rec |
|---|---|---|---|---|---|
| colortrans on | 5983 / 9029 (66 fps) | 14.9 (draw+finish 9.5, rga 5.3) | 11.8 | 29.9 → 34.4 | 2.0 → 4.0 |
| colortrans off | 7744 / 9028 (86 fps) | — | 11.4 | 29.7 → 29.7 | 2.0 → 2.2 |
| colortrans off, rkvenc governor `performance` (400 MHz) | 9010 / 9014 | — | 9.2 | 31.0 → 31.1 | 2.2 → 2.1 |

- The period is 10.0 ms; the two stages overlap, so the recorder runs at
  the slower one and the latest-wins mailboxes drop the rest.
- The colour stage (14.9 ms) is the first cap and the whole live-latency
  cost: with it on, `rkvdec` utilisation goes 38 → 53 % at the same
  100 fps (memory bus), and the lock spent 22 of 92 rows in `acq`; with it
  off, e2e and `dec` do not move and the lock held throughout.
- The encoder alone is the second cap: 11.4 ms at 297 MHz (rkvenc ~62 %
  utilised at 66 fps in the first run), because `venc_ondemand` never
  raises the clock. Pinned at 400 MHz it takes 9.2 ms and records every
  vblank.
- The bench drops more than flight 0209 (66 vs 79 fps encoded) — the
  1440p scanout's extra memory traffic.

Files: GS `record-0183` (colortrans on), `record-0184` (off), `record-0185`
(off, 400 MHz).

## Not done

- Nothing changed in code. Options left on the table: `every = 2` at 100 Hz
  (works today, still ~+3.5 ms live), recording flat with the encoder
  clock pinned while recording, or cutting the stage (BGRA straight to the
  encoder, no RGA pass — the GPU pass alone is 9.5 ms, still borderline).
- The phase lock's allowance coupling and its 60 fps gain constant.
