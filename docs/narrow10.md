# 10 MHz bottom rung (opt-in, gs10 / d11)

Below MCS0/20 the whole link can re-clock to 10 MHz narrowband: about
3 dB more link budget for half the air capacity (MCS0/10 = 3.25 Mb/s
nominal, so the 500 kb/s bottom-rung cap still fits). Backlog item 10.
Unflown and unbenched: the first runs are for the logs.

## Switch

GS `[link.narrow10] enable = true` (off in the bundle). The drone follows
any GS order; its only knob is `radio.narrow_rescue_ms` (1500).

## How a move works

10 MHz is a radio mode on both ends, not a frame width: a 10 MHz receiver
hears only 10 MHz frames and a 20 MHz one none of them. Frames keep 20 MHz
radiotap; the halved sample clock narrows them (devourer
`docs/narrowband.md`). So every move is coordinated (`gs/src/narrow_mode.h`):

1. **Enter.** MCS0/20 losing picture after FEC (`enter_post_loss_pct`) for
   `enter_ms`: the RCF op becomes MCS0/10, with its own FEC
   (`overhead_base` / `overhead_enh`). One spare card (not the TX card)
   re-clocks to 10; the TX card stays at 20 so the order keeps reaching the
   drone, which re-clocks before its next frame.
2. **Confirm.** The spare card must hear the drone at 10 within
   `confirm_ms`; then every card follows. If not, the order is undone and
   nothing re-enters for `cooldown_ms`.
3. **Rescue.** At 10, no drone frame on any card for `rescue_ms`: the GS
   goes back to 20. The drone does the same on its own after
   `narrow_rescue_ms` without an RCF, and stays out until the GS orders a
   wider rung; the 3 s failsafe backs both up.
4. **Leave (blind).** Nothing can probe 20 MHz from a 10 MHz radio, so the
   way back needs `clean_ms` without loss and the best card's SNR (read at
   10) at least 3 dB (the narrowband gain) + `promote_margin_db` above
   where 20 MHz failed. The spare card goes first again; if it does not
   hear the drone at 20 within `confirm_ms`, the link stays at 10.

While at 10 the ladder is frozen below its rung 0 (it resumes at rung 0),
hops and spare-card scouting pause, and nothing is probed.

## What to read in the logs

- GS: `maburgs narrow10: enter|on|cancel|rescue|leave|stay|off: ...` with
  the lead card, post/pre-FEC loss, best and entry SNR; card switches log
  `maburgs radio: card N width -> 10 MHz ... (10 MHz rung)`.
- Drone: `maburd: width A -> 10 MHz on ch C (10 MHz rung) in T ms`,
  `maburd: 10 MHz rescue: no RCF for T ms`, and `n10_rescue=` on the stats
  line. A drone at 10 shows `w=10`.

## Counterparts

- Half the capacity: re-sends (NACK) and the FEC repair window get tighter
  in airtime.
- 10 MHz is more sensitive to the frequency offset between the radios
  (devourer bench: 5 MHz at 5 GHz was CFO-limited; 10 MHz was fine there).
- Each move costs the frames in flight during the switches, plus the
  confirm time when a move fails.
- The blind promote can bounce: up to 20, fail, back down after `enter_ms`.
  `cooldown_ms` (20 s) bounds how often.
