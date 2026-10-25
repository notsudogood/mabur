# What the VTX SD recorder costs the link — 2026-10-24 / 25

**Status:** measurements stand, mechanism superseded. The next day's
`docs/findings/2026-10-25-vtx-recorder-engine-cost.md` showed the writer is
innocent (a sink that discards every byte costs the same) and the cost is
the record channel's second encode. Current reference →
`docs/vtx-recorder.md` "Link latency cost".

**Question.** With `[record]` on at the bundle's 40 Mb/s, glass latency
rises and flights show spikes and stutter. The 2026-09-26 spike had shown
the recorder costing the link nothing once ch1 is bound before the link
channel. What moved, and where?

**Answer.** Not the encoder. The MI per-stage counters read the same with
the recorder idle and recording (link channel enqueue → dequeue 7.0 ms;
ch1 queues behind it). The cost comes from the writer: ~5 MB/s of `fwrite`
plus one `fsync` per fragment onto the vfat card, all of it on core 0
alongside the TX writer, the four USB threads and the FEC worker. Core 0
goes from ~54 % to ~79 % busy, the drone's TX-queue wait doubles, and the
GS capture-time scheduler, whose allowance is the transit p99 − p1 over the
last 600 frames (`docs/phase-lock.md`), widens its window and holds every
frame longer. Bench: **+2.0 ms e2e p50** at 40 Mb/s, **+0.2 ms** at 5 Mb/s.
Flight 0239: **+4 ms p50** against the no-recorder flights at the same
rungs, with the allowance pinned at its 16 ms cap 88 % of the time, twice
the spike episodes, and a drone congestion shed (enhance layer dropped,
30 fps for ~2 s) after demotes.

## Bench

Drone on the bundle config (1920x1080@60, `[record]` 1920x1080 60 fps
40 Mb/s on VPE port 0, low-power off), GS at 1920x1080@60 with the phase
lock on and `sched_cap_ms` 16, rung 5 (mcs4/40), bench scene delivering
17–23 Mb/s. maburplay ran from a temporary copy of its config with
`dvr.target = "vtx"` so the record button drove only the drone. 50–60 s
windows, 12–15 s settle after each toggle, GS sessions 0234/0235.
Per-frame segments from `frame.log`, allowance from `reg.log`, drone MI
stage delays from `/proc/mi_modules/mi_venc/mi_venc0` after
`reset_counter`, per-core CPU from `/proc/stat` sampled every 5 s.

### Interleaved on/off, recorder at 40 Mb/s (bundle)

| window | delivered | e2e p50 | e2e p99 | `dq` p99/max | `air` p90 | `reg` p50 | `alw` p50 |
|---|---|---|---|---|---|---|---|
| on 1 | 23.3 Mb/s | 36.6 | 37.5 | 2 / 5 ms | 2.1 | 11.5 | 9.4 |
| off 1 | 23.1 | 34.6 | 35.5 | 2 / 2 | 1.7 | 10.5 | 8.2 |
| on 2 | 23.0 | 36.4 | 37.8 | 2 / 3 | 2.2 | 11.3 | 9.3 |
| off 2 | — | 34.6 | 36.6 | 2 / 2 | 1.6 | 10.8 | 8.4 |

e2e +2.0 ms p50, +2.0 ms p99, repeatable. The link's `enc` moves +0.2 ms
p50 (p99 +0.3), `fec` +0.3–0.4, `air` +0.2–0.3 p50; `reg` carries +1.0 ms
on every frame because the allowance grew; `dec` p99 +1.2–1.6 ms (the
streamed decoder waits on later-arriving slices). `dvr.target = "both"`
on the same bench added a further ~0.5 ms (e2e p50 35.7 vs 35.2).

### Recorder at 5 Mb/s (same encode, one eighth of the writes)

| window | delivered | e2e p50 | e2e p99 | `alw` p50 |
|---|---|---|---|---|
| on 3 | 18.4 Mb/s | 34.5 | 36.0 | 8.8 |
| off 3 | 18.6 | 34.3 | 35.3 | 9.3 |
| on 4 | — | 35.2 | 37.3 | 9.6 |
| off 4 | — | 34.9 | 36.9 | 10.5 |

+0.2 ms p50. The second 1080p60 encode is identical in both variants, so
the encode is not what costs; the byte volume written is.

### Where the time goes on the drone

- **Engine, unchanged.** Link channel `OnPreProcessInputTask` 16.7 ms →
  `DequeueInputTask` 23.8 ms (MI origin) idle and recording; ch1 enqueues
  at 23.8 and dequeues at 30.9, behind the link. `MbRate` 147 % with ch1
  created, as before. The bind-before-link rule holds.
- **Core 0.** Process affinity puts every thread but the frame producer
  (`mbr-hot`, alone on core 1) on core 0: `venc-enc` (SCHED_FIFO), the TX
  writer `mbr-txw`, four `mbr-usb`, `mbr-fecw`, `mbr-msp`, and the
  recorder's `mbr-rec` and `mbr-recdrain`. Per 20 s at 40 Mb/s: `mbr-rec`
  114 ticks, `mbr-recdrain` 45, `mmcqd` 10, kernel writeback ~20, against
  the TX path's ~900 (bitrate-proportional). Core 0 busy p50 54 % → 79 %
  (max 87 %), iowait 0 → 4.5 %; core 1 22 → 31 %. At 5 Mb/s: `mbr-rec` 29,
  core 0 48 → 65 %.
- **TX path.** Drone `txq_wait_ms` p50 3 → 6 (max 5 → 12); per-AU `dq`
  (true TxQueue wait) p99 1 → 2 ms, max 2 → 4–5; `air` p90 +0.4–0.7 ms.
  `kswapd0` ran on core 1 during recording (page cache from the writes).
- One 75 ms transit stall in four recording windows (fec 75 ms, e2e
  103 ms, the allowance hit its cap for ~10 s). Not reproduced.
- The sideport `drone.sys.cpu_pct` rises ~12 points at both recorder
  bitrates, which the per-thread ticks do not explain. Not chased.

### How the GS turns jitter into latency

With the phase lock on, the regulator places each frame at capture time
plus the delivery floor plus `alw`, where `alw` = transit p99 − p1 + 1 ms
over the last 600 frames, capped at `display.sched_cap_ms` (bundle 16). A
p99 that moves 1 ms moves every frame 1 ms. The recorder's tails
(`dq`/`air`/`fec`/`dec` p99) widen the window from ~8.2 to ~9.4 ms on the
bench; the rest of the +2 ms is the transit p50 itself.

## Flight 0239 (dvr.target both, 2026-10-25)

Recorder on from 130.6 to 284.5 s, i.e. the whole flight, rungs 2–5,
~19 Mb/s delivered. Compared with flights 0232 and 0233 (same rungs and
bitrate, no recorder):

| | 0239 recording | 0232 / 0233 |
|---|---|---|
| e2e p50 / p99 | 43.2 / 64.1 ms | 39.1 / 55.7 |
| `alw` at the 16 ms cap | 88 % of seconds | 15–37 % |
| `dq` p99 / max | 4 / 28 ms | 1 / 2 |
| `air` p99 / max | 10.7 / 53 ms | 4.8–7.1 / 18–28 |
| e2e > 50 ms episodes | 7.5 / min | 3.4–4.1 / min |
| pts gaps > 25 ms | 53 / min | 12–27 / min |
| demotes | 10.1 / min | 7.2–11.4 / min |

Two spike families:

- **Link loss** (`fec` 30–78 ms), each on a rung 5 → 4 `residual` demote
  after the scene burst to 26–32 Mb/s at rung 5. Demote rate equals the
  no-recorder flights: the link, not the recording.
- **Drone backlog** (`dq` 5–25 ms, `air` 17–44 ms, at 137.8, 140.7, 157.8,
  161.7, 230.7, 240.8, 248.6, 259.7 s). After a demote the drone flushes
  frames encoded at the old rate through a slower PHY; with core 0 carrying
  the writer, `txq_wait_ms` reads 16–33 ms and CPU 62–72 %. The 140.7 s
  episode held e2e above 50 ms for 5.5 s. The same demotes in 0233 leave
  `air` ≤ 28 ms and `dq` ≤ 2 ms.

The stutter is the drone's congestion shed: at 158.3 s, queue wait 27 ms,
`congestion_shed` fires and the enhance layer stops; `frame.log` shows
base-only frames at 33 ms spacing for ~2 s (30 fps on the glass). The
lost-slot counter jumps 10–28 and `replaced` reaches 7–9/s at those
moments.

## Open

- The lever is the writer's I/O on core 0, not the encoder: lower
  `record.bitrate_kbps`, a writer/writeback placement or priority away from
  the TX threads, or larger fragments (fewer fsyncs). None built or tried.
- `record.fps` 30 (halves encode and writes) untested.
- The ~12-point `cpu_pct` rise at both bitrates; the one 75 ms stall; the
  recorder's own file was not inspected for drops in flight 0239.
