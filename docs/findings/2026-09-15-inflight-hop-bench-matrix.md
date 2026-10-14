# In-flight channel hop: bench matrix and early limitations (2026-09-15 → 2026-10-04)

**Status:** historical bench record of the reactive hop (dwell cost, condition matrix, verdict at rest, the limitations the first bench found); several of those limitations were fixed since (origin back-off via `flee()`, relay bursts, `recovered_min`). What is current now → `docs/inflight-channel-hop.md`.

Extracted from docs/inflight-channel-hop.md during the 2026-10-11 docs tidy.

## Build status at the first bench (2026-09-15)

Built without hardware, then benched on 2026-09-15: deployed both ends,
four fixes forced by the bench (see Measurements), a co-channel hop
confirmed in 268 ms onset-to-video on two cards and 427 ms on one, with
`ausniff` clean through it. Not flown. `docs/findings/2026-09-15-handover-inflight-hop-bench.md`
covers what is still owed — read it before flying auto.

## Measurements

Bench, 2026-09-15 (`docs/findings/2026-09-15-handover-inflight-hop-bench.md` has
the run-by-run record). Two RTL8822EU cards on the GS, the drone on the
bench, interferer = `tools/bench/benchjam.sh` (a spare 8822EU on the host,
QoS-Data 1000 B at 6 Mbit/s, 250 frames/s, ~35 % airtime, `DEVOURER_TX_SA`
set to a non-canonical address so the GS counts it as `foreign`). The
three code fixes the bench forced (verdict units, session gating, TX-card
freeze — commits `dfe09ad`, `fd1b8d5`, `17b7764`) were in place for every
row below except where the row says otherwise. Not run: the non-802.11
(O4) interferer and the fade row (both need hardware the bench did not
have), and no flight yet.

**Dwell cost** (`D` lines with `sess 1`; `to_us` = FastRetune to the
candidate, `read_us` = the real counter read, `back_us` = FastRetune back;
the discard read between retune and observe is not logged separately and
costs about what `read_us` does):

| step | planning figure | measured (median, two cards, 333 ms period) |
|---|---|---|
| retune to candidate (`to_us`) | 4 ms | 2.9–3.2 ms (max 19 ms) |
| discard read | 6.5 ms | ~0.9 ms (not logged; same call as `read_us`) |
| observe (`dwell_observe_ms`) | 5 ms | 5 ms |
| real read (`read_us`) | 6.8 ms | 0.8–1.0 ms (max 19 ms) |
| retune back (`back_us`) | 4 ms | 2.3–2.4 ms (max 5 ms) |
| **total** | **26 ms** (target ~10 ms) | **~12 ms** (`to+read+back` median 6.2–6.5 ms first run, 5.3–5.4 ms over the whole day per `flightreport`; p90 7.7 ms, max 31 ms) |

A candidate that IS the current op channel (the boot pick is always one
of `radio.scan.candidates`) costs nothing to "retune" to (`to_us` 4 us,
`back_us` 2 us) but still spends the 5 ms observe on-channel; a third of
the periodic dwells are that today.

**Bench condition matrix:**

| condition | onset→video (H lines) | rung restored (ctl.log) | ausniff fps/gaps with dwells on |
|---|---|---|---|
| co-channel 802.11 neighbour, 333 ms dwell, two cards | first `V interfered` → `H order` **150 ms**, `lead_confirm` **+118 ms** (onset→video **268 ms**), `verify_pass` +1001 ms; drone followed 165→120 and stayed | `E hop_restore 5 5` at the order (no demote had happened in the 150 ms detection) | 59.6 fps, 0 frame_id gaps, 0 incomplete over 100 s spanning onset, hop and verify |
| same, one-card GS (`[[radio.cards]]` pinned) | `H order` 153 ms, `one_card_retune` +258 ms (5 RCFs at `feedback_ms` 50), `lead_confirm` +274 ms (onset→video **427 ms**), `verify_pass` +1002 ms; drone followed 165→136 | `hop_restore 5 5` at the order, then a **4-rung demote cascade** in the 750 ms after the retune (`residual` 0.88, `util` 1.15–1.48: the drone retunes on the first RCF it hears, the sole GS radio only after all five, so the ladder's loss windows see ~200 ms of 100 % loss), re-promoted to rung 5 over the next 15 s | 59.5 fps, 10 frame_id gaps, 1 incomplete over 85 s |
| non-802.11 (analog VTX co-channel, 2026-09-25, one-card host GS) | **no hop**: 0 `interfered` over 176 windows (FA 0 on every one — an FM carrier never trips the preamble detector), 131 `unknown`; see `docs/findings/2026-09-25-analog-vtx.md` | n/a — ladder shed 6→3 five times on `util` and re-promoted each time | 19–20 fps (from 31), 4–6 gaps / 45 s while the carrier desensed the card (RSSI −58 → −70…−79) |
| analog VTX co-channel, two-card GS, NHM `blocked` (2026-09-26, VTX ~3 m from GS, op 132+136) | first `V interfered 21` (blocked, both cards 100 % busy) → `H order` **150 ms**, `lead_confirm` +83 ms, `verify_pass` +1016 ms (136→144); operator cycling E-channels then chased the link 144→112 (+82 ms, `verify_fail` on 112's FA background → hold) and later escape/return hops, 6 hops all confirmed 82–116 ms, drone followed every one; see `docs/findings/2026-09-25-nhm-airtime-spike.md` "Analog VTX and DJI O4" | not read | 30.8–31.6 fps, 0 gaps on every pass (disarmed 30 fps) |
| DJI O4 co-channel, two-card GS (2026-09-26, O4 ch2 20 MHz, link home 153 = 149+153) | run 1 (candidates [161,144], `confirm_ms` 500): blocked `0x61/0x71` at onset but loss 0 for ~3 s (busy-but-healthy holds), order at 48 % loss → 161 +460 ms; O4 read 80 % on BOTH 144 and 153, 161 25–37 % → stuck lossy, 13 gaps/10 s. Run 2 ([161,112]): 161→112 clean escape, then a false `raised` on 112 ordered 153, **withdraw at 510 ms** with the drone already moved → **60 s split**. Run 3 ([161,112], `confirm_ms` 1000): order **152 ms** after the first blocked window, `lead_confirm` +468 ms, `verify_pass`, no further H | not read | run 3: 31.8 fps, 0 gaps, 0 incomplete on 161 |
| channel-core extraction regression (2026-10-04, two-card GS, benchjam 250 fps / 1000 B / 6M at the lowest power step on op 64; `docs/findings/2026-10-04-channel-set-bench.md` "Bench 2026-10-04 — channel core extraction") | `H order` → `lead_confirm` **+122 ms** → `verify_pass` +1009 ms (64→144), the same H/M sequence as before the refactor. At default jam power the jam leaked onto every channel: 144→64→40→112 via `verify_fail`, a hold on 112 until the jam stopped, then back to 64 with `verify_pass` | not read | 30.2 fps, 1 frame_id gap (181 ms before the order, jam loss), 0 incomplete over 60 s |
| fade (no hop expected) | not run | n/a | not run |
| jammer on a candidate only (149 jammed, link on 120) | **no hop**: zero `H` lines, `hop.target` never left 120 | n/a | 59.7 fps, 0 gaps over 85 s |
| dwell period 100 ms (regression check) | n/a | n/a | 59.5 fps, 0 gaps; 11.5 dwells/s; step medians unchanged (6.4 ms) |
| RCF `rx_pps` with dwells on vs off | median 17.5 (dwells on, n=84 samples) vs 18.4 (old binary, no dwells, n=1762); p10 16.5 both | n/a | n/a |

The candidate-jam row is a weaker exclusion than the table reads: a 5 ms
observe catches about one frame of a 250 frame/s jammer, so 149's dwell
score rose from a median of 0 (mean 0.8) to a median of 6 (mean 6.7, max
25) — and 165's rose to 6 as well from near-field leakage. Enough to rank
it below a clean channel, not enough to call it interfered. The same
blindness cuts the other way: the channel the link just left scores ~6
too, so after a `verify_fail` on the target the ranker can pick the
jammed origin channel straight back (seen in the first run; the origin is
not backed off on hop-away). See Known limitations.

**Verdict at rest** (calibration input for `[hop.verdict]`): on the clean
bench link at rung 5, 65 % of windows carry the `impaired` bit and
classify `unknown` — pre-FEC loss never exceeded 2.5 % (threshold 3 %),
but `recovered > recovered_x (3.0) × its 5 s trailing mean` trips on any
single lost aggregate, because the trailing mean is ~4.6 symbols/window
(p50 2) and one lost aggregate recovers 10–26. Consequences: `ref_rung` is
frozen almost continuously (the thaw needs 3 consecutive `healthy`
windows, ~4 % likely at that rate), so the "pre-onset rung" a real hop
restores can be many seconds stale; nothing hops on it, since `unknown`
never triggers. A floor on the recovered term (an absolute count, or a
fraction of expected symbols) is the obvious retune; left to the
observe-only flights per the spec's open items.

## Known limitations as recorded then

Found on the 2026-09-15 bench (the handover page has the traces):

- ~~A CPE510 relay never bursts~~ — fixed 2026-10-02 for `can_scout` cards,
  then fixed for the relay itself 2026-10-05: `pick_burst_card()` is now a
  5-way order (scout non-TX, sweep non-TX, scout TX, sweep TX, none) and a
  relay-only GS bursts via an async `SCAN` ("Which card bursts" in §3,
  `docs/cpe510-relay.md`).

- **A withdrawn two-card order strands the drone.** The drone retunes on
  the first RCF carrying the order; if the GS withdraws (no video on the
  target within `confirm_ms`), the withdrawal rides RCFs on the OLD
  channel, which the drone — now on the target — cannot hear. It sits
  there for `move_confirm_ms` (2 s), homes, and rendezvous back, and every
  order the GS places meanwhile is sent to a drone that is not listening.
  In the first run that burned the whole `max_hops_per_min` (4) inside
  2 s and the controller held for 36 s under active interference. The
  TX-card freeze (`tx_selection_frozen`) removed the cause of those
  withdrawals; the strand-then-re-order pattern itself is unchanged and
  will recur on any genuine withdrawal. A guard that refuses a new order
  until the drone has been heard again (video, or any Telem on the new
  channel) is the candidate fix.
- **The origin channel is never backed off.** Only withdrawn/failed
  targets are. With a 5 ms observe scoring a 250 frame/s jammer at ~6,
  the jammed channel stays "ranked clean" and is a legal next hop after a
  `verify_fail` on the target (first run: 165 → 149 `verify_fail` → back
  to 165 → `hold_cap`).
- **`fa_pps` 100 is marginal on a busy candidate.** 149 (boot-scan busy
  109–306 on this bench) read FA 70–133/s on both cards after the follow,
  which is what failed that verify. Calibration, not code.
- **One-card hops cost the ladder four rungs** (matrix table): the
  five-RCF order window leaves the drone ahead of the GS by ~200 ms, the
  loss windows are not blanked, and the cascade lands after the confirm.
  `blank_store` covers the rung store only; the demote decisions are the
  knob if this matters in flight.

Carried over from the implementation session:

- **A one-card GS needs two freshness-burst passes before it can hop to
  a real candidate.** `HopRanker` requires at least 2 fresh visits before
  a channel counts as ranked (§3), and on one card the burst — rate-limited
  to one pass per `hop.dwell_period_ms` (§3) — is the only source of
  visits. So the first trigger's burst (which fires immediately) leaves
  every candidate still unranked, `hopc`'s `best` stays `nullopt`, and the
  controller orders home (or holds, if already home); only the *second*
  burst, one `dwell_period_ms` later, gives the ranker enough visits to
  name a candidate. Derived (not measured — no hardware this session)
  figures for the shipped defaults, named by milestone since the two
  differ by a full second: **~633 ms** to the order decision (300 ms
  detection + one 333 ms wait for burst 2); **~883 ms** to the physical
  retune (+ `one_card_repeats` (5) × the RCF period — 250 ms, **not**
  500 ms: the RCF period is `link.feedback_ms`, whose shipped bundle
  default (`gs/bundle/maburgs.default.toml`, 50 ms) overrides the
  struct default in `gs/src/config.h` (100 ms) — computing from the
  header alone gives the wrong, too-pessimistic answer); **~0.9–1.1 s**
  to `Confirm` (video lands on the target, `HopState::Verifying`) — the
  milestone the spec's "under one second" goal is about, since that is
  when video is restored; **~1.9–2.1 s** to `verify_pass` (a further
  `hop.verify_ms`, 1000 ms, before the hop is declared to have stood).
  **These figures are best/typical-case and provisional**: derived from
  config defaults and code paths only, assuming interference present
  continuously from t=0 and phase-aligned onset (no wait for the next
  verdict window or RCF slot); real jitter adds up to ~150 ms
  (verdict-window phase, `hop.window_ms`) and ~50 ms (RCF-cadence phase)
  at the tails, and none of it is measured — pending independent
  re-derivation and, eventually, a bench run. Two-card GSes are
  unaffected — the periodic scout thread keeps the ranker continuously
  warm, so a two-card burst typically finds already-ranked candidates on
  its first pass.
- **A one-card freshness burst takes the sole radio off-air for ~30 ms
  per sweep** (three default candidates, a few ms each). Reconciled the
  same tick: `InflightScout::dwell()`'s own return-to-`back` retune, then
  a `cur_ch` resync from live `fe.channel()`, then (if either failed) the
  ordinary mechanical per-card retune loop — all before the tick ends, not
  deferred to the next one.
