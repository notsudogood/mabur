# 10 MHz bottom rung (gs10 / d11; on since gs14)

Below MCS0/20 the whole link can re-clock to 10 MHz narrowband: about
3 dB more link budget for half the air capacity (MCS0/10 = 3.25 Mb/s
nominal, so the 500 kb/s bottom-rung cap still fits). Backlog item 10.

Flown (flights 0019/0020, drone 1, ch 132): three spells at 10, 21-53 s
each. Once on, the picture was clean (60 fps, no loss after FEC) 88 %,
38 % and 60 % of the time and gone 0 %, 8 % and 1 %. Against that, every
one of them started late: the picture had already been gone for 1-18 s,
and 10 MHz came only after frames returned by themselves (the gs21 rule
below is the fix, unflown).

## Switch

GS `[link.narrow10] enable` (on in code and bundle since gs14; `false`
turns it off, and `link.static_mcs` does too). The drone follows any GS
order; its only knob is `radio.narrow_rescue_ms` (1500).

## How a move works

10 MHz is a radio mode on both ends, not a frame width: a 10 MHz receiver
hears only 10 MHz frames and a 20 MHz one none of them. Frames keep 20 MHz
radiotap; the halved sample clock narrows them (devourer
`docs/narrowband.md`). So every move is coordinated (`gs/src/narrow_mode.h`):

1. **Enter.** Either of:
   - MCS0/20 losing picture after FEC (`enter_post_loss_pct`, judged per
     250 ms window) for `enter_ms` without a break;
   - (gs21, `silence = true`) no base-layer video at all for `silent_ms`
     (400) on MCS0/20. A window with no video also counts as a losing one
     for the first rule instead of breaking its run.

   The RCF op then becomes MCS0/10, with its own FEC
   (`overhead_base` / `overhead_enh`). One spare card (not the TX card)
   re-clocks to 10; the TX card stays at 20 so the order keeps reaching the
   drone, which re-clocks before its next frame. Both rules need the GS in
   session: it leaves the session 1 s after the last video
   (`VrxController`'s fixed `link_lost_ms`) and only a session's RCFs carry
   the order, which is why `silent_ms` is capped at 900. Before gs21 a
   window with no video read as "no data" and restarted the run, so a
   picture that stopped completely never ordered 10 MHz.
2. **Confirm.** The spare card must hear the drone at 10 within
   `confirm_ms`; then every card follows. If not, the order is undone and
   nothing re-enters for `cooldown_ms`, or for `retry_ms` (5 s) when the
   order came from a video stop (the spare card left no 20 MHz video
   behind). With `silence` on, a GS that leaves the session while entering
   keeps waiting out `confirm_ms` (that is the expected state after a video
   stop); with it off, the order is undone at once.
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
  the lead card, post/pre-FEC loss, best and entry SNR, how long the video
  had been silent and whether the GS was in session or searching; a
  video-stop entry reads `enter: MCS0/20 video stopped, ordering 10 MHz`.
  Card switches log `maburgs radio: card N width -> 10 MHz ... (10 MHz rung)`.
- GS stats (`flight.jsonl`): `narrow10` = `state`, `entries`,
  `silent_entries` (entries made on a video stop), `cancels`, `rescues`,
  `promotes`, cumulative since maburgs started; `link.op.bw` reads 10 while
  the op is the 10 MHz rung.
- Drone: `maburd: width A -> 10 MHz on ch C (10 MHz rung) in T ms`,
  `maburd: 10 MHz rescue: no RCF for T ms`, and `n10_rescue=` on the stats
  line. A drone at 10 shows `w=10`.

## Counterparts

- Half the capacity: re-sends (NACK) and the FEC repair window get tighter
  in airtime.
- 10 MHz is more sensitive to the frequency offset between the radios.
  devourer records 5 MHz at 5 GHz as CFO-limited on the 8822B; 10 MHz
  between two 8812EUs at 5 GHz has not been measured here.
- Each move costs the frames in flight during the switches, plus the
  confirm time when a move fails.
- The blind promote can bounce: up to 20, fail, back down after `enter_ms`.
  `cooldown_ms` (20 s) bounds how often.
- The video-stop rule only helps while the drone still hears the GS: if
  the uplink died with the downlink, the order never arrives and the
  attempt is undone after `confirm_ms`. In flights 0019/0020 the drone's
  last report before each long outage had the uplink at -90/-91 dBm with
  59-66 % of RCFs delivered; nothing shows what it heard during them. A
  rule where both ends try 10 MHz on their own after losing each other
  would cover a dead uplink too (a drone change; not built).
- It needs the link already on MCS0/20 when the video stops. From a higher
  rung the ladder only reaches rung 0 on its 1 s feedback timeout, the
  same moment the GS leaves the session, so a sudden total stop from MCS1
  or above still does not order 10 MHz.
- Whether 10 MHz would have held the picture through 0019's and 0020's
  17-18 s outages is unknown: there are no signal readings from inside
  them, and the fade may have been deeper than the ~3 dB 10 MHz gains.
- A false trigger (a short full stop on MCS0/20) costs the climb, not the
  picture: the bottom rung's video is capped at 500 kb/s either way, but
  leaving 10 needs `clean_ms` and the SNR margin (the flown spells lasted
  21-53 s). A drone switched off on MCS0/20 costs one attempt (the spare
  card at 10 for `confirm_ms`); after that the GS is searching and the
  rule needs a session. The TX card stays at 20 throughout.
