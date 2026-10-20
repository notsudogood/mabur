# colortrans — reversing the drone's ColorTrans sensor tuning on the GS

The drone's `venc.sensor_bin` (`imx415_greg_fpvXIX_colortrans.bin`) flattens the
picture on purpose (a 3x3 "ColorTrans" matrix plus a luma offset in the ISP)
so the encoder spends bits evenly. `maburplay` undoes it in three places (the
live picture by one of two mechanisms, whichever the display hardware has), all from
one evaluator, `gs/player/src/colortrans.{h,cpp}` (`kColorTrans3`, a
transcription of the operator's `colortrans3.glsl`); the web GS carries a
pinned JS/GLSL copy for its video:

| Where | Mechanism | Cost |
|---|---|---|
| Live picture (RK3566) | VOP2 CRTC `CUBIC_LUT`, 9x9x9, 12-bit entries, built by `build_cubic_lut()` and attached on every modeset commit (`drm_presenter.cpp`) | none (scanout hardware) |
| Live picture (H618) | the video plane's CSC: `affine_3x4()` folds `forward()` into one 3×4, attached as the DE33 VI plane's `CTM` property (Orange Pi image kernel patch `0002`), which the kernel combines with its YUV→RGB table. The OSD plane never passes through it, so the OSD is not pre-inverted. Used only when the CRTC has no CUBIC_LUT; a kernel that refuses the matrix (out of register range) costs one log line and a flat picture (`DrmPresenter: kernel refused the video-plane CTM`) | none (scanout hardware) |
| OSD (both overlays) | RK3566 only (the LUT covers the OSD planes too): pre-inverted at the source: `OsdFont::set_inverse()` on the MSP atlas, `set_colour_inverse()` in `gs_draw` for the GS tokens and shadow | once at startup |
| Burned DVR | `FrameColorTrans` (`frame_colortrans.cpp`): NV12 dmabuf -> GLES2 shader (colortrans3 verbatim) -> ARGB GBM target -> RGA -> NV12, on the recorder's ct thread, pipelined with `encode()` (see "Keeping the burned stage off the display path" below) | ct thread only; `burn_ctfb=` on the fps-log counts frames that fell back to flat, `burn_ct_ms=`/`burn_enc_ms=` are the per-window mean stage times |
| Web GS video (`web/`) | `web/ui/src/lib/colortrans.js`: each decoded `VideoFrame` -> `texImage2D` -> the same shader on a WebGL canvas stacked over the flat 2D one; the MSP OSD is its own canvas above, so nothing is pre-inverted. Page toggle, see `docs/web-gs.md` ("Colour correction") | one GPU pass per frame; off = the plain 2D `drawImage` path |

Config: `[colortrans] enable = true|false` in `maburplay.toml` (default false in
code, true in the bundle). Retuning = edit `kColorTrans3` in `colortrans.cpp`
AND the constants in `kFrag` in `frame_colortrans.cpp`, rebuild, redeploy — AND `CT_PARAMS` in `web/ui/src/lib/colortrans.js`;
`tests/test_colortrans.cpp` pins the C++ side to the glsl reference values,
`web/tests/colortrans.test.mjs` pins the web copy to the same values and to
`colortrans.h`'s defaults (a retune that misses the page fails it).
The H618's 3×4 is derived from `kColorTrans3` at startup, so a retune needs
no extra edit there; a retune to `gamma != 1` is not affine and leaves the
H618 flat (logged).

The ct thread also SCALES: the burned DVR's picture is the OSD surface's
size, not the decoded size, because the encoder lays the OSD 1:1 from the
top-left while the panel stretches the video under it — recording at the
decoded size would put the OSD offset and clipped on a 720p drone with a
1080p panel. With colortrans the shader draw scales (target = OSD size,
LINEAR sampler); without it, one RGA NV12 resize. So the ct thread runs
whenever there is an OSD region, colortrans or not, and a 720p drone
records a 1080p file.

## Kernel facts — RK3566 (GS kernel 6.1.84, `rockchip_drm_vop2.c`)

- Entries are 12-bit (`& 0xfff`), 729 per table; the blob is 729
  `struct drm_color_lut` (16-bit fields, only the low 12 bits used).
- The driver keeps a RAW pointer to the last applied blob's data and
  re-applies it on the next modeset even after the blob is destroyed;
  clearing the property to 0 does not disable the LUT. So maburplay ALWAYS
  installs a table (identity when `enable = false`) and keeps the blob alive
  for its whole run.
- No `CTM`/`DEGAMMA_LUT` on this CRTC; `GAMMA_LUT` (1024) is unused here.
- Axis order: **red varies fastest**, `index = r + 9g + 81b`
  (`gs/player/src/colortrans.cpp`, `cubic_lut_index()`, `LutAxis::kRedFastest`
  — the shipped default). Confirmed 2026-09-17 on the GS against real VOP2
  hardware: with the overlay up and the LUT installed
  (`DrmPresenter: CUBIC_LUT present (size 729)` followed by
  `colortrans: display LUT on (axis=rgb), OSD pre-inverted`), the REC
  marker read RED and body text read WHITE. A `kBlueFastest` table would
  have swapped the marker to blue and cast the text.
  Re-run this check on any new kernel — the axis is an undocumented VOP2
  implementation detail, not a spec guarantee. The check itself: boot the GS
  with the overlay up (the drone can be off) and look at the REC
  marker/"fault" colour; RED is correct, BLUE (or an obvious cast on body
  text) means the axis is inverted. Two field remedies need no rebuild —
  export `MABUR_COLORTRANS_AXIS=bgr` from `S97maburplay` to flip the axis
  persistently, or set `[colortrans] enable = false` to turn the whole
  feature off. The permanent fix is to change the
  default in `main.cpp` (`lut_axis = LutAxis::kBlueFastest`), update the
  `axis=` string in the same log line, rebuild, and redeploy.

## Kernel facts — H618 (Orange Pi image kernel 6.18.35, `sun8i_csc.c`)

- No `CUBIC_LUT`. Each VI channel has a 15-word CCSC: three pre-offsets
  (10-bit units, subtracted), a 3×3 in S2.17, and a post-constant per
  row. The image kernel's patch `0002` adds a `CTM` blob property to the
  VI planes: 12 × u64, row-major `[A | b]`, S31.32 sign-magnitude, on RGB
  in [0,1], folded in as `C' = A·C`. The post-constants cannot carry a
  negative offset (set to −0.345 of full scale the picture saturates to
  white), so the offset `b` goes into the **luma pre-offset** instead:
  raising `d_Y` by Δ shifts every output row by `−C'[r][Y]·Δ`. That holds
  `b` exactly when it is a grey shift, which colortrans's lift is
  (`kColorTrans3`: `d_Y` 64 → 190, U/V stay 512, post-constants stay 0).
  Unset = the stock table, bit for bit. The plane's default encoding is
  BT.709 limited.
- Coefficients are signed 20-bit: |coefficient| < 4.0, and a larger one
  wraps rather than saturates (black picture, or a colour cast that
  flips). `kColorTrans3` combined with BT.709 limited peaks at 3.41
  (Cb→B). The output clamps at full scale. A CTM that does not fit — a
  coefficient of 4.0 or more, an offset that is not a grey shift, a `d_Y`
  outside 0..1023 — fails the commit with `-ERANGE`.
- Plane state outlives the process that set it: a `CTM` left on the plane
  by one run stays applied under the next. So `maburplay` states the
  property on every modeset and splash-handoff commit — the matrix, or 0
  when colortrans is off or the kernel refused it — the same reason a LUT
  CRTC always gets a table.
- The 3×4 skips `forward()`'s clamp after the camera matrix: same mean
  error as the RK3566's 9×9×9 LUT (4.0 vs 3.7 LSB), worse on strongly
  saturated colours (max 36 vs 20 LSB). Record:
  `docs/findings/2026-10-12-opi-colortrans-probe.md`.

## Build

maburplay is glibc-dynamic (Mesa EGL/GBM cannot be linked
statically). `tools/build-arm64.sh` stage 6 builds it with the Buildroot SDK
in `../sbc-groundstations-gilankpam/output/radxa_zero3_defconfig/host`
(`MABUR_BR_HOST` overrides). That sysroot must contain mesa3d + librga:

    cd ../sbc-groundstations-gilankpam
    make -C buildroot O=$PWD/output/radxa_zero3_defconfig BR2_EXTERNAL=$PWD radxa_zero3_defconfig
    make -j$(nproc) -C buildroot O=$PWD/output/radxa_zero3_defconfig BR2_EXTERNAL=$PWD mesa3d librga

The defconfig must also set `BR2_PACKAGE_MESA3D_LLVM=y` (already committed in
the sbc-groundstations repo). Without it, Buildroot's Kconfig **silently
drops** `BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_PANFROST` — panfrost hard-`depends
on` LLVM in Buildroot 2025.08.1 — which in turn drops EGL, GBM and GLES from
the built sysroot. The `make mesa3d librga` step above still reports success;
there is no error, only a sysroot missing every header stage 6 links against.
Anyone regenerating this defconfig from scratch should set the LLVM symbol
explicitly before building, not just add panfrost/EGL/GLES and expect them to
stick.

Run `tools/build-arm64.sh` **bare** — do NOT wrap it in the
`nix-shell -p pkg-config libusb1` that this repo's own CLAUDE.md prescribes
for host cmake/ctest work. That wrapper does not apply to this script: when
`libusb1` is present in the shell, Nix's pkg-config wrapper overrides
`PKG_CONFIG_LIBDIR` unconditionally, which leaks the host's libusb into stage
6's Buildroot-sysroot build underneath it and silently points the glibc build
at host x86_64 headers instead of the Buildroot sysroot. The script detects
this leak and fails loudly with an explanatory message rather than let CMake
surface a confusing "path does not exist" error — but the safe move is to
just not wrap the script in the first place.

`out/arm64/maburplay-static` is the musl build from stage 5 (no GPU stage;
with colortrans on it records flat and `burn_ctfb` climbs) — the rollback
build. maburgs stays static.

Confirm the dynamic build's actual link list with:

    ../sbc-groundstations-gilankpam/output/radxa_zero3_defconfig/host/bin/aarch64-none-linux-gnu-readelf -d out/arm64/maburplay | grep NEEDED

which on the current build reads: `libEGL.so.1`, `libGLESv2.so.2`,
`libgbm.so.1`, `librga.so.2`, `libdrm.so.2`, `librockchip_mpp.so.1`, plus the
usual `libstdc++.so.6`, `libm.so.6`, `libgcc_s.so.1`, `libc.so.6`, and the
loader `ld-linux-aarch64.so.1`.

## Keeping the burned stage off the display path

A recording must not cost the live picture a vsync. What makes that hold:

- **Explicit in-fence on video-plane commits.** The GPU stage imports the
  decoder's NV12 dmabuf, and panfrost attaches a write fence to every BO in
  a job; through the shared `dma_resv` the KMS commit worker would wait on a
  job that only reads. `DrmPresenter` mints one already-signalled
  `sync_file` (a `DRM_SYNCOBJ_CREATE_SIGNALED` syncobj on the panfrost
  render node; rockchip-drm lacks `DRIVER_SYNCOBJ`) and sets it as
  `IN_FENCE_FD` on every video-plane commit, so the plane helper takes no
  implicit fence. Startup log:
  `DrmPresenter: video plane commits carry an explicit IN_FENCE_FD (...)`.
- **Pipelined ct thread.** The stage runs on its own thread (EGL is
  thread-bound) with three destination buffers and a latest-wins handoff to
  the encode thread (`burn_recorder.h`, THREADING).
- **GPU governor pinned to `performance`** for the stage's lifetime
  (simple_ondemand never clocks up at a pipelined stage's utilisation);
  restored at stop, logged both ways (`FrameColorTrans: GPU governor …`).
- **Import cache.** The EGLImage import of the decoder's dma-buf is cached
  per source buffer (the decoder pool is 24 fixed buffers), so imports
  happen once per buffer per recording, not per frame.
- **Palette memoised at boot** (`maburplay: burn palette built: N entries`),
  so a record start does not histogram the MSP atlas on the main loop. A
  record start/stop still costs a one-shot GPU bring-up/teardown.
- **Schedule-based `dvr.burned.fps_cap`** (`gs/player/src/fps_cap.h`,
  `test_fps_cap`): a frame is admitted when its slot is due and slots
  advance one interval per admit, so a source at or under the cap passes
  whole even though decoded frames arrive in pairs.
- **File I/O off the encode thread.** The mux writes through
  `mabur::ThreadedSink` (`common/include/mabur/threaded_sink.h`,
  `test_threaded_sink`). A closing fragment is ~2.5 MB at 20 Mbit/s and
  takes ~22 ms to write to the GS's vfat card. The encode thread's budget at
  60 fps is 16.7 ms with ~14 ms of encode, so written in line it would
  displace the next frame at the `ct_out` handoff, once per fragment. The
  writer thread (`mbr-dvrw`) trickles: each queued write reaches the file
  in 64 KB slices with an 8 ms sleep between them (`ThreadedSinkPacing`),
  so a fragment's kernel work (the page-cache copy and a FAT buffer head per
  512-byte sector) is spread over ~320 ms instead of landing in one ~22 ms
  burst. The burst was the live picture's cost: with the encoder, the GPU
  stage, the decoder and the writer all busy at once, the main loop woke a
  few ms late, or its commit ioctl or the decode ran long, and the frame
  missed its vblank about once per 45-90 s of recording. Lowering the
  writer's priority does not prevent that (a thread that just woke from a
  long sleep keeps the CPU against the loop's wake-up until the tick), so
  the writer runs 10 nice steps below the player only as a tie-breaker.
  The gaps stop while the queue holds 4 MB or more (the card is slower than
  the trickle: catch up) and during the sink's destructor (record stop
  drains it on the main loop). Record of the search and the gate:
  `docs/findings/2026-10-19-burned-dvr-live-repeat.md`.

- **A longer vsync lead while recording.** The trickle halves the live
  cost but does not remove it: the main loop still wakes 3-6 ms late a few
  times a minute while the recorder runs, and the 5 ms lead
  (`display.vsync_lead_ms`) leaves ~3 ms to the latch. At record start the
  regulator switches to `display.rec_vsync_lead_ms` (bundle 8) and back at
  stop (`maburplay: rec: vsync lead 5 -> 8 ms for the recording`); the phase
  lock re-steers the sensor to the moved release. +3 ms of glass latency
  while recording, 0 repeats on the bench.

`BurnRecorder: stopped` reports `ct_ms=mean/max enc_ms=mean/max`; the ct
thread logs `colortrans stage mean per frame ...: import, draw+finish, rga`
at stop. Bench affordance: `kill -USR1 $(pidof maburplay)` toggles a
recording exactly like the GPIO button, so a mid-session record start can be
measured without a hand on the GS.

Bench record of the three fixes and their numbers:
`docs/findings/2026-09-17-colortrans-recording-latency.md`.

## Not done / follow-ups

- A record start or stop blocks the main loop for 70-180 ms:
  `BurnRecorder::start()` (encoder init, palette upload) and `stop()`
  (thread joins, encoder teardown, mux close) run synchronously from
  `rec_start`/`rec_stop`.
- Live retune (config + restart is the loop).
- EGL YUV colour-space / range hints on the dmabuf import (none are passed;
  a mismatch is a slight tint between the display and the DVR).
- Raw DVR mode is the bitstream and stays flat: post-process with
  `ffmpeg -vf "libplacebo=custom_shader_path=colortrans3.glsl"`.
- The splash is not inverted.
- The stage is memory-bound (GPU + RGA traffic per 1080p frame); feeding
  the encoder BGRA directly and deleting the RGA pass is the untested next
  lever.
