# The VTX SD recorder's link cost is the second encode, not the writer — 2026-10-25

**Status:** measured, not fixed. Supersedes the writer-side mechanism in
`docs/findings/2026-10-24-vtx-recorder-latency.md`: the card, the page
cache, the fsync and the writer's placement are innocent. The cost is the
record channel's encode itself and scales with the pixels it encodes per
second. Current reference → `docs/vtx-recorder.md` "Link latency cost".

**Question.** The 2026-10-24 record attributed the recorder's +2 ms (bench)
/ +4 ms (flight) glass latency to the writer's ~5 MB/s of vfat writes on
core 0, and listed levers: writer placement and priority, writeback
tuning, fewer fsyncs, lower bitrate. Which lever removes the cost at the
bundle's 40 Mb/s?

**Answer.** None of them, because the writer is not the cost. Recording to
a sink that discards every byte (no card, no page cache, no fsync) costs
the link the same +1–2 ms as recording to the card, and so does recording
at 5 Mb/s. Halving the recorder's frame rate removes about 40 % of the
cost; recording 1280x720 instead of 1920x1080 removes about 60–70 %. While
the record channel runs, every thread of `maburd` — the TX writer, the USB
threads, the FEC worker, the frame producer alone on core 1, the encoder
thread — spends 30–45 % more CPU on the same work, which is the signature
of memory-system contention from the engine's second 1080p60 encode, not
of scheduling. The 2026-10-24 record's core-0 numbers were real; its
reading of them was wrong.

## Method

Same bench as the 2026-10-24 record: drone on the bundle config (1920x1080
at 60 fps, `[record]` 1920x1080 60 fps 40 Mb/s), GS at 1920x1080@60 with
the phase lock on and `sched_cap_ms` 16, rung 5, maburplay from a temporary
copy of its config with `dvr.target = "vtx"`, so `kill -USR1` drove only
the drone's recorder. Each variant is one 50 s window with the recorder on
followed by one with it off, 12 s settle after each toggle; most variants
were run twice. Per-frame segments and the allowance from the GS session's
`frame.log` and `reg.log` (sessions 0240–0246); drone per-core `/proc/stat`
every 5 s and a per-task tick snapshot at each window edge. The scene
bitrate drifted 8–24 Mb/s across the afternoon, so only adjacent on/off
pairs are compared, and the noise floor of a pair is about 0.5 ms (a pair
with the recorder stopped by an error measured +0.5 and 0.0).

Variants with the stock binary needed no rebuild: a small static helper set
a thread's affinity or scheduling class by name, and the sysctls and
`/proc/irq` masks were written directly. Two bench binaries were built from
the day's branch: one wrapping the recorder's file in `mabur::ThreadedSink`
(the GS burned DVR's paced writer, 64 KiB per 4 ms), and one that, under an
environment variable, replaces the file with a sink whose write, flush and
sync return true without doing anything. Neither is in the tree.

## Results

e2e p50 added by the recorder, each entry one on/off pair:

| recording | sink | lever | cost (ms) |
|---|---|---|---|
| 1080p60 40 Mb/s | card | stock placement | +0.7, +1.4, +2.8 |
| 1080p60 40 Mb/s | card | writer threads on core 1 | +2.3 |
| 1080p60 40 Mb/s | card | writer threads SCHED_IDLE | +1.9 |
| 1080p60 40 Mb/s | card | SD irq + writeback workqueue on core 1 | +0.6, +3.9 (with dirty tuning) |
| 1080p60 40 Mb/s | card | dirty_background 1 MB, dirty 8 MB, 1 s writeback | +0.2, +0.9, +2.2 |
| 1080p60 40 Mb/s | card | paced ThreadedSink writer | +2.6, +2.4 |
| 1080p60 40 Mb/s | **null** | — | **+2.0, +1.1** |
| 1080p60 5 Mb/s | null | — | +2.2, +1.0 |
| 1080p30 40 Mb/s | null | — | +1.5, +0.9 |
| 720p60 40 Mb/s | null | — | +0.9, +0.3 |
| none (recorder in error) | — | — | +0.5, 0.0 |

The allowance (`alw`, transit p99 − p1 over 600 frames) carries most of
the cost in every recording window (+0.5 to +2.0 ms), `reg` p50 follows
it, and `enc`, `fec` and `air` p50 each add 0.2–0.7 ms. Writer-side
variants scatter inside the drift of the scene bitrate; the null sink
sits in the middle of them.

Side effects seen: the paced writer dropped one AU in two of three
recordings (its per-fragment sync waits for the trickle plus the fsync,
and that exceeded the recorder's 1 s AU queue), and moving the writer
threads onto the producer's core was the worst placement, as the core
policy's comment in `drone/src/main.cpp` predicts.

## Where the CPU goes

Per-task ticks per 20 s, 1080p60 at 40 Mb/s with the null sink, recording
vs idle at the same ~23 Mb/s link rate:

| thread | idle | recording | |
|---|---|---|---|
| `mbr-usb` (4) | 334 | 485 | +45 % |
| `mbr-hot` (core 1, alone) | 313 | 440 | +41 % |
| `mbr-fecw` | 231 | 303 | +31 % |
| `venc-enc` | 133 | 188 | +41 % |
| `mbr-txw` | 95 | 143 | +51 % |
| `maburd` (SDK threads) | 126 | 165 | +31 % |
| `venc0_P0_MAIN` (MI) | 35 | 65 | |
| `mbr-recdrain` + `mbr-rec` | 0 | 63 | |
| all tasks | 1554 | 2018 | +30 % |

The recorder's own threads and the MI venc thread explain about 90 of the
460 extra ticks. The rest is every existing thread running slower on
unchanged work, on both cores, with no change in context switches,
reschedule IPIs or SD interrupts (54/s while writing). With the card in the
path the same table gains ~120 ticks of writer and iowait, which is the
part the 2026-10-24 record saw and mistook for the cause. The MI engine's
per-stage delays are unchanged (that record), so the engine finishes the
link's frames on time; what it costs is memory bandwidth and cache while
it does them, paid by the CPU threads in the TX path.

At 30 fps the slowdown is +28–30 %; at 720p60 +21–25 %. Both track the
pixels encoded per second, neither tracks the bitrate (5 Mb/s: +44 %).

## Open

- No zero-cost configuration of the second channel was found: 720p60 at
  40 Mb/s is within the method's noise on one pair and +0.9 ms on the
  other; 1080p30 about +1 ms; 1080p60 +1–2 ms on the bench and +4 ms in
  flight (0239, where core 0 is busier at the higher rungs). Recording the
  link's own stream instead of a second encode would cost nothing but is
  not a constant-bitrate recording.
- Not measured: whether an encoder attribute on the record channel (GOP,
  reference count is already 1, rate-control mode) changes its memory
  traffic; the H.264 codec for the record channel; recording from a binned
  sensor mode.
- The ~+12 points of sideport `cpu_pct` at both recorder bitrates in the
  2026-10-24 record is this slowdown.
