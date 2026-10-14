# Channel scan and auto channel selection — findings 2026-09-13

**Status:** spike validated the scan/floor sensor; the boot-time auto-pick it recommended was later built. Current: docs/channel-select.md "GS: search and measure (`ChannelScout`, gs/src/channel_scout.{h,cpp})"

Status: **spike answered, sensor built and GS-validated, nothing in mabur
changed yet.** Follow-up is a bounded mabur change (boot-time pick) described
at the end.

## Question

Can `../devourer` scan for a clean channel so mabur can set its channel
automatically?

## Answer in one paragraph

Yes. devourer already ships the whole scan/score/migrate stack in
`src/chanmig/` (dwell scheduler, survey record with our-video vs foreign
airtime, `RecommendEngine`, `MigGate` with Off/Advisory/Manual/Automatic, a
MAC'd ground-proposes/drone-commits protocol), `FastRetune` costs ~2.4 ms on
the 8812EU under live RX, and monitor RX is fully promiscuous. mabur never
retunes: `radio.channel` is applied once via `InitWrite` on both ends, the GS
sends `Disc.op_channel` but the drone deliberately ignores it and echoes its
own config channel in `DiscAck.agreed_channel`, which no GS code reads. The
one missing sensor — an absolute dBm noise floor on Jaguar3 — was built this
session in devourer.

## What was built (devourer, branch `jgr3-nhm-abs-floor`, UNMERGED)

Four commits on top of devourer master `3b15c7a`:

| commit | what |
|---|---|
| `6cc166e` | **fix** `read_nhm` pre-shifted NHM th[8..10] before the masked BB write, which shifts to the mask itself, so those thresholds read back 0 on every generation (background histogram buckets 9-11 were junk). Selftest pins every threshold register against a fake BB. |
| `6666dfa` | **feature** `DEVOURER_RX_NOISE_FLOOR` on 8822C/E fills `abs_noise_floor_dbm` from a second ~2 ms NHM window with absolute thresholds (`nf::kNhmAbsThDbm`, −104…−70 dBm) and cca/tx-on samples excluded — the vendor ACS's own method (`hal_dm_acs.c`, NHM_ACS); the vendor driver has NO idle-noise report for this chip (`phydm_noisemonitor.c` returns 0). Math in `src/NoiseFloorMath.h`. |
| `7082fc7` | **guard** reject the frozen readout (below). |
| `ab1fbaa` | `DEVOURER_RX_SWEEP` bins now carry the floor. |

ctest 57/57. Validation script: `tests/rx_noise_floor_active_onair.sh` (now
expects a J3 floor). Merge = merge commit per branch in devourer.

## GS validation (two 8812EU on the GS, maburgs stopped per run)

rxdemo cross-built from the mabur arm64 tree (`build-arm64/devourer/rxdemo`),
run on the GS with `DEVOURER_USB_BUS=2 DEVOURER_USB_PORT=1.1|1.4`.

- ch36/44/100/136/149: **−95…−96 dBm on both cards**, flat for 60 s once
  settled, 1 dB card-to-card. Passive rssi−snr floor where frames existed
  (ch36) was 4-9 dB away.
- **Frozen readout.** For the first 1-7 s after bring-up the BB reports a
  constant power estimate: ≈−97 for two reads, then a fixed −85 regardless
  of card/channel, each a single 3 dB bucket holding 241-255 of 255 samples;
  a transition (one window saturated in-band, `0x1d70` momentarily reading
  `0x50505050`) then makes the estimate live (spread over 2-3 buckets).
  On 2.4 GHz (ch6) the frozen −85 persisted for 60 s with rare live samples
  (−94, matching the passive −92.5). **Trigger not identified**: no devourer
  code path writes at that moment (RX-only sessions run only the phydm
  tick); IGI toggle, CCX hw-restart and CCA-off each change nothing; a
  `FastRetune` re-arms it. The guard rejects peak bucket ≥ 236 (frozen min
  241 vs live max 220 — a thin margin measured on two units) so the floor
  is null rather than a fake −85. The idle-duration guard is inert on J3
  (it reports the full window as duration).
- The EU cards' front ends are 5 GHz parts, but the chip did decode 2.4 GHz
  beacons on ch6, so the 2.4 GHz caveat is moot for mabur, not for devourer.

## 5 GHz band map (2026-09-13, drone OFF, GS location)

Sweep `36-64/4,100-144/4,149-177/4`, 2.5 s dwell, 3 rounds, card 2-1.4, then
the hot channels confirmed on card 2-1.1.

| channel | verdict | evidence |
|---|---|---|
| 36, 44, 48 | Wi-Fi APs | decoded frames, strongest −40…−56 dBm |
| 40 | busy | ~700 CCA events/dwell + some frames (40 MHz side of the APs) |
| 157, 161 | **undecodable energy, every round** | 1.7k-8k CCA/dwell, fa≈cca, 0 frames; 5785-5805 MHz = FPV video band; floor +2-3 dB |
| 108, 128 | bursty energy | hundreds-thousands CCA some dwells, 0 frames |
| 52-64, 100-124 (not 108), 132-144, **136**, **149**, 153, 165-177 | clean | ≤45 CCA/dwell, floor −95/−96 |

Card 2-1.1 (high-gain antenna) counts 3-10x more CCA events than 2-1.4 on
the busy channels and about the same on quiet ones — extra gain lifts
sub-threshold signals over the energy-detect threshold, thermal noise not.
Occupancy is therefore **not comparable across cards**; rank within one card.
The floor was the same on both.

Recipe (also in memory `gs-band-scan`): GS has no bash — pipe scripts to
`sh -s`; `/etc/init.d/S96maburgs stop` first, `start` after.

## How the scan measures (for the record)

Per dwell, one `rx.energy` record: (1) decoded 802.11 frames + strongest
RSSI; (2) the BB's CCA busy-event counter and false-alarm counter, read and
reset per dwell — event counts, not a time fraction; fa≈cca with 0 frames =
energy the OFDM decoder cannot sync to; (3) the idle floor from the 2 ms NHM
window. 5 GHz is OFDM-only (the CCK path is switched off at channel set), so
only the OFDM counters mean anything there.

## Robustness assessment for mabur

Solid: ranking channels within one card by busy/false-alarm/frames; the
floor as a confirming signal; the scan is cheap and live from the first
dwell once the card settled.

Fragile, with mitigations:

1. **Agreement between ends** — the drone ignores `op_channel`; the uplink
   loses 30-50 % of frames; a mismatched pair has no link and looks exactly
   like the stale-caps deadlock. Commit must be idempotent, the drone keeps
   listening on the new channel once moved, both ends fall back to the
   config channel on timeout; a drone reboot returns it to home, so the GS
   must re-rendezvous there on link loss. `RC_VERSION` flag day.
2. **Calibration** — TX-power wall tables exist for ch136 and ch149 only
   (`docs/calibration.md`); a wrong table parks every rate ~1.5 dB off.
   Candidate set = calibrated channels until more are calibrated.
3. **Scanning costs a card** — in flight a scout misses video and card 0
   already collapses at range: boot-time only, or a third card.
4. **Flapping** — boot-time pick cannot flap; in-flight needs chanmig's
   residency/cooldown/probation, unflown.
5. **Short windows** — 2.5 s dwells miss bursty interferers (108/128 varied
   round to round); ≥3 rounds; busy counters over the dwell matter more than
   the 2 ms floor window.

Value today is modest: ch136 is clean and the ladder plus FEC ride out
ordinary Wi-Fi. What this buys is the ch161 case — a continuous carrier from
another video transmitter, which the ladder cannot fix.

## Recommended follow-up (bounded mabur change, not started)

While the GS waits for discovery it is idle: one card scans the calibrated
set continuously (rank by CCA/FA/frames on that card, floor as tie-break),
the other listens on the home channel. When the drone appears the GS already
has a ranked list — no boot-time cost. Carry the choice in DISC; the drone
honours `op_channel`, retunes, acks from the new channel; both fall back to
home on timeout. Prerequisites: read `DiscAck.agreed_channel` on the GS (it
is never read today); `RC_VERSION` bump; per-card channel is already plumbed
(`RadioFrontend::Cfg::channel`, forced equal at `gs/src/main.cpp`). In-flight
migration (devourer chanmig, Advisory mode first) is a separate, later design.

Open items: merge the devourer branch; the frozen-readout trigger; the GS ssh
host key changed with the reimage (known_hosts still stale); after the last
restart maburgs showed card 1 at 0 frames/s vs card 0 ~100 foreign/s — check
with the link up whether that is just the antenna.
