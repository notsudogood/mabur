# Does a display-side trim servo hold the phase lock with the drone's sensor steer off?

**Status:** spike; parked, nothing built. The shipped code still steers
the drone's sensor (`docs/phase-lock.md`). Parked because a display loop
bounded tightly enough to be gentle on the goggles acquires in up to ~27 s
against the sensor steer's 7–12 s, and no GS-side signal can tell whether
a given display tolerates the trim at all. This closes the loop that
`2026-10-22-hdmi-clock-trim-spike.md` opened (the actuator alone); the
throwaway rig (a 3-line env override in maburplay, `trimd.py`, `hpll.py`,
`vbl.py`, `phymon.py`, `step.py`, `op.py`) lives only in the machine-local
spike directory and is described below well enough to rebuild. Two runs the
same day: the bench monitor (first half) and the Avatar Goggles X (second
half, "Goggles"), which blanked under the servo for a reason this record
could not pin down.

## Question

The 10-22 spike showed the Radxa can trim its HDMI pixel clock live
(HPLL fractional divider, frac-only writes, no blank). Can that actuator
replace the sensor steer as the phase lock's output: with the drone's
`snr_off` held at 0, does the panel-following loop acquire, stay locked, and
survive a drone restart and a player restart?

## Rig

- **maburplay**: master `4c94bc23` plus an uncommitted 3-line patch in
  `main.cpp`: `MABUR_SPIKE_CTL_PORT` overrides the port
  `CoreControlClient` sends its RecControl datagrams to (8401 by default).
  Built with stage 6 of `tools/build-arm64.sh` (glibc, Buildroot sysroot),
  deployed over `/usr/local/bin/maburplay` with the shipped binary kept as
  `.pre-trimspike`, started with `MABUR_SPIKE_CTL_PORT=8402` in the init
  script's environment. Configuration unchanged: `display.phase_lock = true`,
  `phase_margin_ms 4`, `sched_cap_ms 16`, `vsync_lead_ms 5`,
  `screen_mode 1920x1080@60`. The controller (`PhaseLock`) and the
  measurement path are therefore exactly the shipped ones; only the
  actuator differs.
- **`trimd.py`** on the GS (python3, `/dev/mem`): binds UDP 127.0.0.1:8402.
  Every line is forwarded to maburgs on 8401 verbatim except
  `phase <mfps>`, which is forwarded as `phase 0` (so every RCF carries
  `snr_off` 0 and the sensor runs on its boot trim) and applied to the panel
  instead: `ppm = -mfps * 1e6 / (fps * 1000)` (16.67 ppm per mfps at 60 fps;
  a faster panel moves the phase like a slower camera), clamped to
  ±3000 ppm. It writes `PLL_CON2` (frac) only, never crossing fbdiv; when it
  finds the PLL in the kernel's integer setting (99, 4×4) it first moves to
  the operating point 4×3, fbdiv 74 + 0.25 (the one write sequence that
  blanks the sink). On SIGTERM it returns to 0 ppm and restores the integer
  setting. Every write is logged to `/tmp/trimd.log` with the mono time.
- **`phymon.py`**: 1 kHz poll of the dw-hdmi `PHY_STAT0` lock bit, logging
  changes. **`vbl.py`**: vblank-rate fit from `DRM_IOCTL_WAIT_VBLANK`.
- **Drone**: 1920x1080@60, `slices 4`, the bench link; `snr_off=0` in the
  drone's stats line throughout the display-trim runs (before the switch it
  read −40). Boot trim request 59790 mfps.
- **Measurement**: the session's `reg.log` (`lock= lerr= loff= lctr=`) and
  `lat.log` (`e2e`), split at the switch; `trimd.log` for the ppm trace.
  Unattended run: the sink was not watched, so sink blanks were not judged.

## Results

The drone-steer rows are the same session (0226) before the switch, same
drone, same panel.

| run | duration | time to lock | lock share | unlocks | \|lerr\| locked p50 / p90 / max | loff range (mfps) | e2e p50 median / min / max |
|---|---|---|---|---|---|---|---|
| drone steer (shipped) | 377 s | 12 s | 96.8 % | 0 | 0.31 / 1.06 / 3.00 ms | −66..91 | 35 / 32 / 44 ms |
| display trim, steady | 636 s | 10 s | 98.4 % | 0 | 0.22 / 0.70 / 2.07 ms | −51..63 | 34 / 33 / 37 ms |
| display trim after `maburd` restart (session 0227) | 159 s | 7 s after the first frame back | 95.6 % | 0 | 0.17 / 0.47 / 1.36 ms | −16..49 | 34 / 32 / 50 ms |
| display trim after `maburplay` restart | 98 s | 6 s | — | 0 | 0.26 / 0.79 / 1.86 ms | −43..52 | 34 / 32 / 36 ms |

- **Steady hold**: 10.6 min locked without an unlock; glass latency flat at
  34 ms (the drone-steer half: 35 ms with excursions to 44). The ~1.5 %
  of rows not locked are the acquire at the start.
- **Drone restart**: frames stopped, the lock fell to `wait` after 2 s
  (offset held), the pts discontinuity 14 s later reset the offset to 0
  (panel back to nominal), `acq` 2 s after frames returned, `lock` 9 s
  after that. maburgs opened a new session (0227) for the new link.
- **Player restart**: the trimmed PLL survived the presenter's modeset
  unchanged (fbdiv 74, fractional, same frac): a modeset at the cached rate
  does not touch HPLL, as the 10-22 finding predicted, so there was no
  second blank and `trimd` logged no second operating-point write. The new
  instance locked in 6 s.
- **Actuator**: 874 frac writes, −1550..+850 ppm (the −1550 was the first
  acquire step; after that the range stayed inside ±650), zero fbdiv
  clamps, one operating-point write. The PHY lock bit read `0xf3` for the
  whole run, no vblank sequence gaps, nothing HDMI-related in `dmesg`.
- **Where the panel settled**: `lctr` +4..+18 mfps, i.e. the panel ran
  −70..−300 ppm (59.98–59.996 Hz) to follow the camera's trim.
- **Dither**: the offset toggles ±10..25 mfps (±170..400 ppm) from one
  second to the next while locked. `PhaseLock`'s gain and slew limits
  (`kKpMfpsPerMs` 30, ±20 mfps locked) are sized for a sensor that quantizes
  to whole lines; the panel is continuous, so the dither is measurement
  noise through Kp. It costs nothing visible (a 400 ppm second moves the
  phase 0.4 ms; the sink tolerated 2500 ppm steps on 10-22) but a built
  version would lower Kp or filter the offset.

## What this means for building it

- **The controller is already right.** The display-side actuator is a
  drop-in for the sensor steer at `PhaseLock`'s output: same offset,
  sign inverted, scaled 1e6 / (fps × 1000) ppm per mfps, written to
  `PLL_CON2`. The measurement path needs nothing: `VblankEstimator` tracks
  the trimmed period inside its ±1 % band and the regulator's capture
  placement works as before.
- **Reliability on this board and sink**: no unlock in ~15 min across a
  drone restart and a player restart; acquire 6–10 s; the actuator state
  survives a player restart.
- **Still to decide for the real thing**: the operating point per mode
  (10-22 finding: 297 MHz at 1080p100/120 and 148.35 MHz at 59.94 need
  their own postdivs) and when to apply it (right after the presenter's
  modeset, so its one blank rides the modeset's); `/dev/mem` from maburplay
  versus a kernel patch in the GS image; the RCF `snr_off` field goes to 0
  (or away) while the display trims; and the fallback to the sensor steer
  where the display cannot be trimmed: the web GS and its spotter mode
  (browser, no clock access), the Orange Pi (H618 video PLL, unspiked),
  sinks that re-sync at smaller steps.
- **Not covered here**: sink blanks (unattended), goggles, 100 fps modes,
  the OPi, ladder events (none were induced on the bench link).

## Goggles (second run, same day)

Sink: Avatar Goggles X (EDID name `AAABox`; the connector's sysfs `edid`
read back empty, the mode list offers 1280x720 and 1920x1080 at 60 and 100
plus 576p/480p). The operator watched the goggles throughout, so the sink
side is judged this time.

Modes and the kernel's PLL setting for them:

| mode | pixel clock | kernel HPLL | frac-only headroom |
|---|---|---|---|
| 1920x1080 @60.000 | 148.5 MHz | integer, fbdiv 99, postdivs 4×4, refdiv 1 | none (integer) |
| 1920x1080 @100.000 | 244.64 MHz (a reduced-blanking timing, not CEA's 297) | **fractional already**: fbdiv 163 + 0x17e4b1/2²⁴, refdiv 4, postdivs 1×4, VCO 979 MHz | −572 / +5559 ppm |

With the player stopped the kernel parks the output on another mode at
123.75 MHz (fbdiv 433, refdiv 12, postdivs 1×7). A daemon that reads "the
nominal" while the player is down reads that one; `trimd.py` v2 learned
to read it on the first offset line instead, after one bad minute in which
it drove the 1080p60 mode at 50 Hz (the player paused on rate mismatch,
the goggles showed the wrong rate; GS-side signals still saw nothing).

### 60 fps, 1080p60, same session (0228)

| run | duration | lock share | unlocks | \|lerr\| locked p50 / p90 | e2e p50 median / min / max |
|---|---|---|---|---|---|
| drone steer (shipped) | 473 s | 85 % (52 s of boot `wait`) | 0 | 0.29 / 0.91 ms | 34 / 31 / 53 ms |
| display trim, 4×3 point | 164 s (135 locked) | 82 % (the bad minute's `oor` tail) | 0 | 0.17 / 0.96 ms | 34 / 33 / 50 ms |
| **control: `phase_lock = false`, no trim** | 278 s | — | — | — | 38 / 28 / 46 ms |

The control is the textbook beat: e2e p50 per 10 s read
`44 43 41 40 38 37 36 34 33 32 30 29 | 44 43 42 40 …`, a 16 ms sawtooth
with a ~120 s period, i.e. the camera's trim lands ~8 mfps from the
goggles' 60.000 Hz. The operator reported no blank during the 60 fps trim
run beyond the expected one at the operating-point write.

### 100 fps, drone 1728x972@100, panel 1920x1080@100.000 (session 0229)

- **Drone steer (shipped), 200 s baseline**: lock 7 s after the first
  frame, then an unlock about every 90 s with a 3–5 ms error jump and a
  9–12 s re-lock (the known allowance-step pattern at 100 fps,
  `docs/findings/2026-10-22-burned-dvr-100fps-throughput.md`). e2e p50
  30 ms. **The sensor steer is pinned**: the 1728x972 mode's ceiling is
  exactly 100.000 fps and the drone's trim landed at 99.960, so every
  request to speed up was clamped (`steer: offset 200 mfps clamped to 31`).
  A sensor at its mode ceiling cannot follow a 100.000 Hz panel from
  above; at 100 fps the display trim is the only actuator with range.
- **Display trim at the kernel's own point** (frac only, no
  operating-point write, so no blank): locked, but on the first
  allowance-step re-acquire the controller asked −720 … −1570 ppm, the
  daemon clamped at the −572 ppm integer edge for 22 s, and the centre
  wound up to 139 mfps, then unwound over 20 s with a 2.5 ms phase
  excursion. **A saturated actuator must report the applied offset to the
  controller**, whose slope-to-centre step assumes the requested offset was
  in force.
- **Display trim at an explicit point** (postdivs 4×2, refdiv 1, N =
  81.547, ±5500 ppm headroom): the first attempt left refdiv at 4 (the
  CON1 hiword mask covered dsmpd and postdiv2 but not the refdiv bits), so
  the pixel clock fell to 61 MHz and the daemon, seeing the point "not
  taken", rewrote it every second for ~30 s: the goggles lost the signal
  for that half minute. Fixed (mask `0x11ff`, and a daemon that exits if the
  point does not take), the loop locked at once; one unlock at 59 s
  (allowance step), re-lock 12 s. **About 2.5 min in, the operator
  reported the goggles blanking repeatedly**, while the daemon was writing
  −170 … −570 ppm in steps of at most 200 ppm, fraction register only. The
  GS saw nothing (PHY lock bit, vblank sequence, dmesg). Killing the
  daemon stopped it at once.
- **Hand tests right after, one write per command, operator watching**:
  at the kernel point −100 ppm, then −400 (a 300 ppm step), then 20 writes
  alternating −200/−400 at 1 Hz: no blank. At the 4×2 refdiv-1 point:
  static hold, the same 20-write alternation, −600 static, −1000 static
  (a 400 ppm step): no blank. So neither the step size, nor the cadence,
  nor the reference divider, nor an offset to −1000 ppm reproduces the
  blanking.
- **Unresolved.** The two things the servo did that the hand tests did
  not: acquire excursions to −1000 … −1800 ppm (panel at 99.82–99.90 Hz)
  minutes earlier, and the quarter-rate accident before that. A bounded
  run (offset clamped to ±1000 ppm) was set up to separate them, but the
  GS was unplugged for the sink swap before it started. With the daemon
  gone and the drone on its trim the operator saw the open-loop sawtooth,
  35 → 25 ms, wrapping every ~25 s (99.960 vs 100.000).

### What the goggles add to "building it"

- The bound: the servo's steady offset was −120 … −400 ppm at 100 fps
  and −70 … −300 ppm at 60; nothing a display must absorb exceeds a few
  hundred ppm once the drone's own trim has run. The ±600 ppm acquire
  swings and the dither are the controller's sensor-era gains, not a need.
- The actuator must feed its applied value back (saturation windup), and
  the operating point must be chosen for the headroom the bound needs,
  from the kernel's own fractional point when it has it.
- Tolerance is per sink and invisible to the GS. The operator switches
  the feature off when a display blanks; the sensor steer remains the
  fallback (and at 100 fps the only one, with its ceiling).
