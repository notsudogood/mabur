# H.265 row slices: salvage and streamed-decode bench, 2026-10-10

**Status:** slice salvage and streamed decode both shipped (bundle `[venc] slices = 4`, maburplay `[decoder] stream = true`); salvage rescued 94–97 % of truncated split AUs with 0 rkvdec resets, streamed decode cut `dec` p50 by ~2.7 ms; the slice count sits at the 4-slice decode knee. Current mechanism → docs/slices.md.

Extracted from docs/slices.md during the 2026-10-11 docs tidy.

Prediction source for every "predicted" number below:
`docs/findings/2026-10-09-venc-slice.md`. Task numbers (11, 11D, 11g) are
the plan's tasks; their reports lived in a machine-local, gitignored
`.superpowers/sdd/2026-10-10-h265-slice-stream/` directory.

## Slice salvage bench (Part 1)

Bench drone (1080p60, 8812EU) + GS restored to the stock CI image (p1
`p1.orig.img`, kernel Sep 26; the rkvdec2 stream-mode spike kernel is off
it). Both ends at 6c27e74, live adaptive GS config.

- **No loss, slices = 4:** maburd logs `[venc] slices: chn=0 4 slices of
  5 CTU rows`; `slice_mismatch=0`, `enhance_disagree=0` through the ladder
  climb to mcs4. ausniff 30 s: 60.5 fps, every AU complete. aucadence
  offset +0.26 ms (gate ±4.0). A ring dump: 189 pictures of 4 slices, 7 of
  1 (refresh starts). au.log is `aulog 5` with `slices 4 kept 4`.
  slices = 1 baseline: 60.5 fps, `slice_mismatch=0`.
- **Loss-sim A/B, NACK on** (`MABUR_LOSS_SIM` maburgs on the live GS
  config, `[link.nack] enable = true`: ~500 requests / ~3 600 symbols filled
  per arm; `s0`+`s1 eff=1.5 burst=4`, 5 min per arm). Salvage acts on the
  truncations NACK could not repair in time:

  | | slices = 4 | slices = 1 |
  |---|---|---|
  | truncated | 258 | 275 |
  | slice_salvaged | 249 (96.5 %) | 0 |
  | fallback | no_template 9 | — |
  | slices kept / filled / after_hole | 491 / 505 / 216 | — |
  | ausniff 30 s window: incomplete / salvaged | 3 / 15 | 24 / 0 |
  | rkvdec `resetting` (dmesg) | 0 | 0 |
  | maburplay e2e p50/p99 ms | 48/66, 34/83 | 46/79, 47/81 |
  | drone cpu_pct p50 | 23.0 | 22.9 |

  The 9 `no_template` AUs lost every slice. `slice_mismatch` stayed 0.
- **Loss-sim A/B, NACK off** (same rig, a config copy with
  `[link.nack] enable = false`, sideport `link.nack` null):

  | | slices = 4 | slices = 1 |
  |---|---|---|
  | truncated | 200 | 200 |
  | slice_salvaged | 189 (94.5 %) | 0 |
  | fallback | no_template 4 | — |
  | ausniff 30 s window: incomplete / salvaged | 0 / 11 | 22 (14 base) / 0 |
  | dropped (never emitted) | 303 | 304 |
  | rkvdec `resetting` (dmesg) | 0 | 0 |

  The 7 truncated AUs neither salvaged nor counted as a fallback are
  unsplit refresh starts (`slice_rows` 0). Across the two A/Bs, NACK turns
  some would-be drops into truncations (dropped ~210 vs ~300) rather than
  cutting the total of damaged AUs much at this loss pattern; the runs were
  minutes apart on an adaptive ladder, so treat that as indicative.
- **Decoder legality of real salvaged AUs:** 60 s ring dump under the same
  loss (complete + 59 salvaged AUs, in ring order) through ffmpeg:
  3 584 frames, zero slice-data / CABAC errors; the only errors are a
  missing-ref chain after dropped AUs (fid gaps) and the dump's mid-GOP
  start. MPP on the GS decoded both arms with no decoder reset.
- **Visual check with motion** (operator waving a hand in front of the
  camera, 60 s, same loss; every complete + salvaged AU captured off the
  ring and decoded with libavcodec one packet per AU, 0 decode errors,
  39 salvaged: 36 enhance, 3 base):
  - The salvaged picture itself: each filled band is a slightly stale
    copy of the previous frame, seamless with the kept slices. No garbage,
    no blocks.
  - Enhance fills (non-reference, TRAIL_N) never propagate: the next
    frames are clean.
  - Base fills are references, so later pictures predict moving content
    from the stale band. Blocky drift appears inside that band only, and
    it clears at the next refresh-start picture (one refresh period,
    ≤ 0.5 s; 67 ms and 200 ms in the two decodable cases). On a static
    scene it is invisible.
  - The missing-middle-slice "slice-3 anomaly" of the findings doc did
    not appear (the decoder never sees a gap).
- **Web GS** (this branch's page served locally, Chrome on the host's
  Intel iGPU / VAAPI, host a81a card, page NACK off; jammer = devourer
  `txdemo` on one GS card, ch 144, 6M 1000 B, ~243 fps on air, 5 min):
  `trunc` 43 (13 base), `salvaged` 37. The video froze at times and the
  console showed `[webgs] decoder error EncodingError`; it never showed the
  stale bands maburplay shows.
  - Attribution, by replaying two ring captures that start at an IDR
    through WebCodecs with the page's own config (`hvc1.1.6.L120.B0` +
    hvcC, `prefer-hardware`, `annexbToLengthPrefixed`): loss on the
    enhance stream only, 4 957 AUs incl. 18 salvaged → 0 errors. Loss on
    both streams, salvaged AUs included → decodes through 9 salvaged base
    + 4 salvaged enhance AUs; the first error is the enhance AU right
    after a base AU that never arrived (fid gap). Same capture with the
    salvaged AUs skipped (the page before slice salvage) → error at the
    first skipped base AU, 808 AUs earlier.
  - So WebCodecs decodes salvaged AUs fine; the errors are missing
    references from dropped or unsalvageable base AUs. On any decoder
    error the page drops its decoder and waits for a key frame (IDR
    request), so it freezes where MPP conceals and keeps decoding — the
    page shows a salvaged picture only between such resets. Not a slice
    salvage defect; a web GS limit (docs/slices.md "Known limits").
- **Turned on:** `bundle/mabur.default.toml` ships `slices = 4`; the bench
  drone runs it (`.pre-slice` backups of binary and config on both
  devices).

## Part 2 bench — platform (GS image `slice-stream`)

Image: kernel + fpvOS 0102, MPP rockchip-linux/mpp `14729dd5` + mabur's
`gs/player/mpp-patches` (no `libmpp_ext.so`). Link mode off at boot:
task-capacity 16 → 1. Bench 2026-10-10, default image
`rootfs.default.squashfs` md5 `f75f5f3f…`, `rootfs.stream0.squashfs`
`bb65f087…`; drone on bundle `slices = 4`, `intra_refresh_frames = 1`.

| check | result |
|---|---|
| `mpi_dec_stream_test` cap4, whole vs streamed (5 ms spread) | md5 equal (`94bdc50b…`), 600/600 frames out both, 0 resets; whole 4506 µs, streamed 1544 µs (repeat 4479 / 1550) |
| `lose_every 7` | 600 frames out, 83 resets (one per lost tail) |
| maburplay whole-AU on the new image | `dec` p50/p99 6/11 ms, `e2e` 36/46.5, 0 resets (second sample 6/10, 36/47) |
| link mode on (`rk_vcodec.rkvdec2_stream=0`) | `dec` p50/p99 7/12.5 ms → link-mode-off cost −1.0 ms p50 (none) |

- The `mpi_dec_stream_test` medians are with the YUV going to `/dev/null`.
  Written into a fifo for `md5sum` (the bit-exactness run) they read
  16 846 / 10 168 µs: the test stamps a frame before writing the one
  before it, so the consumer's speed is in the number. The spike's own logs
  through its python hasher read 28.7 / 22.7 ms for the same reason.
- `dec`/`e2e` are medians over 120 of maburplay's 1 Hz `lat:` lines
  (anchor=ok), integer ms. The means of the per-second p50s resolve
  further: `dec` 6.58 / 6.64 ms on the default image vs 7.14 ms link mode
  on, so turning link mode off costs nothing (≈ −0.5 ms). Stock CI image
  (stock MPP, link mode on), same maburplay binary, same drone: `dec`
  7/12, `e2e` 40/49 (means 7.38 / 40.04).

## Part 2 bench — streamed decode

GS on the `slice-stream` image (Part 2 platform bench above), ring v4
(maburgs md5 `113fce18…`). Bench drone `/etc/mabur.toml` `[venc]`:
`size = "1920x1080"`, `fps = 60`, `slices = 4`, `intra_refresh_frames = 1`,
`intra_refresh_qp = 36`. `dec`/`e2e` are medians of maburplay's 1 Hz `lat:`
lines (anchor=ok, integer ms), 2 × 120 s arms per side, each arm a fresh
maburplay process; Δ = mean(on1, on2) − mean(off1, off2).

Ring wake is measured on closed-record delivery (t_complete → delivery);
the per-slice wake rides the same doorbell path, so it stands in for the
per-append wake. The on arms also run the parser with
base:fast_parse = 0 (required for stream mode); Δdec includes that.

### First bench (Task 11, ring read on the render thread)

| | stream on | stream off |
|---|---|---|
| `dec` p50 / p99 ms | 6 / 9.5 | 7 / 11 |
| `e2e` p50 / p99 ms | 37.5 / 48.5 | 38.5 / 48.25 |
| rkvdec `resetting` during decode, on arms | 0 | — |
| `stream_aborted`, no loss | 0 | — |

- Δdec p50 −1.0 ms (means of lines −1.15), Δdec p99 −1.5 ms, Δe2e p50
  −1.0 ms.
- Ring wake p50 682 µs, p99 5797 µs (medians of the 5 s `stream:` lines).
- `whole` ≈ 7.0 /s with stream on (+836 and +798 over 115 s): ≈ 90 % of
  split AUs streamed, the rest closed before the player saw their first
  slice and went whole.
- One rkvdec reset in the A/B window, at the on2 → off2 restart (see the
  attribution below), none while a stream-on player was decoding.

### Reset attribution: killing a stream-on player (Task 11D)

- 26 restarts with uptime stamps: kills of a stream-on maburplay reset
  rkvdec 3 of 13 times (4 of 14 with Task 11's on2 kill); kills of a
  stream-off maburplay 0 of 13; a 600 s stream-off soak with no restarts
  0.
- Each reset lands 0.13–0.14 s after the kill, before the next maburplay
  is spawned (≈ 1.1 s after the kill): it belongs to the teardown of the
  killed stream-on process, whatever mode the next process runs.
- The picture the kill leaves open is ended with an empty LAST / abort,
  and that empty LAST / abort itself causes the reset (`resetting… /
  reset done`, 0.4 ms, no other kernel output).
- The dmesg (printk) clock on this GS runs 7.17 s ahead of
  CLOCK_MONOTONIC (`/proc/uptime`, `lat.log`, `au.log` stamps): convert
  with monotonic = dmesg − 7.17 s, and re-measure after a reboot
  (`echo mark > /dev/kmsg` bracketed by `/proc/uptime`; +7.16 s on the
  Task 11g boot).

### With the decoder-input thread (Task 11g)

maburplay `9271c81` (md5 `eed579d8…`): FeedLoop reads the ring and feeds
the decoder on its own thread, woken by the AU doorbell; on SIGTERM it
finishes an open streamed picture with its real last slice before MPP
teardown.

| | stream on | stream off |
|---|---|---|
| `dec` p50 / p99 ms | 3 / 8 | 6 / 9.5 |
| `e2e` p50 / p99 ms | 40 / 49.5 | 42 / 49.75 |
| rkvdec `resetting`, no loss (Steps 3–5, A/B incl. its 5 restarts and on3) | 0 | 0 |
| `stream_aborted`, no loss | 0 | — |

- Δdec p50 −3.0 ms (means of lines 3.83 vs 6.49 → −2.67), Δdec p99
  −1.5 ms (means −1.95), Δe2e p50 −2.0 ms (means 39.85 vs 42.13 → −2.28),
  Δe2e p99 −0.25 ms. Regulator: Δe2e p50 is more than half of Δdec p50,
  so the gain passes through.
- Absolute `e2e` is higher than in the first bench in both arms (off 42
  vs 38.5); the difference sits upstream of maburplay (`enc` p50 8 vs 7,
  `fec` p50 7–8 vs 5 on this drone boot), while `dec` off dropped from 7
  to 6. Compare Δ within a bench, not absolutes across the two.
- Ring wake p50 350.5 µs, p99 825.5 µs (medians of the on1 + on2 `stream:`
  lines; per-line p99 710–1011 µs).
- `whole` 2.10–2.23 /s with stream on (on1/on2/on3 +256/+242/+244 over
  115 s) vs 60.0 /s off; `streamed` ≈ 289 per 5 s line, 96.3–96.5 % of
  records; `parks` 0 in every on arm.
- Kills mid-stream: `kill_resets` 0/10 (10 restarts of a stream-on
  maburplay, 20 s apart): `stream finished` 2 (both `, decoded`, waited
  5 ms and 3 ms), `stream idle` 8, `stream aborted` 0. The A/B's 3
  stream-on kills and the loss-sim's restart were `stream idle`, 0 resets.
- Loss-sim (`s0` + `s1 eff=1.5 burst=4`, 5 min, stream on): streamed
  20181 (salvaged 167), `stream_aborted` 0, rkvdec resets 0, decode
  watchdog 0; ausniff (30 s) salvaged 11 (s1) + 1 (s0), incomplete 0,
  frame_id_gaps 3; `dec` 4/11, `e2e` 40/78.5 (300 lines).
- The 5 s stream: line's MppBackend read on the feed thread is
  hardware-only and not ThreadSanitizer-covered; it is safe because the
  watchdog recreates the backend only while the feed is parked.

## Slice-count sweep 2026-10-10 — measured vs predicted

Every count the 1080p geometry allows (1, 2, 3, 4, 5, 6, 9, 17), checked
against the predictions in `docs/findings/2026-10-09-venc-slice.md`
"Slice cost, slice count, and fit with the transport". Rig: GS on the
`slice-stream` image, maburplay `eed579d8` with `[decoder] stream = true`,
maburgs = the `MABUR_LOSS_SIM` build of the ring-v4 code (`--loss-sim
8310`, run from a shell loop in place of `S96maburgs`), live adaptive GS
config with NACK on. Bench drone 1080p60 at `cmd_kbps` 24000, `/etc/mabur.toml`
`slices` edited per count plus a `maburd` restart (one session directory
per count: GS `/media/dvr/log/0066`–`0073`, control `0074`/`0075`). Per
count: 45 s settle, 120 s clean, then 180 s `s0` + `s1 eff=1.5 burst=4`
(the ladder sits at rung 0, mcs0/40, under that loss). Afterwards a
control re-run of 1 and 4, to measure run-to-run noise. `dec`/`e2e` are
the means of `lat.log`'s 1 Hz p50s (anchor=ok); loss counters are
sideport deltas over the loss arm; the shares come from `au.log`
(`aulog 5`) rows.

### Decode overlap (clean arm)

Δdec vs the 1-slice arm (whole-AU decode, `dec` 6.24 ms; control
6.38). Prediction = the findings model (a fixed ~1.2 ms plus the last
slice's share of the 4.38 ms harness whole-picture decode) minus 4.38.

| slices (CTU rows) | Δdec measured | Δdec predicted | ring wake p99 | streamed |
|---|---|---|---|---|
| 2 (9/8) | −1.92 | −1.12 | 351 µs | 96.4 % |
| 3 (6/6/5) | −2.57 | −1.89 | 616 µs | 96.3 % |
| 4 (5/5/5/2) | −2.63 (control −2.71) | −2.66 | 882 µs | 96.6 % |
| 5 (4/4/4/4/1) | −2.69 | −2.92 | 925 µs | 96.6 % |
| 6 (3×5, 2) | −2.67 | −2.66 | 993 µs | 96.6 % |
| 9 (2×8, 1) | −2.58 | −2.92 | 1463 µs | 96.4 % |
| 17 (1 each) | −2.15 | −2.92 | 2717 µs | 96.6 % |

- The model is right at 4 and 6. It under-predicts the gain at 2 and 3:
  the live whole-picture decode (~6.3 ms) is longer than the harness's
  4.38 ms, so each row is worth more than the model assumes.
- It over-predicts 5, 9 and 17. From 3 slices on the gain sits at a
  floor of about −2.65 ms (`dec` ~3.6 ms); the 3–9 spread is within
  run-to-run noise (control: ~0.1 ms). 17 is clearly worse (−2.15): the
  ring wake grows with the number of doorbells per AU, and so does the
  per-append work.
- `e2e` p50 is 1–2 ms lower than the 1-slice arm at every count (42 →
  40–41 ms).
- `dec` p99 is 7.5–8 ms at every count (10 ms whole).
- `whole` ≈ 2 /s at every count = the refresh-start pictures, which stay
  one slice at every count, as predicted (~230 `nslices` 0 AUs per
  120 s arm).

### Loss confinement (loss arm)

On this rig almost every truncation is an enhance AU. Base AUs are
dropped whole (94–128 per arm, flat across counts; slicing cannot help a
picture whose first fragment never arrived — also predicted), or
truncated 0–4 times per arm. The base predictions are therefore
untested. Share of a truncated enhance picture that is shown, in % of
the 17 CTU rows:

| slices | head only, measured | bench model | flight prediction | incl. kept slices after the hole | salvaged / truncated |
|---|---|---|---|---|---|
| 1 | 0 | 6 | 6 | 0 | 0 / 131 |
| 2 | 24 | 26 | 21 | 34 | 93 / 140 |
| 3 | 27 | 32 | 27 | 43 | 131 / 147 |
| 4 | 32.5 (control 35) | 35 (37) | 29 | 52 | 138 / 141 (154 / 155) |
| 5 | 36 | 37 | 32 | 56 | 152 / 153 |
| 6 | 40 | 40 | 34 | 59 | 138 / 141 |
| 9 | 41 | 41 | 37 | 62 | 163 / 163 |
| 17 | 47 | 45 | 39 | 67 | 140 / 140 |

- **Head only** = the rows of the slices before the first missing one
  (`kept − after_hole`), the quantity the findings predicted; an
  unsalvaged truncation counts as 0.
- **Bench model** = the findings' method (bytes ∝ rows, cut at the last
  complete slice) applied to this bench's own truncated-prefix
  distribution, measured in the 1-slice arm. The enhance prefix p25/p50/p75
  is 0.23/0.46/0.75, against 0.15/0.32/0.59 in the flight logs. That is
  why the flight prediction runs low here.
- **Head only vs bench model.** The measured head share tracks the bench
  model within 2–5 points (the control's model moves by 2 points on
  sampling alone).
- **Salvage beats the head-only prediction.** Salvage also keeps complete
  slices past the hole. Counting those (their rows estimated at the mean
  row count of the slices past the hole) adds 10–20 points.
- **2 slices is weak.** A third of its truncations lost every slice
  (`no_template` 45). `no_template` falls to 16, 3, 1, 2, 0, 0 from 3
  slices up.
- Under loss: `dec` p50 4 ms (6 at 1 slice), `stream_aborted` 0, rkvdec
  resets 0, `slice_mismatch` 0 at every count.

### Cost

- **Bitrate cost: untestable on this link.** The encoder is CBR, so
  bytes are pinned: the link carries 23.4–23.7 Mb/s at every count.
  Clean-arm AU p50 also drifted 44 → 51 kB over the hour, independent of
  the slice count, which swamps a ~1 % effect. The findings'
  fixed-QP numbers (+0.65 % at 4 slices, +4.2 % at 17 on the SSC338Q;
  x265 +1.2 % / +1.9 % / +5.4 % at 4 / 6 / 17) stay the only cost
  data.
- **The first 1-slice arm's base/enhance gap was drift.** That arm
  showed base 56 kB vs enhance 44 kB, where every sliced arm had them
  equal. It ran first, ~15 min after a cold drone power-on; the control
  1-slice arm read 50.9 / 50.2 kB.
- **Drone `cpu_pct`** p50: 30.4–31.5 at 1 slice, 32.6–34.3 in every
  sliced arm except 3 slices (30.7). That is about +2 points, noisy, with
  no trend across counts.

### Verdict

- 4 is at the decode knee.
- 6 decodes as fast and shows ~8 points more of a truncated picture
  (head only 40 vs 32.5–35 %, with the after-hole slices 59 vs 52 %).
  The price is the findings' predicted +0.7 % bytes at flight frame sizes
  (more on the small bottom-rung frames, where the per-slice part
  weighs more), unmeasured here.
- 5 vs 4 (spec open question 6): +3 points of picture, no decode gain.
- 17 costs decode time and bytes for the last ~7 points.
- The bundle stays at 4 until a flight shows how often base truncations
  (untested here) happen.

## Follow-up evidence (open items in docs/slices.md)

- **Web GS replay of salvaged AUs.** A replay of 31 salvaged AUs through
  Chrome WebCodecs (VAAPI, the page's own config) matches libavcodec at
  45.3–46.9 dB (colour-conversion rounding only), fill bands ≥ 43.9 dB, one
  output per chunk, no error. Those captures were static (kept bands changed
  ~0.15 grey levels frame to frame), so "decoded" and "repeated the previous
  frame" are not told apart. The operator saw no band edge on the phone
  (/edge, jam, 2026-10-10). Harness: aucap (ausniff with framed AU dump) + a
  WebCodecs page that reads frames back via canvas; it lived in a session
  scratchpad.
- **Task 11g `fec` drift.** The on arms ran about +0.7 ms mean `fec` over
  the off arms (8.1–8.5 ms vs 7.4–7.6 ms) — not investigated; could be
  noise, or the feed thread's CPU competing with maburgs on the same core.
