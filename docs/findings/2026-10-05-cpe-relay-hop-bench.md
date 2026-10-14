# CPE510 relay interference detection + hop (protocol v4): evidence and bench, 2026-10-05

**Status:** passed (rows 1, 3, 5, 6, 7; row 4 skipped, row 8 partial) — a relay-only GS detects, sweeps, ranks and hops; the three findings at the end are still open. Current behaviour → docs/cpe510-relay.md "Interference + hop on a relay".

Extracted from docs/cpe510-relay.md during the 2026-10-11 docs tidy. Code then: relay side `../mabur-openwrt` branch `relay-scan-v4`, GS side this repo branch `relay-hop` (merged, PR #72).

## Evidence: busy − rx is the interferer signature

**Evidence (2026-10-05 bench, live CPE, drone off): busy − rx is the
interferer signature.** Manual sweeps (`iw dev mon0 set channel N HT20`,
1 s dwell, `iw survey dump`) over channels 36–177:

| source | busy % | rx % | busy − rx |
|---|---|---|---|
| WiFi traffic (e.g. ch153) | 34 | 33 | ≈ 0 |
| analog VTX carrier, on-channel (ch153) | 100 / 97 | 0 / 11 | ≈ 100 |
| analog VTX carrier, adjacent (ch157) | 12 / 82 | 6 | — |
| DJI O4 (ch165+169, ≈40 MHz around 5835) | 64/64, 73/73 | 13/12, 5/4 | 50–70 |

`busy` alone cannot tell WiFi from an interferer (both can read high);
`rx` is the relay's own count of frames it could actually decode, so
`busy − rx` is non-decodable airtime — near 0 for real WiFi, high for
anything the relay can hear energy from but not demodulate. The hardware
cycle counters **reset when the radio tunes onto a channel** (a
before/after snapshot across a retune gives negative deltas — read after
the dwell, or baseline right after the tune, which is what `SURVEY`'s
`gen`-keyed baseline does) and carry a **~3 ms "busy" artifact right after
every tune**, present even on a silent channel (subtracted the same way).
**Noise is not a usable signal** (cal rejects out-of-range NF; bench
2026-10-05): the same channel read noise −46 one pass and −95 the next —
`busy − rx` is the metric, not `ah->noise`.

## Timing budget — derived (spec §7) vs bench

Timing budget — derived (spec §7), now superseded by the bench below
(4-channel set / 3 candidates, 2 × 20 ms observe):

| milestone | relay-only (derived) | relay-only (bench 2026-10-05) |
|---|---|---|
| detection (2 of 3 × 150 ms) | ~300–350 ms | first interfered window +0, trigger +150–300 ms |
| sweep (2 passes) done | ~630 ms | SCAN_RESULT +376–525 ms (sweep itself 224 ms) |
| order (5 RCF × 50 ms) | ~880 ms | +625–774 ms |
| relay `TUNE` + video lands → Confirm | **~0.9–1.05 s** | **+895 ms (jam) / +1121 ms (O4)** |
| `verify_pass` | +1 s | +1 s |

## Bench 2026-10-05 (relay v4 + `relay-hop`)

Rig: CPE510 on the v4 relay (`relay-scan-v4`, hot-swapped binary; the v3
binary kept at `/root/mabur-relay.pre-v4`), wired to the **host** (USB NIC,
10.83.11.1), not the GS. Relay-only rows ran the host-built `maburgs` with
the bench GS's `maburgs` stopped (one commander); the host's RTL8822EU was
first unplugged, then (for the jam rows) used as the jammer with `maburgs`
run in an unprivileged user+mount namespace whose `/dev/bus/usb` is an
empty tmpfs (`unshare -r -m`), so the auto-scan finds no USB card. Drone
disarmed (30 fps), set `{40, 64, 112, 144}`, auto.

| row | result |
|---|---|
| 1 nl80211 timing | **PASS** — `sweep id=77 done in 224 ms (max retune 19 ms)` for 3 ch × 2 passes × 20 ms; silent channels read 0 % busy (the 3 ms tune artifact is gone); SURVEY every ~52 ms; relay CPU 0 % → 2–3 % with a subscriber and SURVEY on |
| 2 `ofdm_err` under the jam | **barely moves** — 0–3 OFDM/HT PHY errors per verdict window under a decodable 802.11 jam (decodable frames are not PHY errors). `raised` contributes nothing on a relay; relay detection rests on `blocked` (non-WiFi) and `contended` (WiFi). `fa_pps` stays shared |
| 3 relay-only, clean | **PASS** — `cards: no USB radio; running relay-only (1 relay)`, owned and tuned, search finds the drone, top rung mcs4/40; ausniff 616 AUs / 20 s, 0 gaps, 0 resyncs; verdict healthy, no false sweep/hop while clean (relay busy ≈ rx ≈ our own video) |
| 4 analog VTX | not run (skipped by the operator) |
| 5 DJI O4 co-channel on 144 | **PASS** — relay busy 65 %, own airtime 12.5 % → `blocked` (evidence 0x61); SCAN 40/64/112 all 0 %; order 64, `one_card_retune` +224 ms, `lead_confirm` +347 ms; **~1.12 s from the first impaired window to video on 64**; verify_pass; ausniff on 64 clean (466 AUs / 15 s, 0 gaps) |
| 6 WiFi jam on 144 (`benchjam`, 6M/1000 B/500 pps) | **PASS** — evidence 0x09 (impaired + `contended`, ~71 foreign/window; busy ≈ rx 67 % — a decodable jam is `contended`, not `blocked`); order 40, `lead_confirm` +895 ms from the first interfered window (≈1.05 s from onset). 11 s later real foreign traffic on 40 (the 36–48 neighbour router) → a second hop: that sweep saw the still-jammed 144 at 62–65 % busy and the ranker chose 112; ausniff clean on 112 |
| 7 web GS relay mode (headless Chrome 147 over CDP, page built from `relay-hop`) | **PASS** — owned the relay, found the drone on 112 (30 AU/s, 0 hitches, 0 relay seq gaps); jam on 112 → `order 64`, `one_card_retune` +244 ms, `lead_confirm` +312 ms, `CHANNEL 64`, verify_pass; healthy on 64 for the remaining ~110 s; clean Disconnect/`DONE`. The page stayed healthy (29–31 AU/s) for ~15 s of jam before the link was actually impaired — no hop while healthy, by design |
| 8 relay + 8812EU | **partial** — clean channel PASS (both cards up, relay own=1 gaps 0, ausniff 616 AUs / 20 s clean, relay busy 17.4 % ≈ rx 18.9 % in the V record, no false sweep); the interference half was not run with the dongle in |

Findings (not fixed in this branch):
- **~250 ms lost between SCAN_RESULT and the order** on every relay hop:
  the trigger is held off while the sweep is pending and the verdict then
  needs 2 interfered windows again. Keeping the trigger latched across the
  sweep would bring onset → video to ~0.8 s.
- **Relay-only auto boot relocates to `channels[0]`** after "link where
  found" with no measurement behind it (seen: 64 → 144 at every start).
  Harmless, but a pointless hop on each relay-only boot.
- **The web page shows a ~1.5 s video gap after a hop** (AU/s 13 → 0 → 15
  → 27; 56 truncated AUs over the run) where `maburgs` resumes within
  ~300 ms — likely the page's key-frame wait after a channel change.
