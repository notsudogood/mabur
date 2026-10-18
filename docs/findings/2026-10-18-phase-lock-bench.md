# Does the built phase lock hold a flat ~35 ms on the bench, and do its exits work?

**Status:** bench gate for the phase lock as built; the reference page is
`docs/phase-lock.md`. Follows `2026-10-17-phase-lock-spike-2.md` (the spike
this productises); flown in `2026-10-18-phase-lock-flight.md`.

## Setup

Bench Radxa ZERO 3 and the bench drone, both on branch `phase-lock` at
dd281b34 (RC_VERSION 16). Session 0148, rung 4 (mcs4/40, ov 0.50), static
scene, GS pinned to channel 144 for the run. The GS had been on auto
selection, which had parked the link on channel 64, where every promote
past rung 1 fell back on residual loss; 144 is where the spikes ran.
Display: `vsync_lead_ms` 5, `chain_budget` 0. The drone's boot trim
locked at a 59718 mfps request (59.992 Hz measured).

- **Arm U (unlocked):** `phase_lock` absent (false), `sched_cap_ms` 0. 300 s.
- **Arm L (locked):** `phase_lock = true`, `phase_margin_ms = 4`,
  `sched_cap_ms = 16`, the bundle's values. maburplay restarted at the arm's
  start, so the window includes acquisition. 407 s, of which the last 378 s
  are the steady state.

Judder = `lat.log` seconds with p99 ≥ p50 + 10 ms. Never-shown = `au.log`
AUs with no `frame.log` row. Holes = `au.log` pts steps over 25 ms or a fid
gap.

## Result

| | e2e p50 / p90 / p99 (ms) | judder s | never-shown /s | pts holes |
|---|---|---|---|---|
| U | 40.6 / 48.1 / 51.0 (sawtooth 33 → 51, ~2 beats in 300 s) | 1 % | 0.003 | 0 |
| L, whole window | 35.4 / 36.5 / 40.2 | 0 % | 0.002 | 0 |
| L, steady state | **35.4 / 36.3 / 37.0** | **0 %** | **0.003** | 0 |

Every bench gate in the spec passed: p50 ≤ 36 ms, p90 − p50 ≤ 2 ms
(0.9), judder no worse than unlocked, never-shown ≤ 0.03/s, no holes.

- **Acquisition:** the first window came 3 s after the restart, and the
  lock reached `lock` 13 s later (16 s from the restart). Its first window
  measured a large slope and stepped the sensor by +189 mfps, about 11
  lines. That step and the rest of the acquisition left no pts hole, and
  the drone logged no refusal.
- **Holding:** locked 97 % of the window, never unlocked. |lerr| while
  locked p50/p90/max 0.19 / 0.65 / 1.14 ms. Hold in the natural slot
  p10/p50/p90 8.1 / 10.7 / 13.1 ms, flat in every 10 s bucket. The offset
  changed 57 times a minute, toggling between neighbouring line counts.
  The centre settled at +11.5 mfps, against +12 predicted by the
  controller simulation for this sensor and a 60.000 Hz panel.
- **Regulator:** in the steady state 0 chained frames, 0 cuts, 1 replaced
  frame and 0 pdrops.
- **ausniff:** 60.5 fps, 0 resyncs, no incomplete AUs. **aucadence:**
  base − enh offset −0.24 ms.

**59.94 Hz:** with `screen_mode = "1920x1080@59.94"` the presenter picked
the panel's 59.940 Hz mode. The lock went from acquiring to locked in 4 s,
and the centre walked to −58 mfps, then settled at −50.7. Over 107 s of
steady state: e2e 35.3 / 36.5 / 37.8 ms, judder 0 %, never-shown 0.02/s.
On `@60` the same panel gives 60.000 Hz.

**Exits:**
- **maburplay stopped while locked:** the drone's stats line read
  `snr_off=0` within 3-4 s (maburgs' 3 s timeout plus the 1 s stats
  cadence). On restart the lock reacquired.
- **maburd restarted mid-session:** the player saw no frames and went to
  `wait`, holding the last offset. The new stream's pts discontinuity then
  reset the centre and offset to 0. The lock reacquired once the drone's
  trim had relocked.

## Answer

The phase lock as built does what the spike promised, with the stall that
spike 2 blamed for its skips now fixed. On this bench it gives a flat
35.4 ms p50 (p99 37.0) against the unlocked stream's 40.6 ms average on a
33-51 ms sawtooth. Judder seconds and never-shown frames are at or below
the unlocked stream's. It locks within ~15 s, holds a 59.94 Hz panel as
well as a 60 Hz one, and every exit returns the drone to its trim. A flight
is the next gate.

Unrelated to the lock: the GS's remembered auto channel (64) could not
climb past rung 1 at this time, while 144 climbed to rung 4 at once.
