# CPE510 relay as a `maburgs` card: bench acceptance, 2026-10-02

**Status:** passed — the relay runs as a `RemoteCard` next to the USB cards (legs 4-attenuator and 6c not run); the `alive()` fix found in leg 6 is in the code. Current behaviour → docs/cpe510-relay.md.

Extracted from docs/cpe510-relay.md during the 2026-10-11 docs tidy.

## Bench acceptance (spec §6)

Standing gate `tools/bench/ausniff.py`; no `aucadence` (nothing touches the
balancer, venc or UEP). One CPE, so every leg is single-relay. In order:

Setup common to all legs: drone on ch136 HT40-, `low_power` off
(full rate, 60 fps); CPE v3 (`mabur-openwrt` 0b8804c); host = this build
box (RTL8822E 0bda:a81a, USB-Ethernet to the CPE, host `maburgs` from
`build/gs/`); Radxa = the GS, 2x RTL8822E + the same USB-Ethernet adapter
moved over (eth0 took 10.83.11.116 from the CPE's DHCP with no config).
`ausniff` = `python3 ausniff.py --ring /dev/shm/mabur-au --seconds 30|60
--json`.

1. Host `maburgs` build, one USB card + the relay over host Ethernet: relay
   row owned+tuned, both rows hear, ausniff clean, relay `gaps` 0.
   Measured 2026-10-02 — relay card 1 `owned and tuned`, boot scan
   committed home 136, SESSION, ladder to rung 4 (mcs4/40); both rows hear
   (USB 418.8k frames / 70 CRC, relay 419.2k / 5 CRC), relay `gaps` 0,
   `your_drops` 0; ausniff 60 s: 3616 AUs, 1808/1808 complete,
   frame_id_gaps 0, resyncs 0, 60.3 fps. The TX selector sat on the relay
   for ~4 s (77 RCFs through it) then returned to the USB card on RSSI
   (within 2 dB).
2. GS with `relays = []`: USB-only regression, ausniff clean, selector sane.
   Measured 2026-10-02 — Radxa, `relays` absent (USB-only regression, new
   `maburgs`+`maburplay`): boots, SESSION, rung 4; ausniff 30 s at t+25 s
   after restart: 1814 AUs, 1 incomplete enh, 1 frame_id gap (post-restart
   window), 0 resyncs; steady state: 1815 AUs, 1 incomplete enh, 0 gaps,
   0 resyncs, 60.5 fps. Rollbacks left on the GS: `maburgs.pre-relay`,
   `maburplay.pre-relay`, `/config/maburgs.toml.pre-relay`.
3. GS + relay (USB-Ethernet adapter on the Radxa): as 1.
   Measured 2026-10-02 — Radxa, 2 USB + relay (card 2): `cards: card 2 =
   relay 10.83.11.1:8310`, owned and tuned, boot scan commit home,
   SESSION, rung 5 (mcs4/40); relay RSSI −72 dBm (panel), loss 0, gaps 0;
   ausniff 30 s x3: one resync in the first run (t+36 s after restart),
   then 1816/1815 AUs, 1 incomplete enh each, 0 gaps, 0 resyncs, 60.5 fps
   — identical to leg 2. GS CPU: `maburgs` 38 %, `maburplay` 11 %, 40 %
   idle.
4. Uplink: `tx_card` pinned to the relay — RCF-heard vs the 94–98 % bench
   record above; unpinned — attenuate the USB antenna, the selector moves to
   the relay on RSSI and back. Measured 2026-10-02 (host) — `tx_card`
   pinned to the relay, 90 s: relay 20.84 RCF/s sent, drone `rcf.rx_pps`
   19.63 ⇒ **94.2 % heard** (record 94–98 %), relay `tx` 1840, `tx_fail` 0,
   `tx_refused` 0, rung 4 held. Unpinned, the selector moved 0→1→0 within
   2 dB in leg 1; the attenuate-the-USB-antenna half is NOT run (no
   attenuator at hand).
5. Hop with the relay as lead (USB card TX): hop inject test; record
   TUNE→ready time. If it dwarfs FastRetune, a relay-aware confirm
   allowance is a follow-up.
   Measured 2026-10-02 (Radxa pinned to ONE USB card via `[[radio.cards]]` +
   the relay, so the relay is the non-TX lead; co-channel jam on op 136
   from a second RTL8822EU with `tools/bench/benchjam.sh` defaults, 70 s):
   the ladder demoted 5→2 under the jam; the hop controller went hold
   (home blocked) → `order epoch 1 target 112` with **`hop_lead card 1` =
   the relay**; the relay's STATUS read ch 112 within the same 200 ms
   sideport tick; **`lead_confirm +160 ms`** (video seen on 112 through the
   relay); `hop_follow` moved the USB card; `verify_pass +1003 ms`;
   `hop_restore` 2→3 and back to rung 5 within 7 s; the link stayed on 112.
   ausniff over the 90 s spanning jam and hop: 5414 AUs, 7 incomplete (6
   enh, 1 base), 0 frame_id gaps, 0 resyncs, 60.2 fps. The relay's
   TUNE→confirm of 160 ms is far below `hop.confirm_ms` (1000), so no
   relay-aware confirm allowance is needed.
6. Failures: Ethernet pulled → dead card → replug → owned again; web page in
   GS mode against the same relay → page reports taken, `maburgs`
   unaffected; CPE reboot onto its default channel → `RemoteCard` re-tunes.
   This leg's Ethernet-pulled case found `alive()` reading UP through the
   whole outage (fixed 2026-10-02: see the Lifecycle section of docs/cpe510-relay.md).
   Measured 2026-10-02 — (a) relay daemon stopped 15 s mid-session (host
   and Radxa): the selector left the relay at once (dead-card path),
   `reconnects` climbed every 2 s, `owned and tuned` 0.4 s after the
   daemon returned, session never dropped, drone RCF rx 19.3/s
   throughout. Found and fixed here: `alive()` read UP between reopens
   (c41f942) — after the fix 323/323 samples read `up=false` with the
   relay unreachable; the fix was confirmed on both the host and the
   Radxa. (b) second UDP client (native `webgs --mode gs`) while
   `maburgs` owns the relay: `relay owned by another client`, `maburgs`
   unaffected. (c) CPE reboot onto its default channel: not separately
   run (the daemon restart in (a) re-reads `BOOT_CHANNEL=136`, the op
   channel, so a re-tune was not exercised). Radxa repeat (2 USB + relay,
   fixed binary c41f942): relay daemon stopped 15 s mid-session — relay
   `up` false within 0.3 s (72/72 outage samples), `reconnects` every 2 s,
   owned+tuned 0.4 s after the daemon returned, session held at rung 5, TX
   stayed on USB card 1.
7. Boot scan with the roster: one USB card scouts at 20 MHz in one-card
   mode (beacons in its home windows), the relay beacons DISC on home
   whenever `ready()`; rendezvous time in the usual range, and with the
   CPE unplugged it still rendezvouses on the USB card alone.
   Measured 2026-10-02 — relay up at boot: SESSION within 0.2 s of the
   first sideport datagram in every run (legs 1/4a/6; the drone was
   already in range); relay daemon down before `maburgs` starts (host):
   SESSION on the USB card alone, scan committed home, rung 4, 94.6 %
   RCF-heard, relay joined ~1 s after its daemon started. Radxa repeat
   (2 USB + relay, fixed binary c41f942): relay stopped before an
   `S96maburgs` restart — SESSION 0.2 s after the first datagram on the
   two USB cards, relay `up` false throughout, and when the daemon came
   back the relay went STATUS `retuning` → `owned and tuned` within the
   daemon's next stats line (**TUNE→ready ≈ 50 ms** when timed directly
   on the wire, 2026-10-04: `retuning` STATUS after ~7 ms, `tuned` after
   50 ms, 30/30 retunes; the "~0.2 s" first written here was the log's
   resolution, not the relay's), so the relay's tune is far below
   `hop.confirm_ms`); ausniff afterwards 1815 AUs, 3 incomplete enh,
   0 gaps, 0 resyncs, 60.5 fps.

Not run: 4 (attenuated-antenna auto-switch half), 6c (CPE reboot onto a
*different* default channel). Left on the GS after the bench: new
`maburgs`/`maburplay` with
`relays = ["10.83.11.1:8310"]`; rollback trio alongside. A later hop test
found the boot scan committing home → 144 with the relay unable to tune
there (see docs/cpe510-relay.md "ath9k quirks") — resolved by the 2026-10-03 CPE image
(full channel grid); the GS's shipped candidates `[144, 112]` stand.
