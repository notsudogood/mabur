# Release thread bench gate: lead 3 does not hold, and the misses are below the player

**Status:** open — lead 3 failed this gate, and at the single lead of 5 the
burned recorder still costs a missed latch about every 2.5 min at 1080p
(the separate recording lead of 8 this branch removed used to absorb it).
Reference: `docs/phase-lock.md` ("Threads", "Configuration"),
`docs/observability.md` (reg.log `rlate=`/`hand=`/`flushes=`/`recstarts=`).

Bench, 2026-10-21, branch `release-thread` (head 6089db47): the player's
deadline work runs on a `SCHED_FIFO` release thread (priority 10, confirmed
at startup and `rt=1` on every reg.log line), decode output on its own
thread, everything else on the housekeeping loop. Radxa GS, desktop monitor.
Each window was read with `tools/bench/latchgate.py`; recording is the
burned DVR (`dvr.mode = "burned"`, toggled with `SIGUSR1`). A miss is a
frame whose commit latched a vblank late; `pend` is the presenter's
mailbox-engagement counter (the authoritative count, see the frame.log note
below).

## Results

| Drone | Panel | Lead | Mode | Window | Frames | pend | Verdict |
|---|---|---|---|---|---|---|---|
| 1280x720@60 | 1920x1080@60 | 5 | idle | 10 min | 36009 | +0 | pass |
| 1280x720@60 | 1920x1080@60 | 5 | rec | 10 min | 36203 | +0 | pass |
| 1280x720@60 | 1920x1080@60 | 3 | idle | 7.5 min (drone powered off mid-window) | 27014 | +0 | pass (short) |
| 1920x1080@60, 24 Mbit/s | 1920x1080@60 | 3 | idle | 5 min | 18001 | +2 | fail |
| 1920x1080@60, 24 Mbit/s | 1920x1080@60 | 3 | rec | 5 min | 17998 | +5 | fail |
| 1920x1080@60, 24 Mbit/s | 1920x1080@60 | 4 | idle | 5 min | 17993 | +0 | pass |
| 1920x1080@60, 24 Mbit/s | 1920x1080@60 | 4 | rec | 5 min | 18010 | +4 | fail |
| 1920x1080@60, 24 Mbit/s | 1920x1080@60 | 5 | idle | 5 min | 18009 | +0 | pass |
| 1920x1080@60, 24 Mbit/s | 1920x1080@60 | 5 | rec | 5 min | 15128 rows | +2 | fail |
| 1728x972@100 | 2560x1440@99.95 | 3 | idle | 5 min | 29971 | +2 | fail |
| 1728x972@100 | 2560x1440@99.95 | 3 | rec (burn at 2560x1440) | 5 min | 29872 | +77 | fail |

`dsp` p50 tracks the lead as designed: 4.8 ms at lead 5, 3.9 at 4, 2.8 at
3. Two 1080p windows run with the encoder stuck near 4 Mbit/s are not in the
table. The DVR card was found 100 % full right after the 1080p windows; how
full it was during each of them is unknown, so the 1080p recording rows may
carry some SD-card back-pressure. The 100 fps windows ran after 35 GB were
freed.

## Every miss has the same shape, and the release thread is not it

- Release lateness (`rlate`, now minus the scheduled release at the moment
  of release) stayed under 1.6 ms in every 60 fps window; the per-second
  worst case had a median of 0.65-0.69 ms idle and 0.30 ms while recording
  (busier cores wake faster, which points at CPU-idle exit for the idle
  tail). At 100 fps with the 1440p recorder it reached 2.4 ms.
- On every missed frame at 60 fps the release was on time and the commit
  ioctl returned 0.26-0.58 ms after it, so the commit reached the kernel
  2.4-4.5 ms before its vblank — and still latched one period late
  (`t_flip − t_commit` 19.0-21.2 ms at 60 Hz, 12.6 ms at 99.95 Hz).
- Clean frames latch `t_flip − t_commit` p50 2.56 ms (p99 2.72, max 2.83) at
  lead 3 and p50 3.55 ms (max 3.83) at lead 4: a commit lands about one lead
  before its vblank. The misses at lead 4 and 5 had more margin than any
  clean lead-3 frame needed, so the stall is an occasional kernel-side delay
  longer than the lead (the nonblocking commit worker runs at normal
  priority; a buffer fence wait is the other candidate), not a margin that
  is merely tight.
- With the 2560x1440 burned recorder at 100 fps the commit ioctl itself
  slows (release → commit p50 0.97 ms, max 1.6 ms on the missed frames) on
  top of the kernel latch slipping: the heavy recorder case of
  `2026-10-20` (the hardware encoder saturated at the panel size).

## Other things the bench found

- The drone's SD recorder refuses a `record.size` larger than the VPE
  capture ("`[record] 1920x1080 exceeds the VPE capture 1728x972 --
  recorder disabled`"); `docs/camera-modes.md` describes the same case as an
  upscale. The page and the code disagree.
- A full DVR card stops every per-session debug log silently (reg.log froze
  at 851968 bytes) and the burned recorder fails; the player keeps playing.
  The window then reads no frames at all.
- While the burned recorder writes to the same card, frame.log loses rows
  (15128 rows in a 300 s window with `present=60` every second), so
  latchgate's miss count under-reads during recording; `pend` does not.
- `present_jitter` on the regulator line reads hundreds of seconds after a
  long video gap: the EMA takes the gap as one interval.

## What would move it

Not the release thread: it is on time when the kernel is not. Candidates
below the player: the DRM commit worker's scheduling class, the recorder's
load on the display and memory path (its encode size follows the panel), and
whether the commit waits on an implicit fence. Until one of those moves,
lead 5 holds at idle and fails roughly once per 2.5 min while the burned
recorder runs at 1080p; the separate recording lead that this branch
removes was the previous answer to the same load.
