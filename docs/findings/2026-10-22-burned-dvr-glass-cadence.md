# Burned DVR on the glass's cadence: bench gate

**Status:** 1080p60 passes every criterion; the 1440p100 window passes cadence
and drops but not latency (recording still costs +3.5 ms e2e there).
Reference: `docs/observability.md` (burned DVR, reg.log `burn_in=`/`burn_enc=`/
`burn_drop=`, `tools/bench/dvrcadence.py`), `docs/colortrans.md` (the stage).

Bench, 2026-10-22, branch `burned-glass` (head b7b5107f). The recorder is
tapped at the presenter's flip on the release thread (`RecorderTap` →
`BurnRecorder::submit`), stamped on the vblank grid × `dvr.burned.every`
(`glass_cadence.h`), and encodes at `dvr.burned.size` (`""` here, so the
decoded size) with the OSD composed at that size. Radxa GS, desktop monitor
(1080p60 and 2560x1440@99.946 modes), `colortrans.enable = true`, burned
bitrate 20000 kbps, `vsync_lead_ms = 5`, `rt_priority = 10`, `dvr.target =
"gs"` (the drone's SD recorder off, so only the GS recorder loads the
system), debug-log sessions on `/media/dvr/log`. Each window is 5 min of
reg.log/frame.log, starting 30 s after a player restart; "unrecorded" and
"recorded" differ only in `dvr.autostart`.

## Results

| Window | Panel | Drone | Record | e2e p50/p99 ms | dec p50/p99 ms | misses | burn_drop (steady) | off-grid |
|---|---|---|---|---|---|---|---|---|
| 1 unrecorded | 1920x1080@60 | 1920x1080@60 | — | 34.73 / 35.87 | 2.80 / 4.38 | 0 | — | — |
| 1 recorded | 1920x1080@60 | 1920x1080@60 | 1920x1080, every 1, 60 fps | 34.88 / 36.73 | 3.10 / 5.21 | 0 | +0 | 0 |
| 2 unrecorded | 2560x1440@99.946 | 1728x972@100 | — | 30.91 / 32.41 | 2.53 / 3.90 | 0 | — | — |
| 2 recorded | 2560x1440@99.946 | 1728x972@100 | 1728x972, every 2, 50 fps | 34.42 / 38.75 | 3.79 / 7.39 | 1 | +0 | 0 |

`latchgate.py` over the same windows: pend +0 / pdrop +0 in window 1 both
ways; pend +1 / pdrop +1 in window 2 recorded (the one miss). `rlate`
max 1.2-1.4 ms recorded vs 0.7-1.2 ms unrecorded.

Gate (from the design): window 1 needs `burn_drop` +0, 0 off-grid samples,
holds only where the glass held, and misses no worse than unrecorded:
**pass**. Window 2 needs an exact 50 fps grid, `burn_drop` +0, e2e p50
within 1.5 ms of unrecorded and `dec` p50 within 0.5 ms: grid and drops
**pass**, e2e (+3.5 ms) and `dec` (+1.26 ms) **fail**. OSD by eye
(frames extracted at 8 s from both files): MSP grid, GS stats bar and REC
field all present and sharp at 1728x972 and at 1920x1080, laid out for the
record size rather than scaled from the panel.

## The files

`dvrcadence.py` on the two recordings (run on the GS):

- 1080p60, 21084 samples: grid 16666.667 µs inferred, `x1=21080 x2=2`, one
  36-unit hold, 0 off-grid.
- 1440p100 / every 2, 16570 samples: grid 20010.719 µs inferred (2 × the
  mode's period), `x1=16565 x3=3`, one 25-unit hold, 0 off-grid.

The long hold is the file's first sample in both: the GPU colortrans stage's
first frame takes ~0.5 s (`ct_ms` max 508 and 536 ms on the stop line), the
displaced-list cap refuses the flips meanwhile, and that is every one of the
36 and 26 drops (`burn_drop` reached them within a second of the start and
never moved again: `in=21120 encoded=21084 dropped=36` and `in=16596
encoded=16570 dropped=26`). The recording therefore opens on a frame held
for ~0.5 s. Warming the stage before the first flip would remove it.

Thinning is exact: in window 2 `burn_in` climbed by exactly 3000 per minute
(50.0 per second) for the whole run.

## Start without video

With `autostart = true` and `size = ""` the recorder starts before the first
decoded frame: the log shows one `maburplay: rec: START deferred -- waiting
for the first decoded frame` and no refusal, then `BurnRecorder: start
1728x972 every=2 fps=50 period=10.005ms osd=1728x972 ct=on` once video
arrives (and `1920x1080 every=1 fps=60 period=16.667ms` in window 1).

## Reading it

- At 1080p60 the recorder now costs the live feed nothing measurable:
  +0.15 ms e2e p50, no missed latches in 5 min at lead 5. The release-thread
  gate (`2026-10-21-release-thread-gate.md`) saw a miss every 1-2.5 min
  recording at that lead with the decode-thread tap and a 24 Mbit/s drone.
  One 5-min window is not enough to call that fixed.
- At 1440p100 the encode is down from the panel's 2560x1440 to 1728x972 and
  thinned to 50 fps, and the cost dropped from the ~+9 ms measured while
  it encoded at 2560x1440 (an earlier bench with no findings page) to
  +3.5 ms, but `dec` still slows (p99 3.9 → 7.4 ms) and the regulator's allowance follows it (reg p50 9.7 → 11.9 ms).
  The same memory-bus contention as before, reduced, not gone. Not tried
  here: colortrans off for the recording, `every = 4`, a smaller `size`.
