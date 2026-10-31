# Camera modes: frame rate, resolution and field of view

The drone's IMX415 sensor offers a handful of readout modes, and `venc.fps`
picks one. This page says which, what each costs in field of view, which
`venc.size` values each can serve, and what the ground station has to do to
show it smoothly.

## The driver

Gilang's drone image loads waybeam_venc's star6e IMX415 driver,
`sensor_imx415_star6e.ko` (from `../waybeam_venc/sensors/star6e/`, waybeam
0c880d8), instead of OpenIPC's stock `sensor_imx415_mipi.ko`. This branch's
image (notsudogood/openipc-builder) ships both and loads the stock one
unless `/etc/sensor_star6e` exists; maburd writes that file when the GS
menu's Camera tab asks for a preset only waybeam's driver has (see "Camera
tab" below). The waybeam driver enumerates five modes:

| idx | mode | readout | sensor window | share of the view (w × h) | aspect |
|---|---|---|---|---|---|
| 0 | 3840x2160 @ ≤ 33 | all pixels, not binned | 3840x2160 | 100 % × 100 % | 16:9 |
| 1 | 2816x1584 @ ≤ 60 | crop, not binned | 2952x1656 → 2816x1584 | 73 % × 73 % (1.36x zoom), with `venc.sensor_crop` | 16:9 |
| 2 | 1920x1080 @ ≤ 90 | 2x2 binned | 3840x2160 | 100 % × 100 % | 16:9 |
| 3 | 1728x972 @ ≤ 100 | 2x2 binned crop | 3456x1944, centred | 90 % × 90 % (81 % of the area) | 16:9 |
| 4 | 1728x816 @ ≤ 120 | 2x2 binned crop | 3456x1632, centred | 90 % × 76 % | 2.12:1 |

maburd finds the mode it wants in that list by output size and maxFps
(`drone/venc/camera_modes.c`, `sensor_select.c`), never by index, and fails
boot with `ERROR: sensor mode WxH@F not offered by the driver` when the
loaded driver lacks it (the stock driver lacks modes 3 and 4). Check what
is running with `grep Cur /proc/mi_modules/mi_sensor/mi_sensor0`.

## Which mode `venc.fps` selects

| `venc.fps` | mode | view |
|---|---|---|
| 1–90 | 1920x1080 binned | full |
| 91–100 | 1728x972 binned crop | 1.11x zoom |
| 101–120 | 1728x816 binned crop | 1.11x wider, 1.32x taller zoom |
| any, with `record.size` above 1920x1080 | 3840x2160 (the loader then requires `venc.fps` ≤ 30) | full |
| 1–60 with `venc.sensor_crop = true` (d18) | 2816x1584 unbinned crop | 1.36x zoom |

### The unbinned crop (`venc.sensor_crop`)

Every mode up to 90 fps reads the sensor 2x2 binned: four photosites become
one pixel before the ISP sees them, which halves the resolution the sensor
delivers. Mode 1 is the one fast mode that is not binned: it reads every
photosite of a centred 2952x1656 window (output 2816x1584) at up to 60 fps,
and the VPE scales it down to `venc.size`. So a 1920x1080 picture is made
from ~1.5x more real pixels per axis than the binned full view, over a
narrower view (73 % of the width and height).

What it costs, none of it measured on this drone yet:

- **View**: 1.36x zoom against the full view (the GS menu cell says 1.4x).
- **ISP rate**: 2816 × 1584 × 60 = 268 Mpix/s through the ISP, against
  124 for the binned 1080p60 and 274 for the 4K mode at 33 fps. Nearest
  thing run before is that 4K mode; this one has not been benched.
- **Noise**: a binned pixel averages four photosites; an unbinned one does
  not, so per-pixel noise is higher before the VPE's downscale averages
  some of it back. The sensor tuning file (`venc.sensor_bin`) was made on
  the binned mode. Expect a noisier picture in low light, unmeasured.
- **Phase lock**: the mode's ceiling is its nominal 60 fps, so the steer
  can only slow the sensor down (as with 1728x972 @ 100). A panel a hair
  faster than 60 Hz leaves the lock out of range (fps cell in caution).
- **Encoder**: the same 1920x1080 @ 60 as the full view (~117 % of rated).
- **Driver**: waybeam's only. The stock driver's crop is 2560x1440 @ 60, which
  the code does not use; on the stock driver the preset reads "new driver".

The loader refuses it above 60 fps, with a `record.size` above 1920x1080,
and with a `venc.size` that is not 16:9 or larger than 2816x1584.

The full view stops at 90 fps because of the sensor, not the encoder: a
frame is VMAX lines × the line time, full height binned needs VMAX ≥ 2250,
and a full-width binned line needs HMAX ≥ ~365 (4.92 µs) or the readout is
malformed (black frames with coloured lines at the right fps). 74.25 MHz /
(2250 × 365) = 90.4 fps. Neither clock nor bit depth moves either floor.
At HMAX 365 a 100 fps frame holds 972 binned lines and a 120 fps frame 816,
which is where modes 3 and 4 come from (waybeam's device mapping:
`../waybeam_venc/documentation/STAR6E_IMX415_HEADROOM.md` §2, §5.6). The
measured views: `docs/findings/2026-10-19-waybeam-imx415-1728x972.md`.

Mode 4's real ceiling is 119.660 Hz (VMAX 1700 at the line rate), not 120:
at `venc.fps = 120` the rate trim locks at the mode's ceiling
(`[sensor] cal: locked at the mode's ceiling`). The trim, the shutter cap
above 90 fps and the steer's ceiling clamp are in `docs/airtime-model.md`
§3 and `docs/phase-lock.md`.

## Sizes

The VPE only scales the mode down; it never crops or upscales it. So
`venc.size` must have the selected mode's aspect ratio and fit inside it
(even, at least 320x180), or the loader refuses the config:

- 1–90 fps: any 16:9 size up to 1920x1080 (1280x720, 1920x1080).
- 91–100 fps: any 16:9 size up to 1728x972 (1280x720, 1728x972). 1920x1080
  is refused (larger than the mode).
- 101–120 fps: the 1728:816 shape up to 1728x816 (1728x816, 1152x544,
  864x408). 1280x720 is refused (another aspect).

Encoding the mode's own size keeps all its detail but costs encoder time:
1728x972 at 100 fps reads 158 % of the H.265 engine's rated macroblock rate
(1280x720 at 100 fps reads 86 %); it held 100 fps on the bench with the
480 MHz encoder clock, with enc 6/8 ms p50/p90 against 4/5 ms at 1280x720.

## The ground station

The GS output has to run at the drone's rate for the phase lock to hold
(`docs/phase-lock.md`): set `screen_mode` to a mode the display offers at
that rate. The player picks only modes the sink's EDID lists, within 0.5 Hz
of the request, and otherwise falls back to the sink's preferred mode (a
log line). Known sinks:

- Avatar Goggles X: 1280x720@100.000 and 1920x1080@100.000 only above
  60 Hz; range limits 50–76 Hz otherwise, so no 90 Hz. Use
  `screen_mode = "1920x1080@100"` with a 91–100 fps drone.

When the drone's measured rate and the panel's refresh are too far apart
for the phase lock (more than 0.25 Hz: a wrong `venc.fps`, or a sink that
fell back), the OSD fps cell reads `source/panel` in caution (`100/60`),
the phase lock pauses and sends nothing, and the player logs
`maburplay: rate mismatch`. A drone in low-power is never a mismatch. If
the rates match but the lock still gives up (the steer cannot reach the
panel, e.g. a panel a hair faster than a mode's ceiling) the fps cell turns
caution without the `/`.

A picture whose aspect differs from the panel (mode 4 on 16:9) is
letterboxed with black bars on the screen, and in the burned DVR when its
size (`dvr.burned.size`) has another aspect, never stretched
(`gs/player/src/fit_rect.h`). The burned DVR's default size is the
decoded picture's own, which needs no letterbox.

## The GS burned recorder at 100 fps

At a 100 Hz panel the GS's burned DVR cannot encode every vblank and
costs live view ~4 ms while it records; run it at `dvr.burned.every = 2`.
Details: `docs/colortrans.md` ("Known limitations").

## The SD recorder above 90 fps

`[record]` keeps its own rules (`docs/vtx-recorder.md`): `record.fps` ≤
`venc.fps`, `record.size` with `venc.size`'s aspect. Above 90 fps:

- A `record.size` larger than the mode (the bundle's 1920x1080 against
  1728x972) is an upscale on VPE port 1.
- A `record.fps` that does not divide `venc.fps` (60 from 100) gives the
  recording uneven frame spacing: the bind's frame-rate limiter passes 60
  of every 100 frames.
- The H.265 engine runs one encode at a time and the live 1728x972@100
  encode alone is past its rated rate, so a recording adds encode time to
  the live link.

## Camera tab (GS menu)

The goggles' menu offers five presets (`common/include/mabur/camera_presets.h`):

| Preset | venc | Sensor mode | View | Driver |
|---|---|---|---|---|
| 1080p 60 | 1920x1080 @ 60 | 1920x1080 binned | full | either |
| 1080p 90 | 1920x1080 @ 90 | 1920x1080 binned | full | either |
| 720p 100 | 1280x720 @ 100 | 1728x972 binned crop | 1.1x zoom | waybeam's |
| 544p 120 | 1152x544 @ 120 | 1728x816 binned crop | wide crop (2.12:1) | waybeam's |
| Crop 60 (d18) | 1920x1080 @ 60, `sensor_crop = true` | 2816x1584 unbinned crop | 1.4x zoom | waybeam's |

Encoder load against its rated macroblock rate: 1080p 60 ~117 %, 1080p 90
~176 % (never benched), 720p 100 86 % (its 1728x972 capture held 100 fps
at 158 % on the bench), 544p 120 ~71 % (not run), Crop 60 ~117 % like
1080p 60 (its ISP side is unbenched, above). Encoding 1440p itself is not
offered: 2560x1440 @ 60 would ask the encoder for ~2x its rated rate. Crop
60 and 1080p 60 share size and rate; `venc.sensor_crop` tells them apart,
and every preset writes it.

A switch is a reboot: the encoder stays claimed until then. maburd
(`drone/src/camera_switch.h`, `camera_ctl.h`):

1. refuses while the FC reports armed, or while a reporting FC has gone
   quiet (`armed`); with no FC connected it cannot tell and allows it;
2. refuses a preset neither driver has (`unavailable`), or one the rest of
   `mabur.toml` does not load with (`bad_config`);
3. writes the new file as `/etc/mabur.toml.cam-new`, checks it loads, keeps
   the running one as `.cam-prev`, writes `.cam-pending` (and
   `/etc/sensor_star6e` if the driver must change), renames the new file
   into place and reboots 1.5 s later. `venc.slices` moves to the nearest
   legal count for the new height (544 lines: 1, 2, 3, 5 or 9), and
   `record.size` follows the new picture's shape when it differs;
4. on the next boot, keeps the switch once 100 frames encode inside 25 s;
   otherwise (or when the encoder does not start) puts `.cam-prev` back,
   removes the driver flag it set, and reboots again (`rolled_back`).

`load_sigmastar` falls back to the stock driver (and deletes the flag) when
waybeam's will not load, so a bad driver ends in a rollback, not a boot
loop. Once a switch has loaded waybeam's driver it stays loaded for every
preset (it has all four modes); delete `/etc/sensor_star6e` and reboot to
go back to stock.
