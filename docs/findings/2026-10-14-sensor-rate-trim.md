# The "little judder with no loss": sensor rate vs the VPE→VENC bind limiter

**Status:** root-caused and fixed — the sensor-rate trim and the src:src
full-rate bind shipped in `drone/venc/sensor_cal.[ch]`; reference:
`docs/airtime-model.md` §3 ("The sensor does not run at `venc.fps`").
The phase lock steers the rate on top of this trim, which stays its base
and fallback: `docs/phase-lock.md`.

Bench, 2026-10-14. Started from two flights, 0090 (6 slices) and 0091
(1 slice), flown to compare slice counts: both felt slightly juddery with
no visible loss.

## What the logs showed

- Slices made no difference: jitter EMA 5.5 vs 5.8 ms, residual 4.1 vs
  4.2 ms, inter-arrival p99 32.5 vs 34.0 ms.
- `au.log` pts stepped two frame periods (33 ms) 147 and 179 times per
  flight with the frame id consecutive and the base/enhance layer
  alternation intact across the hole (132 of 136): the encoder never coded
  the frame. Holes were 3.4–3.6 s apart, not random.
- The clean pts step averaged 16592 µs = 60.27 Hz. Older sessions
  (0040–0086, 0088) ran 16645 µs = 60.078 Hz with holes every ~10 s; 0030,
  0087, 0089–0091 ran 60.27 Hz. The pts clock agreed with the GS clock to
  0.01 %, so the sensor really runs fast.

## What the bench showed

Drone proc counters (`/proc/mi_modules/mi_vpe/mi_vpe0`, `mi_venc0`,
`mi_vif0`) sampled over 60–120 s windows while a ring reader on the GS
counted pts holes:

| state | sensor | holes | VPE DropCnt |
|---|---|---|---|
| boot bind 90:60 | 60.078 Hz (VMAX 3386) | every 10.15 s | +1 per hole |
| POST fps=90 (bind 90:90) | same | 0 in 60 s | frozen |
| back to 90:60 | same | every 10.15 s | climbing |
| `MI_SNR_SetFps(0, 59717)` → VMAX 3391, bind 90:60 | 59.990 Hz | every 26.4 s | +1 per hole |
| VMAX 3391 + bind 90:90 | 59.990 Hz | 0 in 120 s | frozen |

- The drop is the SDK's frame-rate control on the VPE→VENC bind. It is
  time-based ("at most dst per second" by the SoC clock, effective
  ceiling ~59.95 fps, not a src:dst count ratio: 90:60 passed every frame
  of a 60.0 input) and is booked on the VPE side as chn `DropCnt` and
  out-port `FailCnt`, which here IS frame loss.
- Sensor timing: fps = 74.247 MHz / (VMAX × HMAX), HMAX 365 in the
  1080p90 mode. `SetFps(60)` → VMAX = 2250 × 90 / 60 = 3375 → 60.271 Hz.
  Live i2c reads (bus 1, 7-bit 0x1a: `0x3024..26` VMAX, `0x3050..52`
  SHR0) on the dim bench showed VMAX 3386 with SHR0 8: the auto-exposure
  at a full-frame shutter makes the driver stretch VMAX to
  `expo_lines + 8`. That is the whole "bimodal per boot" story — scene
  brightness, not the boot path.
- The driver's `pCus_SetFPS` accepts fps in `[min, max]` or milli-fps in
  `[min*1000, max*1000]` (`vts = vts_30fps × max_fps × 1000 / mfps`), and
  the MI layer passes a milli value through (`GetFps` reads 59716 back).
- GS panel mode: `1920x1080 148500 2200×1125` = 60.000 Hz.

## What shipped and what it measured

`sensor_cal.c`: 4 s median-step windows, proportional re-request in
milli-fps, band `[fps − 0.030, fps − 0.004]`, 6-round cap with restore to
the boot request; full-rate binds src:src. First warm start on the bench:
`60.082 Hz → 59901 mfps`, `60.165 Hz → 59720 mfps`, locked at 59.992 Hz
after 2 rounds; 0 holes in 120 s, `DropCnt` frozen. The player's `lat:`
e2e p50 then drifts 17 ms down over ~100 s and resets: the one panel
repeat per beat against the 60.000 Hz panel, the floor without genlock.

## Tools

The ring reader (`ptsholes.py`, an ausniff-based pts-hole counter) and the
counter snapshot (`dsnap.sh`) were scratch scripts; `tools/bench/ausniff.py`
plus a pts-delta pass over `au.log` reproduce the measurement offline.
