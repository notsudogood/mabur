# CPE510 relay: TX-mode bench and the wss spike, 2026-09-29

**Status:** TX mode (protocol v3, carried unchanged into v4) passed — RCF-heard over the relay ≥ USB, TX cost on the CPE negligible; wss rejected as too expensive on the CPE (Chrome LNA lets the hosted page use plain `ws://` instead). Current behaviour → docs/cpe510-relay.md.

Extracted from docs/cpe510-relay.md during the 2026-10-11 docs tidy.

## Bench record — TX mode, 2026-09-29 (CPE v3, `mabur-openwrt` 44f0190)

Setup: CPE on host USB-Ethernet at the CPE's stock OpenWrt address
192.168.1.1 (its address before 2026-09-29); drone on ch136 HT40-. Full numbers and the per-window breakdown are in the relay
repo's `docs/verify-mabur-relay-on-device.md` ("TX mode" section); this is
the summary.

- **TX alone** (native `webgs live --relay 192.168.1.1:8310` (pre-2026-09-29
  address) `--mode gs --ch
  136 --w 40 --secs 60`, no other GS on air, drone `low_power` on): SESSION
  + `peer_acked` within the first second, ladder climbed to rung 4 (mcs4/40)
  by ~15 s. `drone_rcf_rx / rcf_sent` = 1129 / 1151 = **98.1 %**; relay
  `seq` gaps 0; relay `tx` 1211, `tx_fail` 0, `tx_refused` 0; RTT ~5–7 ms.
  Proves ath9k honours the injected MCS0 + LDPC + STBC combination (the
  drone decodes it) and confirms the own-echo filter (`txecho` = 2422 =
  2 × 1211, kept).
- **A/B vs. USB + CPE load** (drone `low_power` off, full rate, ch136/40,
  mcs4; 4 legs alternating USB `maburgs`-on-Radxa and the CPE relay via
  native `webgs --relay`): RCF-heard 94.5 % / 94.3 % over USB vs. 100.4 % /
  100.7 % over the relay (after the first 10 s) — relay ≥ USB, inside the
  ±5-point bar (the >100 % readings are telemetry-counter lag at window
  edges: USB's denominator counts every GS TX frame, the relay's counts
  RCFs only). At the same time the CPE carried full-rate video
  (3170–3250 frames/s over UDP) plus ~20 TX/s: CPU 53.5–59.6 % (5 s
  windows, ≤ the 75 % gate), `relay_gaps` 0, `rxdrop` 0, `tx_fail` 0, RTT
  7.5–8.7 ms — the same as the RX-only UDP baseline (58 %, docs/cpe510-relay.md
  "Measured limits"): TX cost on the relay is negligible.
- **Not measured on hardware**: the browser path (page → WebSocket → relay
  Worker → ring → core) — no browser in the bench session. The native CLI
  exercises the same `RelayLink`/`RelayClient` over UDP instead. WS-mode
  full-rate CPE load was 71 % RX-only in an earlier run (docs/cpe510-relay.md
  "Measured limits"); not re-measured with TX traffic added.

## Why not wss (spike, 2026-09-29)

Measured on the bench CPE (AR9344, 74Kc 560 MHz), relay idle:

| | result |
|---|---|
| mbedTLS 3.6.7 (device lib) cipher | ChaCha20-Poly1305 14.3 MB/s, AES-128-GCM 1.3 MB/s |
| OpenSSL 3.5 cipher | ChaCha 15.4 MB/s, AES-GCM 5.5 MB/s |
| one stream, plain TCP, 4.5 MB/s / 2 MB/s | 22 % / 10 % CPU |
| one stream, TLS ChaCha, 4.5 MB/s / 2 MB/s | 96 % / 42 % CPU (saturates 4.8 MB/s) |

TLS costs ~16 CPU points per MB/s. The relay's WS path is already 71 % at
full rate (4.5 MB/s), so wss cannot carry the top rungs. The mbedTLS test
server also hung in the handshake against OpenSSL clients (not debugged).
The web GS doesn't need it anyway: Chrome 142+ lets an `https://` page
(the hosted GitHub Pages build) open plain `ws://` to a private IP literal
or `.local` name once the user allows local network access — see
`docs/web-gs.md`, "CPE relay radio".
