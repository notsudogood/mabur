# Orange Pi Zero 2W: garbage picture after a cold boot — DE33 scaler coefficients never land 2026-10-14

**Status:** fixed in the image kernel (patch `0004-drm-sun4i-de33-vsu-coeff-lut-access.patch`, image repo `board/orangepi/zero2w/linux-patches/`). Current: docs/opi-zero2w.md ("Display").

**Question.** After roughly half of all cold boots the Orange Pi GS shows
the drone's video with wrong colours: grey reads white/green, edges are
smeared, the picture looks high-passed. The OSD is always correct. A soft
reboot keeps the bad state; a maburd restart on the drone sometimes clears
it. The 2026-10-12 colortrans record put it down to a mid-session join.
What is actually broken?

## Setup

- The bench Orange Pi on its hybrid p1 (the board's own squashfs with the
  image repo's 6.18.35 kernel: patches `0001`–`0003`), maburplay
  (v4l2req/Cedrus) with colortrans on, the drone's live 1080p60 feed.
- Bad and good boots rolled by switching the GS's mains off for 20 s and
  back on; each picture judged by eye on the HDMI monitor.
- Display engine registers read and written with `devmem`, one register at
  a time (never through the display regmap's debugfs dump, which hangs the
  board). Where a test must not race the kernel's per-frame register
  writes, maburplay is frozen with `SIGSTOP` first: the plane keeps
  scanning out its last buffer and nothing rewrites the registers.
- Channel 0 (the VI channel behind the video plane) lives at physical
  `0x1100000`: CSC `+0x800`, overlay `+0x1000`, VSU (scaler) `+0x4000`;
  blender at `0x1281000`; DE top at `0x1008000`.

## Ruling things out (frozen player, bad boot)

| Test | Result | Reading |
|---|---|---|
| Decoded frames dumped from the decoder's buffers | clean | not the decoder |
| The buffer on screen, read back from physical RAM | clean | not the join, not the buffer |
| Grey bars painted into the on-screen buffer | render white/green | the fault is in scanout |
| Same bars with VSU ctl (`0x1104000`) = 0 | correct | the scaler |
| Luma ramp with the scaler on | flat cyan, lines at its top and bottom edges | a zero-sum vertical filter: the coefficients are wrong |
| Zero all six VSU coefficient tables (`+0x200`…`+0x800`) | **no change** | the hardware is not using what the registers say |
| Register snapshot of channel 0 + top, bad vs good boot | identical | the bad state is not in any readable register |
| Randomise the disabled post-processing blocks' LUT SRAM (FCE, peaking, LTI, BLS, FCC, DNS, CDC) | no change | inert while disabled |
| `scale_mode` (`0x1104010`) 1 → 2 | luma right, chroma still wrong | not the fix |

Restarting maburplay or maburgs never healed it. Restarting maburd on the
drone healed it 2 times out of 3 this session.

## What a maburd restart does to the display

With a logger watching the video plane's KMS state and the channel-0
registers every 50 ms: every stream restart makes the player release the
framebuffer on screen, so the kernel disables plane 33 for 1.5–2.6 s and the
next commit re-enables it. The geometry is the same before and after
(1920×1080, 1:1). A maburgs restart goes through the same disable/enable
and never healed; one maburd restart with `drm.debug` on went through six of
them and did not heal either. Turning the layer and its blender pipe off
for 3 s by hand, with the player frozen, did not heal. So the disable
itself is not the cure; something in the re-enable lands only sometimes.

## The cause: the coefficient SRAM drops CPU writes

The kernel rewrites the VSU coefficient tables on every commit
(`sun8i_vi_scaler_setup`). The DE33 keeps them in SRAM, and an earlier
probe had already found that reading them back needs bit 0 of the DE top's
debug control register (`0x10080e0`, in the clock register space) set. The
vendor driver names that bit `de_top_set_lut_debug_enable` and never calls
it: it programs the display engine through the RCQ, not by CPU writes.

On one bad boot, with the player frozen, the kernel's own 1:1 tables were
written by hand three ways:

| Write | Result |
|---|---|
| bit 0 clear, layer off during the write | still garbage |
| bit 0 set during the write, restored after | **healed** |

On the next bad boot, with the player **running**, bit 0 was held set for
one second, so only the kernel's own per-commit writes went in: **healed**.
The register reads `0x10` at rest (bit 4 is the vendor's "AHB config
flag"); both tests ORed bit 0 in and restored `0x10`.

So, with the bit clear, CPU writes to the tables are dropped (or only
occasionally land, which is what the random maburd cure was). The scaler
runs on whatever the SRAM held at power-up, which is random after a cold
boot and kept across a soft reboot. NV12 always goes through the scaler,
even at 1:1, because it upsamples chroma, so every video frame shows it.

## Fix and verification

Patch `0004` in the image repo: the DE33 clock driver exposes `0xe0` on the
plane-mapping syscon the planes driver already holds; each layer keeps
that regmap; `sun8i_vi_scaler_setup` sets bit 0 before the coefficient
writes and clears it after.

Flashed as a hybrid p1 (only `/boot/Image` replaced). Before the patch,
this session's cold boots came up bad 4 times out of 6. After it: one warm
boot plus **5 cold boots in a row, all clean**, with colortrans on and the
bit back at `0x10` at rest. At a 50 % bad rate, five clean boots in a row
happen by chance about 3 % of the time.

## What this corrects

The 2026-10-12 colortrans record says garbage after a GS reboot was "a
mid-session join, not colortrans". The second half holds; the first does
not: the decoder and the join were never at fault, and the bad state comes
from the display engine's power-up SRAM.
