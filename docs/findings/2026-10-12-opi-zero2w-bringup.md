# Orange Pi Zero 2W GS bring-up (Cedrus via libav v4l2request) — findings 2026-10-12

**Status:** built and running on the bench board; latency against the Radxa measured later on the phase-lock bench (`2026-10-18-opi-phase-lock-bench.md`: ~2 ms under it unlocked, 33 vs 35 ms locked). Current: docs/opi-zero2w.md.

**Question.** Does the `v4l2req` maburplay build (Allwinner H618, Cedrus
HEVC decode through the jernejsk FFmpeg 7.1 v4l2request hwaccel, DRM-PRIME
NV12 straight to `DrmPresenter`) run the bench link as a full GS, beside an
unchanged musl-static `maburgs` driving an RTL8812EU through devourer?

## Setup

- Board: Orange Pi Zero 2W, 1 GB, 4× A53 at 1.416 GHz, kernel 6.18.35
  (sunxi, DE33 VI-plane scaler patch), still on its pre-mabur SD image;
  mabur installed by hand on the overlay (`/usr/local/bin`, `/etc/*.toml`,
  init scripts), old citrusplay/wfb-ng/fpvd services renamed out of the way.
- Radio: one RTL8812EU (`0bda:a81a`) on the board's USB 2.0 host port.
- Sink: HDMI monitor (preferred 2560×1440@60), `screen_mode` 1920x1080@60.
- Drone: the bench VTX, `venc.gop_s = 0.5`, `intra_refresh_frames = 1`, no
  flight controller attached (so no MSP OSD traffic).
- Player: branch `opi-zero2w` (`backend = "v4l2req"`, `dvr.mode = "raw"`,
  colortrans off, `decoder.stream = false`, `[input.rec]` removed).

## Results

**The drone's stream has no IRAP mid-session.** A NAL-type histogram over
300 ring AUs: 150 × TRAIL_N, 131 × TRAIL_R, 19 × VPS+SPS+PPS+TRAIL_R (the
`gop_s` refresh). The first backend build gated decode on `au_is_irap()`
and decoded nothing: `--decode-only --seconds 30` → `frames=0`,
`gated=1799`. Gating on an entry point instead (an IRAP or an
SPS-carrying AU, `mabur::au_is_entry_point`) fixed it.

**Decode-only, 30 s:** `frames=1799 fps_active=60.15 errors=0
errors_after_sync_3s=0`, `shape_drops=0`, `gated` 0–23 at join (under
0.4 s). Cedrus exports single-object linear NV12, which `prime_to_dma`
accepts as is.

**Display.** `DrmPresenter: topology: video on overlay NV12 plane 33 at
zpos 1; OSD plane 39 (primary, ARGB8888 linear 1920x1080, blend driver
default) at zpos 2; no backdrop`. `CUBIC_LUT absent`. `PAGE_FLIP_ASYNC
rejected (EINVAL) -- latched to vsync-paced flips`. Regulator `vsync=locked`,
`present_jitter` 0.02–0.05 ms at steady state.

**Latency (`lat:` lines, steady state, p50/p99 ms):** `e2e=33..35/36..51
enc=8 dq=0 air=1..2 fec=5 dec=6..7/8..11 reg=3..7 dsp=5/5`. Early lines
after a link change read higher (`e2e` 46–74 while `air`/`fec` settle).
No Radxa run was taken in the same session, so this is not yet a parity
comparison.

**GS OSD cost** (`--fps-log`, 35 s each, `[osd.gs]` on vs off): fps 59.96 vs
59.98, flips/s 59.20 vs 59.58, present jitter 0.05 vs 0.02 ms, regulator
skips 6 vs 0. The MSP OSD was not exercised (no FC on the bench drone).

**Raw DVR.** A recording started mid-session wrote `0 samples`: `RawDvr`
opens a file only at an IRAP, and the drone sends one only at session start
or an encoder reconfiguration. Restarting maburd during a recording gave
an IDR and a 4,510-sample, 84 MB `record-0001.mp4`; ffprobe reads 4,388
1920×1080 HEVC frames, ffmpeg decodes it end to end (a few
missing-reference messages from link loss), and a frame at 30 s is clean.

## Conclusions

- The v4l2req build works end to end on the H618 at 1080p60 with zero
  hard decode errors; the IRAP-only gate was wrong for this encoder and is
  now an entry-point gate.
- The display path needs no H618-specific code: the presenter's existing
  probes pick the right planes, and the vsync servo locks in paced mode.
- Raw DVR on this board starts only at a real IRAP, which mid-session
  means the next encoder reconfiguration; `dvr.autostart = true` records
  from session start.
- Open: latency parity against the Radxa (same drone, rung and mode),
  the visual checks (full-screen scaling, OSD alpha, colour), and the
  rebuilt SD image (CONFIG partition on an MBR card) — not run here.
