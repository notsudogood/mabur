# Why does the burned DVR lose a frame every second on a clean link?

**Status:** fix built (branch `burn-async-sink`, `mabur::ThreadedSink`); the
reference page is `docs/colortrans.md` ("Keeping the burned stage off the
display path", "Not done / follow-ups"). Follow-up 1 is closed by
`docs/findings/2026-10-19-burned-dvr-live-repeat.md` (the trickle write);
follow-ups 2-5 are open.

## Question

Flight 0151's burned recording (`record-0040.mp4`) showed holes from its
first second on. Were they the drone, the link, or the GS? And did live view
lose frames too?

## Setup

- **Flight 0151:** Radxa ZERO 3 GS, `dvr.mode = "burned"`, `bitrate_kbps`
  20000, `fps_cap` 60, `fragment_ms` 1000, record button. 60 fps drone on a
  60 Hz panel, phase lock on.
- **Bench:** the same GS config and drone, rung 4 (mcs4/40), static scene.
  Record presses come from `kill -USR1 $(pidof maburplay)`.
  - Session 0152: 3 × 20 s recordings on the deployed player.
  - Session 0153: A/B, 3 × 30 s per arm.
  - Session 0154: A/B/C, 3 × 60 s per arm, one drone power-on.
- **Builds:** A = the deployed maburplay (dd281b34; no player, common or GS
  code differs from master 5d642c0d). B = A + `ThreadedSink`. C = B + the
  writer at nice +10 (what the branch commits).
- **Method:** every burned mp4 sample is stamped with the drone's capture
  pts, so each sample maps exactly to an `au.log` row. In 0151 all 19977
  samples matched. Each frame missing from a file is then classified by
  where it stopped:
  - not in `au.log` (it never reached the GS);
  - in `au.log` but not decodable;
  - decoded but no `frame.log` row (not shown live);
  - shown live but not recorded.

## Findings

### 1. The drone and the link are not the cause

In 0151's clean seconds (119 s), and on every bench run, there are no holes
in the drone's capture timestamps and no frames missing from `au.log`.
All of the file's holes are on the GS.

### 2. Record start and stop freeze the live picture

`record-0040.mp4` is 343.078 s long. The two player-side freezes of 0151
were 343.08 s apart: the moments the recording started and stopped.

| | 0151 (flight) | Bench (0152–0154) |
|---|---|---|
| **Start** | ~180 ms on glass; 13 frames never shown; decoder output stalled ~230 ms | 67–133 ms |
| **Stop** | ~85 ms; 4 frames never shown | a steady 83–117 ms |

`rec_start`/`rec_stop` run `BurnRecorder::start()` (MppEncoder init,
palette upload) and `stop()` (thread joins, encoder teardown, mux close)
synchronously on the main loop.

The recording also loses its first 0.2–0.5 s (10–45 frames). The GPU
stage's first frame takes 140–480 ms (`ct_ms` max) while the stage starts
up. Out of scope here: the operator does not mind the start.

### 3. The once-a-second loss is the fragment write on the encode thread

After the start, the recording loses about one frame per second. Each lost
frame was shown live and arrived whole. Evidence:

- **Drop position:** 38 of the 39 steady-state drops in 0152's files sit
  exactly one sample after an fMP4 fragment start. Nine of those starts are
  not keyframes, so it is the fragment, not the keyframe.
- **Cost of the write:** `DvrMux::flush_fragment` writes the closing
  fragment (~2.5 MB at 20 Mbit/s) with `fwrite` + `fflush` inside
  `encode()`'s packet callback, on the encode thread. The thread's budget is
  16.7 ms and the encode alone averages 14.1 ms.
- **The slot:** the frame waiting in the one-slot `ct_out` handoff is
  overwritten by the next one, which counts as `frames_dropped`.

Timing a 2.5 MB fwrite+fflush once a second on the idle GS:

| Target | Median | Max | Writes over 16.7 ms |
|---|---|---|---|
| `/media/dvr` (vfat SD) | 21.6 ms | 24.3 ms | 19 of 20 |
| tmpfs | 8.5 ms | 15 ms | 0 of 10 |

Encode plus write is 36–40 ms, which matches the recorder's `enc_ms` max of
38–42 ms (that timer wraps `encode()`, mux callback included). Even a write
to tmpfs would sometimes overrun, so the problem is the shared thread, not
the card.

### 4. Moving the write to a thread fixes the recording

`ThreadedSink` wraps the file sink. The mux still serialises on the encode
thread; the sink copies the bytes into a queue capped at 32 MB, and a writer
thread (`mbr-dvrw`) does the file I/O. Session 0154, each arm 3 × 60 s:

| | A: original | B: writer thread | C: writer at nice +10 |
|---|---|---|---|
| Recorded frames lost / 60 s (after 0.6 s) | **51 / 48 / 47** | 2 / 3 / 2 | **0 / 1 / 6** (the 6 is during a link demote) |
| `dropped` (recorder counter) / 60 s | 54–59 | — | — |
| Live, recording: one-vblank repeats, nothing lost | **0** | 4 | **2** |
| Live, recording: late-arrival skips | 2 | 4 | 1 |
| Live, idle: any gap | 0 | 0 | 0 |
| Live e2e p50, recording | 37.7 ms | 38.1 ms | 37.7 ms |
| Stop freeze | 83–100 ms | 83–117 ms | 100 ms |
| File integrity | pts strictly increasing, no duplicates | same | same |

The 0153 A/B (3 × 30 s per arm) agrees:
- recorder `dropped`: 33/29/30 per file (A) → 8/9/8 (B);
- steady-state file holes: 23–25 → 0;
- `enc_ms` max: 38.5–40.6 ms (A) → 27.0–27.5 ms (B), one file at 38.9 ms.

The `ct_out` handoff still drops 8–9 frames per 30 s in B. Those frames
were displaced, not lost to a fragment write, since the files themselves
show 0 steady-state holes. A per-site drop counter would place them.

### 5. The threaded write costs live view a rare one-vblank repeat

B and C show a live event that A never does across 264 s of recording. A
frame arrives on time and decodes on time, but the main loop releases it
~5 ms late. It misses the latch by under 1 ms (flip − commit ≈ 15–17 ms
instead of ~4.5) and shows one vblank late, so the screen repeats a frame.
Nothing is lost.

- **Timing:** all 12 occurrences (0153 B, 0154 B, 0154 C) fall 53–72 ms
  after the fragment-cut frame reached the GS. That is the writer's window:
  the cut frame is muxed ~35 ms after arrival, then the ~22 ms write runs.
- **Lower priority halves it:** 4 occurrences in B to 2 in C, each over
  177 s.
- **Not the free-space check:** the player's 1 Hz `statvfs` of the card
  stays at p50 0.17 ms, max 0.57 ms, while another thread writes
  2.5 MB/s to the card. Its phase against the events also varies.
- **Working hypothesis, untested:** the GS kernel is `PREEMPT_VOLUNTARY` at
  HZ=250, and the page cache is full (82 MB free of 1979 MB, so each write
  also reclaims pages). A writer inside `write()` cannot be preempted at
  any nice value. With the encode thread, the GPU stage and maburgs all
  runnable during the write, the main loop's wake-up can slip by about one
  4 ms tick. In A the encode thread itself was the writer, so the pipeline
  was stalled during the write and one fewer thread competed.

Net: about one one-vblank repeat per 90 s of recording, against ~48 recorded
frames lost per minute before.

## Follow-ups

1. **Spread the fragment write (next).** Write each fragment in small slices
   (e.g. 64 KB) with a short pause between them. There is a full second
   before the next fragment, so a ~50 ms trickle is free and keeps each
   syscall short. Gate: 0 one-vblank repeats while recording, against A, on
   the same 3 × 60 s A/B protocol.
2. **Record start/stop off the main loop.** Build the recorder (encoder
   init, palette) and tear it down (joins, encoder deinit, mux close) on a
   helper thread, so a press costs the live picture nothing.
3. **Raw DVR mode has the same write on the main loop.** `RawDvr::feed` runs
   on the main loop (`gs/player/src/main.cpp`), and its `DvrMux` writes each
   fragment synchronously through the plain file sink. At the drone's
   bitrate that is a ~20 ms main-loop block per fragment, a likely live
   stutter. Unmeasured; raw is not the bundle mode. `ThreadedSink` drops in
   the same way.
4. **Place the remaining `ct_out` drops.** Count each drop site on its own
   (box, `ct_out`, dead path), so the 8–9 per 30 s in B can be attributed.
5. **Recorder start-up hole.** The first 0.2–0.5 s of each file is lost to
   the GPU stage's first frame (140–480 ms).
