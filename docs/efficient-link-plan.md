# Efficient link — plan

**Status 2026-10-09: written; step 1 (the ladder) is a config change ready to
fly, nothing else is built.** This replaces the feedback-repair rollout
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
2. **Protection paid as FEC on every frame.** 0.5/0.25 is heavier than both
   peers, and FEC is the only thing standing between a lost symbol and a
   stalled frame.

## The design

- **Ladder:** 20 MHz for MCS0–4, then 40 MHz MCS3/4 at the top (where 40 MHz
  reaches as far as 20 MHz MCS5/6 with more capacity). Optionally a 10 MHz
  narrowband floor rung below MCS0 (~+3 dB, half the bitrate; devourer
  supports it on the 8812EU and 8822E, mabur does not yet).
- **Protection:** thin, flat FEC (target ~0.2–0.25) made safe by turning a
  failed FEC block into a concealed band instead of a stalled frame — per-slice
  H.265 with GS-side skip-slice concealment (the waybeam-link §6.3b approach;
  `docs/feedback-repair-rollout.md` phase-5 notes) — plus the GS's two cards
  (better-spaced antennas are the cheapest further dB).
- **Smoothness:** a vsync-locked player; each frame's source bodies sent
  before its repair bodies; the encoder's GDR and I:P cap as today; the ~0.65
  airtime budget kept as the headroom it is.
- **Uplink for control only:** RCF, telemetry and MSP stay on the robust
  MCS0/20 LDPC+STBC path. Video integrity never waits on the uplink — the
  feedback-repair flights showed it is least reliable exactly when loss
  happens.

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
2. **Vsync-locked player** — player-only (`docs/latency-budget-findings-2026-08-31.md`
   follow-up 1, ~10 ms of the 10–25 ms `dsp` swing). *Gate:* `lat.log` `dsp`
   and e2e p50 on the bench, then a flight.
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
5. **Trim FEC** — only after step 4: 0.5/0.25 → 0.35/0.2 → 0.25/0.2, one
   A/B flight per step. `fec.log`'s "would fail at ov X" column says how many
   episodes each step turns from repaired into concealed (the 2026-10-09
   garage flight: at 0.25, ~1 in 5 base and ~2 in 5 enh episodes at MCS0).
   *Gate:* concealed bands vs stalls per minute, and the bitrate it buys
   (~+20% video per MCS at ~0.2, arithmetic not measured).
6. **Optional range work:** a 10 MHz floor rung; GS antenna spacing / a
   third card; per-rung TX power at the calibrated walls (`docs/calibration.md`).

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
