# Efficient link — plan

**Status 2026-10-10: upstream's slice-salvage branch (row slices + GS-side
skip-slice fills, the software NACK, link pairing, the channel set) is
merged, with three NACK fixes on top — section "Merged 2026-10-10" below.
Step 4 (slices) is therefore built and on (`[venc] slices = 4`); NACK is
built, off by default, with an in-flight on/off A/B. Step 1 (the ladder) is
a config change ready to fly; step 2 (genlock) flew twice and was retired
on 2026-10-18 for Gilang's phase lock (merged from his master,
`docs/phase-lock.md`; the history below is kept as the record); step 5 is FEC shaped per rung, its
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
   (`docs/findings/2026-09-23-bw40-sweep.md`). No measurement accompanied the
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
2. **Genlock: the camera locked to the screen.** *Retired 2026-10-18:
   replaced by Gilang's phase lock (`docs/phase-lock.md`), which steers the
   boot-trimmed sensor through `Rcf::snr_off` and places frames by capture
   time while locked. `T_GENLOCK`/`CAP_GENLOCK` are gone from the wire
   (RC_VERSION 16); old `[genlock]` / `display.genlock` config keys still
   load and do nothing. What follows is the record of what ours did.* The vsync-locked player this
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
     (`docs/findings/2026-08-30-dejitter.md`) does not describe this
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
(`docs/fec-nack.md`, `docs/findings/2026-10-06-fec-nack-bench.md`,
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

## Second build, 2026-10-10 (night): what the flight showed, what changed

**Genlock flew.** 528 s of `genlock:` lines: steering 442 s, calibrating
17 s; camera 60.002 fps over the flight against a 60.003 Hz screen (free
running it beats a slip every ~13 s). Phase error |err| p50/p90/max
1.6/5.3/6.8 ms, within 2 ms in 60 % of steered seconds, never past half a
refresh. *Against it:* the bench hold was much tighter (p50 0.3 ms over
209 s), so flight costs lock quality; flightreport still counted 27
sawtooth-shaped drops in lat.log's e2e p50 (median 14 s apart), not split
by steering state, so some fall in the 86 s it was not steering; and the
bench calibration read native at 60.132 against the fitted model's 60.078
(the flight's read 60.082). Two fixes:

- **Native is measured from two sends, not one a second.** The drone
  re-applies an unchanged setpoint after 5 s, and each apply gives ~1 s at
  the bare (faster) period, so a 1 Hz resend during the native stage
  inflated the reading. The calibration now sends at the stage's ticks 0
  and 2 only (`genlock.h`, `Tick::send`). Simulated: native within 0.01 of
  60.078 (`test_genlock`). Unflown.
- **A jump in the GS's capture timing is absorbed, not chased.** The
  phase is `capture_us` against the drone's pts; when their offset steps
  by more than 400 µs (and under 1 s) the target and the loop state move
  with it instead of steering the camera toward a phantom error. Counted
  as `ref=` on the `genlock:` line and in flightreport. Simulated: a
  −2.5 ms step at 150 s costs < 0.7 ms of extra error. How often real
  steps happen is what the `ref=` count will show.

**Spikes and recording.** The remaining 46→90 ms spikes were lost enhance
layer aggregates nothing repaired (`docs/fec-nack.md`, "Second build"):
this build adds NACK for the enhance layer, asks once FEC is final rather
than at the burst's end, and finishes a doomed frame at once instead of
waiting out the gap timeout. The other spike source was GS **burned**
recording (58 % of seconds with p99 ≥ 70 ms vs 5 % without): the default
is now raw recording with `.osd`/`.srt` sidecars (`docs/gs-recording.md`).

**maburcal** sweeps the flight frame (LDPC/STBC on) and a lone shallow
miss no longer ends a row (`docs/calibration.md`). That raises MCS 4–7 by
4–6 dB on the next run; the EVM cost of that is unmeasured.

**flightreport** gained: loss size per rung and layer (FEC), the enh
tracker, `held_fec` and early give-ups (NACK, SLICE SALVAGE), genlock lock
share and reference steps, and GS RECORDING (latency per recorder state,
from `rec=`).

## Third build, 2026-10-11: two flights, four fixes

**Channel 64 cost the first flight's first half.** The GS's start-up scan
moved the link off 144 (clean all of 2026-10-10) onto 64 on a near-tie,
and 64 lost 0.5–3.8 % pre-FEC at 33–40 dB SNR with no visible cause: at
equal signal it lost 10–20× what 112 or 144 did, the ladder sat at 20 MHz
(17 % of time at 40 MHz vs 81 % on 112), and 46 % of seconds had an e2e
p99 ≥ 70 ms (10 % on 112). Nothing hopped for 100 s because the verdict
called the loss `unknown`. EVM on 64 was as good as on 112 and the drone's
own receiver was clean, so it was not the drone's transmitter; the home
Wi-Fi on 36–48 was busy all flight but its activity did not track 64's
loss (correlation 0.0), so the source is unidentified. Genlock was not
the cause of those spikes: the stage that grew in spiky seconds was the
wait for repairs (p99 15 → 43 ms) while the phase error stayed as small
as in calm seconds. Fixes:

- **A. Hop on sustained unexplained loss** (`[hop.verdict] sustained_*`,
  `docs/inflight-channel-hop.md`). Replayed: fires at 216 s instead of
  313 s on channel 64; never on 112, 144 or the second flight.
- **B. The start-up pick keeps a known channel** unless another is 5
  points of airtime calmer (`radio.scan.pick_busy_margin_pct`,
  `docs/channel-select.md`).

**Raw recording was the second flight's spikes** (`docs/gs-recording.md`):
91 % of seconds ≥ 70 ms with it on at 24 Mbps vs 11 % off, all in the
decode stage, on a clean link (0.06 % loss) with genlock locked 88 % of
steered seconds. Fixes:

- **C. Ask the drone for a keyframe** while a recording waits for one.
- **D. Recorder I/O on a writer thread**, and genlock's calibration
  restarts if a recording starts inside it.

*Against all four:* one or two flights each; A's threshold was chosen on
the first two flights (the second 2026-10-11 flight is the only
out-of-sample check), B can still keep a channel that has gone bad (A is
then what leaves it), and C and D were unflown when built.

**First flight on that build (2026-10-11 evening, GS eMMC session 0027,
channel 112 throughout, raw recording on).** D: armed at 24 Mbps with
recording on, decode p99 15 ms and 14 % spiky seconds (was 41 ms / 91 %;
recording off 14 ms / 11 %). B: the start-up scan kept 112 where the event
rule would have moved to 144 (29 vs 0 events, all halves 0.0 % busy). A:
no sustained-loss window and no hop, correctly -- 0.08 % loss while armed;
replayed over the flight, 0 fires. C: the one record press came 4 s
before arming and a real IDR arrived within that press's second with no
rung change, arm or link-up near it, which fits the request; one press,
not proof. No frame lost on either layer for the first time (0 failed FEC
episodes), 80 % of armed time at 40 MHz, genlock native 60.078 and
within 2 ms 77 % of steered seconds (2 s past half a refresh). *Against
it:* a clean channel at mostly short range (RSSI p50 −45 dBm), recorded to
eMMC rather than an SD card, and the player/maburgs logs were not kept.

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
