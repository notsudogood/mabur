# Next build backlog

What the operator has agreed goes into the next GS (and drone) build, in
order. Each item names its evidence. Remove an item when it ships.

## 1. maburcal: stop calibrating rates to full power

Evidence (flight 2026-10-12, d4 drone + gs4 GS, calibrated on the bench the
same morning): the calibration wrote `rate_walls_rel = [63, 63, 63, 63, 40,
21, 23, 17]` and switched `power_mode` to `"offset"`. In flight the GS
measured EVM −10.5 dB at MCS1/2 and −13.5 dB at MCS3, flat across SNR 25–45
dB on both cards (the transmitter distorting, not the receivers overloading).
Flight 0027 at default power (`power_mode = "none"`) had −16.5 / −18.0 /
−19.5 dB at the same rates. MCS3 test packets lost 10 % (15 of 26 probe
episodes failed), so the ladder never left MCS2/20. Rolled back with
`/etc/mabur.toml.pre-cal`.

Cause, in `gs/src/cal_analysis.h`: a power level passes at 90 % delivered
(`CalThresholds::deliver_pct`), so a rate losing ~10 % at full power reads
as "no dip" and parks at the rail (`kRailRel`). The 2026-10-10 noise-cell
rule (forgive one 75–89 % coarse cell when the next two are back at 90 %)
also helps a row climb through the band where compression starts.

Changes:
1. A rate with no dip keeps its pre-calibration power instead of moving to
   the rail.
2. Raise the pass bar to what the ladder's probe gate accepts (~99 %), with
   enough frames per cell to measure it (longer run than 75 s).
3. Revert or narrow the noise-cell rule so it cannot forgive cells on the
   way up.
4. Check whether sweep frames now carry EVM (the flight probe bodies do;
   `docs/calibration.md` says the sweep frames carried none when it was
   tried) and reject any level where EVM falls sharply.

Bench-check before shipping: re-run on the same pair and compare EVM per
rate against `power_mode = "none"`.

## 2. 80 MHz top rung

Plan, constraints and go/no-go measurements: `docs/bw80-research-2026-10-12.md`.
First step is `linkbench --bw 80` plus a bench drone build carrying it; flight
code only after the bench says go.

## Offered, not yet decided

- `.srt` in Walksnail's line format so walksnail-osd-tool renders it, with
  the detailed line kept in a second `record-NNNN.link.srt`.
- Port Gilang's RTL8812CU 40 MHz TX fix (gilankpam/devourer `7c10751`,
  `3919a35` and follow-ups: DISDATAFB on wide 8822C frames, IGI floor) into
  our devourer for a Wyvern drone build. Does not apply cleanly; his own
  caveat is an estimated 4–8 dB (unmeasured) loss of uplink RX sensitivity
  on the CU while tuned wide.
