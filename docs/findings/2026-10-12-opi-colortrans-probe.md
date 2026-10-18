# Orange Pi Zero 2W: DE33 CCSC as the colortrans stage — probe and acceptance 2026-10-12

**Status:** built; the H618 runs colortrans in the video plane's CSC. Current: docs/colortrans.md ("Live picture (H618)", "Kernel facts — H618").

**Question.** The H618 has no CRTC `CUBIC_LUT`. Can each VI channel's colour
space converter (the CCSC: three pre-offsets, a 3×3, a post-constant per
row) carry colortrans as a single 3×4 — `kColorTrans3` folded into the
BT.709-limited YUV→RGB table — at no scanout cost? That needs combined
coefficients up to 3.41 (Cb→B), an output offset of −0.345 of full scale,
and an output that clamps rather than wraps.

## Fidelity of a 3×4 (numerical, before the bench)

`forward()` is affine at `gamma = 1` except for a clamp after the camera
matrix, which a 3×4 cannot reproduce. Against exact `forward()` over an
even 48³ grid of in-gamut RGB (8-bit LSB, max per-channel error):

| | mean | p99 | max |
|---|---|---|---|
| 3×4 affine | 4.0 | 31 | 36 |
| 9×9×9 LUT, trilinear (the RK3566) | 3.7 | 18 | 20 |

The affine's large errors are all on strongly saturated colours (one
channel negative before the clamp); low-chroma pixels (max−min < 0.3)
stay within 7.3 LSB.

## Setup

- The bench Orange Pi on its pre-mabur SD image (squashfs p1, overlay p2,
  Wi-Fi AP through the out-of-tree `sprdwl_ng`). Test kernels went on as a
  hybrid p1: the board's own squashfs with only `/boot/Image` and
  `/lib/modules/6.18.35/{kernel,modules.*}` swapped (the image repo's
  current rootfs would have run a different `/init` against this card).
- Probe kernel: 6.18.35 + `0001` + a throwaway patch making the 15 CCSC
  words of the video channel writable live through module parameters
  (`ct_probe_en`, `ct_probe_regs`), the stock words logged once.
- Judged by eye on the HDMI monitor against the drone's live feed
  (indoor, night).

## Probe results

Stock words for the plane (BT.709 limited, the plane's default):
`40,200,200,2542a,0,395e2,0,2542a,ffff92d2,fffeef27,0,2542a,4398c,0,0` —
pre-offsets Y 64, U/V 512 (10-bit units), coefficients S.17.

| Run | Words | Seen |
|---|---|---|
| E0b | every coefficient ×0.5 | clearly darker: the parameters reach the hardware |
| E1a | ×2.5 | torch solid white, the rest brighter but recognisable: **output clamps** |
| E1b | ×4 (Y 4.66) | all black, light or not: Y coefficient wrapped |
| | ×3.35 (Y 3.90, Cb→B 7.07) | picture back, bluish cast |
| | ×1.85 (Cb→B 3.91) | warm, natural |
| | ×2.05 (Cb→B 4.33) | bluish |

The coefficient fields are **signed 20-bit, S2.17: |c| < 4.0, and a larger
value wraps** (a negative Cb→B under warm light turns the picture blue).
Read back through `/dev/mem`, `0xFFFF92D2` returns as `0x000F92D2`.
`kColorTrans3` peaks at 3.41 — inside, 15 % headroom.

The post-constant was not probed directly (the night run stopped there).
The first CTM kernel encoded −0.345 as −353 in two's complement
(`0xFE9F`) and the picture went **solid white** — the post-constant does
not carry a negative offset (ignored or read as a large positive value).

## The fix: the offset rides the luma pre-offset

The CCSC computes `C'·(in − d) + k`; raising the Y pre-offset by Δ shifts
every output row by `−C'[r][Y]·Δ`. A colour grade's lift is a grey shift,
and `kColorTrans3`'s rows of `A` sum to the same 2.396, so its offset lies
exactly along `C'[.][Y]` (2.79 on every row): `d_Y` 64 → 190, U/V stay
512, post-constants stay 0, residual under 0.4 of an 8-bit step. The kernel
patch refuses any offset that is not a grey shift, and any `d_Y` outside
0..1023.

## Acceptance (kernel `0002`, maburplay on branch `opi-colortrans`)

| Row | Result |
|---|---|
| R1 kernel without `CTM` | `DrmPresenter: video plane 33 CTM absent`; video fine |
| R2 `enable = false`, CTM kernel | flat stock picture |
| R3 `enable = true` | `video-plane CTM on, OSD untouched`; picture "perfect", OSD in its normal colours |
| on → off, no reboot | flat stock picture again (see below) |
| R4 out-of-range matrix (`gain` 6.0, throwaway build) | `kernel refused the video-plane CTM (Numerical result out of range) -- dropped, display flat`; flat picture, video uninterrupted |
| R5 HDMI unplug/replug | picture stays corrected; the DE33 keeps the CRTC and plane through a replug, so there is no presenter re-acquire on this board |
| latency, off vs on (60 s each) | p50 `e2e` 35 / 35, `dec` 3 / 3.5, `reg` 11 / 11, `dsp` 5 / 5 ms |
| `ausniff`, 30 s | 1815 AUs, 60.5 fps, 0 frame-id gaps, 0 resyncs, 0 incomplete |

**Plane state outlives the process.** The first build left the `CTM` alone
when colortrans was off; a matrix set by the previous run stayed applied
(white until a reboot). The fixed build states the property on every
modeset and splash-handoff commit — the matrix, or 0 — and the on → off
row above is that fix on the hardware.

## Detour: garbage after a GS reboot is not colortrans

Between the night run and the acceptance run the GS showed garbage after
every reboot — a green cast, purple posterised blobs, luma reduced to
edges — on the original kernel too, with old and new maburplay. The drone
had been running since before the GS booted; `maburplay` joined mid-session
and never recovered, while the board's ffmpeg (Cedrus, `+showall`) decoded
the same mid-stream capture cleanly. Restarting `maburd` (a session-start
IRAP) fixed it at once. Every acceptance row above starts from a `maburd`
restart. The DE33 VSU coefficient registers (`planes + 0x4200…`) read back
non-table values on a good kernel too — they are not readable, not a lead.

## Conclusions

- The H618 carries colortrans at zero scanout cost in the video plane's
  CCSC, with the OSD untouched — the coefficients through `C' = A·C`, the
  offset through the luma pre-offset.
- Limits the kernel enforces: |coefficient| < 4.0 (S2.17, wraps), offsets
  only as a grey shift, `d_Y` in 0..1023; the post-constant is left at 0.
- Open: the Orange Pi's mid-session join (above) — a separate bug.
