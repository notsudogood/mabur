# An SSC378QE air unit (IMX415 + RTL8812EU): design note and status

Status 2026-11-02: **built, never booted.** openipc-builder has a device
`ssc378qe_fpv_generic-imx415-8812eu` (release tag `efficient-link-i6c-a1`)
whose maburd cross-compiles and whose encoder process cross-compiles, on
the toolchain OpenIPC uses for this SoC. No one here owns the board yet, so
nothing below has run on silicon: not the boot, not the radio, not the
camera. Read every number as "expected", not "measured".

## What the SoC is, and why that matters

The SSC378QE is SigmaStar's **Infinity6C**, not a faster SSC338Q:

- **One CPU core** (the SSC338Q has two). The kernel config OpenIPC ships
  for it says `CONFIG_BROKEN_ON_SMP=y`; waybeam's profiling calls it
  "single-core" outright.
- **Kernel 5.10.61, musl libc** (the SSC338Q: 4.9.84, glibc). Everything
  mabur builds for it is built on the musl toolchain.
- **A different MI SDK.** No VPE (scaling is the SCL block, the ISP is
  driven directly), every `MI_SYS` entry point takes a leading SoC id, the
  whole stack is one `mi.ko` instead of one module per block, and the user
  3A runs on the ARM core because there is no 3A coprocessor.
- **USB 2.0 only** (EHCI), which is also what the SSC338Q boards give the
  radio in practice.

mabur's own encoder (`drone/venc/`, waybeam's Star6E pipeline folded in) is
the SSC338Q's pipeline and does not run here. Porting it is weeks of work
plus a board to test on. What this image does instead:

## The arrangement: waybeam as the encoder, maburd as the link

```
IMX415 -> waybeam_venc (Maruko backend, its own process)
             |  whole H.265 frames, frame-shm ring /mabur_f (format v2)
             v
          maburd (built with MABUR_INPROC_VENC=OFF)
             |  FEC, SBI framing, NACK, RC, telemetry, MSP OSD
             v
          RTL8812EU via devourer  ---air--->  the ground station, unchanged
```

- Upstream OpenIPC waybeam_venc has a **working Maruko backend** for this
  exact SoC, bench-verified by its authors on an SSC378QE + IMX415, and it
  can publish into the same frame-shm ring mabur read from before the
  fold-in. `openipc-builder/package/waybeam-venc` builds it (commit
  0c880d8, 0.84.0) and installs `/etc/waybeam.json` + `S01waybeam`.
- maburd's hot thread already reads that ring (`FrameSource`). Two things
  changed for this image, both tested on the host:
  - the ring consumer accepts **format version 2** (waybeam 0.69+); v2
    carries GDR counters where v1 has `enc_us`, so `enc_us` reads 0 from a
    v2 ring (the "enc" latency segment is unavailable on this image);
  - `mi_ready.h` accepts the unified `mi` module as "encoder live".
- The rate-control verbs (`set_bitrate`, `set_fps`, `set_roi_qp`,
  `request_idr`) go to waybeam's HTTP API on 127.0.0.1:80 through
  `drone/src/ext_venc.cpp` -- `GET /api/v1/live/set?video0.bitrate=N`
  (applies live, no flash write) and `GET /request/idr`. Each call is
  bounded to ~600 ms worst case and a failure is a counted `false` that
  RcAgent retries on its next tick. `MABUR_EXT_VENC=host:port` in the
  environment moves the API address.
- Boot order: S00mabur starts maburd, which insmods the MI stack
  (`load_sigmastar -i`) under its radio bring-up as on the SSC338Q;
  S01waybeam waits for `mi` + a sensor driver to be live (30 s cap) and
  starts waybeam, which creates the ring; maburd attaches to it when it
  appears and reattaches across waybeam restarts.

### The two configs must agree

`/etc/mabur.toml` `[venc]` `fps` / `size` / `slices` and `/etc/waybeam.json`
`video0.fps` / `video0.size` / `video0.sliceCount` describe the same
stream and are edited by hand on both sides; maburd stamps the slice
geometry it was configured with onto every frame it forwards. On this image
`slices = 3`, not the SSC338Q's 4: waybeam splits in 32-pixel rows
(`ceil(34/n)`) and mabur stamps 64-pixel CTU rows (`ceil(17/n)`), and 3 is
the smallest count where both land on the same boundaries (12 half-rows =
6 CTU rows); 4 would have waybeam ask for 9 half-rows against mabur's 10.
The GS reads real slice addresses from the slice headers, so a mismatch
would mis-stamp, not mis-decode -- but it is not worth flying.

waybeam treats `gopSize: 0` as a GOP of **one frame** (every frame intra),
the opposite of mabur's `gop_s = 0` (no periodic keyframe; the GS asks),
so waybeam.json carries a long GOP in seconds instead.

## What this image does not have (vs the SSC338Q images)

- **No Camera-tab presets** (the GS menu's 1080p90 / 100 / 120 / Crop 60):
  the mode table and the sensor driver switch are in-process venc code.
  The GS sees no offered modes.
- **No onboard (VTX) recording**: `venc_record` is in-process.
- **No phase-lock sensor steering and no cold-boot sensor trim**
  (`sensor_steer` / `sensor_cal`): the goggles' rate match falls back to
  its unsteered behaviour.
- **No in-process scene detector**: waybeam's own runs (`sceneThreshold`),
  off by default here.
- **No `enc_us`** in the latency rows (above).
- The GS's first-boot floor calibration, TX-power calibration (`maburcal`:
  radio only), channel hop, FHSS, NACK, FEC, MSP OSD, telemetry and the
  playback/recording features on the GS side are all unchanged.

## What to expect, and what will decide it

**CPU is the question.** Everything mabur does per frame on the SSC338Q
runs on one of two cores at 45 Mb/s (cpu0 p90 85 %, cpu1 p90 38 %, flight
2026-11-01) -- and the SSC338Q's 3A runs on a coprocessor. On the SSC378QE,
waybeam measured **60 % of the single core at 90-100 fps for the ISP, 3A
and encode output alone**, at 16 Mb/s, before any FEC or radio work (its
`documentation/MARUKO_CPU_PROFILE.md`; one bench board, one tuning). The
likely outcome is that this board carries a 60 fps 1080p link at a lower
bitrate than drone 1 -- 20-25 Mb/s is a guess, not a measurement -- and
that the MCS4/80 top rung is CPU-bound rather than air-bound. waybeam's
`isp.aeEngine = "custom"` (a 15 Hz manual AE instead of the SDK's
per-frame one) saved it ~24 % of the core at 120 fps and is the first knob
to try if the link starves; it costs exposure responsiveness.

**The sensor driver.** The image uses OpenIPC's stock Infinity6C IMX415
driver (modes 3840x2160@30, 2560x1440@60, 1920x1080@90, 1472x816@120).
waybeam's own Maruko IMX415 driver documents that the vendor binned 1080p
tables run a 4.9 µs line (HMAX 365) whose burst exceeds this ISP's drain
and "only ever delivered 115-118 of 120 fps"; the stock driver's 1080p90
table has the same HMAX, so 60 fps from it is a risk, not a given.
waybeam's fixed driver (`drivers/sensor_imx415_maruko.c`, 1920x1080@100
non-binned at 1485 Mbps) is the fallback and needs a kernel-module package
built against this image's kernel.

**The SDK libraries.** waybeam's Maruko code was verified against MI
libraries pulled from an OpenIPC image of early 2026; this image installs
OpenIPC's current ones (synced 2025-10-31 upstream), which differ by hash.
A struct-layout change between them would show up as the pipeline refusing
to start -- `/tmp/waybeam.log` is where it would say so.

**The board.** DTB (vendor demo board's), flash size (16 MB assumed), the
flight-controller UART (`/dev/ttyS2` assumed) and the USB port the radio
sits on are all unknowns until a board is in hand. `openipc-builder
docs/ssc378qe-first-flash.md` lists what to read off the stock firmware
first.

## First boot: what to look for

```
ssh root@<board> 'grep -E "video: external encoder|MI modules|radio" /tmp/mabur.log | head; \
  grep -E "Frame-SHM|sliceSplit|ERROR" /tmp/waybeam.log | head; \
  curl -s 127.0.0.1/api/v1/version; echo; \
  ls -la /dev/shm/; cat /proc/loadavg; head -1 /proc/stat'
```

- `video: external encoder -- frames from frame-shm /mabur_f ...` from maburd.
- `> [maruko] Frame-SHM output: mabur_f (384 KB slots)` and a
  `sliceSplit ... requested=3` line from waybeam, no `ERROR`.
- The GS links and shows video; then the CPU: `top -d 5` on the board
  while streaming at the top rung the link reaches, read as busy % of the
  one core (the load average is meaningless here -- the SDK's kernel
  threads sit in D-state and inflate it).

## Later, if the board earns it

The in-process port: fold waybeam's `maruko_*.c` (~15 k lines; the
pipeline alone is 5.2 k) into `drone/venc/` the way the Star6E backend
was, behind the same `venc_core.h` facade, bringing back the Camera tab,
VTX recording, sensor steering and `enc_us`. Weeks, with the board on the
bench throughout. Not before the two-process image has shown the CPU can
carry a useful link.
