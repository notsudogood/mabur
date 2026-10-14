# Channel select (home + candidates design) — bench 2026-09-13

**Status:** describes the superseded home/candidates design (home, split_home/reunite, A records, home_window_ms are all gone). What survives into the current code: every scout dwell is silent, and the ranker subtracts own frames and the TX leak (leak_per_frame = 1.0) → docs/channel-select.md. The spike that preceded it: docs/findings/2026-09-13-channel-scan.md.

Extracted from docs/channel-select.md during the 2026-10-11 docs tidy.

### The `cca − own` assumption (2026-09-13)

The ranker's busy score (`gs/src/channel_ranker.{h,cpp}`) was:

```
busy = (cca_ofdm − canonical_frames) + fa_ofdm + foreign_frames
```

`cca_ofdm` counts every OFDM CCA event the chip saw, including the ones
caused by our own beacon and RCF traffic — subtracting `own` (canonical-SA
frames decoded) is meant to leave foreign busy only, on the assumption
that one decoded own-frame costs exactly one CCA event. If a decoded frame
actually costs more than one CCA event (aggregation, retries at the PHY),
the subtraction under-corrects and channels get scored busier than they
are. The formula has since grown a third term, `leak` (above) — the same
assumption, extended to cover TX that is never decoded as "own" at all
because the scout card isn't the one receiving it.

### Bench validation 2026-09-13

The committed record behind `leak_per_frame = 1.0` (the `cca − own`
rows: one decoded single-MPDU frame is one CCA event) and behind the
silent dwell. It describes the superseded home/candidates design — home,
`split_home`/`reunite`, `A` records and `home_window_ms` are all gone —
and is kept verbatim as what was measured then.

Bench, drone on the desk, GS session `/media/dvr/log/0069` (`scan.log`
carries every K/M line below). Deployed binary-before-config on both ends;
rollbacks `maburgs.pre-chansel` / `maburd.pre-chansel` + `*.toml.pre-chansel`
(the drone's `maburd.pre-cal` was pruned to make room).

| check | result |
|---|---|
| C record, both GS cards | `RTL8822E jaguar3 2x2 1f 5080-6165 1 1 1 1 1` — all four sensors valid incl. the absolute floor; the drone (no floor opt-in) reports `floor=0` |
| scout cadence, two cards | 11 visits per channel in 18 s (round ≈ 1.1 s at the defaults) |
| pick, two cards, home 136, cands 149/153/161 | `K 136 44 136:5:-95 149:52:-94 153:53:-92 161:53:-95`, `M all 136 136 commit`, no drone retune |
| pick, two cards, home 153, cands 136/149/161 | `K 153 15 153:9 136:152 149:45 161:39` — home won again; 136 took one 152-busy visit |
| pick + move, ONE card, home 153 | `K 161 17 153:32 136:42 149:12 161:8`, `M all 153 161 commit`, drone `retune 153 -> 161 (disc)`, video in SESSION on 161; `scoutgate=3` sends held during the join window |
| ausniff on the moved link | 60.2 fps / 1 gap first pass (post-restart phantom), then 60.3 fps / 0 gaps / 0 incomplete |
| link loss (drone stopped), one card | `M 0 161 153 split_home` at loss + 5 s |
| drone restart, one card | drone boots on 153, caught in a home window: GS `M 0 153 161 reunite`, drone `retune 153 -> 161 (disc)`, 60.4 fps |
| `cca − own` (beacons) | on home the scouting card saw own=12 beacons and cca=12: one decoded single-MPDU frame is one CCA event |
| `cca − own` (video, A records) | cca 226-948 against own ≈ 2200 frames/s: CCA counts PPDUs, so under A-MPDU `cca − own` clamps to 0 and the A record's busy is `fa + foreign` |
| A records at 1 Hz | no change in ausniff cadence (60.3 fps with them on) |
| restored flight config (home 136, two cards) | `K 136 10 136:1 149:50 153:53 161:58`, commit home, 60.5 fps |
| interference test after the fix: home 136, candidates 44 (router, 80 MHz on 36-48), 120 (clean), 149/153/157/161 (neighbourhood), drone off then on | 13 rounds: 44 worst 274 (305 foreign frames), 157 worst 145 (undecodable), 149 worst 20 (43 frames), 120/136/153/161 at 1-2; at freeze (26 rounds) `K 136 26 136:6 44:336 120:62 149:140 153:67 157:445 161:16` — the worst-visit rule caught later bursts on 120/153 and home won; commit home, 60.4 fps |

Not done: the 2 s RF fade (needs an antenna pull), the two-card split/reunite
(needs a home-losing pick, see the bias below), the `tx_gate` exclusive
latency measurement, and `lat.log` e2e with A records on vs off.

### Bench findings 2026-09-13: own beacons dominated every reading (fixed the same day)

With two cards the beaconing card's DISC TX (every 20 ms, a few cm from
the scout) leaked into the scout on EVERY channel: on home the leak
decoded as our own frames and `cca − own` subtracted it (home read ~2), on
a candidate it was undecodable energy and counted as busy (~25-40 per
250 ms with zero frames). The same channel read 5 as home and 152 as a
candidate minutes apart. With one card the card's own TX leaked into its
receiver during the home window (home read ~25 against candidates at
0-12). Silencing the beacon only for the home dwell, and parking the home
card 40 MHz away, changed nothing — the leak is on the candidate side — so
parking was dropped again.

Fix: every dwell is silent. Two cards: the TX card holds its DISC while
the scout is observing (`quiet()`). One card: the home/op cycle is a beacon
phase, a quiet gap, then a silent observe with its own discard read. After
the fix (drone off, home 153, candidates 136/149/161): two cards, scout =
card 1: 136 0.7 / 149 6.6 / 153 2.9 / 161 1.6 mean busy (worst 5-10),
floors −92…−96 — home was no longer privileged. One card, card 0: 0.0 /
5.8 / 4.1 / 1.1; card 1: 0.4 / 8.6 / 5.0 / 3.8. Clean channels sat within
~10 units of each other, so with no improvement margin the pick among
clean channels is effectively arbitrary — this is exactly why
`pick_margin` exists and why the shipped default channel set
(`docs/bw40.md` "Channels") is curated to channels worth flying rather
than left to an automatic search of the whole band.

### Two small findings from the same bench (2026-09-13)

**`[[radio.cards]]` needs decimal VIDs.** The TOML subset rejects `0x0bda`
(`'0x0bda' is not a valid value`) and maburgs crash-loops on the respawn;
the bundle's commented example now says `3034`.

**The GS's own Wi-Fi AP drops stations around a restart.** The `aicwf_sdio`
AP logged STA churn at the moment of a maburgs restart (its USB resets), and
a laptop on that AP saw "no route to host" for ~20 s. Not a reboot: uptime
and the session directory were continuous.
