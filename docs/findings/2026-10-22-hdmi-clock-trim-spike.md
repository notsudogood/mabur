# Can the Radxa GS trim its HDMI refresh rate live, so the display follows the camera?

**Status:** spike; parked, nothing built. The phase lock still steers the
drone's sensor (`docs/phase-lock.md`). The closed loop (this actuator
driven by the shipped controller, sensor steer off) and the reasons it was
parked are in `2026-10-24-display-trim-servo-spike.md`. The throwaway tools (`hpll.py`, `vbl.py`,
`phymon.py`) live only in the machine-local spike directory; everything
needed to rebuild them is below.

## Question

The phase lock removes the sensor/panel beat by steering the camera to the
panel. The reverse also works: make the camera the reference and trim the
display's pixel clock so the panel follows it. Another FPV system reportedly
does this by nudging the fractional divider of the PLL that clocks only the
display controller and the HDMI transmitter, by up to ±0.1 %, without a
mode change. Can the Radxa ZERO 3 (RK3566, kernel 6.1.84) do that without
the sink blanking?

## Clock tree

`/sys/kernel/debug/clk/clk_summary` (debugfs is not mounted at boot) at
1920x1080p60:

- `pll_hpll` 148.5 MHz → `dclk_vop0` 148.5 MHz (video port 0, the HDMI
  port) and `clk_hdmi_ref` 148.5 MHz (the dw-hdmi PHY's reference).
- Nothing else live hangs off HPLL (`dclk_vop2` and `hpll_ph0` have no
  enabled consumers).

HPLL is a PMUCRU PLL of the RK3328 type at `0xfdd00040` (`PLL_CON16..18`):

| reg | bits |
|---|---|
| CON0 `0xfdd00040` | [14:12] postdiv1, [11:0] fbdiv; hiword write mask |
| CON1 `0xfdd00044` | [12] dsmpd (1 = integer), [10] lock, [8:6] postdiv2, [5:0] refdiv; hiword write mask |
| CON2 `0xfdd00048` | [23:0] frac, plain read-modify-write |

The kernel's setting: fbdiv 99, postdivs 4×4, refdiv 1, integer mode:
24 MHz × 99 / 16 = 148.5 MHz. The layout was cross-checked against PPLL
(`0xfdd00000`: fbdiv 100, 6×2 → 200 MHz, as `clk_summary` reports).

The dw-hdmi PHY lock bit is `PHY_STAT0` bit 0 at `0xfe0ac010` (0x3004 × 4
from the HDMI base); `0xF3` = locked, HPD high.

## Method

- **Rate:** `DRM_IOCTL_WAIT_VBLANK` (relative, +1) on `/dev/dri/card0` next
  to the running maburplay (it needs no DRM master), with a least-squares
  fit of the vblank timestamp against the sequence over 5–10 s. Precision
  about ±1 ppm over 10 s; vblank timestamps carry ~37 µs RMS of jitter.
- **Trim:** Python `mmap` of `/dev/mem`. A ppm request is converted to
  `N = 148.5 MHz × postdivs / 24 MHz × (1 + ppm)`, split into fbdiv and a
  24-bit frac, and written frac first, then fbdiv only if it changed, back
  to back.
- **PHY:** a ~1 kHz poll of `PHY_STAT0` logging every change.
- **Sink:** the operator watching the bench monitor (EDID "Mi Monitor"),
  showing maburplay's idle background with no video, one step at a time.

## Results

The vblank rate follows the PLL exactly: +500.0, −499.5, +1000.4 and
+2000.4 ppm measured for +500, −500, +1000 and +2000 requested. The PHY lock
bit never dropped, no vblank was missed and `dmesg` stayed empty in every
run, including the ones that blanked. Blanking is a sink re-sync, invisible
from the GS side.

| step | write | sink |
|---|---|---|
| integer → fractional mode, frac 0 (same rate) | CON1 dsmpd 1 → 0 | no blank |
| rewrite fbdiv with the same value | CON0 | no blank |
| −10 ppm crossing fbdiv 99 → 98 | CON2 then CON0 | **blank** |
| move the operating point to postdivs 4×3, fbdiv 74, frac 0.25 (same rate) | CON2, CON0, CON1 | **blank** (expected) |
| −500 ppm, frac only | CON2 | no blank |
| −500 → +2000 ppm in one write, frac only | CON2 | no blank |
| +2000 → −2000 → 0 ppm in 20 ppm steps, 302 writes over 55 s | CON2 | no blank |
| back to the kernel's integer 99, 4×4 | CON2, CON0, CON1 | (expected blank) |

The earlier unannotated runs agree: two blanks near the end of a sequence
whose only fbdiv crossings came at its end.

**Why the crossing blanks:** frac and fbdiv are in separate registers. For
the moment between the two writes the PLL targets the old fbdiv with the new
frac (99 + 0.999 for a −10 ppm request): a ~1 % excursion, not 10 ppm. The
sink sees it and re-syncs. A step inside one fbdiv, even 2500 ppm at once,
is tolerated.

**The fix:** choose postdivs so the trim range never leaves one fbdiv.
With 4×3 the VCO is 1782 MHz and N = 74.25; ±0.33 % (the range the
sensor-side lock uses) spans 74.005–74.495. The frac step is 24 MHz / 2²⁴
on the VCO, about 0.01 ppm.

## What this means for building it

- **Works on this board and this sink:** the fractional HPLL trim moves the
  panel rate by at least ±2000 ppm with no blank, as long as only CON2 is
  written. The drone-side steer clamps at ±200 mfps (±3300 ppm at 60 fps);
  the 4×3 operating point reaches −3367 / +10100 ppm before an fbdiv
  crossing. Only the sink's tolerance limits the range: ±2000 ppm is all
  that was tried.
- **The operating point is per mode.** Each pixel clock needs postdivs
  that put N mid-integer with margin for the trim (297 MHz at 1080p100/120:
  6 → 74.25; 148.35 MHz at 59.94: 4×3 gives 74.176, too close to 74, so it
  needs another pair). Moving to the operating point blanks once, so it
  belongs in the kernel's rate table (a modeset blanks anyway), or straight
  after maburplay's own modeset.
- **The kernel does not know.** The clock framework still caches 148.5 MHz
  and integer mode. A modeset at the same rate is skipped by the cache; one
  at a different rate reprograms the PLL from the table and drops the trim.
  `/dev/mem` from maburplay works on the bench; a small kernel patch in the
  GS image (frac-safe table entries plus a trim interface) is the clean
  version.
- **The servo already exists.** The phase lock measures the margin against
  the vblank grid and outputs a rate offset; a local actuator replaces the
  RCF `snr_off` path, with the sign inverted (a faster panel has the same
  effect on phase as a slower camera).
- **Not covered here:** the Orange Pi (H618 video PLL, its own spike);
  other sinks (goggles, capture devices) may re-sync at smaller steps; the
  web GS and its spotter mode present through the browser and cannot touch
  the display clock at all.
