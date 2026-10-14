# colortrans — reversing the drone's ColorTrans sensor tuning on the GS

The drone's `venc.sensor_bin` (`imx415_greg_fpvXIX_colortrans.bin`) flattens the
picture on purpose (a 3x3 "ColorTrans" matrix plus a luma offset in the ISP)
so the encoder spends bits evenly. `maburplay` undoes it in three places, all from
one evaluator, `gs/player/src/colortrans.{h,cpp}` (`kColorTrans3`, a
transcription of the operator's `colortrans3.glsl`); the web GS carries a
pinned JS/GLSL copy for its video:

| Where | Mechanism | Cost |
|---|---|---|
| Live picture | VOP2 CRTC `CUBIC_LUT`, 9x9x9, 12-bit entries, built by `build_cubic_lut()` and attached on every modeset commit (`drm_presenter.cpp`) | none (scanout hardware) |
| OSD (both overlays) | pre-inverted at the source: `OsdFont::set_inverse()` on the MSP atlas, `set_colour_inverse()` in `gs_draw` for the GS tokens and shadow | once at startup |
| Burned DVR | `FrameColorTrans` (`frame_colortrans.cpp`): NV12 dmabuf -> GLES2 shader (colortrans3 verbatim) -> ARGB GBM target -> RGA -> NV12, on the recorder's ct thread, pipelined with `encode()` (see "Keeping the burned stage off the display path" below) | ct thread only; `burn_ctfb=` on the fps-log counts frames that fell back to flat, `burn_ct_ms=`/`burn_enc_ms=` are the per-window mean stage times |
| Web GS video (`web/`) | `web/ui/src/lib/colortrans.js`: each decoded `VideoFrame` -> `texImage2D` -> the same shader on a WebGL canvas stacked over the flat 2D one; the MSP OSD is its own canvas above, so nothing is pre-inverted. Page toggle, see `docs/web-gs.md` ("Colour correction") | one GPU pass per frame; off = the plain 2D `drawImage` path |

Config: `[colortrans] enable = true|false` in `maburplay.toml` (default false in
code, true in the bundle). Retuning = edit `kColorTrans3` in `colortrans.cpp`
AND the constants in `kFrag` in `frame_colortrans.cpp`, rebuild, redeploy — AND `CT_PARAMS` in `web/ui/src/lib/colortrans.js`;
`tests/test_colortrans.cpp` pins the C++ side to the glsl reference values,
`web/tests/colortrans.test.mjs` pins the web copy to the same values and to
`colortrans.h`'s defaults (a retune that misses the page fails it).

The ct thread also SCALES: the burned DVR's picture is the OSD surface's
size, not the decoded size, because the encoder lays the OSD 1:1 from the
top-left while the panel stretches the video under it — recording at the
decoded size would put the OSD offset and clipped on a 720p drone with a
1080p panel. With colortrans the shader draw scales (target = OSD size,
LINEAR sampler); without it, one RGA NV12 resize. So the ct thread runs
whenever there is an OSD region, colortrans or not, and a 720p drone
records a 1080p file.

## Kernel facts (GS kernel 6.1.84, `rockchip_drm_vop2.c`)

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

`BurnRecorder: stopped` reports `ct_ms=mean/max enc_ms=mean/max`; the ct
thread logs `colortrans stage mean per frame ...: import, draw+finish, rga`
at stop. Bench affordance: `kill -USR1 $(pidof maburplay)` toggles a
recording exactly like the GPIO button, so a mid-session record start can be
measured without a hand on the GS.

Bench record of the three fixes and their numbers:
`docs/findings/2026-09-17-colortrans-recording-latency.md`.

## Not done / follow-ups

- Live retune (config + restart is the loop).
- EGL YUV colour-space / range hints on the dmabuf import (none are passed;
  a mismatch is a slight tint between the display and the DVR).
- Raw DVR mode is the bitstream and stays flat: post-process with
  `ffmpeg -vf "libplacebo=custom_shader_path=colortrans3.glsl"`.
- The splash is not inverted.
- The stage is memory-bound (GPU + RGA traffic per 1080p frame); feeding
  the encoder BGRA directly and deleting the RGA pass is the untested next
  lever.
