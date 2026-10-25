# Configurable fps and resolution on the bench: modes, ceiling, mismatch, letterbox, and a 100 fps unlock traced to the allowance

**Status:** the build is shipped on `fps100` (camera-mode table, per-mode
size rules, trim ceiling, steer ceiling clamp, rate-mismatch warning,
letterbox); reference: `docs/camera-modes.md`, `docs/phase-lock.md`
("Rate mismatch"), `docs/airtime-model.md` §3. The 100 fps periodic unlock
below is open.

Bench, 2026-10-19: drone on waybeam's `sensor_imx415_star6e.ko` (copied over
the stock module's filename), GS on the 2560x1440 monitor (99.946 Hz mode;
1920x1080@60 for the 60 fps runs). Builds from `fps100` up to the
letterbox commit. Follows `docs/findings/2026-10-19-waybeam-imx415-1728x972.md`.

## Mode selection by geometry

`maburd` asks for the mode `camera_mode_for` names and finds it by size and
maxFps:

| `venc.fps` / `venc.size` | `Sensor pad selected` | trim | encoder `Fps_1s` |
|---|---|---|---|
| 60 / 1920x1080 | mode 2, 1920x1080, max 90 | locked 59.988 Hz, 1 round | 59.97 |
| 100 / 1728x972 | mode 3, 1728x972, max 100 | locked 99.960 Hz, 2 rounds | 100.02 |
| 120 / 1728x816 | mode 4, 1728x816, max 120 | see below | 119.60–119.66 |

Mode 4's ceiling is real: at 120000 mfps the sensor measures 119.660 Hz and
the driver refuses any higher request (`MI_SNR_SetFps(120325 mfps) refused`
before the ceiling change). With it, the trim locks there at once:
`[sensor] cal: locked at the mode's ceiling, request 120000 mfps measured
119.660 Hz after 0 round(s)`.

Startup after a restart took up to ~2 min on this drone (`MI modules not
live: running load_sigmastar -i`, `warning: MI modules not live after
15000 ms, starting venc anyway`); a restart overlapping a still-exiting
`maburd` lost the radio (`USB adapter in use`). Bench scripts wait for the
old `maburd` to exit before starting.

## Rate mismatch

- Drone 119.66 fps (mode 4) on the 99.946 Hz monitor: `maburplay: rate
  mismatch: source 119.66 Hz, panel 99.945 Hz -- phase lock paused`, lock
  `wait`, the drone's `snr_off=0`.
- Drone 60 fps on the 99.946 Hz monitor: `rate mismatch: source 60.03 Hz,
  panel 99.951 Hz`, same.
- `screen_mode = "1920x1080@100"` on the monitor (which lacks it):
  `DrmPresenter: warning: screen_mode "1920x1080@100" not offered by
  connector 119; falling back to 1920x1080@60` — the case the warning is
  for.
- Not run: low-power suppression (no flight controller reporting disarmed
  on the bench; covered by `rate_match_is_silent_in_low_power`).

## Letterbox

Drone 1728x816@120 into the 2560x1440 monitor, burned DVR (colortrans on,
the GPU path): the recording is 2560x1440 with ~116 px black bars top and
bottom and an unsquashed picture. The RGA path (colortrans off) and the
on-screen plane were not captured.

## Soaks

| pair | windows locked | unlocks | late | pdrop | replaced | \|lerr\| mean / max | e2e p50 (median, range) / p90 median |
|---|---|---|---|---|---|---|---|
| 1280x720@100 (mode 3) → 99.946 Hz, 10 min | 525 / 601 | 7 | +0 | +0 | +27 | 1.10 / 4.96 ms | 26 (23–35) / 27 |
| 1920x1080@60 (mode 2) → 60.000 Hz, 10 min | 600 / 600 | 0 | +0 | +0 | +11 | 0.29 / 2.01 ms | 35 (33–37) / 35 |

The 60 fps pair on waybeam's driver matches the stock-driver phase-lock
bench (35 ms flat, locked throughout).

### The 100 fps unlocks are allowance steps

Every one of the seven unlocks follows a 2.5–4 ms step in the regulator's
transit allowance (`alw`) within one or two windows (9.7 → 7.1 → 5.8,
10.2 → 6.9 → 6.4, 8.4 → 9.2 → 11.6 ms, ...). The phase lock targets a slot
that includes the allowance, so a step reads as a phase error of its size;
at 100 fps that is more than the 3 ms lock-out, a third of the 10 ms
period. In the window of the step the lock's centre (frequency estimate)
also jumps by ~−80 mfps, and the next correction moves the sensor two lines
and the error past the lock-out. Ruled out:

- the drone's capture timing: 70 000 consecutive pts steps all within
  ±25 µs of 10.0 ms, no long or short frame;
- the ceiling: the steer sat at its +42 mfps clamp (100000 mfps) as
  designed, with no refusals;
- the phase lock's 60 fps-tuned gains: a host simulation of this mode
  (99958 mfps → VMAX 2035, 100000 → 2034) holds ten minutes without an
  unlock into both 100.000 and 99.946 Hz, with or without refusals.

At 60 fps the allowance stepped by ≥ 2 ms only 3 times in 600 windows with
no unlock; at 100 fps it changed in 533 of 685 windows across 5.2–11.6 ms.
The same mechanism caused the one unlock in the first phase-lock flight
(`docs/findings/2026-10-18-phase-lock-flight.md`).

## Driver regression at 60 fps

Dim scene, same build, 1920x1080@60, snapshot on each driver (stock
restored by file swap + reboot): luma mean 68.2 (waybeam) vs 68.4 (stock),
p5/p95 62.3/74.1 vs 62.7/74.2, mean RGB within 0.3, horizontal-gradient
detail 0.467 vs 0.469, mean absolute difference 2.06 levels. No exposure or
colour change from the driver.

## Gates

Host `ctest -R 'test_|host_e2e|gs_e2e|gs_au_e2e|player_e2e'` all pass (the
GS bar's render hash re-blessed for the wider fps column, type size
unchanged). `tools/bench/ausniff.py` on the GS, 30 s at 60 fps: 1816 AUs
all complete, 0 fid gaps, 0 resyncs, 60.5 fps.

## Open

- The allowance-step unlock at 100 fps: the phase lock and the regulator's
  allowance interact; a fix is GS-side (allowance held or slewed while
  locked, or the lock re-referenced across an allowance step) and is
  design work, not in this build.
- SD recorder at 100 fps (upscale on port 1, 60-of-100 spacing, link
  latency with recording on): the bench drone has no SD card.
- The goggles soak (1920x1080@100.000) and an RGA-path letterbox capture.
- Not flown.
