# Two spatial streams (2SS) instead of 80 MHz: research note

Status 2026-10-31: **unmeasured.** Nothing in this stack has ever flown or
benched a two-stream video frame. `linkbench-tx --nss 2` (d19) is the first
tool that can send one; the ladder, encoder caps and power tables are all
one-stream.

## The idea

Both ends are 2T2R 8812EU (drone 1, Janet, the GS cards; the Wyvern is a
2T2R 8812CU). Two streams carry two different symbols at once, one per
antenna, so MCS4 at 40 MHz becomes HT MCS12: 162 Mb/s PHY instead of 81,
about MCS4/80's 175.5. If it holds up, the top rungs could stay at 40 MHz:
more usable channels, none of the 80 MHz-only trouble (upper-block spikes,
GS cards hearing the 20/40 rungs 1-1.5 dB worse when tuned 80, the 40/80
wide-TX stall).

## Why it is not "double for free"

- **It replaces STBC.** Every video frame already uses both drone antennas:
  one stream, sent twice in space-time code (STBC). Two antennas can carry
  two streams *or* one stream with STBC, not both. 2SS gives up STBC's
  protection against fades.
- **At best it matches 80 MHz, it does not beat it.** Each antenna has its
  own amplifier. One stream with STBC puts both amplifiers' power into one
  stream over 80 MHz of noise; two streams put one amplifier's power into
  each stream over 40 MHz of noise (half the noise). On a perfect channel
  that comes out the same signal-to-noise per symbol, at about the same
  PHY rate (216 vs 234 data subcarriers). Every real-world imperfection
  below costs 2SS, not 80 MHz.
- **It needs two separable paths.** The GS card separates the streams only
  if its two antennas hear the two drone antennas differently. Drone 1 and
  Janet fly RHCP, and the GS's omni and patch are RHCP too (operator,
  2026-10-31): every antenna has the same polarisation. In line of sight
  that leaves only the antennas' spacing to tell the paths apart, and a few
  centimetres on the drone is far too little at range (line-of-sight MIMO
  wants the two spacings' product near wavelength x distance / 2: about
  26 m² at 1 km on 5.8 GHz). So with today's antennas two streams should
  work only close in or with strong reflections, and fall off well before
  the 1SS rungs. What could work in the air: one RHCP and one LHCP antenna
  per radio, at both ends. Opposite hands stay apart however the drone
  rolls or yaws (unlike a vertical/horizontal pair), and FPV circular
  antennas keep the two hands ~15-20 dB apart on axis, less off axis
  (unmeasured on ours). The cost: the 1SS STBC rungs below lose about the
  3 dB the GS card now gets from both its antennas hearing both drone
  antennas (estimate, line of sight), and a reflection, which flips the
  hand, lands on the other antenna instead of being rejected. Both antennas
  of a two-stream GS card must be the same type: an omni beside a patch
  leaves one stream on the weak antenna wherever the patch points.
- **The GS loses its array gain.** Today each card combines both antennas
  for one stream (~3 dB). With two streams both antennas go to separating
  them. The second card still gives packet-level diversity.
- **Path A is the 8812EU's weaker TX chain.** 1SS frames use path B only,
  chosen on merit: path A delivered ~2.5x fewer MCS7 frames at ~3 dB worse
  EVM on the bench module (devourer `docs/8822e-quirks.md`). 2SS uses both,
  so one stream rides the weak chain.
- **The 2SS power shape is uncalibrated.** With `power_mode = "offset"` the
  drone's per-rate table covers MCS0-7; devourer leaves MCS8-15 and VHT 2SS
  at the reference, with no back-off for 16-QAM. They may compress.
- **50 Mb/s is a CPU problem too.** MCS4/80's 45 Mb/s cap is set by the
  drone's CPU (FEC work), not the air. 2SS changes the air, not the CPU.

## Bench plan (d19 drone, any GS)

Drone 1, channel 36 (spur-free), daemons stopped at both ends as in
`docs/bw80-research-2026-10-12.md`. A single probe is worth ±3 points.
Run it twice:

- **Today's antennas (all RHCP)** at short range: proves two-stream frames
  air and decode, and finds the distance where they stop. Expected:
  well short of MCS4/80.
- **RHCP + LHCP** on the drone and on one GS card (same antenna type on
  both of that card's ports): the real test. Run the 1SS STBC rungs on
  this setup too, to measure what they lose.

1. **Capacity**: `--nss 2 --bw 40 --mcs 3` and `--mcs 4` (HT MCS11/12),
   `--ldpc`, no `--stbc`, at `--bitrate 90M`, beside today's MCS4/40 and
   MCS3/80 / MCS4/80 with `--stbc`. Read as the B1 table there, trap
   included: the drone has never been pushed past ~100 Mb/s of air, so a
   flat result across MCS11/12 is its USB or CPU, not the air.
2. **What aired**: `linkbench-rx --rate-hist`; two-stream frames show as
   rate codes 0x17/0x18 (HT MCS11/12) or 0x39/0x3A (VHT 2SS MCS3/4).
3. **Both GS chains alive**: the range output's per-chain RSSI. A dead
   chain B makes every 2SS frame fail, and a total-frame count hides it.
4. **Range**: `--bw 40 --range-sweep --range-bw-mask 2 --range-mcs-mask 0x18`
   with `--nss 2`, then the same without `--nss` and with `--stbc`, and
   MCS3/4 at 80 MHz; alternate the runs, several of each.

**Go** only if MCS12/40 delivers what MCS4/80 does at the same distance
on an antenna arrangement the drone can fly with, and the 1SS rungs on that
arrangement still reach far enough. **No-go** if it
needs the drone closer than MCS4/80 does: the 80 MHz rung is then the
better top.

## Flight code (only after a go)

A rung gains a stream count; `[[link.ladder]]` and `[[encoder.rung]]`
take `nss`; the airtime model, the RCF/Telem rung encoding, the cal sweep
(HT MCS0-7 only today) and the GS rate parse learn MCS8-15 / VHT 2SS; a
2SS power shape is measured (`maburcal`); the OSD shows `MCS12/40` or
`2x MCS4/40`. The 1SS STBC rungs stay below as the fall-back.
