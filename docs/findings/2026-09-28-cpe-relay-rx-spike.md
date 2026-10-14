# CPE510 relay RX spike, 2026-09-28

**Status:** the CPE510 decodes every mabur rung once tuned to the link's width; it became the `mabur-relay` remote card → docs/cpe510-relay.md.

Extracted from docs/cpe510-relay.md during the 2026-10-11 docs tidy.

## Why it works

- ath9k enables **RX LDPC** and **RX STBC** on AR9344, so every mabur rung is
  decodable.
- The CPE must follow the link's **width**, not just its channel: tuned
  `136 HT20` it heard almost nothing of an HT40 link; `136 HT40-` heard
  15,415 video frames in 5 s at 0.21 % dot11-seq loss.
