# What does the phase lock do in flight?

**Status:** first flight of the phase lock; the reference page is
`docs/phase-lock.md`. Follows `2026-10-18-phase-lock-bench.md` (bench
session 0148, same build).

## Setup

Session 0150, flown on the build the bench record used (RC_VERSION 16).
The GS ran the bundle's display settings: `phase_lock = true`,
`phase_margin_ms = 4`, `sched_cap_ms = 16`, `vsync_lead_ms = 5`,
`chain_budget = 0`. The drone ran demand refresh (`venc.gop_s` 0). The
link climbed to rung 4 and then churned between rungs 0 and 4 at 17.7
ladder events a minute. The session has 247 s of video. The GS was up for
188 s before the drone powered on.

The comparison flights ran without the lock:

- **0112-0114:** a periodic refresh (`gop_s` 0.5), `chain_budget` 0.
- **0141 and 0142:** demand refresh, otherwise the same build line.

Definitions:

- **Judder second:** a `lat.log` second with p99 ≥ p50 + 10 ms.
- **Near ladder:** within ±2 s of a `ctl.log` E row.
- **Near loss only:** within ±2 s of a `fec.log` episode row and not near ladder.
- **Never-shown:** an `au.log` AU with no `frame.log` row.

## Result

| flight | e2e p50 / p90 / p99 (ms) | judder s | near ladder | near loss only | calm | never-shown /s | ladder events /min |
|---|---|---|---|---|---|---|---|
| 0112 | 46 (p50) | 22 % | 30 % | 23 % | 16 % | — | 11.4 |
| 0113 | 46 (p50) | 20 % | 28 % | — | 12 % | — | 16.4 |
| 0114 | 46 (p50) | 32 % | 34 % | 29 % | 23 % | — | 22.7 |
| 0141 | 47.0 / 53.5 / 67.6 | 23 % | 27 % | 24 % | 10 % | 0.68 | 21.7 |
| 0142 | 46.6 / 53.5 / 58.0 | 14 % | 25 % | 4 % | 3 % | 0.33 | 13.4 |
| **0150** | **41.3 / 42.7 / 44.2** | **9 %** | **14 %** | 0 % | **3 %** | **0.34** | **17.7** |

For 0112-0114 the table gives only the median of the per-second `lat.log`
p50. The p90/p99 and never-shown columns come from `frame.log` joined to
`au.log`, which was not run on those sessions.

**The lock held through the churn.**
- It reached `lock` 9 s after the first frame and was locked for 93 % of
  the video time.
- The offset ranged from −66 to +62 mfps and changed 34 times a minute.
- |lerr| while locked was p50/p90 0.28 / 1.28 ms.
- Flightreport's "locked 53 %" counts the 188 s before the drone powered
  on, when the lock was waiting for frames.

**Latency was flat at 41 ms instead of the 46 ms average on the 38-54
sawtooth.** Every 10 s bucket's p50 sat between 37 and 43 ms. The gap to
the bench's 35.4 ms is the allowance, not the lock. In flight the
lateness window's p99 − p1 is wider (NACK fills, FEC waits), so the
allowance ran at 12.7 / 15.4 / 16.0 ms (p10/p50/p90) against ~10 ms on
the bench, and it sat at the 16 ms cap much of the time. The lock held its
4 ms margin in both cases: `hold` p50 was 12.7 ms in flight and 10.7 ms on
the bench.

**Judder fell most where it used to be worst.** Near ladder events,
judder seconds dropped from 25-34 % to 14 %. The calm seconds matched
0142's best (3 %).

**One unlock, on a calm stretch.** At ~214 s, with the link on rung 4 and
no ladder event for 20 s, the lock dropped to `acq` for 7 s.
- The allowance had just stepped from 16.0 to 12.8 ms, as the late samples
  from the first seconds of video aged out of the 10 s window.
- A step like that moves every frame's `want`, and so the margin, by
  ~3 ms at once. That tripped the "|e| > 3 ms for 2 windows" rule. The lock
  reacquired as soon as the phase followed.
- No judder second fell in that stretch.

**Never-shown frames did not improve:** 0.34/s, against 0.33 in 0142.
66 of the 83 are near ladder events: frames that missed their
capture-time slot during a loss burst. The regulator line's `lslot` and
`replaced` counters climb together in the same 10 s buckets.

**Other observations, none caused by the lock:**
- **pts holes:** 5 `au.log` pts holes, all 33 ms steps on demote IDRs. They
  are source holes on the drone, each answered by a `lost` refresh request
  healed in 3-167 ms.
- **REFRESH heal_ms max 182 463:** this is the boot `start` request the
  player sent before the drone was on.

## Answer

In flight the lock does what it did on the bench, except for the level it
holds. Latency is flat at ~41 ms instead of riding a 38-54 ms sawtooth.
Judder seconds fall to 9 %, mostly because frames near ladder events stop
missing their slot. The lock survived 73 ladder events. It dropped once,
for 7 s, when the allowance stepped by 3 ms on a calm stretch, and nothing
visible came of it.

Two levers remain:

- **The flight latency is set by the allowance cap.** Getting closer to the
  bench's 35 ms means a tighter or slower-moving allowance.
- **The lock should not unlock on an allowance step.** For example, it
  could measure the margin against a slowly filtered allowance, or re-seat
  the centre on a step instead of counting it against the lock.
