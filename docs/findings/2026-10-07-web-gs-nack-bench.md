# Web GS software NACK bench (2026-10-07)

**Status:** USB passed (base truncations 0.06× of control); the CPE relay was borderline (0.105×); the browser showed a fill-latency tail that native did not. NACK stays on by default in the web GS; current behaviour → `docs/web-gs.md` ("Software NACK"), protocol → `docs/fec-nack.md`.

Extracted from docs/web-gs.md during the 2026-10-11 docs tidy.

Native `webgs live --overlay` against the bench drone (fec-nack maburd),
pinned mcs2/40 on 144 (stop rule inert), control (`[link.nack] enable =
false`) vs NACK, 5 min per arm, counters taken from 5 s in (see the join
burst below). Interference: devourer `txdemo` on one of the GS's 8822EU
cards with maburgs stopped, `benchjam.sh`'s frame shape (6M, 1000 B
QoS-Data, foreign SA) at 333 frames/s — 180 was too gentle from that card
(1 truncation/min), 500 starved the drone's TX (AUs/s fell to 30-50, frames
lost whole, nothing a NACK can fill). Order A1, B1, B2, A2 over USB, then
RA, RB over the CPE relay (`--relay 10.83.11.1:8310`):

| arm | AUs | trunc | trunc_base | req | syms | fill | waste | drop | drone_rx | drone_refused |
|---|---|---|---|---|---|---|---|---|---|---|
| A1 control | 17512 | 149 | 79 | | | | | | | |
| B1 NACK | 17619 | 63 | 3 | 335 | 6596 | 4787 | 868 | 41 | 296 | 0 |
| B2 NACK | 17606 | 75 | 5 | 374 | 6977 | 4627 | 1057 | 155 | 313 | 0 |
| A2 control | 17550 | 115 | 54 | | | | | | | |
| RA relay control | 17521 | 140 | 57 | | | | | | | |
| RB relay NACK | 17589 | 89 | 6 | 529 | 10035 | 5170 | 1663 | 247 | 437 | 4 |

- **USB: pass.** Base truncations 133 → 8 (0.06×) at equal AUs;
  wasted 13-15 % of requested symbols; `drone_refused` 0; `suppressed` 0;
  `drone_rx`/`sent` 84-88 % (the jam hits the uplink too, repeats cover
  it). Fill latency only exists per 1 s window on this line: the
  fill-weighted median of the per-window p90 was 13-14 ms, the 90th
  percentile of it 25-28 ms, max 38/50 ms.
- **Relay: borderline.** 57 → 6 (0.105×, the 0.1 mark), wasted 17 %,
  `drone_refused` 4, repeats 26 %, `drone_rx`/`sent` 83 %. Over the CPE
  relay radio the request rides the relay's TX round trip; that showed up
  as more repeats (26 % of requests vs 11-15 % over USB) and a lower
  `drone_rx`/`sent`, not as `lead_skipped` (0 in every arm).
- **Surviving base truncations** (~1/min with NACK) all fall in seconds
  with a 70-115-symbol request burst where the uplink NACK was lost
  (`drone_rx` < `sent`) or entries hit the deadline (`drop`). The page
  requests no IDR on a truncated AU (`Gate.onAu` → `skip: 'truncated'`),
  so each is a silent smear until the drone's GDR/IDR. Not acted on: a
  conditional IDR trigger stays the next bounded change if flights show it.
- **Join burst.** The first poll after link-up books the whole gap before
  the first heard symbol as missing (850-1000 syms, 400-430 `drop`, 4-18 requests),
  and the drone refuses most of it from its bucket (`drone_refused` ≈165
  in one shot). Harmless, but it inflates every cumulative counter read
  from link-up — take deltas.
- **Browser (headless Chrome, WebUSB, same pin + jam, 75 s):** linked,
  Debug NACK group moving (requests 18 → 101, filled 104 → 710, `drone rx`
  13 → 84), base truncations 3 in ~70 s. Most fill windows read 12-22 ms
  p50, but 3 of 14 read 62-114 ms — past the 50 ms gap timeout, where a
  fill is too late to save the AU. Native never showed that tail; it is
  the page's own scheduling (worker/WebUSB), unmeasured beyond this run.
