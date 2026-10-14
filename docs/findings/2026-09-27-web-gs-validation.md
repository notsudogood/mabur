# Web GS bench validation (2026-09-27 … 2026-10-04)

**Status:** Every page feature benched here shipped and passed: GS mode, the spotter, the redesigned UI, local recording (host gates), the channel set with relocation and one-card hop, the relay search-only scout, and the spotter follower. Items marked "seen, not fixed" below are historical. What the page does now → `docs/web-gs.md`.

Extracted from docs/web-gs.md during the 2026-10-11 docs tidy. This is the page's former `## Validation` section, plus dated measurements that had been inlined in its reference text (last section). The text is kept as written at the time, so builds, form fields and log lines may have changed since.

## Bench 2026-09-27 (first page)

Bench, 2026-09-27, bench drone on ch136 HT40, drone `low_power` off:

- **maburgs regression** (this page's changes don't regress the native
  daemon — same `LinkHealthAssembler`): two A/B pairs (master vs. this
  branch's `maburgs`, second pair run in reversed order), each leg fed two
  15 s `benchjam` episodes (power 63, 2000 pps, 1500 B, 6M) from a host
  8822EU. Identical demote reasons, ~1 s cascade to rung 0, probed
  re-climb 0→5 in 7–9 s in every leg; rung-5 `s3_residual` bounce rate 1.21
  vs. 1.13/min in the reversed pair (the *first* pair's 2.6 vs. 1.7/min
  tracked the time slot, not the binary — not attributable to this
  branch). `ausniff` 30 s clean in every leg (~1815 AUs, 60.5 fps, ≤3
  incomplete enh).
- **Web GS mode, GS daemon stopped**: linked (DISC→ACK→SESSION), climbed
  to the bundle ladder's top (rung 4, mcs4/40). Hand-covering the host
  card's antenna (RSSI −83..−86 dBm, SNR 9–13 dB) demoted to rung 1–2 and
  it recovered via clean probes in ~15–20 s; after a 30 s total outage it
  re-linked by itself. txfail 0. capture→glass p50 ~24 ms, p99 ~45 ms;
  link RTT ~9 ms (min 3–4 ms); FEC/assembly p50 ~6.6 ms; decode ~0.8 ms;
  hand-off 0.1 ms.
- **Spotter mode next to a running `maburgs`**: browser `sends` 0 /
  `rcf_sent` 0 / `txfail` 0 (Spotter never transmits, confirmed on the
  wire, not just by code inspection) — drone RCF rx rate 18.7/s over 30 s
  matched `maburgs`'s own sideport `drone.rcf.rx_pps` 18.5 (a transmitting
  browser would roughly double it). Video started after an 11 s wait for
  a natural IDR (see "Spotter key frames" above), then ran at 61 AUs/s.
- **Seen but not fixed**: 2 WebCodecs `EncodingError` decoder resets in
  ~10 min (each recovered on the next IRAP — the existing `gate.armed =
  false` / re-arm path, `onDecoderError()`). Left as a follow-up; no
  drone/wire signature was found in the ~10 min sample to attribute it to.

**Redesigned page, bench 2026-09-27** (Chrome 147 driven over DevTools,
bench drone on ch136 HT40, GS box off for GS mode):

- **Connect → Disconnect → Connect ×10** (GS mode, drone on): 10/10, no
  reload fallback; Disconnect ~200 ms (`teardown: rx 10–20 ms, chip stop
  45–100 ms` — the stdout teardown line of that build; it is now stderr
  `maburgs radio card 0: teardown rx <ms> [(reads aborted)]`, see
  Connect/Disconnect under Use), relink ~6.2 s (dominated by the card's ~5 s init), video
  back at once (link-up IDR).
- **Disconnect with the drone powered off**: before the interface-release
  abort (above) the core never exited and the 3 s reload fallback fired
  every time (recovery itself worked: page back on Config, next Connect
  claimed the card, "No drone on ch 136 / 40 MHz (still trying)" after
  10 s). With the abort: 3/3 in-page, click → Disconnected 584 ms
  (`rx 305 ms (reads aborted), chip stop 79 ms`), next Connect claims the
  card; drone-on cycles unchanged afterwards (5/5, 150–200 ms).
- **Config form**: Max MCS 3 → 4 rungs, link tops out at MCS 3; Fixed
  MCS 2 → `Pinned`, steady MCS 2 at 40 MHz.
- **VTX record** round trip: Record reached the drone and its `Telem`
  answer came back — this drone has no SD card, so the page showed
  `REC!` with "No SD card" (windowed button title and immersive tag). A
  recording that actually runs was not exercised.
- **Latency** (60 s steady, rung 4 mcs4/40): capture→glass p50 23 ms /
  p99 34 ms, FEC p99 11–13 ms, decode p99 1.0 ms, hand-off 0.1–0.2 ms,
  60 fps, jitter 2.6 ms — within noise of (or better than) the numbers
  above.
- **Layouts**: windowed, F → fullscreen immersive, config side panel,
  844×390 phone emulation (236 px float panel; its fullscreen button
  toggles the real Fullscreen API — see docs/web-gs.md "Use"),
  390×844 portrait notice; float-panel drag follows the pointer and clamps
  to x = 844 − 236 − 8, y = 390 − 48; video kept playing across every
  layout switch (one canvas).
- **Spotter next to `maburgs`**: `sends 0 / rcf_sent 0 / txfail 0`,
  60 AUs/s, Record disabled with its reason, Disconnect 200 ms.
- Seen, not attributed to this page: 4 TX `bulk_send` timeouts (rc −7) in
  ~5 min of GS mode, each followed by one decoder reset; one drone boot
  came up with a deaf radio (no RX on the host card natively either) until
  the drone was power-cycled.

Not yet exercised: a real phone over LAN TLS (layouts were checked in
Chrome's device emulation only), and the spotter mirroring a
`maburgs`-started recording (the GS's record wish was reset to off right
after `vtx_rec on` on the bench).

## Local recording, 2026-09-28

Host gates only — the live browser bench (this feature's own plan, Task 9
Step 2: record-and-play round trips, the mid-recording tab-kill/reopen
case, disconnect/card-loss downloads, `Both`-target start/stop, Recordings
list delete) is pending; the controller will add its results here after
the human runs it on the bench card and drone.

- **Host suite**: `ctest --test-dir build -R 'test_|host_e2e'` 162/162
  passed; `ctest --test-dir build -R 'gs_e2e|gs_au_e2e|player_e2e'` 3/3
  passed.
- **WASM/native parity** (`web/tests/test_wasm_parity.sh`, native `webgs`
  vs. `webgs_node`, includes the `--record` mp4 path added for this
  feature): 5/5 fixtures passed — byte-identical AUs, control traces and,
  where a run reaches a sync point, byte-identical mp4s. The standing
  fixture and the 25 %-drop long run correctly record nothing on both
  sides (no complete parameter-set AU reaches `RawDvr` in either case, by
  construction of those fixtures — see the script's own comments); the
  clean and 10 %-drop long runs each recorded a matching ~1.4–2.0 MB mp4
  on both sides.

## Channel set, relocation, one-card hop — bench 2026-10-04 (plan 2)

Branch `web-gs-core` (the card stack + `ChannelCore` in GS mode). Bench
drone on the desk, host RTL8812EU (0bda:a81a) as the page's card, headless
Chrome 147 driven over CDP against `python3 web/serve.py 8808`, the real
link key loaded into the page (`webgs.key`). `maburgs` on the GS was
stopped for the page runs (one commander). The GS itself first ran the
branch's `maburgs` (the `ChannelCore` papercuts of this plan) as the
standing regression gate.

| row | result |
|---|---|
| maburgs regression (branch `maburgs` on the GS, drone remembering 144) | PASS — `set [40,64,112,144] mode auto start 144 (remembered)`, `pick frozen on 144 (commit) after 3 rounds`; ausniff 60 s: 1815 AUs, 0 frame_id gaps, 0 incomplete, 30.3 fps |
| web GS, USB, auto, start 40, drone parked on 112 (A: page still on the default key) | drone found on 112 but **Key mismatch** (the drone rejects the default key, as designed); the pick froze on 40 at `max_ms` after 6 rounds; the header walked the scout dwells (`CH 112 · 40 MHz · scanning`, …); Disconnect: `teardown rx 304 ms (reads aborted)`, `DONE` |
| same, B: real key loaded | **PASS** — card up 6.8 s after Connect; `one-card prelude ranking picks 64 (op 40)` +10.2 s, `CHANNEL 64`, `commit card -1 40 -> 64`; `drone found on 112 (op 64): the link forms there` +14.2 s, `CHANNEL 112`, `link_found`, `pick frozen on 64 (one-card linked) after 2 rounds`, `relocate 112 -> 64 placed`, `one_card_retune` +238 ms, `lead_confirm` +253 ms, `hop_follow`, `CHANNEL 64`, `verify_pass` +1002 ms; header `Linked · CH 64 · 40 MHz · hop` then `CH 64 · 40 MHz`; drone log `retune 112 -> 64 (hop)`, drone `/etc/mabur.channel` 64; `localStorage.webgs.channel` 64 at the end. The same K/M/H order as `maburgs` row 5 (`docs/channel-select.md`) |
| Disconnect while scouting, Connect again in the same tab | **PASS** — Disconnect 8 s into the scan: `teardown rx 20 ms`, `DONE`, header `Disconnected` within 2 s, no reload fallback; Connect again: `webgs live: mode gs ch 64` (the remembered member), prelude picked 112 this time (`commit 64 -> 112`), `drone found on 64 (op 112)`, relocate 64 → 112, `one_card_retune` +211 ms, `lead_confirm` +238 ms, `verify_pass` +1002 ms, `CHANNEL 112`; the drone followed. Note: two consecutive auto sessions picked 64 then 112 and moved the drone each time — the co-located bench's ranker noise (`docs/channel-select.md` rows 6/7), not a page defect |
| reactive hop against the bench jammer (page pinned on 144, drone on 144; jam = devourer `txdemo` cross-built for arm64 and run on the GS box's spare card 2-1.4 because the host's only Realtek card is the page's radio: QoS-Data 1000 B at 6M, 4 ms interval, SA `02:4a:41:4d:00:01`, `DEVOURER_TX_PKT_OFSET=0`, 40 s) | **PASS** — link at +6.7 s after Connect; jam from +60 s; at +68.5 s `hold_exhausted`/`hold_end` (epoch 0) then `order epoch 1 target 64`, `one_card_retune` +243 ms, `hop_one_card 144 -> 64`, `lead_confirm` +262 ms, `hop_follow`, `CHANNEL 64`, `verify_pass` +1003 ms; header `Linked · CH 64 · 40 MHz`; the Radio group's residual read 11.7 % at the order and 0.00 % again within ~5 s, and stayed 0.00 % for the remaining 80 s with the jam still running on 144; drone log `retune 144 -> 64 (hop)`, drone `/etc/mabur.channel` 64. One-card hop timings match `maburgs`'s 2026-10-04 one-card row (+216/+235 ms). The `txdemo` arm64 build only resolves its include dirs when the cross `g++` wrapper runs inside a `nix-shell`; bare it silently fails to find `libusb.h` (environmental, not reproduced to a cause) |
| relay GS, search-only scout (CPE at 10.83.11.1 over the host USB NIC, set `{40, 64, 112}` — no 144 — auto, start 40; drone moved onto 112 ~40 s after Connect) | **PASS** — `maburgs relay card 0 (10.83.11.1:8311): owned and tuned` at once; `scan off` throughout (the relay search-only path, spec §3.7) with the header walking the search bursts (`CH 40`/`CH 64`/…) and `scan_rounds` climbing ~0.7/s; `drone found on 112 (op 40): the link forms there` +59 s, `CHANNEL 112`, `link_found card -1 40 -> 112`; `Linked · CH 112 · 40 MHz`, pre-FEC 0.0 %, residual 0.00 %, linked for the remaining 120 s. Seen: the relay card's state line flaps `waiting for STATUS` ↔ `owned and tuned` on every search-burst TUNE (`RemoteCard::log_transition` logs each flip; noise, not a fault) |
| relay spotter | NOT RUN — the spotter follower is plan 3; a relay spotter in this build only sits on its start member. Host-side coverage of the relay path: `web_relay_e2e` (12 cases: owned/refused/tune-failed/lost/taken early and late, spotter refused when not owner, `scan_state` `off`, 20 MHz sets) |

Seen but not fixed: `RadioFrontend::stop()` prints a second
`teardown rx 0 ms` line when the destructor runs after an explicit `stop()`
(cosmetic). The page's AU/gap counters were not captured by the driver in
these runs (the Debug tab was read before it re-rendered); the `Linked`
tag, the drone's `retune` line and the `verify_pass` (which needs video on
the target) stand in for them.

## Spotter follower — bench 2026-10-04 (plan 3)

Branch `spotter-follow` (fffd1db) page, served from `web/dist` and driven
headless (Chrome 147 over CDP, the WebUSB-granted profile; the driver
clicks Spotter + the radio, writes the channel set into `webgs.cfg`, and
reads the 1 Hz STATS through a `JSON.parse` hook plus the Debug tab's
Channel row every second). The GS ran plan 2's `maburgs` a155bf32 — **not
redeployed by this plan** (its only GS-side changes are a log-transition
rule and an Emscripten-only guard) — under a temporary bench config: one
card (`[[radio.cards]] index = 0`, which came up on usb 2-1.4, leaving
2-1.1 for the jammer) and `channels = [40, 64, 112]` (no 144: the page
still carried its stale "CPE cannot tune 144" refusal, removed the same
night), remembered 64; the drone's `/etc/mabur.channel`
was set to 64 for the bench. Both configs were restored afterwards (GS
`[40,64,112,144]` two-card auto, drone 144). Jam = plan 2's recipe
(`txdemo` on the GS's free card: QoS-Data 1000 B at 6M, 4 ms interval,
non-canonical SA, `DEVOURER_TX_PKT_OFSET=0`, 40 s). STATS are 1 Hz, so
every "within a second" below is that resolution.

| row | result |
|---|---|
| A — USB spotter opened mid-flight (link on 64, spotter set `[40,64,112]`, remembered channel cleared, so it starts on 40) | **PASS** — `webgs live: mode spotter ch 40` at +1.0 s after Connect; card up +5.3 s later (the usual ~5 s init); the first STATS, one second after that, already read `locked` on 64 (`follows 0`): the 150 ms dwell on 40, one FastRetune and the lock on the first frame all fit inside the first STATS interval. Header `CH 40 · 40 MHz` from module start (the start member, not the form's `auto`), then `CH 64 · 40 MHz`; 30 AUs/s, 0 truncated, `sends 0`, one `teardown rx 50 ms` line at Disconnect |
| B — USB spotter follows `maburgs` hops (jam on 64 from +55 s for 40 s) | **PASS** — `maburgs`: `hold_exhausted` → `order epoch 1 target 112` → `one_card_retune` +253 ms → `lead_confirm` +294 ms, then the jam leaking onto every member at bench distance (as in plan 2 and `docs/channel-select.md` row 6) chained `verify_fail epoch 2 → 40` and `escape epoch 3 → 64` within ~3 s, a 27 s hold, `order epoch 4 → 112`, a `hold_cap` run, and `order epoch 5 → 40`. Spotter STATS: `locked 64 follows 0` → `locked 112 follows 1` → `locked 40 follows 2` → `locked 64 follows 3` in three consecutive seconds (+55.8/+57.1/+58.4 s), `locked 112 follows 4` at +86.5 s, `locked 40 follows 5` at +118.5 s — six orders, six follows, every one re-locked on the target inside the next STATS; no STATS sample ever read `following` or `sweeping`. AU rate 26–31/s throughout; truncated AUs 0 → 49 during the jam window (jam loss and the hops), flat at 49 afterwards; `sends 0`. Drone `/etc/mabur.channel` followed (40 at the end) |
| C — CPE relay spotter follows via TUNE (CPE 10.83.11.1, link on 40, jam on 40 from +55 s) | **PASS** — `maburgs relay card 0 (10.83.11.1:8311): connecting` / `waiting for STATUS` / `owned and tuned` once at start and **never again** (the plan-2 flap on every TUNE is gone; `relay_state` read 0 in all 149 STATS samples); `locked 40` on the first STATS; `maburgs` ordered `epoch 6 → 64` (`verify_pass`) 3 s into the run on its own, then under the jam `epoch 7 → 40`, `verify_fail epoch 8 → 112`, `epoch 9 → 64`, `escape epoch 10 → 112`, `epoch 11 → 40`; the spotter's `follows` went 1…6 in step (+4 s, +55 s, +56 s, +86 s, +87 s, +117 s), each sample `locked` on the new member (the TUNE's 50 ms plus the lock fit inside one STATS interval). 30 AUs/s, 5 truncated in total, `sends 0`, relay TX 0 |
| D — withdrawn hop / timeout return | **not provoked** — no `withdraw` line appeared in `maburgs`'s log across rows B–E (18 orders/escapes/verify_fails, every one confirmed); the bench has no fault injector for a drone that fails to follow. Covered host-side by `tests/test_spotter_follow.cpp` (`timeout_and_return`, `withdrawal_heard_on_back_returns_at_once`, `standing_order_after_timeout_return_is_ignored`) and `tests/test_web_gs.cpp` `spotter_follow_timeout_returns_the_card` |
| E — Disconnect / Connect in spotter mode (USB) | **PASS** — Disconnect at +30 s: exactly one `teardown rx 60 ms` line (the plan-2 double line is gone), `DONE` 1.0 s after the click, header `Disconnected · CH auto`; Connect again 5 s later: `webgs live` +1.0 s, card up +4.3 s, `locked 64` on the first STATS; second Disconnect: one `teardown rx 80 ms`, `DONE`. In the first session the spotter also followed an unprovoked `maburgs` hop 40 → 64 at +25 s (`follows 1`) |

Seen, not a page defect: `maburgs` hopped twice without a jammer (row C
+3 s, row E +25 s) and chained `verify_fail` hops under the jam — the
one-card verdict on the co-located bench, `docs/inflight-channel-hop.md`,
not the spotter. With the drone off, a spotter reads `locked` on
whichever member the GS is beaconing on (the DISC search bursts are mabur
traffic; see "Spotter follower"). The `localStorage` `webgs.channel` is
never written in spotter mode (no `CHANNEL` line without a `ChannelCore`),
so a spotter always starts on the pin or the first member and sweeps —
one dwell per member, which row A shows costs well under a second.

## Other measurements formerly inlined in docs/web-gs.md

**PWA install (Serve).** Benched headless (Chrome 147, 2026-09-30):
no installability errors, all 7 files in the worker cache, button gate
correct; a real phone install is untested.

**Relay TUNE vs a USB FastRetune (Spotter follower, Known limits).** A CPE
relay TUNE reports tuned after 50 ms (measured 2026-10-04). Each TUNE
silently costs 50 ms of beaconing: the scout offers no DISC until the
relay reports tuned (a CPE TUNE answers `retuning` after ~7 ms and `tuned`
after 50 ms, flat over 30 retunes, measured 2026-10-04), so a 100 ms search
burst on a non-op member sends 2–3 DISCs instead of 5. Measured against a
USB card on the same search-only schedule (pinned, 4 ms FastRetune): 48.8
vs 56.0 DISCs per round at the same 0.7 rounds/s, 13 % fewer; nothing is
dropped (`relay_tx_fail` and `relay_tx_refused` stay 0). Kept as built
(spec §3.7's "wait for `ready()`" was not implemented): finding still
works, and the wait would cost ~100 ms per round.

**FastRetune over WebUSB (Design).** FastRetune between HT40 members over
WebUSB: median 3.75 ms, p95 ≈ 4.1 ms, worst seen ≈ 12.6 ms (native 3.75 /
3.76 / 11.5); width change 40→20 ≈ 55 ms, 20→40 ≈ 71 ms; a full scout dwell
≈ 300 ms (spike 2026-10-04, `docs/superpowers/specs/2026-10-04-web-gs-channel-core-design.md`
§8 — gitignored, machine-local).

**RTL8821AU and LDPC (Use).** Bench 2026-10-02, ch 136 HT20, 1000 x 1400 B
MCS0 from an 8812EU into the 8821AU: plain 900+, STBC 900+, LDPC 1,
LDPC+STBC 0 received. The 8821AU's own uplink, LDPC included, decodes fine
at an 8812EU (600-700+), and STBC on its RX side is fine. LDPC coding gain
given up by flying BCC: 2-3 dB measured at the same MCS.

**RTL8812AU on Linux (Known limits).** Bench 2026-09-28: with
`rtw88_8812au` unloaded, GS mode on an 8812AU (C-cut 2T2R, USB 3) connects,
flies the ladder and plays video.

**Chrome LNA from the hosted origin (CPE relay radio).** Measured
2026-09-29 on Chrome 147 against the bench CPE from the real
`gilankpam.github.io` origin: `ws://` to a private IP literal or `.local`
name is exempt from mixed-content blocking (console: "Insecure access is
deprecated"); a public DNS name resolving to the LAN is still blocked. Until
`local-network` is granted Chrome holds the socket on a one-time prompt
(headed: still pending after 12 s); headless or denied it dies with close
1006. Granting `local-network` via CDP made the same socket open from both
the page and a Worker; the older `local-network-access` name does not
unblock it. Chrome's LNA blog post at the time still said WebSockets were
"not yet gated".
