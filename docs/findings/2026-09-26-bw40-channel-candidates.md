# 40 MHz channel candidates and the channel-64 loss — 2026-09-25/26

**Status:** history of the candidate lists that flew beside the old home 136, plus an unexplained loss on 60+64 that is still unresolved (bench row 8: `maburcal` on 64 and 112, fixed-rung linkbench 40/2 on 64 vs 136; swap 64 for 128 if it fails). The current default set and its rules → docs/channel-select.md "Config", docs/bw40.md "Channels".

Extracted from docs/bw40.md during the 2026-10-11 docs tidy.

## Candidate lists before the channel set (2026-10-03)

The shipped default `channels = [40, 64, 112, 144]` (2026-10-03) replaced the
old home-136-plus-candidates split entirely: there is no home any more, and
136 itself is not in the new default. Earlier candidate lists all flew
alongside the separate home 136 that redesign removed: 2026-09-25
`[144, 64, 112]`, 2026-09-26 `[144, 112]`, and before that `[144, 40, 128]`.

36+40 (primary 40, 5190-5230) was readmitted as the set's fourth member on
2026-10-03. It was dropped from the 2026-09-25/26 candidate lists as
"DJI O4's 5.1 GHz band; the bench saw a home router at 80 MHz across
36-48" — that finding was this bench's own local RF environment and has
not been re-measured since, so treat it as a live caveat on 40, not a
cleared one.

## Channel 64: unexplained loss (2026-09-26)

60+64 (primary 64, 5290-5330) showed unexplained loss on the 2026-09-26 bench
with only 136 calibrated. After a hop there the bench link sat at 40/1 —
every 40/2 probe came back lossy (~1 body in 12) and per-card loss ran
4 % / 14 % against 0 % on 136, with RSSI 5 dB STRONGER than on 136, SNR
equal, and NHM busy ≈ own airtime (no interference). Unmeasured suspect:
drone TX power on an uncalibrated channel (walls are per channel; only
136 was calibrated at the time). 64 ships in the default set anyway,
pending `maburcal` on 64 and 112 plus a fixed-rung linkbench 40/2 on 64
vs 136 — swap 64 for 128 (the primary of 124+128) if it fails. 108+112
is equally uncalibrated and has not been flown on.
