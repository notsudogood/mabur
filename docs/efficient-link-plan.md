# Efficient link — plan

**Status 2026-10-10: upstream's slice-salvage branch (row slices + GS-side
skip-slice fills, the software NACK, link pairing, the channel set) is
merged, with three NACK fixes on top — section "Merged 2026-10-10" below.
Step 4 (slices) is therefore built and on (`[venc] slices = 4`); NACK is
built, off by default, with an in-flight on/off A/B. Step 1 (the ladder) is
a config change ready to fly; step 2 (genlock) is built, off by default,
waiting on a bench probe of the camera; step 5 is FEC shaped per rung, its
first half a config change.** This replaces the feedback-repair rollout
(`docs/feedback-repair-rollout.md`, parked the same day — its results and why
are recorded there) as the line of work on the link.

## The goal, and the trade it cannot escape

Three things, measured the same way on every step:

- **Range** — how far the slowest rung reaches (dB of link budget at the floor).
- **Bitrate per MCS** — the share of the radio's rate that becomes video
  (commanded video bitrate ÷ nominal PHY rate of the rung).
- **Jitter** — e2e p50 and the worst seconds in `lat.log`, and frames lost
  or stalled per minute (`au.log`, `fec.log`).

They trade against each other through airtime and SNR margin: at a given
distance more video means a faster MCS (less margin) or less protection or
less headroom (more jitter). Nothing here removes that. What it does is move
the whole frontier outward, from two places where the link today sits well
inside it.

## Where the link is (2026-10-09 garage flight, all-40 MHz ladder)

| rung | 40/0 | 40/1 | 40/2 | 40/3 | 40/4 |
|---|---|---|---|---|---|
| video | 4.8 Mb/s | 9.4 | 14.1 | 18.8 | 24 (cap) |
| share of nominal PHY rate | 36% | 35% | 35% | 35% | 30% |

The ~65% that is not video is per-packet MAC/preamble cost (delivered ≈ 0.75
of nominal, `docs/airtime-model.md` §7), the 0.65 airtime budget (headroom
that keeps queues short), and FEC (0.5 base / 0.25 enh ≈ 29% of what is
left). For comparison (different rigs, different measurement):
snokvist/waybeam-link carries 19 Mb/s at HT20 MCS4 (~49% of nominal) on 20%
P-frame FEC with receive diversity and slice concealment as its protection;
the same rung by mabur's formula would carry ~14 Mb/s. wfb-ng's default is
33% FEC (8/12).

The two places the link is inside the frontier:

1. **Range at the floor.** Since 2026-09-26 the bundle ladder runs every rung
   at 40 MHz. The range test it replaced said the same MCS reaches 3.5–5 dB
   less at 40 MHz, MCS0 4–6 dB less, and that bottom rungs stay 20 MHz
   (`docs/bw40-sweep-findings-2026-09-23.md`). No measurement accompanied the
   switch (commit 6d9d6ba, a config-comment cleanup).
2. **Protection paid as FEC on every frame, at the same weight on every
   rung.** 0.5/0.25 is heavier than both
   peers, and FEC is the only thing standing between a lost symbol and a
   stalled frame.

## Smoothness: what the 2026-10-09 garage flight shows

Full power, 1.6 min, the last ~38 s in the garage (flightreport's DISPLAY
SMOOTHNESS and EVENTS sections; the player's own drop counters were not in
the session logs before this date, so on-screen drops are not in here):

- **Late frames are most of the stutter.** 87 AUs/min completed more than
  one refresh behind the fastest AU around them, 37/min more than two, 16/min
  more than three — each one a refresh the screen fills with the previous
  frame. The first flight, mostly not in the garage: 25/min, 0.4/min, 0
  (diluted: about half of it was the disarmed 30 fps mode).
- **Rung churn.** 16 rung changes in the garage's ~38 s; climbs lasted
  0.4–3 s before falling back, and 11 of the flight's 13 falls were the enh
  layer's loss (`s3_residual` / `s3_util`) — the layer flying 0.25 FEC. Each
  change leaves transition debris (a third of the floor's FEC failures).
- **Frames never delivered are rare** (garage flight 23 at full rate, first
  flight 7) — and not all are radio loss: one frame vanishes every
  **10.15 s** like clockwork at full rate in both flights (15.2, 25.4,
  35.5 … s; 57.2, 67.3, 77.5 … s — 6 of the 23, all 7 of the first
  flight's), a drone-side drop. One candidate is the encoder path discarding the camera's
  surplus over 60 fps; step 2's bench probe tells (slow the camera to 59.900
  and see whether the holes stop).
- **Deliberate drops:** `display.chain_budget = 3` drops a frame to end a
  chain of back-to-back releases (bench 2026-09-02: 1.44 drops/s for
  e2e p50 −3.9 ms; 6 was 0.56/s for −2.4 ms). Its flight rate is unknown
  until a flight carries the `regulator:` lines.

What each lever buys, in the steps below: genlock removes the beat's
thrown-away frames and the slow swing, and `genlock_miss_pct` is the
smoothness-for-latency dial (lower = later phase = fewer late frames shown
late); thicker enh FEC at the low rungs (step 5a, widened) should cut both
the enh stutters and the churn they drive, since the controller's enh budget
grows with it; `chain_budget` 0 or 6 trades its ~4 ms for fewer dropped
frames; source-first ordering (step 3) evens out clean frames. The worst
late frames (> 50 ms) are loss recovery — only link margin or concealment
(step 4) moves those. Not proposed: a deep jitter buffer, which would hide
late frames by making every frame later.

## The design

- **Ladder:** 20 MHz for MCS0–4, then 40 MHz MCS3/4 at the top (where 40 MHz
  reaches as far as 20 MHz MCS5/6 with more capacity). Optionally a 10 MHz
  narrowband floor rung below MCS0 (~+3 dB, half the bitrate; devourer
  supports it on the 8812EU and 8822E, mabur does not yet).
- **Protection:** FEC shaped per rung instead of one weight everywhere —
  thick at the floor, where the controller has no slower rung left to fall
  to and picture quality no longer matters, thin at the top once a failed FEC
  block turns into a concealed band instead of a stalled frame (per-slice
  H.265 with GS-side skip-slice concealment, the waybeam-link §6.3b approach;
  `docs/feedback-repair-rollout.md` phase-5 notes) — plus the GS's two cards
  (better-spaced antennas are the cheapest further dB). Every
  `[[link.ladder]]` rung already carries its own `overhead_base` /
  `overhead_enh` (up to 2.0); nothing new is needed to shape it.
- **Smoothness:** the vsync-locked player (built 2026-08-31, on) plus genlock
  — the camera's frame rate steered onto the screen's refresh grid; each
  frame's source bodies sent before its repair bodies; the encoder's GDR and
  I:P cap as today; the ~0.65 airtime budget kept as the headroom it is.
- **Uplink for control, plus best-effort re-send requests:** RCF, telemetry
  and MSP stay on the robust MCS0/20 LDPC+STBC path. Video integrity still
  never *depends* on the uplink — the feedback-repair flights showed it is
  least reliable exactly when loss happens — but since 2026-10-10 the GS may
  also ask for base-layer re-sends (upstream's NACK, "Merged 2026-10-10"
  below) on top of unchanged FEC: a lost request costs nothing the link had
  before, and the ladder still books every loss.

## Steps, each gated on a measurement

1. **Ladder: 20 MHz bottom, 40 MHz top** — config only (GS
   `[[link.ladder]]`; the drone already airs each rung at the width the GS
   names, given `radio.width = 40` and the per-width `air_clock.efficiency_20`
   / `ampdu.min_mcs_20` keys in `/etc/mabur.toml`). Bundle restored
   2026-10-09.
   - *Expect:* the floor rung carries ~2.8 Mb/s instead of 4.8, and the link
     holds a given rung further out; in the garage the drone should sit at
     20/1–20/2 where it fell to 40/0.
   - *Gate:* a garage flight on the 2026-10-09 route against that flight:
     lowest rung reached and time spent there, GS RSSI at the far point,
     truncated + dropped AUs per minute in the garage, `fec.log` failed
     episodes. *Kill:* if the garage stretch loses no fewer frames than on the
     all-40 ladder, the bench range result does not carry to this site — keep
     whichever loses fewer.
2. **Genlock: the camera locked to the screen.** The vsync-locked player this
   step first named already exists (`gs/player/src/frame_regulator.h`,
   `vblank_estimator.h`, 2026-08-31, on in the bundle; `dsp` 5/5 ms in both
   2026-10-09 flights). What it cannot fix is the beat between two clocks:
   - *Measured:* the camera runs **60.078 fps** (both 2026-10-09 flights:
     median pts step 16645 µs, sd ~7 µs over 10 000+ frames; drone–GS clock
     skew ~10 ppm, so this is the camera, not the clocks) against the
     60.000 Hz screen — a 12.8 s beat. `lat.log` shows it: e2e p50 climbs
     ~1.3 ms/s from ~34 to ~51 ms and snaps back every 12–13 s, and the camera
     makes ~4.7 frames a minute more than the screen can show (one thrown
     away per wrap). The earlier "59.939 fps sensor, slower than the panel"
     (`docs/dejitter-findings-2026-08-30.md`) does not describe this
     firmware; a 59.94 Hz screen mode would make the beat twice as fast.
   - *Built (off by default):* maburplay measures, per frame, the phase from
     the frame's capture to the next release deadline (clock-only, no link
     jitter in it) and the spread of frame readiness; a PI loop turns the
     phase error into a camera-rate setpoint in milli-fps once a second
     (`genlock.h`), the player hands it to maburgs over loopback
     (`genlock_client.h` → `genlock_control.h`, port 8402), maburgs sends
     `T_GENLOCK` to a drone advertising `CAP_GENLOCK`, and the drone trims
     its sensor's frame length through `MI_SNR_SetFps`'s milli-fps path
     (`star6e_controls_apply_sensor_mfps`: ±1% of the configured rate, 1:1
     bind only, an unchanged value re-written every 5 s). The target phase
     puts `display.genlock_miss_pct` (10%) of frames one refresh late.
     Measurement runs whenever `vsync_lock` is on, steering or not: the
     1 Hz `genlock:` line in `lat.log` and flightreport's CAMERA vs SCREEN
     section, which also reads the camera rate straight from `au.log`.
   - *Expect (simulated, not measured):* on the 2026-10-09 readiness spread,
     with whole-line rate steps (~27 mfps), a 2 s command delay and 1 in 5
     setpoints lost: lock in ~20 s (after ~18 s of calibration, below),
     phase held within ±1 ms, mean shown
     latency 18.9 → 15.5 ms, the 17 ms swing and the beat's thrown-away
     frames gone. Against it: the 10% of frames slower than the target pay a
     full refresh, so the mean gains less than the p50 (~7 ms from the
     sawtooth's low point vs its average in the flights); and the setpoint
     rides the uplink, which is least reliable when the link is bad — losing
     it leaves the drone holding its last rate, which at a good lock drifts
     a refresh in minutes, not seconds.
   - *Bench 2026-10-10 (IMX415 on the SSC338Q, GS loop off, rates held
     from the drone's debug port, camera period from the GS `genlock:`
     lines):* the driver takes a milli-fps request, but not at face value.
     59.400 → 16758 µs (59.67 fps), 59.725 → 16669.5 µs (59.990), 59.650 →
     16689.5 µs (59.916), each steady for the whole hold and back within
     ~2 s on `0`; 59.800, 60.000, 60.600 and the configured 60 all → 16645
     µs (60.078, the flights' camera). So a request sets a frame length
     (60.000 would mean ~60.27 fps), but a settled frame never runs shorter
     than 16645 µs, and right after a change into that region the shorter
     length shows for about a second (60.17–60.20). The first loop seeded
     itself assuming the request was the rate and steered between 59.89 and
     60.02, on that boundary: the camera never went slower than 60.078 and
     each write bought a burst, so it averaged 60.10–60.20 and the beat got
     *faster* (a slip every 6–9 s against 12.8 s free). The simulated
     camera in `test_genlock.cpp` is now fitted to these holds; the old loop
     reproduces the bench failure on it (setpoints 59.887–59.997, camera
     60.20), which is the best evidence the model is the right shape.
   - *Fix (2026-10-10):* the player measures before it steers. On its first
     steering tick it sends `0` (configured rate) and measures the camera,
     then a probe 0.7% below nominal and measures that, and takes the
     camera's rate per requested mfps from the pair (`genlock-cal:` line in
     `lat.log`; `cal=` 1/2 on the per-tick lines, ~18 s). The loop starts at
     the request for the screen's rate (~59.72 here) and never asks for
     faster than the screen + 0.05% or native − 0.1%, so it stays where the
     request is in control; the whole-line steps (~7 µs, ~27 mfps) leave a
     ~0.2 ms/s residual at any one request, which the loop removes by moving
     between neighbouring lines. A camera that does not slow for the probe
     is put back to its configured rate and not steered again until the
     drone restarts (`genlock-cal: refused`). Simulated on the fitted
     camera with a 2 s command delay and 1 in 5 setpoints lost: phase within
     1.2 ms, setpoints 59.727–59.741. *Against it:* three holds on one
     bench, one still scene; the floor is probably the exposure (the drone
     caps it at a full 1/60 s), so a darker scene could move it and that is
     unmeasured; the loop's top end sits 0.05% above the screen, which
     limits how fast it can pull the phase forward (~0.5 ms/s); and none of
     this has flown.
   - *Gate:* bench first (`[genlock] enable = true` on the drone,
     `display.genlock = true` on the GS): the `genlock:` lines settle to
     |err| under ~2 ms, the CAMERA vs SCREEN section shows the camera at the
     screen's rate, `lat.log` e2e p50 flat instead of a sawtooth. Then a
     flight. *Kill:* the lock does not hold through rung changes and loss,
     or e2e p50 does not drop.
3. **Source-first FEC ordering** — the drone sends an AU's source bodies before
   its repair bodies, so a clean frame completes without waiting behind repair
   air (`docs/airtime-model.md` §5). `SwEncoder`'s sliding window is built
   around interleaved repair, so this is a design question first: deferred
   repair must still cover the whole frame. *Gate:* `fec` segment p50 for
   clean AUs (base 12.6 / enh 6.2 ms in the 2026-08-31 budget), loss-sim
   residual unchanged.
4. **Slices and GS-side concealment** — the big one, in order:
   1. drone `MI_VENC_SetH265SliceSplit` at a count that divides the picture
      (1080p: 1, 2, 3, 4, 5, 6, 9, 17), verified in the recorded stream;
   2. bench: a recorded sliced stream with one slice cut, decoded on the
      RK3566 with the player's own MPP settings — the gap vs a skip slice in
      its place (waybeam-link measured MPP silently mis-decoding the gap);
   3. `FrameStream` keeps the whole slices after a hole and fills the hole
      with a skip slice instead of truncating;
   4. optional, later: per-slice decode as slices arrive (~3–6 ms; the fpvOS
      kernel patch, BSP 6.1 port).
   *Gate:* loss-sim frames shown vs dropped; a flight's stalls per minute.
5. **FEC shaped per rung.** Each rung already carries its own overhead pair,
   so this is configuration, in two halves:
   - *5a, thicken the floor — config, fly after step 1's flight:* 20/0 from
     0.5/0.25 to 1.0/0.5 (floor video ~2.8 → ~2.2 Mb/s); if the churn above
     persists on the new ladder, widen it to enh 0.5 on 20/1–20/2 in the
     same A/B. The 2026-10-09
     garage flight's `fec.log` at the floor (40/0 then): the enh layer at
     0.25 failed 6 of its 15 real loss episodes and every one would have been
     repaired at 0.5; the base layer at 0.5 failed none of its real episodes
     (max needed 0.49 — its 5 failures were rung-transition debris, which
     more FEC does not fix). Against it: FEC repairs scattered loss inside a
     block; a fade that takes the repair bodies with the video is not
     repaired at any overhead, so this buys survival at the floor's edge
     (roughly a dB), not range — range at the floor is step 6. A thicker
     floor also changes how the controller reads loss there (`u = loss /
     (ov/(1+ov))`, budget 0.33 → 0.5): the floor looks cleaner at the same
     loss, but leaving it still needs the next rung's probe to pass against
     that rung's own budget.
   - *5b, thin the top — only after step 4:* 40/3–40/4 toward 0.35/0.2 then
     0.25/0.2, one A/B flight per step. The bench at the top rung needed
     0.40–0.46 for one lost A-MPDU aggregate (`docs/observability.md`,
     2026-09-16), so below that a lost aggregate is a broken frame until
     concealment exists. `fec.log`'s "would fail at ov X" column says how many
     episodes each step turns from repaired into concealed. *Gate:* concealed
     bands vs stalls per minute, and the bitrate it buys (~+20% video per MCS
     at ~0.2, arithmetic not measured).
   - Not done: one weight for every rung (this plan's first draft). The
     controller climbs until loss appears, so a high rung is clean only when
     it is pinned at the top with margin; the floor is where loss runs
     without a rung to fall to.
6. **Optional range work:** a 10 MHz floor rung; GS antenna spacing / a
   third card; per-rung TX power at the calibrated walls (`docs/calibration.md`).

## Merged 2026-10-10: upstream NACK + slices, and three fixes

`gilankpam/mabur` slice-salvage (4c66e11, which contains master b627b9a)
is merged into this branch. What it brings, all upstream-measured
(`docs/fec-nack.md`, `docs/fec-nack-bench-findings-2026-10-06.md`,
`docs/slices.md`, `docs/venc-slice-findings-2026-10-09.md`):

- **Slices + salvage (step 4 above):** the drone encodes 4 row slices; the
  GS keeps the complete slices of a truncated frame and fills the lost ones
  with a skip slice (the previous frame's pixels). Upstream loss-sim: 94-96 %
  of truncated AUs salvaged. Against it: on upstream's flight logs a
  truncated base frame keeps a median ~1/3 of its picture and 4 slices
  would show ~25 % of it; a frame where nothing arrived gets nothing; a
  filled band in a base frame smears with motion until the next refresh
  (≤ 0.5 s); ~1 % bitrate at 4 slices, ~2 % at the floor.
- **Software NACK (base layer):** the GS lists base symbols FEC could not
  rebuild; the drone re-sends them from a 150 ms ring within a 5 % air
  budget. Upstream bench under a jammer at mcs2: base truncated + dropped
  12 → 0, fill p90 10-16 ms. Against it: 35-37 % of requested symbols
  wasted (FEC got there first), and at rung 0 nothing was ever filled — the
  drone heard the requests and re-sent, but every one landed wasted (62 %)
  or past the deadline. In the slices bench, NACK turned drops into
  truncations more than it cut damaged frames.
- **Link pairing, channel set, web GS, relay:** control frames are SipHash-
  tagged under a shared key (missing key file = built-in default on both
  ends); the drone and GS share a channel set (`[radio] channels`) and the
  GS picks among it (`channel = "auto"`), replacing home channel 136 and
  the old scan.

Our three NACK fixes (`docs/fec-nack.md` "Changes on notsudogood/mabur"):
ask only for the shortfall (the decoder's free unknowns — a seq an arrived
repair covers is never requested), send a first request only after its base
burst has ended unless the deadline is near, and put re-sends on the drone's
voice hardware queue, never shed first on overflow.

Genlock is ported to the tagged wire (`T_GENLOCK` = 8, `CAP_GENLOCK` =
0x0008). The parked turnaround bench and listen window are removed.

**Deploy (both ends, configs first).** The new parser rejects keys it does
not know, and both saved configs carry old ones: the drone's
`/etc/mabur.toml` (if it was ever edited it lives in the overlay and
survives a reflash) has `vtx_id`, `channel` and `follow_gs`; the GS's
`/config/maburgs.toml` has `vtx_id`, `[turnaround]`, `[listen]` and the old
`[radio.scan]`/`[hop]` keys. Restore both to the new defaults after
flashing (drone: `cp /rom/etc/mabur.toml /etc/mabur.toml`; GS: delete
`/config/maburgs.toml` and reboot, the image copies the default back), then
re-apply your own edits. `maburplay.toml` is unchanged.

**Flying the NACK A/B.** GS `/config/maburgs.toml`:
`[link.nack] enable = true`, `ab_s = 30` (30 s on, 30 s off). Fly the
garage route of 2026-10-09. Read `flightreport.py`'s NACK section:

- *the A/B lines:* lost AUs/min, base abandoned symbols/min and fills/min
  per arm, overall and per rung — compare a rung only where both arms spent
  real time on it;
- *wasted share* against upstream's 35-37 %, and `held_covered` /
  `held_burst` / `urgent` (how often each fix acted);
- *fills at rung 0* — upstream's was zero;
- *drone refused* (air budget) and `dropped_deadline`.

*Kill:* if the on arm loses no fewer base frames than the off arm across
two flights, NACK costs air and buys nothing on this route; turn it off.
If waste stays near 35 %, the shortfall rule is not what drove it (late
repairs from the next base AU would be the next suspect). Caveats: one
route, 30 s arms that cover different places (`ab_s = 10` interleaves them
more finely), and a single delivery probe is worth ±3 points.

## Kept from feedback repair

The shadow log (`arq.log`) and `fec.log` stay on every flight — they are how
steps 1 and 5 are judged. The turnaround bench and the listen window stay in
the code, off (`[turnaround] rate_hz = 0`, `[listen] ms = 0`).

## Caveats

- The 4–6 dB is one bench range test; step 1's flight is what decides it here.
- waybeam-link's numbers are mostly bench-measured and its jitter is not
  reported; its link runs closer to its capacity edge than mabur's 0.65.
- The +20% for thinner FEC is arithmetic. Concealment is built only in
  waybeam-link; nothing of step 4 exists in mabur yet.
- Genlock's numbers are a simulation over one flight's readiness spread;
  nothing about it has run on hardware, and the sensor's milli-fps path is
  unproven until the bench probe in step 2.
