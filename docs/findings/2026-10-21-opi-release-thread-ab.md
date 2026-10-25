# Orange Pi: the release thread, the v4l2req blocking poll, and lead 4

**Status:** closed — the branch runs on the Orange Pi unchanged apart from
`V4l2ReqBackend` implementing `PollWait`; lead 4 holds there with no
burned recorder to disturb it, lead 3 does not (the same kernel-side late
latch as the Radxa), and the board drives the 2560x1440@99.95 panel mode
with the drone at 100 fps. Reference: `docs/opi-zero2w.md` ("Decode
path"), `docs/phase-lock.md` ("Threads", "Configuration"),
`docs/findings/2026-10-21-release-thread-gate.md` (the Radxa gate).

Bench, 2026-10-21, Orange Pi Zero 2W GS on the 1080p60 desktop monitor,
`backend = "v4l2req"`, `stream = true`, raw DVR, one radio card, channel
pinned, drone at 1920x1080@60. Four player builds in turn, same maburgs
(unchanged between them), one debug-log session; every window is 5 min
read with `tools/bench/latchgate.py` and per-frame percentiles over
`frame.log` (`dec = t_decoded − t_complete`, `dsp`, `e2e`). The lock was
let settle 60 s after each player restart before the window opened. A
miss is a frame whose commit latched a vblank late.

## Results

| Arm | Build | Lead | Frames | Misses | dec p50 / p99 | reg p50 | dsp p50 | e2e p50 / p99 | latch max | rlate max |
|---|---|---|---|---|---|---|---|---|---|---|
| C | before the release thread (the fps100 base) | 5 | 18000 | 0 | 2.94 / 5.42 | 10.06 | 4.93 | 32.81 / 34.16 | 4.87 | — |
| A | release thread, no PollWait on v4l2req | 5 | 17999 | 0 | 2.15 / 4.03 | 9.46 | 4.96 | 31.05 / 33.87 | 4.91 | 0.12 |
| B | release thread + `V4l2ReqBackend::PollWait` | 5 | 17999 | 0 | 1.65 / 3.64 | 9.34 | 4.96 | 30.65 / 33.39 | 4.88 | 0.11 |
| A again | as A, after B and C | 5 | 17996 | 0 | 2.14 / 4.02 | 9.63 | 4.96 | 31.81 / 34.67 | 4.88 | 0.14 |
| B | as B | 4 | 17999 | 0 | 1.75 / 3.65 | 9.25 | 3.96 | 29.82 / 32.59 | 3.87 | 0.19 |
| B | as B | 3 | 17994 | 2 | 1.69 / 3.28 | 9.16 | 2.96 | 28.71 / 31.13 | 19.43 | 0.11 |
| B, 2560x1440@99.946, drone 1728x972@100 | as B | 4 | 28560 | 2 | 3.60 / 5.61 | 9.91 | 3.93 | 30.22 / 37.07 | 13.64 | 0.27 |

All times ms. `latch` is `t_flip − t_commit`; `rlate` is the release
thread's lateness (reg.log, max over the window).

## What it says

- **The release thread takes 0.8 ms off `dec` and about 1.8 ms off e2e
  on this board** (C → A). Before it, the housekeeping loop collected
  decoded frames between its other work; the decode-output thread
  collects them as they land.
- **The blocking poll takes another 0.5 ms off `dec`** (A → B), the
  expected half of the pump's 1 ms fallback sleep: without `PollWait`
  the pump woke every 1 ms and a decoded picture waited up to that long
  in the handoff's ready queue.
- **The repeat A window reproduces A's `dec` to 0.01 ms**, so the
  decode-side differences are the builds, not drift. e2e moved 0.8 ms
  between the two A windows: that is link transit (`reg` and the
  allowance follow it), the reason `dec` is the figure to compare.
- **Lead 4 holds**: zero misses in 5 min, every commit 3.79-3.87 ms
  ahead of its vblank, the release thread never later than 0.19 ms.
  The Radxa's lead-4 failure is under the burned recorder, which this
  board does not have, so the OPi gets the lower lead for free.
- **Lead 3 does not hold**: 2 misses in 5 min. Both have the Radxa gate's
  shape — release on time (`rlate` max 0.11 ms), commit returned 0.20 ms
  after the release, the commit in the kernel 2.8 ms before its vblank,
  and the latch one period late (19.4 ms). Kernel side, as on the Radxa.
- **2560x1440 at 99.946 Hz scans out from the H618** with the drone at
  1728x972@100: real vblank events every 10.008 ms (p1 9.989, p99 10.027),
  the lock acquired and held, `dec` p50 3.6 ms (Cedrus on 100 fps of
  1728x972, p99 5.6 ms, under the 10 ms period), e2e p50 30.2 ms. Two
  misses at lead 4 in 5 min (pend +2), and 28560 rows of a 30000-frame
  window: the regulator at 100 fps drops or replaces about 5 % of frames
  on this link, as the Radxa does at that rate. Whether the picture is
  right was not checked by eye; the kernel accepted and flipped the mode.
- **The release thread's `SCHED_FIFO` request is granted** on the image
  (`rt=1` on every reg.log line).

## Trap: the OPi's debug log fills the root overlay

The OPi image has no `/media/dvr` mount, so `/media/dvr/log` lives on the
root overlay (185 MB). A session at 60 fps writes about 3 MB/min
(`au.log` and `frame.log` most of it); the first lead-3 window ran with
the overlay at 100 % and logged nothing, silently — `lat.log`,
`frame.log` and maburgs's own files just stop, and the player keeps
playing. Copy sessions off and delete them, and leave `debug_log.enable
= false` on this board between benches.

## Not covered

- 1080p at 100 or 120 Hz: the monitor offers no such mode, so 100 fps on
  this board was only run at 2560x1440 (the DE33 scaler upscaling
  1728x972).
- A by-eye check of the 1440p100 picture.
