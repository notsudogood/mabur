# Why does the writer thread still cost live view a one-vblank repeat?

**Status:** fixed on branch `burn-async-sink`: `ThreadedSinkPacing` (the
writer trickles each fragment in 64 KB slices, 8 ms apart) plus
`display.rec_vsync_lead_ms` (the regulator's lead 5 -> 8 ms while the
burned recorder runs). Reference pages: `docs/colortrans.md` ("Keeping the
burned stage off the display path") and `docs/phase-lock.md`
("Configuration"). Continues
`docs/findings/2026-10-18-burned-dvr-fragment-write.md`, whose follow-up 1
this closes; its follow-ups 2-5 stay open.

## Question

With the fragment write moved to `mbr-dvrw` (build C of the 10-18 record),
the recording no longer loses a frame per fragment, but the live picture
repeats one frame about once per 45-90 s while recording, always 53-72 ms
after a fragment cut. The original build A never does. What is the
mechanism, and what removes it without bringing A's recorded loss back?

## Setup

Same bench as the 10-18 record: Radxa ZERO 3 GS, build C deployed, rung 4,
static scene, record presses by `kill -USR1`, session 0155 throughout
(one drone power-on). Every arm is 3 x 60 s recordings; a repeat is a pair
of consecutive `frame.log` rows 33 ms apart with consecutive `fid` (nothing
lost, one frame shown twice). Interleaved controls on the untouched build C
ran between the arms (2-4 repeats per 177 s each, 14 control arms in all).

Instruments added today: the kernel's per-thread schedstats
(`/proc/PID/task/TID/sched`, `wait_max` = worst runqueue wait), ftrace
(`sched_switch`/`sched_waking` with a monotonic trace clock lined up with
`frame.log`, and a `function_graph` of the writer's `write()`), and a
standalone writer (`wtest`, 2.5 MB fwrite+fflush once per 250 ms at nice
+10) to test the write in isolation.

## Findings

### 1. The write alone does not reproduce it

The standalone writer, with the player live but not recording, gave 2
repeats in 2520 whole 2.5 MB writes, 2 in 1200 writes of 64 KB slices
without pauses, and 0 in 1920 sliced writes with 1 ms pauses. Inside the
player the rate is 2.3 % of fragments. The recorder's own load is part of
the mechanism.

### 2. Three late points, one window

Breaking the repeat rows into segments: most are a late regulator release
(the main loop woke 3-6 ms late), a few are a commit ioctl of 3-7 ms (typical
0.35 ms), one or two are a commit issued on time whose flip latched a vblank
late, and under some tweaks the decode itself ran 7-13 ms (typical 4.5-6).
All fall in the write window.

### 3. What the trace shows

Two repeats inside a 120 s ftrace capture:

- The main loop's release timer fired while `kswapd0` (nice 0, woken by
  the write dirtying pages at the watermark) ran 9.2 ms uninterrupted on
  that CPU; the loop waited 7.5 ms. CFS gives a task that just woke from a
  long sleep a vruntime credit, so the loop's wake-up does not preempt it;
  only the tick ends its slice (9 ms with two runnable tasks).
- The main loop was runnable for 14.6 ms while the decoder parser, the
  encoder's `mpp_h265e` thread, the ct thread, maburgs's threads and
  kworkers took turns on every CPU.

The 30 s capture's worst runnable waits were 3.3 ms behind `mbr-dvrw`
itself (nice +10, in `write()`, reschedule pending the whole time) and 2-3 ms
behind nice-0 peers. The writer's `write()` is preemptible every page
(`cond_resched` 11 700 times in 12 s), so this is the scheduler declining to
preempt, not a missing preemption point. vfat with 512-byte blocks costs a
buffer head per sector: 58 747 `fat_get_block` calls for 8797 pages.

### 4. Nothing single-component moves it

Each of these ran live, 3 x 60 s on build C, against interleaved controls
(repeats per 177 s arm):

| Tweak | Repeats | Control |
|---|---|---|
| `min_free_kbytes` 5683 -> 65536 (no direct reclaim during the arm) | 3 | 4, 4 |
| recorder threads (encode, ct, `mpp_h265e`) at nice +10 | 3 | — |
| main loop `SCHED_FIFO` 10 | 19 (+36 skips) | — |
| `kswapd0` at nice +10 | 5, 2 | 3 |
| writer `SCHED_IDLE` | 2, 2 | 2 |
| main loop at nice -10 | 4, 3 | 4 |
| DDR governor `performance` (one DMC switch per recording, not per fragment) | 3, 2 | 3 |
| CPU governor `performance` (ondemand makes ~73 switches/s regardless) | 1 | 3 |

The main loop's `wait_max` under recording is 3.4-8 ms on A and C alike, so
a long wait by itself is not the trigger; the trigger is the write window,
where the recorder's whole pipeline runs concurrently with the write. In A
the write stalls the encoder for 22 ms (that is A's recorded loss), which
leaves the machine quiet for exactly that window.

### 5. Spreading the write halves it

Build D = C with the writer trickling: 64 KB per inner write, 8 ms sleep
between slices (~320 ms per 2.5 MB fragment, 8 MB/s), no sleeps while 4 MB
or more is queued or while the destructor drains.

| Arm (3 x 60 s) | Repeats | Skips | Main-loop `wait_max` (ms) |
|---|---|---|---|
| D | 0 / 0 / 0 | 0 / 1 / 0 | 5.8 / 5.8 / 3.3 |
| C (control) | 3 / 2 / 2 | 2 / 4 / 2 | 5.8 / 5.5 / 4.0 |
| D again | 3 / 1 / 1 | 2 / 1 / 0 | 9.3 / 4.1 / 5.8 |
| A (original) | 0 / 0 / 0 | 1 / 0 / 1 | 3.6 / 3.6 / 4.9 |

Recorded frames lost per 60 s file after the first 0.6 s (fMP4 `trun` pts
steps over 1.5 frames): D 1/1/1 and 3/2/0, C 1/5/2, A 50/51/47. The
writer thread's fix holds; the trickle leaves about half the live cost.

### 6. A longer lead while recording removes the rest

The residual misses are 3-6 ms late against a lead of 5 ms
(`display.vsync_lead_ms`; the commit lands 4.5 ms before the vblank, the
kernel's latch ~1 ms before it). Build D with the lead at 8 ms (config,
two arms):

| Arm (3 x 60 s) | Repeats | Skips | commit->flip typ. | e2e p50 | Recorded loss |
|---|---|---|---|---|---|
| D, lead 8 | 0 / 0 / 0 | 1 / 1 / 0 | 7.5 ms | 40-41 ms | 1 / 2 / 0 |
| D, lead 8 again | 0 / 0 / 0 | 1 / 0 / 0 | 7.5 ms | 41-42 ms | 4 / 1 / 4 |

The +3 ms of e2e is the lead, so the player applies it only while a
burned recording runs (`display.rec_vsync_lead_ms`, regulator
`set_lead_ms` at record start and stop; the phase lock re-steers the
sensor to the moved release). The final build (trickle + the switch), two
arms of 3 x 60 s: repeats 0 / 0 / 0 and 1 / 0 / 0, the one 2 s after a
record start with the decode itself 5 ms late (the lock still re-steering);
skips 0 / 2 / 1 and 0 / 1 / 1; commit->flip 7.5 ms while recording and
4.5 ms in the idle gaps between recordings (the lead goes back); recorded
loss 1 / 3 / 0 and 1 / 1 / 2 per 60 s file.

## Follow-ups

- Record start/stop freeze (10-18 follow-up 2), raw DVR mode's main-loop
  write (3), `ct_out` drop attribution (4), the start-up hole (5): unchanged.
- The CPU governor arm (1 repeat vs 3) and the decode-time shift under it
  (comp->dec 3.4 ms vs 5.5) suggest `ondemand`'s ~73 switches/s cost the
  display path margin in general, recording or not. Not pursued; a GS-image
  decision.
