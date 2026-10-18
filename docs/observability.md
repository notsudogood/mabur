# Observability — which tool for what

The **stats sideport** is the primary debug surface. maburgs pushes a JSON
datagram (schema v1) to UDP `127.0.0.1:8300` every 500 ms: per-card and
per-RF-class signal stats, link op point, per-stream FEC health, video/AU-ring
health, TX/injection rates, airtime estimate, and drone telemetry (state,
applied op, encoder, queues, uplink RF, temps — via the 1 Hz T_TELEM
control frame); `link.ctl` carries the ladder controller's rung, util,
probation/counters, and last-transition reason, and `tools/flightreport.py`
is the post-flight analyzer over a recorded jsonl. ⚠ Superseded
2026-09-04: promotes used to probe the candidate MCS by moving the whole
ENH stream to it (`CAP_S3_PROBE`/`CAP_ENH_PROBE`, both deleted, RC_VERSION
5 → 6); the drone now always sends a dedicated probe-stream canary (SBI
sid 5) behind every ENH AU and the ladder's promote trigger consults the
continuous verdict instead of starting a discrete probe — see
`docs/link-adaptation.md` "Probe stream" and `link.probe` below. s3
residual/util loss demotes in steady state are unchanged (kill-switch
`link.s3_demote`); spec
`docs/superpowers/specs/2026-09-04-probe-stream-design.md` (historical:
`docs/superpowers/specs/2026-08-05-s3-probe-promote-design.md`). Since
2026-08-31 (latency-accounting) `link.video.lat` carries the end-to-end
head-segment latency breakdown (`n`, and p50/p99 pairs for `enc`/`dq`/`air`/
`fec`) once the AU ring's SlotHdr v2 stamps are flowing — omitted, not
null, until then — and `maburplay`'s OSD grows a matching latency block
above the fps line, reading `--` while its own e2e-latency tracker is
cold or discontinuous.

**Per-card `crc_fail` is live since 2026-09-08.** The GS asks devourer to
keep FCS-failed frames (`rx.keep_corrupted`, RCR ACRC32|AICV on the 8822E,
set unconditionally in `gs/src/radio_frontend.cpp`), so a corrupt frame
reaches the aggregator, bumps that card's `crc_fail`, stays out of the seq
walk, and hands its body to the UEP decoder, which keeps the SBI
sub-blocks whose own CRC16 still passes. Per stream the sideport carries
`corrupt` (FCS-corrupt bodies delivered), `salvaged` (CRC16-clean
sub-blocks taken out of them) and `sub_fail` (sub-blocks that failed, from
any body), and since 2026-09-09 `salvage_only` — seqs whose ONLY arrival
inside the arrival guard was a salvaged sub-block (a second "heard clean"
bit in the ArrivalTracker, booked at settle time). `salvaged` is the
upper bound and `salvage_only` the value: the other card's clean copy of
the same body shadows most salvaged sub-blocks (flights 0043/0044,
`docs/findings/2026-09-09-sbi-salvage-flights.md`), and a salvaged repair symbol
never counts here because it carries no seq of its own.
`flightreport.py` prints them as a SALVAGE section — totals per
card/stream and the deltas per rung next to that rung's abandoned
symbols, which is the post-flight answer to "what did salvage buy"
(`salvage_only` appears only on recordings that carry the key). Before
that date the WMAC dropped those frames on the
chip and `crc_fail` was structurally 0 — not "no damage", "damage never
seen". Foreign traffic that fails FCS is counted too (the SA filter cannot
trust a corrupt address), so a busy channel shows a slow `crc_fail` creep
with the drone off; `MABUR_GAPLOG=1` prints one `crcfail card=… sid=…`
line per event, `sid=-1` being the foreign ones.

Since 2026-09-01 that block is **two rows, `LAT P50 <n> ms` and
`LAT P99 <n> ms`**, right-flushed under the `RTT` row. Each is ONE REAL
FRAME from the last 1 s window, ranked by e2e. Read them together:
`P99` alone is a tail statistic sitting among averages (fps/jit/mbps)
and gets misread as typical — an operator reporting "fec is 10–30 ms"
while the median was ~10 ms is what prompted the second row
(`docs/findings/2026-08-31-dq-spike.md` §20).

**2026-09-06: the per-segment breakdown left the OSD.** Each row used to
render its frame's own seven segments (`enc dq air+ fec dec reg dsp`)
beside the headline — never independently-ranked percentiles, so a row's
segments summed to its own headline. That is now a post-flight question
only: the `lat:` jsonl line and `flightreport.py` carry the same
decomposition with more resolution, and on the glass it cost a
full-width column whose left edge reached into the centre-of-frame
keep-clear band. **The sideport `link.video.lat` block is unchanged** —
segment p50/p99 pairs still ship there, so maburtop and the recorders
lost nothing.

Since 2026-09-02 (link-rtt) `link.rtt` carries a clock-sync-free
**control-path RTT** and the pts-clock offset behind the absolute LAT
floor: the drone echoes which RCF `rcf_age_ms` ages against
(`Telem.rcf_seq_echo`, validity = flags bit3) plus its MI-domain clock at
telem build (`pts_at_build`), and maburgs matches the echo against its
recorded send times — `rtt = (telem_rx − rcf_send) − rcf_age`, every
1 Hz telem a sample. Keys: `ms` (EWMA), `min_ms` (session min — the
floor bound), `n`, `pts_off_us` (min-RTT-filtered `pts − GS-mono`; null
until the drone ships a usable pts clock), `floor_ms` (maburgs' anchor +
offset = absolute network floor). The whole block is null until the
first matched sample. Read it as CONTROL-path time: the telem reply
queues behind video on the drone's half-duplex TX, so `ms` inflates
under saturation (honest congestion signal, not PHY RTT), and the
offset's residual error is the up/down asymmetry of the best samples,
±1–2 ms class. On the player OSD the offset (combined with the player's
OWN anchor) folds the floor into the `LAT P50`/`LAT P99` headlines, and
each carries a `~` prefix while its e2e is still relative (estimator
cold, or sync lost at range: the floor freezes at last-good and drifts
~1 ms/min of outage). Before 2026-09-06 the floor was folded into the
`air+` segment too, so the on-screen breakdown still summed to the
headline; with the breakdown gone only the headline takes it. An
`RTT <n> ms` field sits one row above `LAT P50`, sharing its right edge;
it is adjacent for one-glance reading but is two-way control time and
never part of the e2e sum. What the absolute rows still exclude: sensor
exposure/readout before the pts stamp (~10–16 ms est.) and everything
past scanout start (HDMI + display processing) — LED/camera-lump
territory, see `docs/findings/2026-08-31-latency-budget.md`.

`link.rcf_slot` (2026-09-03) counts how the RCF slotter released each
control-frame send: `au` (at an AU completion with idle ahead, incl. the
in-grace immediate sends), `probe` (2026-09-05: at the probe body's
arrival — the release for every ENH burst while a probe is commanded),
`timeout` (hold reached `link.rcf_slot_hold_ms`), `passthru` (slotter
off, or no AU in the last 100 ms). Cumulative; diff across records.
`tail_ub_ms` is the slotter's learned completion→probe deadline (a lost
probe's fallback release; ceil(decaying max) + 1). Healthy video = almost
all `au`+`probe`, `timeout` well under 1 % — ⚠ at a high duty cycle
(bench pinned mcs4, ~4 ms idle per 16.7 ms) `timeout` runs ~43 %, and
with `feedback_ms` 50 = 3 AU periods those timeouts are phase-locked to
the burst, see `docs/findings/2026-09-05-probe-blanking-fix.md`. See
`docs/link-adaptation.md` "RCF slotting".

`link.streams[].arr_expected / arr_arrived / arr_expected_stale /
arr_arrived_stale / arr_late` (2026-09-05, arrival tracker): cumulative
arrival-time pre-FEC accounting, the ladder's util input since ctllog
11 — `expected` is seq advance past a 32-seq settle line, `arrived` what
was heard, `_stale` the pre-transition share the watermark attributes to
the old rung, `late` symbols heard after their seq was already booked
missing (healthy = 0; a rising `late` means reorder deeper than the
guard and the tracker is under-counting arrivals). Current-only loss =
`1 − Δ(arrived−arrived_stale)/Δ(expected−expected_stale)`.

Consume the same numbers programmatically with:

- `maburtop` on the GS (`tools/maburtop.py`) — full-screen console,
  grouped by link; color thresholds carry the judgment. Binds :8300
  directly — nothing else holds the port any more.
- Debug logs: maburgs writes a per-session directory when `debug_log.enable`
  is set — `<debug_log.dir>/NNNN/` holding `ctl.log`, `probe.log`, `au.log`,
  `scan.log`, `fec.log`, `arq.log` and `flight.jsonl` (sessions before
  2026-10-10 may also hold `ta.log`, from the removed turnaround bench); maburplay writes `lat.log` into the same directory by
  following the `/tmp/mabur-session` marker and holds no logging config of
  its own. The loader default is **off**, but the shipped bundle
  (`gs/bundle/maburgs.default.toml`) turns it **on** since the
  feedback-repair data-gathering build (`docs/feedback-repair-rollout.md`,
  which needs `arq.log` from every flight). A GS whose `/config/maburgs.toml`
  predates that keeps its own value: the image seeds `/config` only with
  files that are missing.
  The marker lives in tmpfs, so a reboot starts a new session while a 2 s
  wrapper respawn rejoins the current one and appends (which is why a format
  marker line can appear more than once in a file). **A drone restart is a
  new flight (2026-09-08):** when T_TELEM's `tlm_seq` steps backwards by
  more than 100 (maburd restarts it from 0; a fade or re-rendezvous keeps it
  climbing) maburgs rotates in place — next NNNN, marker rewritten, all five
  files (`flight.jsonl`, `ctl.log`, `probe.log`, `au.log`, `scan.log`)
  reopened under it with their format marker at the top — with no
  process restart and no video blink; maburplay's `lat.log` follows the
  marker within 5 s. Cumulative sideport counters do NOT reset at a
  rotation (same maburgs process), so the new `flight.jsonl` starts at
  nonzero values; `flightreport.py` differences within the file. A maburd
  crash-restart mid-flight therefore splits that flight in two, by design. Every file shares one
  CLOCK_MONOTONIC clock, so any two rows join directly — there is no `# sync`
  bridge any more. `ctl.log`, `probe.log` and `au.log` share one writer
  thread (`gs/src/log_writer.h`) whose per-stream ring can fill under load;
  when it does, the writer counts the dropped lines and, at its next 1 Hz
  flush, appends `# dropped N` into the affected file so the gap is always
  visible in the data itself — this is the v4 hole-signal, replacing the
  external reader's `# resync` marker (below). `flight.jsonl` opts out
  (`mark_drops=false`: NDJSON can't carry a comment line, and a gap is
  already visible there from the datagram's own `seq` field). Analysis:
  `flightreport.py <session-dir>` (also
  `flightjitter.py`, `airdrain.py`, `probesend.py`); with no argument they
  take the highest-numbered session. Replaces `flightrec.py`/`S95flightrec`,
  deleted 2026-09-06.

  `cal.log` (`gs/src/cal_log.h`, `callog 1` format marker) sits in the same
  session directory but does not follow the rule above: it writes
  regardless of `debug_log.enable` — that knob governs continuous
  per-second flight logging, and a calibration run is a bounded (~380-line)
  trace on a deliberate operator action, the sole record of what
  `maburcal start` measured. With logging off it falls back to
  `<debug_log.dir>/cal` instead of a numbered session, and either way
  `maburgs` prints the resolved path to stderr at the start of a run. Its
  shape is `callog 1` once, then one `R <nonce> <base_ref> <margin_db>` per
  calibration run (a retry — narrow flag, move the drone, run again — adds
  another `R` line to the same file rather than rotating it, so one
  `cal.log` can hold several runs; a reader keys `C`/`W`/`V` rows by the
  most recent `R`), `C` per measured cell, `W` per rate at each phase-end
  (coarse then fine — last-write-wins), and `V` per verify cell.
  `maburcal report <cal.log>` re-renders a saved run; see
  `docs/calibration.md`.

  `au.log` carries per-AU meta rows, SlotHdr v3 since 2026-09-06 (the
  air-clock 12th column, `air_ms`), behind a `# aulog 4` marker line —
  written at file open and re-written on every reopen, so a respawn
  within the session appends a second copy (`t_us pts sid fid len flags
  nal0 t_first t_complete enc dq air_ms`; a log without the marker is
  the pre-2026-08-31 7-column v1 format — `t_us pts sid fid len flags
  nal0` only — or, with a `# aulog 2` marker instead, the 2026-08-31
  SlotHdr v2 format, which adds `t_first t_complete enc dq` but not the
  trailing `air_ms` column). `aulog 4` is fed IN-PROCESS from
  `AuRingWriter::last_record()` (`gs/src/main.cpp`'s FrameStream finish
  callback; `gs/src/au_log.h`, `gs/src/au_ring.h`) — maburgs is the ring
  WRITER, so there is no mmap, no seqlock copy, and no epoch resync; unlike
  an outside reader, the writer cannot miss an AU to a ring overrun. Its
  hole-signal is the shared `# dropped N` marker described above — the v4
  equivalent of the `# resync` marker below. `t_us` in `aulog 4` is
  CLOCK_MONOTONIC µs, the same clock every other file in the session
  directory uses — in `aulog 1..3` (the last of those, `# aulog 3`, the
  SAME SlotHdr v3 columns as `aulog 4` but written only by the now-deleted
  `flightrec.py`, which read the rows from the `/dev/shm/mabur-au` ring
  from OUTSIDE maburgs exactly like `ausniff.py` — seqlock copy, epoch
  resync ⇒ `# resync` marker; attaches at the write head so pre-attach
  history can't be stamped with attach time) it was WALL-clock µs and the
  file carried `# sync <t_us> <t_ms>` anchors every 10 s to bridge to the
  jsonl's `t_ms`; a v4 log has neither the wall clock nor the anchors (see
  the scale-break note in `docs/data-provenance.md`).
  `t_first`/`t_complete` are the AU's SlotHdr v2 mono-µs latency stamps
  (first body / finish()); `enc`/`dq` are the drone's SBI-latched
  `enc_us`/`drone_q_ms` (venc encode time, TX queue wait) carried through
  on the AU's first fragment. Since 2026-08-31 (streaming push) `q_ms` is
  stamped at the body's actual TxQueue push and is the TRUE queue wait,
  ~0 when healthy — before that it also swallowed the venc-ring wait and
  the FEC/SBI CPU (~6 ms standing; see the scale-break note in
  `docs/data-provenance.md`).
### `link.refresh` — demand refresh

`link.refresh{armed, requests, retries, heals, heal_ms_last, by_reason{hole,
salvaged, lost, start, resync, decoder, dvr}}`: the GS's `RefreshRequester`,
counters cumulative since the daemon started. `armed` is true while a
request is outstanding (waiting on a complete base IRAP to clear it);
`requests` counts arming events, `retries` the repeated asks before each one
cleared (they only accrue while maburgs is in a session), `heals`
completed clears and `heal_ms_last` the most recent arm-to-heal span —
counted from arming, so a request armed before the link is up (the
player's boot `start`) reads as a long heal; `by_reason` tallies every
arming source as it fires, including ones that land while a request is
already outstanding. maburtop
draws a `refresh:` line in the LADDER panel, right after `nack:`, folding
`start`/`resync`/`decoder`/`dvr` into one `player` figure. Mechanism:
`docs/fec-nack.md` "Demand refresh"; the per-request/heal trace is
`fec.log` below.

### `cards[]` — per card

  Since 2026-10-10 (slice salvage) the marker is `# aulog 5`: the
  `aulog 4` twelve columns plus four trailing `slices kept filled
  after_hole` columns (`gs/src/au_log.h`) — all 0 on an AU that was
  neither split nor salvaged.

  `tools/flightjitter.py` is the analyzer: reproduces the player's jitter
  EMA from the AU rows — using each row's `t_complete` as the arrival
  basis when present (the writer-stamped ring completion time, sharper
  than `t_us`'s reader-poll stamp), falling back to `t_us` for v1 rows —
  splits it into size-explained vs residual (airtime-model §4
  decomposition), and classifies each stutter event — `gap` /
  `rung-change` / `size` / `fec-wait` / `loss-recovery` / `transport` —
  using the jsonl for ladder and loss context (clock offset from the sync
  anchors on a pre-v4 au log; a v4 log needs no offset — same clock as the
  jsonl). `fec-wait` is direct per-frame evidence (the stutter AU's own
  `t_complete − t_first > 20 ms`), checked before the jsonl-inferred
  `loss-recovery` class so it wins when both apply. Tests: `ctest -R
  test_flightjitter`.

  Until 2026-09-06 the GS boot-started `/etc/init.d/S95flightrec` running
  `/root/flightrec.py`, an always-on Python daemon reading the sideport off
  UDP :8300 to write the jsonl and reading the AU ring from outside maburgs
  (seqlock copy, epoch resync) to write the au log — the only consumer of
  either, which meant `S95flightrec stop` before pointing maburtop or any
  ad-hoc UDP listener at :8300, and `start` after. The 2026-09-06
  consolidation deleted it: maburgs now writes `flight.jsonl` itself
  (straight from `StatsExporter`, no UDP round-trip, no second reader of
  the AU ring) and :8300 has exactly the one producer and no default
  consumer, so `maburtop` binds it directly. The adaptive-link record
  survives exactly as before, just relocated: maburgs writes its own
  compact `ctl.log` into the session directory whenever `debug_log.enable`
  is set in `/etc/maburgs.toml` (shipped default `false` — the bench GS
  turns it on), a `ctllog 11` header (v1 before 2026-08-14, v2/v3 that
  day's two waves, v4 since 2026-08-15 — pooled-RF note in
  `docs/link-adaptation.md` — … v10 since 2026-09-04, probe stream, v11
  since 2026-09-05, arrival-booked u/u3) followed by compact S/E/P/N/R
  lines (rung/state, ctl events, probe gate edges,
  penalties, per-rung EWMA store snapshots). Since ctllog 10 the `S` line
  carries three trailing probe columns (`probe_rung probe_u probe_n`, `-1
  nan 0` when nothing is commanded) and the `P` line is REPURPOSED: it used
  to be one row per discrete 2 s probe attempt (`pass|fail|abort`), and is
  now one row per PROBE GATE STATE EDGE (`clean|lossy|noinfo`) — a gate
  edge, not a probe outcome; do not pool v9-and-earlier P rows against v10
  ones. `flightreport.py` auto-detects this format alongside the jsonl
  format, so no separate invocation is needed. The controller-side tuning
  invariants that govern what those lines mean live in
  `docs/link-adaptation.md`. Its `PROBE GATE` report scores the edges as a
  demote predictor for the v2 threshold: per demote episode, `lead` is
  the time from the FIRST lossy edge since the held rung was entered (the
  last `E` line before the episode, either direction) — edges before that
  transition were measured under a different hold and never count (the
  2026-09-05 version searched unbounded and reported 5-19 s "leads" that
  simply predated the promote) — and a lossy edge is a false alarm unless
  the next `E` line is a demote within 10 s, so an edge the gate overrode
  with a promote stays a false alarm even if that next hold then fails.

  ⚠ Since 2026-09-04 the ctl log (and with it the probe log below) also
  opens in **static-pin mode** (`link.static_mcs >= 0`) — it used to be
  skipped there, since a pinned link never ticks the adaptive controller
  and so had no rung decisions to record, but the pinned bench runs are
  exactly the ones whose per-body probe log matters (spec §8.3 steps 1-2).
  A pinned `S` line's controller columns are frozen and uninformative
  (`u`/`util` read 0, `E`/`P`/`N`/`R` records never fire): only its probe
  columns move, `<probe_rung> nan <probe_n>` — `probe_rung` is
  `min(probe.rung_offset, top)` off a frozen `idx_ == 0`, `u` is `nan`
  because the gate never leaves `Off`, and `probe_n` is the real count of
  expected blocks (nonzero whenever `link.probe.pin_mcs >= 0`, 0
  otherwise) — so in pin mode only the probe rows, and the `probe.log`
  they summarize, carry information.
Top level, unconditional (idle defaults while pinned): `hop = {verdict,
evidence, ref_rung, epoch, state (idle|ordered|verifying|hold), target,
hops, holds, last_ms, sweep_timeouts, sampler_dropped}` — `ref_rung` and
`target` are `null` while unfrozen / before the first-ever order, `last_ms`
is `null` until any hop event has fired. `sweep_timeouts` counts relay
`SCAN`s that got no `SCAN_RESULT` before their timeout
(`max(1000, passes·n·(observe_ms+40)+300)` ms); 0 on a GS that never sweeps
a relay. `sampler_dropped` counts channel-health window records the
sampler thread published that the core loop never consumed (replaced by
the next window): the core consumes only the newest record, and a
replaced one hands its FA/CCA delta on to the record that replaced it; a
record sampled before a hop-active edge is discarded uncounted. It grows
only when the core loop falls a whole `hop.window_ms` behind -- which a
dwell burst does by design: the core runs the burst for longer than a
window, so a record published just before the burst can be replaced by
the one the sampler publishes once the burst releases the card.
maburtop's header shows `hop <state>/<verdict>`. Full key semantics, the
OSD `(h)` mark and the `flightreport.py` HOP section:
`docs/inflight-channel-hop.md`.

  Per-body raw probe log: with `debug_log.enable` on, maburgs also writes
  `probe.log` into the session directory (`gs/src/probe_log.h`) — one row
  per finalized received probe body, at roughly the enh AU rate (≈5 MB/h
  at 30/s), far denser than the ctl log's dwell-period `S` lines and so
  its own file. Since the 2026-09-06 consolidation `probe.log` and
  `ctl.log` from one boot are siblings in the same session directory, so
  they pair by directory rather than by a shared NNNN (before that, NNNN
  was taken from the paired `CtlLog::index()` so a `probe-NNNN`/`ctl-NNNN`
  pair from one boot lined up). Header `probelog 3 bpb=<bpb>`, then `<t_ms>
  <seq> <mcs> <bw> <enh_fid> <blocks_ok> <card_mask> <snr_c0> <snr_c1>
  <evm_c0> <evm_c1> <first_ms>` per row (`probelog 2`, to 2026-09-24,
  lacked `bw` and is all 20 MHz; `probelog 1`, 2026-09-04 only, also lacked
  `first_ms`) — `t_ms` is the finalize tick (~10 ms coarse), `first_ms`
  the radio's arrival stamp of the body's first sight on any card (mono ms
  to 3 decimals, same CLOCK_MONOTONIC as `au.log`'s `t_complete`),
  `blocks_ok` is the union of surviving blocks, `card_mask` the bitmask of
  cards that delivered any block, snr/evm per-card in dB (`nan` when that
  card heard nothing this row). A row is written for EVERY finalized body,
  on- or off-profile: `mcs`/`bw` are that body's OWN profile, not the commanded
  one, so an RCF-lag body (arrives just after a profile switch, before the
  drone has caught up) still logs a row at its stale mcs instead of
  vanishing. A wholly-lost probe body is the only case with no row;
  `flightreport.py` derives it from `seq` gaps and the enh AU count and
  joins rows to `au.log` on `enh_fid` (nearest completion in time — the id
  wraps every ~36 min) to print the completion→probe offset percentiles
  (the episode report also prints an "s3-settle-refire canary" since
  2026-09-05 — demotes landing 300–360 ms after an `s3_residual` demote,
  the debris double-step `transition_edge.h` removed; ~0 expected)
  (`flightreport.py <session-dir>`, or the legacy explicit `probe-NNNN_
  <date>.log [au-NNNN.log]`; without the second argument the legacy form
  picks the au log in the same directory or `./log` whose mono range
  overlaps). `tools/bench/probesend.py` adds the GS send stamps from a
  `MABUR_GAPLOG=1` run to place every send against the probe
  (`docs/findings/2026-09-05-probe-blanking-fix.md`). Never fatal, like
  the ctl log.

  **fec.log (feclog 3; feclog 1 from 2026-09-15 had no `bw`, feclog 2 from 2026-09-24 no `rtx`).** Per-episode FEC loss record, the
  measurement behind "is the rung table's overhead pair oversized" —
  written by maburgs into the session directory (`gs/src/fec_log.h`),
  rotating with it, never fatal. A *loss episode* is a run of source
  symbols on one video layer that the channel never delivered directly
  (repair-recovered-then-heard symbols do NOT count: the repair merely won
  an arrival race), merged with any other such run within one repair
  window of it, since those compete for the same repairs. `SwDecoder`
  books it at horizon eviction — the only point where "never delivered" is
  final — so a row lands roughly a horizon after the loss. Header
  `feclog 3`, then `<t_ms> <sid> <mcs> <bw> <ov> <first_seq> <span> <m>
  <rec> <rtx> <aband> <stale> <r> <w>` per row: drain tick (mono ms, ~10 ms coarse),
  video layer (0 base / 1 enh), the op MCS and width (`feclog 1` rows are
  20 MHz) and that sid's commanded
  overhead at drain time (so a row scores against its own rung with no
  ctl.log join), wire seq of the first missing source, seqs spanned,
  missing = recovered + rtx + abandoned (`rtx`: filled by a software-NACK
  retransmit, `docs/fec-nack.md`; feclog 1/2 rows read as 0), of those how many fell below the
  transition watermark (`stale`, the same debris class the ladder
  excludes), distinct covering repairs received (`r`, a two-card copy
  counts once) and the repair window as flown (`w`). `flightreport.py`'s
  FEC EPISODES section groups rows per (sid, mcs, bw, ov) and prints, for the
  non-stale ones, the overhead each episode would have needed,
  `ov_req = (sqrt(1+4c)−1)/2` with `c = m·ov·(1+ov)/r` — at overhead x the
  same lost air carries `m(1+ov)/(1+x)` sources against `r·x/ov` covering
  repairs — plus how many episodes would have failed at 0.25/0.35/0.50/
  0.75/1.00. Read it as the input to a *static* retune of
  `[[link.ladder]]`'s `overhead_base`/`overhead_enh`: a single lost agg-6
  aggregate at ov 1.0 / w 32 models to ~0.5, so the shipped 1.0/0.5 pair
  is the first thing a flight's max/p99 will judge. Nothing consumes the
  file live; `fec.log` alone is also a valid `flightreport.py` argument.
  Bench 2026-09-16 (session 0088, 80 s at rung 5, ausniff clean): base
  episodes are one lost aggregate each — `m` p50/max 11/16 against `r`
  ≈ 41, `ov_req` p50/max 0.40/0.46, zero would-fail at 0.50; enh (ov 0.5)
  `m` 12–16 against `r` ≈ 21, `ov_req` 0.38; ~0.75 episodes/s, the known
  ~1 lost agg-6/s. ⚠ The first ~18 s after a maburgs restart log a block
  of rung-0 episodes with `m` ≈ 200 and ~170 abandoned, `stale` 0 — the
  same boot warm-up loss that pollutes the rung-0 residual EWMA
  (`link.rungs[0]`), not a rung-0 verdict; drop them by time, or read
  only rungs the ladder actually held.

**arq.log (arqlog 1, 2026-09-28).** Feedback-repair shadow mode, phase 1
of `docs/feedback-repair-rollout.md`: at every burst end, how many repair
symbols each video layer is short -- exactly what a live "send me k more"
request would ask for -- written by maburgs (`gs/src/arq_shadow.h`,
`gs/src/arq_log.h`) into the session directory, rotating with it, never
fatal. Observes only: nothing extra is transmitted. The shortfall is
`SwDecoder::deficit()` (unknown live seqs minus pending independent rows,
0 exactly when nothing is missing), sampled `settle_ms` (4) after the burst
ends so the other card's tail copy lands first. A burst ends at the other
layer's first body (base and enh AUs alternate), at the trailing probe, at
a same-layer body after an 8 ms RX-stamp gap, or after 15 ms of silence.
Header `arqlog 1`, then two record types:
`E <t_open_ms> <sid> <mcs> <bw> <ov> <bpb> <dur_ms> <grow_ms> <nack> <d0> <dpk> <aband> <stale>`
is one shortfall episode -- consecutive short burst ends of one layer,
closed by the first one back at 0: when it opened (burst-end mono ms), the
op and overhead at open, FEC blocks per body (symbols -> bodies), how long
until the layer was whole again (`dur_ms` -- an in-band fix if `aband` is 0,
else the eviction of what was lost), how long the shortfall kept growing
(`grow_ms`: 0 = one burst, large = an outage), how many burst ends were
short (`nack`: the requests a live loop would have sent), the first and
peak shortfall in symbols, and the symbols abandoned inside the episode
(`stale` of them transition debris, excluded like fec.log's).
`S <t_ms> <sid> <bursts> <short>` every ~10 s per layer that had bursts:
the denominators. `flightreport.py`'s ARQ SHADOW section turns it into the
go/no-go: short-burst share and request rate per layer, peak shortfall in
aggregates (assumes `ampdu.max_num` 6 bodies), in-band fix delay and what a
repair at an 8 ms round trip would save, and lost episodes split into
burst-shaped (<= 2 aggregates, grew <= 50 ms: one repair round could have
saved them) vs outage-shaped. `maburgs --dry-run` runs the same tracker
over a replay and prints `arq_shadow <sid>:` totals on stderr (order-only:
the replay clock is synthetic). First flight data: two indoor flights,
2026-10-03 (`docs/feedback-repair-rollout.md` "Phase 1 results").

**link.listen (2026-10-07; removed 2026-10-10 with the feature, recordings
before then only).** The listen window, phase 3 of
`docs/feedback-repair-rollout.md`. GS side, cumulative: `on` (statuses going
out now: `[listen] ms` > 0, the A/B phase, in session, the drone advertises
`CAP_LISTEN`, no calibration sweep), `ms`, `ab_s`, `sent` and its split by
what marked the burst end (`probe`, `deadline`, `completion`), `late_max_ms`
(worst core-loop delay from that mark to the send). `drone` is the latest
`T_LWSTAT`, per drone period — sum it once per `seq` (both cards hear it):
`ms` (the gap the drone kept), `status_rx`, `hist` (8 bins of RX time minus
the AU's modelled burst end: <0, 0-1, 1-2, 2-3, 3-4, 4-5, 5-7, ≥7 ms),
`nofid` (no gap on record for that AU), `gate_holds` / `gate_hold_sum_ms` /
`gate_hold_max_ms` (AU first bodies and late repairs the gap held), and
`direct_holds` (control/MSP/pong sends it held); `rx_ms` is the GS arrival.
null until the first report. **Phase 3b (2026-10-09)** drones send
`T_LWSTAT` v2 (`rc_proto.h`): `hist` bins become <0, 0-2, 2-4, 4-6, 6-8,
8-10, 10-15, ≥15 ms after the burst's modelled end, and the report carries
`inside` (statuses that landed inside their window), the learned window
`delay`, and `fit_skips` (windows cut to nothing before the next AU) in
place of `nofid` and `direct_holds`. A GS built since exports `v` (1 or 2)
and, for v2, `inside`, `delay_ms`, `fit_skips`; an older GS exports the v2
body raw — `ms` with bit 7 set (132 = v2, 4 ms), `nofid` = inside,
`direct_holds` = delay_100us << 8 | fit_skips. `flightreport.py` decodes
both.

**ta.log (talog 1, 2026-10-04; removed 2026-10-10 with the bench, sessions
before then only).** The turnaround bench, phase 2 of
`docs/feedback-repair-rollout.md`: how long a status frame takes to turn into
a reply on air while the drone's video queue is loaded, per drone hardware TX
queue. Written by maburgs (`gs/src/ta_log.h`) into the session directory,
rotating with it, never fatal, and only when `[turnaround] rate_hz` is set
(it is 0, off, in the bundle). The GS sends `T_TA_PING` at jittered random
times (not in the RCF slot, so every queue state gets sampled), lanes round
robin; maburd answers each with `T_TA_PONG` on the hardware queue the ping
names (`drone/src/ta_responder.h`, devourer `TxMode::hw_queue`). Raw
sightings, paired offline:
`talog 1 rate_hz=<r> lanes=<a,b,..> frames=<n> bytes=<b>` header (re-written
on a respawn, which also restarts ping seqs at 0 -- never pair across one);
`S <seq> <lane> <card> <t_call_us> <t_done_us>` a ping handed to TX card
`card` (the two stamps bracket the synchronous send);
`H <card> <seq> <tsfl> <t_us>` card `card` heard the GS's own ping -- the
witness, which can only be a card that did not send it;
`O <card> <seq> <lane> <idx> <n> <tsfl> <t_us> <hold_us> <txq> <pool> <backlog_100us> <rssi>`
card `card` heard reply frame `idx` of `n`, with what the drone stamped: its
hold from RX callback to that frame's send call, its TxQueue and USB-pool
depths and its air-clock backlog (0.1 ms units) at that call, plus the
frame's RSSI (dBm, better chain; 0 without PHY status);
`X <card> <type> <len>` card `card` heard an FCS-clean `T_TA_PING`/`T_TA_PONG`
(`type` 7/8) of `len` body bytes that did not parse -- a wire/parser mismatch,
counted so it cannot pass for replies that never came (the first phase-2
flight lost every pong this way: the GS RX body keeps the 4-byte FCS and the
pong parser read its CRC from the very end). `tsfl` is the
receiving card's hardware RX TSF (us, low 32 bits, wraps every ~71.6 min),
so `O.tsfl - H.tsfl` for one seq on one card is the on-air turnaround on a
single clock; `t_us` is GS host mono time (the host round trip, which adds
both USB paths). `flightreport.py`'s TURNAROUND section pairs them: per
lane, replies received, on-air turnaround p50/p90/p99 (first reply frame,
and the whole burst when `n` > 1), host round trip, drone hold, loaded vs
idle queue, per rung when `ctl.log` is beside it, and the phase-2 gate on the
on-air p99. tsfl's latch point (frame start or end) is undocumented; either
way it moves the turnaround by one short control frame's airtime.

**scan.log (scanlog 2).** New per-session file (spec
2026-09-13-auto-channel-select, extended by
2026-09-14-inflight-channel-hop), opened alongside `ctl.log` whenever
`debug_log.enable` is set. Six record letters, one line each — `A` (the
1 Hz in-flight energy poll) is **gone**, along with
`radio.scan.energy_period_ms`: the in-flight hop's verdict-window reads
(§below) replaced it as the in-session energy source, at ~150 ms cadence
instead of 1 Hz:
maburgs writes a per-session directory when `debug_log.enable` is set
(shipped default `false` — the bench GS turns it on): `<debug_log.dir>/NNNN/`
holding `flight.jsonl`, `ctl.log`, `probe.log`, `au.log`, `fec.log` and
`scan.log` (`au.log` only while `[au_ring] enable`); maburplay writes
`lat.log` and `frame.log` into the same directory by following the
`/tmp/mabur-session` marker and holds no logging config of its own
(`gs/src/debug_session.h`). Nothing is written until the knob is set, and
`enable = false` removes the marker, which stops maburplay too.

- **Session identity.** NNNN is one past the highest existing index — never
  a timestamp, since the GS RTC is wrong at boot. The marker lives in tmpfs,
  so a reboot starts a new session while a 2 s wrapper respawn rejoins the
  current one and appends (which is why a format-marker line can appear more
  than once in a file).
- **A drone restart is a new flight.** When T_TELEM's `tlm_seq` steps
  backwards by more than 100 (maburd restarts it from 0; a fade or
  re-rendezvous keeps it climbing; `gs/src/drone_restart.h`) maburgs rotates
  in place — next NNNN, marker rewritten, all six files reopened under it
  with their format marker at the top — with no process restart and no video
  blink; maburplay's `lat.log` and `frame.log` follow the marker within 5 s. Cumulative
  sideport counters do NOT reset at a rotation (same maburgs process), so the
  new `flight.jsonl` starts at nonzero values; `flightreport.py` differences
  within the file. A maburd crash-restart mid-flight therefore splits that
  flight in two, by design.
- **One clock.** Every file stamps CLOCK_MONOTONIC, so any two rows join
  directly.
- **One writer thread.** All six maburgs files go through one writer thread
  (`gs/src/log_writer.h`) whose per-stream ring can fill under load; when it
  does, the writer counts the dropped lines and at its next 1 Hz flush
  appends `# dropped N` into the affected file, so a gap is visible in the
  data itself. `flight.jsonl` opts out (`mark_drops=false`: NDJSON can't
  carry a comment line, and a gap is already visible from the datagram's own
  `seq`). Every logger is non-fatal.

Older recordings (`ctl-NNNN_<date>.log`, `au-NNNN.log`, `lat-NNNN.log`,
`# sync` anchors, wall-clock `t_us`) and every previous format version are
described in `docs/data-provenance.md`; the analyzers still read them.

### `flight.jsonl`

One sideport snapshot per `stats.interval_ms`, written straight from
`StatsExporter` (no UDP round-trip). No format marker; the datagram's `v`
and `seq` are the self-description.

### `ctl.log` (`ctllog 12`)

The adaptive-link record, written also in static-pin mode. Header
`ctllog 12 <header_info>` (the ladder with per-rung width, thresholds), then
compact lines — formats locked in `gs/src/ctl_log.h`:

- `S` — dwell sample every `debug_log.ctl_period_ms`: rung, util `u`/`u3`
  (arrival-booked pre-FEC loss), residuals, RF labels, fade deltas, and three
  trailing probe columns `probe_rung probe_u probe_n` (`-1 nan 0` when
  nothing is commanded).
- `E` — rung transition with reason.
- `P` — probe gate STATE EDGE (`clean|lossy|noinfo`) — a gate edge, not a
  probe outcome.
- `N` — penalty booked. `R` — per-rung EWMA store snapshot, every
  `debug_log.rung_period_s`.

In static-pin mode (`link.static_mcs >= 0`) the `S` line's controller
columns are frozen (`u`/`util` read 0, `E`/`P`/`N`/`R` never fire); only
its probe columns move, `<probe_rung> nan <probe_n>` — `probe_rung` is
`min(probe.rung_offset, top)` off a frozen rung 0, `u` is `nan` because the
gate never leaves `Off`, and `probe_n` is nonzero whenever
`link.probe.pin_mcs >= 0`. In pin mode only the probe rows, and
`probe.log`, carry information.

`flightreport.py`'s `PROBE GATE` report scores the edges as a demote
predictor: per demote episode, `lead` is the time from the FIRST lossy edge
since the held rung was entered (the last `E` line before the episode,
either direction) — edges before that transition were measured under a
different hold and never count — and a lossy edge is a false alarm unless
the next `E` line is a demote within 10 s. The episode report also prints
an "s3-settle-refire canary" (demotes landing 300–360 ms after an
`s3_residual` demote, the debris double-step `transition_edge.h` removed;
~0 expected). Controller invariants behind these lines:
`docs/link-adaptation.md`.

### `probe.log` (`probelog 3`)

One row per finalized received probe body — roughly one per AU, far denser
than the ctl log's `S` lines (`gs/src/probe_log.h`). Header
`probelog 3 bpb=<bpb>`, then
`<t_ms> <seq> <mcs> <bw> <enh_fid> <blocks_ok> <card_mask> <snr_c0> <snr_c1> <evm_c0> <evm_c1> <first_ms>`.
`t_ms` is the finalize tick (~10 ms coarse), `first_ms` the radio's arrival
stamp of the body's first sight on any card (mono ms to 3 decimals, same
clock as `au.log`'s `t_complete`), `blocks_ok` the union of surviving
blocks, `card_mask` the cards that delivered any block, snr/evm per card in
dB (`nan` when that card heard nothing). A row is written for EVERY
finalized body, on- or off-profile: `mcs`/`bw` are that body's OWN profile,
so an RCF-lag body (arrives just after a profile switch) logs at its stale
mcs instead of vanishing. A wholly-lost body is the only case with no row;
`flightreport.py` derives losses from `seq` gaps and joins rows to `au.log`
on `enh_fid` (nearest completion in time — the id wraps every ~36 min) to
print the completion→probe offset percentiles. `tools/bench/probesend.py`
adds the GS send stamps from a `MABUR_GAPLOG=1` run to place every send
against the probe (`docs/findings/2026-09-05-probe-blanking-fix.md`).

### `au.log` (`# aulog 7`)

Per-AU meta rows, fed in-process from `AuRingWriter::last_record()`
(`gs/src/main.cpp`'s FrameStream finish callback; `gs/src/au_log.h`) —
maburgs is the ring WRITER, so the log cannot miss an AU to a ring overrun.
Twenty columns:

    t_us pts sid fid len flags nal0 t_first t_complete enc dq air slices kept filled after_hole t_last t_drain t_push vcl

`t_us` is CLOCK_MONOTONIC µs. `t_first`/`t_complete` are the AU's mono-µs
latency stamps (first body / finish()). `enc`/`dq` are the drone's
SBI-latched `enc_us`/`drone_q_ms` (venc encode time, true TxQueue wait —
~0 when healthy) carried on the AU's first fragment. `air` is the drone's
modelled air backlog at that frame (`AirClock`, `docs/link-adaptation.md`
"Drone air clock"). `slices kept filled after_hole` are slice salvage
(`docs/slices.md`), all 0 on an AU that was neither split nor salvaged.
The next three are GS stamps (mono µs, 0 = unknown) that split the fec
segment per frame. `t_last` is the arrival of the AU's LAST body:
`t_last − t_first` is the arrival span (drone hand-off plus air),
`t_complete − t_last` the GS publish tail. `t_drain` and `t_push` belong to
the AU's last-pushed fragment and split that tail further: `t_drain −
t_last` is the wait in the RX queue before the core thread drained the
body's batch, `t_push − t_drain` the work on earlier bodies of that batch
plus FEC decode, `t_complete − t_push` FrameStream emit and the ring write.
`vcl` is the first slice NAL type of a complete, parameter-set-led AU
(IRAP 16..21, a refresh start's TRAIL_R is 1), 0 when the AU did not open
with VPS/SPS/PPS — the GS-requested-IDR clearing signal.

### `fec.log` (`feclog 4`)

Per-episode FEC loss record (`gs/src/fec_log.h`), the measurement behind
"is the rung table's overhead pair oversized". A *loss episode* is a run of
source symbols on one video layer that the channel never delivered directly
(repair-recovered-then-heard symbols do NOT count: the repair merely won an
arrival race), merged with any other such run within one repair window of
it, since those compete for the same repairs. `SwDecoder` books it at
horizon eviction — the only point where "never delivered" is final — so a
row lands roughly a horizon after the loss. Header `feclog 3`, then
`<t_ms> <sid> <mcs> <bw> <ov> <first_seq> <span> <m> <rec> <rtx> <aband> <stale> <r> <w>`:
drain tick (mono ms, ~10 ms coarse), video layer (0 base / 1 enh), the op
MCS and width and that sid's commanded overhead at drain time (so a row
scores against its own rung with no ctl.log join), wire seq of the first
missing source, seqs spanned, missing = recovered + rtx + abandoned (`rtx`:
filled by a software-NACK retransmit, `docs/fec-nack.md`), of those how many
fell below the transition watermark (`stale`, the debris class the ladder
excludes), distinct covering repairs received (`r`, a two-card copy counts
once) and the repair window as flown (`w`).

`flightreport.py`'s FEC EPISODES section groups rows per (sid, mcs, bw, ov)
and prints, for the non-stale ones, the overhead each episode would have
needed, `ov_req = (sqrt(1+4c)−1)/2` with `c = m·ov·(1+ov)/r` — at overhead
x the same lost air carries `m(1+ov)/(1+x)` sources against `r·x/ov`
covering repairs — plus how many episodes would have failed at
0.25/0.35/0.50/0.75/1.00. Read it as the input to a *static* retune of
`[[link.ladder]]`'s `overhead_base`/`overhead_enh`. Nothing consumes the
file live; `fec.log` alone is also a valid `flightreport.py` argument. The
first ~18 s after a maburgs restart log a block of rung-0 boot warm-up
episodes — drop them by time. Bench record:
`docs/findings/2026-09-16-fec-log-overhead-bench.md`.

`feclog 4` adds two row kinds that start with a letter instead of a
timestamp digit, so an episode row is still distinguishable by its leading
`t_ms`: `I <t_ms> <reason> <epoch>` logs an IDR request going out (`reason`
is whatever armed it — `hole`, `salvaged`, `lost`, `start`, `resync`,
`decoder`, `dvr` — and a retry repeats the same reason; retries only go
out while maburgs is in a session, so an outage adds no rows), and
`H <t_ms> <heal_ms> <retries>` logs a complete base IRAP clearing that
request (`heal_ms` counts from arming, dead time before the link came up
included). `load_feclog()` skips both; they are read by `load_refresh_rows`,
and `flightreport.py`'s REFRESH section (requests by reason, retries,
heal_ms distribution, and how many requests fall within 2 s of a ladder
demote / a FEC episode) prints from it — on a bare `fec.log` argument and
in session mode alike.

### `scan.log` (`scanlog 6`)

The channel-selection record (`gs/src/scan_log.h`, formats locked there).
Six record letters, one line each:

- `C` — a card's adapter-caps identity plus sensor validity flags, once at
  bring-up.
- `D` — one scout dwell (the channel-ranker's raw input) — boot-time or
  in-session, distinguished by a trailing `sess` flag and three step-timing
  columns the in-flight scout added; since `scanlog 6` (2026-10-05, CPE510
  relay interference sweep/hop) a further trailing rx % field for a relay
  sweep entry (`-` for a USB dwell), `docs/cpe510-relay.md`.
- `K` — the pick at freeze, with the full ranking.
- `M` — a GS retune that changes where the link lives (`commit`,
  `ack_override`, `link_found`, plus the hop reasons
  `hop_lead`/`hop_follow`/`hop_withdraw`/`hop_one_card`; `split_home`/
  `reunite` only in `scanlog 4` and earlier files).
- `V` — one verdict-engine window, on every verdict change and every
  non-healthy window.
- `H` — one hop-controller event (`order`, `lead_confirm`,
  `one_card_retune`, `verify_pass`, `verify_fail`, `withdraw`, `hold_cap`,
  `hold_exhausted`, `relocate`, …; `would_`-prefixed rows exist only in
  recordings made before 2026-10-04, `docs/data-provenance.md`).

Full formats, the config, the sideport keys it feeds, and the
`cca − own` ranking assumption for the boot-time (`C`/`D`/`K`/`M`) records
are in `docs/channel-select.md`; the in-flight hop's rule table, evidence
bits, hop sequence, and Known limitations are in
`docs/inflight-channel-hop.md`.

**Sideport: `hop` and `cards[i].dwell`.** Since 2026-09-14
(in-flight-channel-hop) a new top-level `hop` object is unconditional
(idle defaults while pinned, matching `link.probe`'s
pattern): `hop = {verdict, evidence, ref_rung, epoch, state
(idle|ordered|verifying|hold), target, hops, holds, last_ms,
sweep_timeouts}` — `ref_rung`
and `target` are `null` while unfrozen / before the first-ever order,
`last_ms` is `null` until any hop event has fired this session.
`sweep_timeouts` (since 2026-10-05, CPE relay hop) counts relay `SCAN`s
that got no `SCAN_RESULT` before their timeout
(`max(1000, passes·n·(observe_ms+40)+300)` ms), cumulative per maburgs
process; 0 on a GS that never sweeps a relay. Per card,
`cards[i].dwell` (`null` until that card's first completed dwell) carries
`{visits, score, cost_us}` — `visits` is cumulative over every dwell,
success or failure; `score`/`cost_us` are the last **successful** dwell's,
since a failed retune produces no visit to score. `cards[i].energy` keeps
its pre-existing shape but is now refilled from the verdict engine's
~150 ms window reads instead of the deleted 1 Hz `A`-record poll. Since
2026-09-25 (NHM airtime evidence, `docs/findings/2026-09-25-nhm-airtime-spike.md`)
it also carries `busy_pct` (NHM busy %, `null` when that window's reading
was invalid or none has landed) and `own_air_pct` (the GS's own
reconstructed airtime on that card, `null` only before the first window).
`tools/maburtop.py`'s header gains `hop <state>/<verdict>` and its
per-card `busy` column tracks `cards[i].energy` at the new cadence, plus a
further `fbusy` column (`max(0, busy_pct − own_air_pct)`, clamped so a
stale `own_air_pct` past a fresher `busy_pct` can't read negative) — the
same foreign-busy-airtime figure the verdict's `blocked` bit and both
rankers use (renamed from `air%` in the final-review fix wave: the column
was always foreign busy, never own airtime). Full key semantics, the OSD
`(h)` mark, and the
`flightreport.py` HOP section are in `docs/inflight-channel-hop.md`.

**Sideport: `cards[i].kind` and `cards[i].relay`.** Since 2026-10-02
(maburgs `RemoteCard`, `docs/cpe510-relay.md`) every card carries `kind`:
`"usb"` or `"relay"` (a CPE510 `mabur-relay` unit from `[radio] relays`;
relays follow the USB cards in the roster). A relay card also carries
`relay = {state, owned, ch, sec, frames, gaps, your_drops, tx, tx_fail,
tx_refused, reconnects, tx_scan_drop, sweeps}` (USB cards: key absent;
the last two since 2026-10-05, protocol v4): `state`/`ch`/`sec` are
the last `STATUS` (state 0 tuned, 1 retuning, 2 failed, 3 refused; `sec`
0 HT20, 1 HT40+, 2 HT40-), `owned` is owned-and-tuned on our target,
`frames` the `FRAME`s seen, `gaps` the relay→GS `seq` gaps — Ethernet/UDP
loss between the CPE and the GS, **not** air loss (air loss stays in the
per-class dot11-seq `delivery`), `your_drops` the relay's own count of
frames it could not send us, `tx`/`tx_fail`/`tx_refused` the relay's
injection counters (`tx_refused` = `TX` messages it would not inject: not
from the owner, `mcs` > 7, a reserved flag bit, a bad length), and `reconnects` the client restarts —
**both** the 5 s refused-restarts and the reopens after a lost relay;
`tx_scan_drop` the owner `TX` frames the relay dropped while a sweep had
it off channel, `sweeps` the `SCAN`s this card has sent. `RelayStatsIn`
also carries `you_own` and the transport's `rx_drops`/`tx_drops` (the
browser ring; 0 over UDP), but maburgs does **not** export them — only the
web GS's stats JSON does (`relay_you_own`, …). On a
relay card the per-class `snr`, `snr_a`, `snr_b`, `evm`, `evm_a`, `evm_b`
are always `null` (the relay's "SNR" is RSSI above a calibrated floor, not
a measurement — `CardCaps::snr_ok = false`); RSSI is real. `energy` is
present but its `cca`/`fa` read 0 and `busy_pct` stays `null` (no NHM);
`own`/`foreign`/`own_air_pct` are real.
`tools/maburtop.py` labels relay rows `r<id>` (USB `c<id>`) and adds one
strip row per relay under the cards grid (`st own gaps drops txref
reconn`, warn when not owned or `gaps` grew); the player OSD draws an `R`
id and a dashed SNR cell and counts a relay card as heard on RSSI alone.
The 5 s stderr `stats:` line appends `relay[own= gaps= drops= reconn=]`
per relay card.

**Sideport: `link.probe` and `classes.probe`.** Since 2026-09-04 the probe
stream's live gate state is exported unconditionally (even in static-pin
mode, where there is no controller) as `link.probe = {on, rung, mcs,
state, u, loss, streak_bodies, n, exp, rx, off_profile, cards:[{loss, rx}]}`
(`streak_bodies` replaced `streak_ms` on 2026-09-16 with the body-count
clean streak, `link.probe.clean_bodies`; recordings before that date carry
`streak_ms`)
— `u`/`loss` are `null` when the gate has no usable sample yet. `classes`
gains a `probe` entry alongside `s0`/`s1`/`msp`/`ctrl` (`RfClass::Probe`,
`kNumRfClasses` 5) with the probe stream's own per-card RSSI/SNR/EVM —
`classes.s2`/`classes.s3` are gone (they were always empty since the
2026-08-29 UEP flatten). `link.rungs[].probe_u`/`probe_n` MEAN something
different from before this date too — see `docs/data-provenance.md`.
`maburtop`'s LADDER panel replaces its old "last probe" line with a live
gate line: `probe: r3 mcs5 clean 54b u0.12 n48 | c0 0.00 c1 0.05` (`54b` =
the clean streak in bodies), and its
signal panel gains the `probe` class in `CLASS_ORDER`.
maburplay's 1 Hz `lat:` stderr line, also appended to `lat.log` in the
current session directory: `# latlog 2`, then one `<mono_us> <lat-payload>`
line per second, line-buffered (`gs/player/src/lat_log.h`). The marker is
re-read every 5 s; a missing session or failed open (DVR not mounted yet)
never blocks and never spams, and the stderr line keeps going regardless.

The player's three session files (`lat.log`, `frame.log`, `reg.log`) are
each written by their own writer thread: the main loop, which also drives
decode and display, only queues a row. Every marker read, open, write and
flush happens on that thread, so an SD card that stalls on writeback (the
burned DVR streams to the same card) delays the log, never the picture. The
queue holds 256 KiB, about 20 s of `frame.log`; a row that does not fit is
dropped and counted, and the file gets one `# dropped N` line where the gap
is, as maburgs' own files do.
`tools/bench/latab.py latA.log latB.log` reads a pair of these logs and
prints the vsync A/B verdict (four log-derived gates: `e2e` p50 B≤A−8,
`dsp` p50 B≤6 (level), `dsp` p99 B≤A−8, `dsp` p50 4 s-bucket sweep ≤3
(flatness) — `anchor=warm` windows are excluded from all four, with a
printed count).

### `frame.log` (`# framelog 3`)

The per-frame rows behind the `lat:` line, from the same `LatTracker`
window (`gs/player/src/lat_tracker.h` `FrameRow`): one row per frame that
completed the whole chain and joined the window — a displaced frame or one
submitted on a cold anchor gets no row, exactly as it is absent from the
`lat:` percentiles. Written by the same follow-the-marker writer as
`lat.log`, in the same 1 Hz burst, on its own writer thread and buffered
so each burst reaches the file in one write. Twenty-two
columns:

    t_flip pts fid sid len flags slices t_submit t_complete t_decoded t_release enc dq air fec dec reg dsp e2e exact t_commit pbits

`t_flip` is the frame's scanout-start stamp (mono µs); `t_submit` is when
the player handed the AU (or its first slice) to the decoder, `t_complete`
the ring's finish stamp (as `au.log`), `t_decoded` when the decoded picture
came back, `t_release` when the regulator released it. `enc dq air fec dec
reg dsp e2e` are the frame's own segments (µs, `dec = t_decoded −
t_complete`, `e2e` the sum); `exact` is 1 when `t_flip` came from a real
vblank event. `pts` joins the row to `au.log` (same value), which carries
`nal0`, `t_first` and the GS stamps; `fid`, `sid`, `len`, `flags` and
`slices` are copied from the AU record so a row reads on its own, `flags`
in hex as `au.log` prints it (`framelog 1` printed it in decimal). A
streamed frame has `t_submit < t_complete`, so `t_decoded − t_submit` is
not decode time for it; split `dec` with `t_complete` instead.

`t_commit` is when the frame's atomic commit ioctl returned (mono µs; 0
when the presenter parked the frame in its mailbox because a flip was
still in flight, or the commit failed), so `t_commit − t_release` is the
present() path's own cost and `t_flip − t_commit` the kernel's latch.
`pbits` are the presenter's commit bits in hex
(`DrmPresenter::CommitBits`): `0x01` the OSD plane rode along (a
vsync-paced two-plane commit), `0x02` `PAGE_FLIP_ASYNC` was set, `0x04`
parked, `0x08` the commit failed, `0x10` a blocking modeset commit.
`framelog 2` and earlier rows end at `exact`.

### `reg.log` (`# reglog 2`)

The 1 Hz `regulator:` line ("Regulator line" below), written by the same
follow-the-marker writer as `lat.log`: `<mono_us> regulator: held= ...`.
Its counters are cumulative since maburplay started; difference adjacent
rows for the per-second rate. It exists because `pend`/`pdrop`/`heals` are
the presenter's side of a missed latch, and `/tmp/maburplay.log` does not
outlive a power-off. reglog 2 adds the phase-lock fields `lock= lerr=
loff= lctr=` (docs/phase-lock.md); reglog 1 rows end at `lslot=` before
`present=`.

### `cal.log` (`callog 3`)

**Loss-sim rig covers the probe stream too.** `LossSim::kStreams` (bench
rig, `MABUR_LOSS_SIM` builds only, see `docs/airtime-model.md` §4) is
`s0..s5`, `5` = probe — `tools/bench/losssim.py s5 eff=<pct>` injects
loss on the probe stream for the gate's refusal test (design spec §8.3
step 3: parked clean, inject, expect `Lossy`/`probe_holds`, no promote).
Host build for that rig: `cmake -S . -B build-losssim
-DDEVOURER_DIR=$PWD/../devourer -DMABUR_LOSS_SIM=ON` (gitignored dir,
separate from the normal `build/`).

**Rule of thumb: if you want to KNOW something about the running link,
read the sideport. Reach for other tools only in these cases:**
### Analyzers

All take a session directory (resolved by `tools/session.py`), a single
legacy file classified by its format marker, or no argument (the
highest-numbered session under `/media/dvr/log` — highest index, never
newest mtime).

- `tools/flightreport.py` — the post-flight report over the whole session
  (sections named above, plus BW40, NACK, REFRESH, wall/rung-store, episode
  reports).
- `tools/flightjitter.py` — reproduces the player's jitter EMA from the AU
  rows (arrival basis `t_complete`), splits it into size-explained vs
  residual (`docs/airtime-model.md` §4), and classifies each stutter event —
  `gap` / `rung-change` / `size` / `fec-wait` / `loss-recovery` /
  `transport` — using the jsonl for ladder and loss context. `fec-wait` is
  direct per-frame evidence (the stutter AU's own `t_complete − t_first >
  20 ms`), checked before the jsonl-inferred `loss-recovery` class so it
  wins when both apply. Tests: `ctest -R test_flightjitter`.
- `tools/bench/airdrain.py` — see "Tools: which one when".
- `tools/bench/probesend.py` — see `probe.log` above.

## Player (maburplay) stderr lines and OSD

maburplay logs to `/tmp/maburplay.log` (tmpfs). maburgs' 1 Hz `stats:`
line is in `/tmp/maburgs.log`, maburd's in `/tmp/mabur.log`. These are
numerically redundant with the sideport but persist on disk until reboot;
the UDP feed is ephemeral — they are the post-mortem when no consumer was
listening.

### `stream:` line (streamed decode)

Every 5 s, printed by the decoder-input thread (FeedLoop;
`docs/slices.md` "Part 2 (as built)"): `on|off:<reason>`, `streamed` (of
them `salvaged`), `aborted` (`stream_aborted`, one rkvdec reset each),
`whole`, `opened`/`open_aborted` (ring records seen open / ended without
closing), `wake_us=p50/p99` and `n` (closed-record delivery − maburgs'
`t_complete`, µs), `refused` (STREAM_APPENDs the decoder refused,
`MppBackend::stream_errors`) and `parks`. `maburplay --oneshot` JSON carries
`streamed`, `stream_aborted` and `stream`. On shutdown maburplay logs
`maburplay: shutdown: <stream idle|stream finished|stream aborted> pts=…
waited=… ms…` (how the picture that was streaming when SIGTERM arrived was
finished before decoder teardown) and `maburplay: feed: delivered=…
submits=… parks=… notes_dropped=…` (the feed thread's tally for the run).

### Regulator line

The 1 Hz `regulator:` line (stderr, and `reg.log` in the session
directory): `held= late= replaced= disconts= hold_ema=
present_jitter= vsync= skips= fallback= pend= heals= pdrop= chained= chain=
chain_max= chains= cuts= alw= lslot= lock= lerr= loff= lctr= present= acq=
req= ioctl=`.

- `present=N acq=p50/max req=p50/max ioctl=p50/max` — the N present()
  calls since the previous line, split into the framebuffer acquire (the
  dma-buf import plus `drmModeAddFB2`, served from the presenter's
  framebuffer cache, `gs/player/src/fb_cache.h`: a pool buffer seen again
  is a lookup, a new one an import of ~3 ms), the atomic request build and
  the commit ioctl, µs. `display.vsync_lead_ms` has to cover their sum: a
  frame whose present() path runs past the lead latches a vblank late and
  the frame behind it is a `pdrop` (`frame.log`'s `t_commit` shows it per
  frame).
- `vsync=locked|fallback` — whether the vblank estimator is locked (servo
  release) or has fallen back to the `anchor_floor(pts) + regulate_ms`
  rule.
- `skips=` — deep-burst slot claims in servo mode: a frame whose natural
  vblank slot AND the next slot are both occupied claims the later one and
  the older occupant is dropped. Ordinary servo drops surface as
  `replaced=` evictions instead. The sensor/panel beat (the drone's fps vs
  the panel's 60 Hz) produces periodic panel repeats, visible in
  `--fps-log`, not here.
- `fallback=` — frames released via the fallback rule while
  `display.vsync_lock` is on (climbs during a cold start or a stale
  estimator; cold start needs 8 exact flips to first warm, validity is
  recency-based, so after a stall the counter stops within one fresh exact
  flip).
- `pend=` — the presenter's present()-while-flip-in-flight mailbox
  engagements, 0 with no presenter.
- `heals=` — chain-break slips: pending releases pushed one slot after two
  mailbox engagements within 100 ms (a backstop that fires ~only at
  startup).
- `pdrop=` — paced-mode mailbox drops: with the servo locked, a parked
  frame is a missed latch and is dropped at flip completion instead of
  resubmitted a period late, which keeps a single miss from becoming a
  one-vsync-late chain. A `pend` increment in servo mode therefore costs one
  dropped frame in the common case (two when the mailbox was already
  occupied — the replaced parked frame counts in `--fps-log`'s `repl=`).
  `pend`, `pdrop`, `replaced=` and `--fps-log`'s `repl=` together are the
  full drop accounting.
- `chained=`/`chain=`/`chain_max=`/`chains=`/`cuts=` — sequential-slot
  chains, the one-vsync-late chain the servo itself creates: one collision
  (two decodes inside one vblank window) shifts every following frame a
  period late until an arrival gap wider than a period frees a slot.
  `chained` = cumulative frames that took natural+1 vblank, `chain` the
  current run, `chain_max` the longest, `chains` runs started (chains/s ×
  mean length = chained/s), `cuts` runs cut by `display.chain_budget` (the
  frame that would extend a run past the budget takes its natural slot and
  the held occupant is displaced — one dropped frame, counted in
  `replaced=` too). The extra period lands in the `lat:` line's `reg`
  segment, so read `chained/held` per second next to `reg` p50. Code and
  bundle default to budget 0 (never cut): a chain then ends only on an arrival gap, so
  through the part of the sensor/panel beat where arrivals sit just past
  the release point the whole stream runs one period late (the `lat:` p50
  sawtooth) but nothing is dropped, and a key frame's ~10 ms of extra
  encode, air and decode fits inside that slack. A budget N instead keeps
  the lower latency and pays one dropped frame per cut; with a periodic
  refresh (`venc.gop_s > 0`, the config these benches ran) the
  refresh-start frame misses its release in about half of each beat cycle
  on a clean link and every miss then costs a repeat plus a cut (bench:
  budget 3 = 59–73 % of seconds with a visible event and 1.6–2.5 drops/s,
  budget 0 = 14 % and 0.16 drops/s, at +12.5 ms median). Records:
  `docs/findings/2026-09-02-regulator-chain-budget.md` (budget 3 chosen),
  `docs/findings/2026-10-15-regulator-chain-cuts.md` (budget 0 chosen).
- `alw=`/`lslot=` — capture-time scheduling (`display.sched_cap_ms`, 0 = off
  in code; the bundle runs 16, the phase lock's placement switch —
  docs/phase-lock.md; bench result and cost:
  `docs/findings/2026-10-15-capture-sched.md`). In servo mode a frame's vblank follows its
  capture time, not its arrival: the first vblank whose release is at or
  after capture + delivery floor + `alw`. The window holds each frame's
  transit, arrival − capture pts on the raw clocks, measured before the
  regulator's own hold (enc+dq+air+fec+dec, not `reg`/`dsp`); raw rather
  than against the pts anchor, whose floor snaps down on one fast frame
  and would shift every later sample. The floor is its p1 over the last
  600 frames (~10 s), `alw` is p99 − p1 + 1 ms capped at `sched_cap_ms`,
  and salvaged AUs stay out of the window. A late-tail frame within `alw`
  keeps its slot, so in the phases where frames arrive near the latch the
  repeat/skip judder (OSD LAT p99 a refresh above p50) becomes one extra
  refresh on every frame instead (p50 meets p99). `lslot` counts frames
  that missed their capture slot (loss, or a delivery shift beyond the
  cap); they are placed by the arrival rule above, and the next on-time
  frame that claims the same vblank wins it, so a stall costs a local
  freeze and catch-up with no trailing chain.
- `lock=`/`lerr=`/`loff=`/`lctr=` — the phase lock's state (`off` when
  `display.phase_lock` is false), last window's error, the offset it sends
  and its centre, mfps (docs/phase-lock.md).

### fps-log, display bring-up, MSP OSD

- `--fps-log` prints a once-per-second `fps-log:` line; with a burned DVR
  it carries the recorder fields including `burn_ctfb=`/`burn_ct_ms=`/
  `burn_enc_ms=` (`docs/colortrans.md`).
- The MSP OSD is rendered by maburplay from the UDP snapshot feed maburgs
  emits; its startup line and blanking notices land in
  `/tmp/maburplay.log`.
- maburplay drives the display from process start: it modesets immediately
  with a splash image (`/usr/local/share/mabur/splash.bin`, raw XRGB8888,
  regenerate with `tools/gen_splash.py`) so the sink locks a mode before
  video exists, and retries display acquisition once a second while none is
  connected — a display plugged in after the player started is picked up
  without a restart. That holds only if a display was NEVER acquired: one
  that disconnects after a successful acquire is deliberately not recovered
  (KMS retains CRTC state and a replug normally re-lights it on its own).
  Neither has a config key. The splash shows only until the first decoded
  frame — it is an aerial photo that would read as a live feed mid-flight,
  so a mid-flight replug comes up on video. Log lines:
  `no display at startup -- retrying …`, `display acquired after N.N s`;
  per-attempt DRM failures are silenced on purpose (/tmp is tmpfs).

### GS link-status OSD

maburplay draws it from the SAME datagram `maburtop` reads (`osd.gs.port`
= one of maburgs' `[[stats.out]]` ports), so the screen and the recorder
cannot disagree — if the OSD shows something surprising, the answer is in
`flight.jsonl`. Config is `[osd.gs]` in `/etc/maburplay.toml` (`enable`,
code default false, bundle true; `stale_ms` dims every link-derived field
after silence; fps/jitter/bitrate/REC are player-measured and never dim).
There is one layout, the compact bar — two rows along the bottom edge,
radio above and picture below, each item an icon, a space and its figures,
with the REC indicator alone in the top-right corner
(`gs/player/src/gs_compact.cpp`). `[name]` below stands for the icon:

```
                                              ● REC 12:47   <- top right
...
[wifi] 149 [tune-vertical] 5/40 [chart-donut] 62% [signal] -70/-72 [waveform] 22/20 [thermometer] 41
[speedometer] 8.1 [aspect-ratio] 720p [filmstrip] 60 [pulse] 5.2 [timer-outline] 45/78 [link-variant-off] 0.3/0.0
```

| icon | item | source |
|---|---|---|
| wifi | channel the RECEIVER is tuned to, `(a)` auto-selected / `(h)` hopped suffix; `KEY?` replaces the item on a link-key mismatch | `link.channel` |
| tune-vertical | MCS / channel width (MHz) | `link.ctl.rung` (else `link.op`) |
| chart-donut | airtime % | `link.air_pct` |
| signal | RSSI per GS card, dBm | `cards[]` |
| waveform | SNR per GS card, dB | `cards[]` |
| thermometer | drone SoC temperature, °C | `drone.sys.soc_temp_c` |
| speedometer | video bitrate, Mb/s | player-measured |
| aspect-ratio | decoded picture height + `p` (1440×1080 reads `1080p`) | player-measured |
| filmstrip | fps on screen | player-measured |
| pulse | frame-interval jitter, ms | player-measured |
| timer-outline | latency p50/p99, ms | player-measured |
| link-variant-off | loss before/after FEC, % | `link.pre_fec_loss` / `link.residual_loss` |

The icons are Material Design icons baked from JetBrains Mono Nerd Font
Mono (`gs_icon` in `gs_compact.h`, `BAR_ICONS` in
`tools/msp/gen_gsfont.py`); each is one cell wide. Only the icons come
from that font — the figures stay upstream JetBrains Mono, whose hinting
the Nerd Fonts patcher changes. A missing value reads
`--`, never zero. `lat` reads `--/--` while the latency anchor is cold —
each figure is ONE REAL FRAME from the last 1 s window, ranked by e2e, and
the pair is read together because p99 alone is a tail statistic that gets
misread as typical (`docs/findings/2026-08-31-dq-spike.md` §20); the
per-segment breakdown is not on the glass (the `lat:` line, `lat.log` and
`flightreport.py` carry it). No meters or bars. Two colour exceptions,
both single-threshold: the fps item turns caution while `drone.low_power`
is set, and temperature turns caution from 70 °C; both tints drop while
the sideport is stale. An icon takes its item's colour. The REC indicator
is a red dot, `REC` and an mm:ss clock (`REC FAULT` in amber), with a VTX
leg sized by `dvr.target` (`docs/vtx-recorder.md`); armed draws nothing.
It sits in the corner because in a row it would eat into the bar's width
and shift that row whenever the recorder was merely armed.

The two rows share columns: each item starts at the same x as the item
above or below it (ch/bitrate, mcs/res, air/fps, rssi/jit, snr/lat,
temp/loss), each column as wide as the wider of its two items' worst
cases, and the grid as a whole is centred. Every item sits in a box sized
for its WORST-CASE string (four cards), so short values leave trailing
space — that is what stops a row reflowing every time a figure gains a
digit. Only a card-count change moves the columns. The
type size is one size for both rows, and it is pinned: the largest baked
size at which a 76-character, 6-item reference row fits between the insets
— 25/37/48/75 px at 720p/1080p/1440p/2160p with the shipped asset
(`tests/test_gs_asset.cpp` pins them). That reference is the widest row
from when the items carried text labels; the icons made the real rows
narrower, and the freed width stays slack instead of growing the bar into
the picture. The worst-case grid (72 cells plus 5 gaps) must fit too, so a
wider item can never overflow. Two rows because all twelve items on one line would cap the type
too small to read on the GS screen.

The glyph atlas is `/usr/local/share/mabur/gs_osd.gfont`, committed and
staged by `tools/build-arm64.sh` — if it is missing, maburplay logs the
reason and runs with the MSP overlay only. Host-side you can see the actual
pixels without hardware: `maburplay --gs-render` dumps a rendered frame
(`--res WxH` and `--lat P50/P99` set the bar's player-measured fields; the
render is pixel-gated in `tests/integration/run_player_e2e.sh` PART D),
`tests/test_gs_asset.cpp` gates the real asset's layout at
720p/1080p/1440p/2160p, and `tools/bench/gs_osd_bench.cpp` measures
draw+quantize per update at 1080p and 2160p. Read that bench before
changing anything on this path: it runs on the 2 ms pump loop, and the rule
of thumb is that anything projecting past ~1.5 ms on the A55 is a defect. A
full repaint costs more than that and is accepted, because it is a
startup/re-layout event (the quantize half is burned-DVR-only, and
`ring.pump(2)` is a `poll()` ceiling over a slotted shm ring, so the cost
lands as one-vsync-late presentation rather than a lost AU). What is NOT
accepted is anything that puts a full repaint on a *cadence*: the burn
restate after an MSP collision is scoped to the fields the collision
actually hit (`osd_compose.cpp`).

### Record button (GPIO)

`maburplay` toggles the DVR from a button on the GS header: `[input.rec]`
in `/etc/maburplay.toml` (`pin` is the header pin number, required,
resolved at startup by matching the kernel's line names — the Radxa ZERO 3
names its header `PIN_7`…`PIN_40` across gpiochip1/3/4;
`active_low`/`bias` default to a button between the pin and GND with the
internal pull-up). No `[input.rec]` table means no button. Short press,
50 ms debounce, edge-triggered; `kill -USR1 $(pidof maburplay)` is the same
press for benches. `dvr.target` (`gs` | `vtx` | `both`) picks what a press
records (`docs/vtx-recorder.md`); on the GS, each start produces one file
in whichever `dvr.mode` is configured:

- **raw** waits for the next sync point before the file opens, so the OSD
  REC indicator visibly lags the press. The sync point is decided by
  `mabur::RawDvr` (`common/include/mabur/raw_dvr.h`, shared with the web
  GS's local recording): a complete IRAP (`au_is_irap()`) once VPS+SPS+PPS
  are known. Older raw files that open on a non-IRAP need repair — see
  `docs/findings/2026-09-29-web-gs-raw-dvr-irap-sync.md`.
- **burned** resumes at the next decoded frame (`docs/colortrans.md`).

Files are `record-NNNN.mp4` under `dvr.dir`, indexed one past the highest
`record-NNNN` already on the card (`common/include/mabur/dvr_name.h`) — no
timestamp, since the GS RTC is wrong at boot — so the index climbs across
boots and never overwrites an earlier flight; files not matching the
pattern are ignored. `dvr.autostart` picks whether the player boots
recording or armed. There is no config kill switch and no `--no-dvr` flag:
`autostart = false` with no button is a player that never records, so give
a `--decode-only` measurement run a config with `autostart = false` —
otherwise the decode gate writes a raw file and charges SD-card I/O to the
fps number. Startup logs the resolved mapping and every toggle logs
START/STOP; the OSD REC field is the live indicator. GPIO failures (pin not
found, line held by another consumer) are non-fatal and logged once.

The same loopback socket the player uses to tell `maburgs` its `vtx_rec`
wish also carries one-shot `refresh <reason>` lines — `start`, `resync`,
`decoder`, `dvr` — each one arming a demand-refresh IDR request
(`docs/vtx-recorder.md` "Wire"; mechanism: `docs/fec-nack.md` "Demand
refresh").

## Tools: which one when

- **Verifying maburgs itself → `tools/bench/ausniff.py`** (on the GS:
  `python3 ausniff.py --ring /dev/shm/mabur-au --seconds 30 --json`). It
  attaches to the AU ring read-only from OUTSIDE the daemon. This is the
  standing regression gate for any change that touches maburgs: the
  sideport is maburgs self-reporting, so gating a maburgs change on the
  sideport is circular — a bug that mangles frames or lies in its own
  counters sails through. Never replace the ausniff gate with sideport
  numbers. (Oneshot/post-hoc reads of a quiescent ring are exact; live
  mode is best-effort — Python cannot fence.) Host-side, the same
  invariant is `ctest -R 'gs_e2e|gs_au_e2e|player_e2e'` (byte-exact
  fixture-to-ring/AU comparisons via `verify_aus.py`/`--out-aus`).
- **Per-frame `air` excess around rung transitions → `tools/bench/airdrain.py`**
  (`python3 tools/bench/airdrain.py ctl-NNNN_<date>.log log/au-NNNN.log
  [--profiles]`, host-side, no lat log needed). Replays the player's
  `air` arithmetic over the AU log — `t_first − enc − q − pts` minus a
  `PtsAnchor`-style leaky min floor with the 2 s pts-discontinuity reset
  — and joins the ctl log's E lines: per cascade the 250 ms-binned excess
  around the first demote with IDRs marked, peak / time-to-peak / settle,
  the 1 s pre-demote excess, and the first-500 ms on-air bytes against
  the new rung's nominal PHY rate; single-demote and promote peaks;
  steady-state excess per rung; standalone spike seconds; and, since
  2026-09-06, a **transition IDRs** table — every non-starved `E` line
  with the first base IDR completing within 600 ms of it, kB p50/max
  by target rung split demotes | promotes and the ratio to the
  post-transition base P (`--profiles` lists every row). That table is
  the `venc.min_iqp` tuning readout (`docs/airtime-model.md`): read the
  rung-0 demote column after a flight, then move the floor. This is the
  §9 measurement of `docs/findings/2026-09-05-probe-stream-flight.md`
  and the A/B instrument for its drain-shed follow-up
  (`tests/test_airdrain.py` pins it on a synthetic cascade). The ctl and
  au indices are separate counters: pair them by mono span. The default
  window runs from the first E line to the last S line; a starve opens
  no episode (until 2026-09-06 it ended the window at the first
  `starved` E line, which read flight 0031 as 72 s of 566). `--model` on
  the three 2026-09-06 flights:
  `docs/findings/2026-09-06-air-clock-flight.md`, which also gives the
  per-AU reconstruction of the drone clock from `air_ms` (a candidate
  `--fit` mode).
- **Verifying AU-completion cadence → `tools/bench/aucadence.py`** (on the
  GS: `python3 aucadence.py --ring /dev/shm/mabur-au --seconds 25 --json`).
  Same outside-the-daemon ring posture as ausniff; reports the base−enh
  completion offset (p50 of `arrival_mono − pts` per class, IDR-excluded —
  metric provenance in `docs/findings/2026-08-29-airtime-balance-spike.md`).
  This is the **standing acceptance number for any change touching the
  AirBalancer, the venc pipeline, UEP overhead, or the bitrate policy** —
  the sideport `jitter_ms` EMA is the symptom of this offset and is noisier
  (±1.4 ms vs ±0.5 ms), so judge on the offset. Baselines recorded at the
  2026-08-30 v4 flag-day acceptance, mcs5 park: **−1.1 to −3.0 ms
  enh-late depending on scene/frame size** (21–33 KB frames, ~9.5 Mbps;
  balancer verified at its equal-air optimum 0.71/1.29 throughout, so the
  residual is encoder-side per-class completion latency — SVC-T
  alternation, scene-dependent) and **+1.1 ms at a pinned-mcs2** run
  (partly the 0.5×cmd rail). Regression criterion at the mcs5 park:
  **`--gate-ms 4.0`** — inside the known encoder envelope anything passes;
  a transport regression (lost balance, one-sided overhead, rate misroute)
  shows as the offset leaving that envelope. The design target ≤0.5 ms
  becomes the gate once the open venc-class-latency work lands. A capture whose
  per-class count is under `--min-samples` (default 100) is refused, not
  scored — enh silence (shed, loss-sim kill) is not a cadence sample.
- **Packet-level forensics → capture tools** (`tools/bench/seqdump.py`,
  `decode_bodies.py`, `live_decode.py`/`live_play.py`). The sideport is
  aggregates; when a summary number looks wrong, these record raw bodies
  for offline dissection. (The RTP-era FU tools — `fu_probe.py`,
  `fu_chain_analyze.py` — survive for reading OLD recordings only.)
- **Player tail-latency persistence → `lat.log`** (since 2026-08-31,
  vsync-locked-regulator; relocated by the 2026-09-06 consolidation). The
  player's 1 Hz `lat:` stderr line used to die in tmpfs with the power-off
  (`/tmp/maburplay.log`, gone on reboot — this is how the flight-0035 tail
  segments were lost, see `docs/findings/2026-08-31-latency-budget.md`). It
  is now additionally appended to `lat.log` inside maburgs' current session
  directory — maburplay holds no logging config of its own, it just follows
  the `/tmp/mabur-session` marker maburgs writes and stops if the marker
  disappears. Format: `# latlog 2` then one `<mono_us> <lat-payload>` line
  per second, line-buffered; there is no `# sync` bridge — maburgs and
  maburplay both stamp CLOCK_MONOTONIC on the same box, so `lat.log`'s rows
  join the session's other files directly. (`# latlog 1`, before this
  change, needed the bridge to line up against the flight jsonl's `t_ms`,
  and lived at `<display.lat_log_dir>/lat-NNNN.log` under its own next-free
  index, unrelated to the flightrec-written `au`/`flight` index or maburgs'
  `ctl` index — pairing an old recording still needs the mono-span/`#
  sync` matching described in `docs/data-provenance.md`; a session
  directory needs none of it, since `lat.log` is simply the file next to
  `ctl.log`.) A failed open (DVR not mounted yet, or no session yet) is
  retried every 30 s and never blocks or spams; it never blocks the stderr
  line either, which keeps going regardless. Since 2026-10-09 the same file
  also carries a 1 Hz `genlock:` line whenever `display.vsync_lock` is on
  (`gs/player/src/genlock.h`, `docs/efficient-link-plan.md` step 2):
  `on=` 1 while steering the camera, `cam=` the camera rate from the last
  ~4 s of pts (drone clock), `panel=` the screen rate, `phase=` the median
  time from a frame's capture to its next release deadline, `target=` where
  the loop holds it, `err=` the wrapped difference, `cmd=` the milli-fps
  setpoint sent (0 when observing), `n=` frames that second, `fps=` frames
  per real second (the 1:1 check: low power reads ~30), and two diagnostics
  of the raw pts the player sees, `pstep=` (median forward step, µs) and
  `pback=` (steps that went backwards or repeated). `cam=` is the phase
  slope against capture time (`f_screen × (1 + slope)`), not a pts step —
  the 2026-10-10 bench read `cam=0` at 60 fps from pts steps while the phase
  slope showed the beat exactly. The 1 Hz
  `regulator:` line (format below) is written there too since the same
  day — its counters are cumulative per player process, and flightreport's
  DISPLAY SMOOTHNESS section differences them (a drop = a player restart).
  Parsers that match `lat:` skip both. `tools/bench/latab.py
  latA.log latB.log` reads a pair of these logs and prints the vsync
  A/B verdict (four log-derived gates:
  `e2e` p50 B≤A−8, `dsp` p50 B≤6 (level), `dsp` p99 B≤A−8, `dsp` p50
  4 s-bucket sweep ≤3 (flatness, separate from the p50 level gate) —
  `anchor=warm` windows are excluded from all four,
  with a printed count); see `docs/bench-protocols-latency-2026-08-31.md`
  protocol 1 for the full arm procedure and the manual gates it doesn't
  cover.
- **Regulator line → vsync servo state** (since 2026-08-31). The 1 Hz
  `regulator:` stderr line in `/tmp/maburplay.log` gained
  `vsync=locked|fallback skips=<n> fallback=<n> pend=<n>`: `vsync=` is
  whether the vblank estimator is currently locked (servo release) or
  has fallen back to the old `anchor_floor(pts) + regulate_ms` rule;
  `skips=` counts deep-burst slot claims in servo mode — a frame whose
  natural vblank slot AND the next slot are both occupied claims the
  later one and the older occupant is dropped (bench steady state
  ~1–1.4/s at the mcs5 park, from fec-batch 4-frame bursts; ordinary
  servo drops surface as `replaced=` evictions instead). The beat wrap
  depends on which clock is faster. The 2026-08-31 bench measured a
  59.939 Hz sensor, slower than the 60.000 Hz panel: a ~16.4 s wrap with one
  panel repeat, visible in `--fps-log`, not here. The 2026-10-09 flights
  measured the camera at 60.078 fps — faster — so each ~12.8 s wrap throws
  one frame away instead (the CAMERA vs SCREEN report section reads the
  camera's rate from `au.log` on every flight;
  `docs/efficient-link-plan.md` step 2); `fallback=` counts frames
  released via the fallback rule while `display.vsync_lock` is on (climbs
  during a cold start or a stale estimator; cold start needs 8 exact flips
  to first warm, but validity is recency-based, so after a stall the
  counter stops within one fresh exact flip of flips resuming — the warm
  count saturates at 8 and never resets, it's only `last_exact_us_` that
  goes stale); `pend=` is the presenter's present()-while-flip-in-flight
  mailbox engagement count — a superset of the displaced-frame subset,
  0 with no presenter (decode-only or init failed). Since the bench
  session later the same day the line also carries `heals=` (chain-break
  slips: pending releases pushed one slot after two mailbox engagements
  within 100 ms — a backstop that fires ~only at startup now) and
  `pdrop=` (paced-mode mailbox drops: with the servo locked, a parked
  frame is a missed latch and is dropped at flip completion instead of
  resubmitted a period late — the mechanism that keeps a single miss
  from becoming a one-vsync-late chain; steady state ~0.2/s at
  `vsync_lead_ms` 6). A `pend` increment in servo mode therefore costs
  one dropped frame in the common case (two when the mailbox was already
  occupied — the replaced parked frame counts in `--fps-log`'s `repl=`,
  not on this line), never a chain. `pend`, `pdrop`, the regulator
  line's `replaced=`, and `--fps-log`'s `repl=` together are the full
  drop accounting. Since 2026-09-02 the line also carries the
  sequential-slot chain accounting — `chained=` (cumulative frames that
  took natural+1 vblank because a predecessor held their natural slot),
  `chain=` (current consecutive run), `chain_max=` (longest run),
  `chains=` (runs started — chains/s × mean length = chained/s) and
  `cuts=` (runs cut by `display.chain_budget`: the frame that would extend
  a run past the budget takes its natural slot and the held occupant is
  displaced — one dropped frame, counted in `replaced=` too).
  These are the OTHER kind of one-vsync-late chain, the one the servo
  itself creates: one collision (two decodes inside one vblank window)
  shifts every following frame a period late until an arrival gap wider
  than a period frees a slot. The extra period lands in the `lat:` line's
  `reg` segment, so read `chained/held` per second next to `reg` p50.
  Bench 2026-09-02 (mcs5 park, agg6+fb6, lead 6, 196 s): 38.9 % of held
  frames chained, per-second fraction 7–71 %, longest run 52 frames,
  `hold_ema` 13.1 ms with no chain running vs 18.1 ms with one — about
  6.5 ms of mean regulator hold (0.39 × 16.7) that the vblank lock does
  not need. Collisions come from arrival jitter (AU completion gaps 8–27
  ms, ~10 % under 10 ms), not from pair bunching — the AU ring shows
  frames completing one per period, not in pairs.
  **`display.chain_budget` A/B, same session, 150 s arms, budget 0 →
  6 → 0 → 3, config-only restarts** (`lat:` p50 means over 1 Hz windows,
  bootstrap 95 % CI vs arm A; A′ reproduced A within noise, e2e −0.2
  [−1.2, +0.8]):

  | budget | chained | chains/s | cuts/s | hold_ema | reg p50 mean | e2e p50 mean | reg p99 | lat n |
  |---|---|---|---|---|---|---|---|---|
  | 0 (A) | 39.7 % | 6.9 | 0 | 15.1 | 15.0 | 45.5 | 25.1 | 59.4 |
  | 6 (B) | 27.1 % | 6.4 | 0.56 | 13.1 | 12.3 (−2.7 [−3.5, −1.8]) | 43.0 (−2.4 [−3.3, −1.6]) | 24.1 | 58.8 |
  | 0 (A′) | 40.6 % | 6.9 | 0 | 15.4 | 15.1 | 45.3 | 25.0 | 59.4 |
  | 3 (C) | 19.6 % | 6.1 | 1.44 | 11.9 | 10.7 (−4.3 [−5.1, −3.5]) | 41.6 (−3.9 [−4.8, −3.0]) | 24.3 | 57.9 |

  Chains start ~7×/s with a mean length of 3.4 frames, so a budget
  only trims the long tail of runs: 6 buys 2.4 ms of median e2e for
  0.56 drops/s, 3 buys 3.9 ms for 1.44 drops/s, and budget 1 would be
  freshest-wins at ~7 drops/s. reg p99 is untouched by any budget (a
  chained frame plus a full phase margin is the p99 whatever the run
  length). present_jitter EMA rose 0.56 → 0.90 → 1.37 ms with the
  drops. Operator picked **3 as the shipped default** (2026-09-02); 0
  restores the unbounded behavior. The counters make either auditable.
- **Post-mortem when no consumer was listening → the 1 Hz stderr stats
  line** in `/tmp/maburgs.log` (maburd's in `/tmp/mabur.log`, maburplay's
  fps-log + respawn history in `/tmp/maburplay.log`; the burned-DVR stage
  times ride the fps-log as `burn_ct_ms=`/`burn_enc_ms=`, see
  `docs/colortrans.md`). It is
  numerically redundant with the sideport but persists on disk; the UDP
  feed is ephemeral. The MSP OSD is now rendered by maburplay itself, from
  the UDP snapshot feed maburgs emits (maburgs no longer draws pixels); the
  OSD startup line and blanking notices land in `/tmp/maburplay.log`
  alongside the fps-log. Since 2026-08-11 maburplay also drives the display
  at startup rather than at the first decoded frame: it modesets immediately
  with a splash image (`/usr/local/share/mabur/splash.bin`, raw XRGB8888,
  regenerate with `tools/gen_splash.py`) so the sink locks a mode before video
  exists, and it retries display acquisition once a second while none is
  connected — a display plugged in or powered on after the player started is
  picked up without a restart, which it never was before. That only holds if
  a display was NEVER acquired: one that disconnects after a successful
  acquire is still unrecoverable, deliberately (a stated non-goal — KMS
  retains CRTC state and a replug normally re-lights it on its own). Neither
  has a config key. The splash shows from process start, or from a late
  display acquire only when no frame has been decoded yet, until the first
  decoded frame — deliberately, because the image is an aerial photo that
  would read as a live feed if it ever appeared mid-flight, so a mid-flight
  replug comes up on video rather than on the photo. Two log lines cover a
  no-display episode (`no display at startup -- retrying`, `display acquired
  after N.N s`); the per-attempt DRM failures are silenced on purpose, since
  /tmp is tmpfs.
- **The GS link-status OSD on the screen is a sideport consumer, not a
  separate instrument.** maburplay draws it from the SAME datagram
  `maburtop` reads: `stats.out` in `/etc/maburgs.toml` is a list, and the
  bench GS fans out to `:8300` (maburtop / ad-hoc capture) and `:8302`
  (maburplay's `osd.gs.port`). So the screen and the recorder cannot
  disagree — if the OSD shows something surprising, the answer is in that
  jsonl, and `flightreport.py` will say the same thing with more
  precision. Config is
  `osd.gs` in `/etc/maburplay.toml` (`enable` default false, `stale_ms`
  dims every link-derived field after silence; fps/jitter/bitrate/REC are
  player-measured and never dim). `osd.gs.style` picks the layout, and
  **the shipped default is `"compact"`**: two plain-text rows along the
  bottom edge, radio above and picture below,

  ```
                                              ● REC 12:47   <- top right
  ...
  ch:149 mcs:5 air:62% rssi:-70/-72 snr:22/20 temp:41
  bitrate:8.1 res:1280x720 fps:60 jit:5.2 lat:45/78 loss:0.3/0.0
  ```

  — no meters, no bars, one type size for both rows: everything the
  four corner blocks show minus FEC, plus the channel, the decoded
  resolution, both latency percentiles and (since 2026-09-21) the drone
  SoC temperature from `drone.sys.soc_temp_c` (`temp:--` while
  unavailable). Two colour exceptions, both single-threshold because the
  palette has no third status colour: the `fps` cell turns caution while
  `drone.low_power` is set (`docs/link-adaptation.md`, "Low-power
  (disarmed) mode"), and `temp` turns caution from 70 °C. Text and layout
  are otherwise unchanged, and both tints drop while the sideport is
  stale. `"essential"` selects the older
  four-corner layout with the signal bars, the airtime meter and the status
  hues. Exactly one renders; there is no both, and an unrecognised value
  fails the config load rather than picking one. Three things about the bar
  are worth knowing before reading one on the bench: `ch` comes from the
  sideport's `link.channel` (the GS's own `radio.channel`, so it says what
  the RECEIVER is tuned to, not what the player believes); `lat` is
  p50/p99 and reads `--/--` while the anchor is cold, exactly as the
  essential rows do; the RECORDING indicator is byte-for-byte the essential
  overlay's (red dot, `REC`, mm:ss clock, `REC FAULT` in amber) because one
  aircraft should not have two recording indicators, and it is anchored
  **top-right**, alone — in a row it would have cost the bar type size and
  dragged that row off centre whenever the recorder was merely armed; and
  every item sits in a box sized for its WORST-CASE
  string, so the gaps between items are uneven and short values leave
  trailing space — that is what stops a row reflowing every time a figure
  gains a digit. The type size is likewise chosen once, for the four-card
  worst case, so a card dropping out never changes the font under the pilot.
  **The two rows exist for that type size and nothing else**: all eleven
  items on ONE line is 132 worst-case characters into 1856 px, which caps
  the type at 22 px on a 1080p panel and is too small to read on the GS
  screen. Split radio-above-picture-below the widest row is ~74 character
  advances and the same rule picks 38 px. Row 1 is the wider one, so moving
  an item between rows changes the size the whole bar renders at. The glyph atlas is
  `/usr/local/share/mabur/gs_osd.gfont`, committed and staged by
  `tools/build-arm64.sh` — if it is missing, maburplay logs the reason to
  `/tmp/maburplay.log` and runs with the MSP overlay only. Host-side you can
  see the actual pixels without hardware: `maburplay --gs-render` dumps a
  rendered frame (`--style compact|essential`, plus `--res WxH` and
  `--lat P50/P99` for the bar's player-measured fields; both styles are
  pixel-gated in `run_player_e2e.sh` PARTS D and E), `tests/test_gs_asset.cpp` gates the real asset's layout at
  720p/1080p/1440p/2160p, and `tools/bench/gs_overlay_bench.cpp` measures
  draw+quantize per update at 1080p and 2160p. Read that bench before
  changing anything on this path: it runs on the 2 ms pump loop, and the
  rule of thumb is that anything projecting past ~1.5 ms on the A55 is a
  defect. **The shipped code already reports 3.7 ms at 1080p and 9.9 ms at
  2160p for a full repaint, and that is accepted, not overlooked** — a full
  repaint is a startup/re-layout event, the quantize half of it is
  burned-DVR-only (raw or no-DVR mode pays the draw column alone: 1.3 ms and
  3.6 ms), and `ring.pump(2)` is a `poll()` ceiling over a slotted shm ring,
  so the cost lands as one-vsync-late presentation rather than a lost AU.
  What is NOT accepted is anything that puts a full repaint on a *cadence*:
  the burn restate after an MSP collision is scoped to the fields the
  collision actually hit for exactly that reason (`osd_compose.cpp`), and it
  used to cost 179,392 px per changed cell instead of ~13,000. Steady state
  is 0.13 ms.
- **Record button (GPIO).** `maburplay` can toggle the DVR from a button on
  the GS header: `input.rec` in `/etc/maburplay.toml` (`pin` is the header
  pin number, resolved at startup by matching the kernel's line names —
  the Radxa ZERO 3 names its header `PIN_7`…`PIN_40` across gpiochip1/3/4;
  `active_low`/`bias` default to a button between the pin and GND with the
  internal pull-up). Short press, 50 ms debounce, edge-triggered: each
  press-pair produces one file, in whichever `dvr.mode` is configured — raw
  mode waits for the next sync point (the next IDR — seconds on the GDR
  link, and maburplay has no IDR-request path) before the new file
  opens, so the OSD REC indicator visibly lags the press, while burned mode
  resumes at the next decoded frame. As of the 2026-09-28 web local-recording
  work, raw mode's sync point is decided by `common/`'s shared `RawDvr`
  (`mabur::RawDvr`, `common/raw_dvr.h`) rather than code local to the player
  — the same class the web GS's local recording uses — and that sync point
  is a complete IRAP (`au_is_irap()`) once VPS+SPS+PPS are known. From
  2026-09-28 to 2026-09-29 it was any AU carrying VPS+SPS+PPS — the GDR
  encoder's TRAIL_R refresh start, a P slice: those files open on a P
  slice, flag every refresh sync, and Apple's decoder refuses them (see
  `docs/web-gs.md` for the repair recipe). **Before 2026-09-28** the
  player's raw DVR synced on sid == 0 (BASE), which since the 2026-08-29
  4→2 stream collapse is every other AU, not the refresh — so a raw file
  recorded before that date marks every base frame as a sync point and its
  fragments run ~33 ms (one base+enh pair) instead of the refresh period.
  Files are `record-NNNN.mp4` under
  `dvr.dir`, indexed one past the highest `record-NNNN` already on the card
  — no timestamp, since the GS RTC is wrong at boot (same reasoning as the
  debug-log session directory's own `NNNN` index, above). The index
  therefore climbs across boots and
  never overwrites an earlier flight; date-stamped `record_<date>.mp4` files
  from before 2026-08-26 do not match the pattern, so they are ignored by
  the scan and keep their names. No `input` block means no button.
  `dvr.autostart`
  (renamed from `dvr.enabled` on 2026-08-11 — the old key now FAILS boot,
  by design) picks whether the player boots recording or armed. There is
  no config kill switch and no `--no-dvr` flag: `autostart: false` with no
  button is a player that never records. That also means a `--decode-only`
  measurement run has no CLI way to opt out of the DVR any more, so give it
  a config with `"autostart": false` — otherwise the hardware decode gate
  writes a raw file and charges SD-card I/O to the fps number. Startup logs the resolved mapping
  and every toggle logs START/STOP to `/tmp/maburplay.log`; the OSD REC
  field is the live indicator. GPIO failures (pin not found, line held by
  another consumer) are non-fatal and logged once — the player runs
  without the button.
- **The drone's encoder, up close → the localhost debug endpoint.** Since the
  2026-08-29 venc fold-in `maburd` serves three routes on
  `127.0.0.1:<venc.debug_port>` (shipped 8301), localhost-only, always on, no
  enable flag — a bind failure logs and disables itself rather than being
  fatal. It is reachable only from the drone, so `ssh root@<drone> 'wget -qO-
  http://127.0.0.1:8301/venc'`:
  - `GET /venc` → `{"req_bitrate_kbps", "ring_fill_pct", "full_drops",
    "frames"}`. `req_bitrate_kbps` is the REQUESTED rate — what RcAgent last
    successfully commanded — not a readback of what the encoder programmed;
    the key name says `req_` for that reason. `frames` advancing is the
    cheapest possible "is the camera alive" check, and the one that
    distinguishes a stalled encoder (fps flat, ring empty) from a starved link.
    There is deliberately no `qp`: this SDK has no encoder-QP readback
    (`h265Info.startQual` is 0 on every frame, no `GetChnStat` QP), and the
    key that briefly carried it on 2026-09-03 was removed the same night.
  - `GET /snapshot.jpg` → a JPEG straight off the encoder's snapshot channel
    at `venc.snapshot_quality`. Answers 503 on a build without the venc core,
    500 on a capture failure — the two are deliberately distinguishable.
  - `POST /venc/set?k=v`, whitelist `bitrate` / `qp_delta` / `roi_qp` /
    `max_ipprop` / `superframe_p_pct` (the last two are volatile encoder
    pokes, `docs/airtime-model.md` §3; `min_qp` was deleted 2026-09-03) /
    `sensor_mfps` (the genlock bench probe: the sensor's own frame rate in
    milli-fps, ±1% of the configured rate, `0` = back to it; prints a
    `> genlock:` line with the outcome; `docs/efficient-link-plan.md`
    step 2). **An
    override is not self-clearing.** RcAgent pushes a bitrate only when its
    computed value changes, so on a parked link whatever you set here holds
    until the next rung change or failsafe entry (measured: 20 s+ with no
    sign of reverting). That is what makes it a usable bench knob, and it is
    also how you wedge a flight if you forget to put it back — see
    `docs/link-adaptation.md`.

  It is a debug surface, not an instrument: nothing records it, and the same
  encoder numbers reach the GS sideport as `drone.enc.*` once per second.
  Reach for it when the GS cannot see the drone at all (`drone` is `null`, a
  half-finished deploy, no video) and you need to know whether the encoder is
  running.
- **Radio/PHY bring-up below mabur → devourer's own tools** (`rxdemo` with
  `DEVOURER_RX_ALLPATHS=1`, `doctor`, etc. — see
  `third_party/devourer/CLAUDE.md`). Use these when the question is about
  the chip/driver rather than the mabur link.
- **Bench harnesses** (`bench/linkbench`) drive special TX/RX modes for
  characterization; they are not monitoring tools. Note: the drone radio
  RX can wedge after a linkbench run — restart maburd.
- **TX-power wall calibration → `maburcal`** (`docs/calibration.md`), the
  GS-side operator CLI that replaced `bench/txagcbench` when it was
  deleted on 2026-09-10. `maburcal start` sweeps a unit's per-rate PA
  compression walls and applies them; `maburcal report <cal.log>`
  re-renders a saved run offline, which is where a run's raw per-cell
  tallies live now.

**"Wire clean" does NOT mean "no frame loss" — venc-ring vanish class,
detected since 2026-08-13.** Frames can vanish INSIDE the drone (between
the encoder and maburd's ring read — a separate waybeam process when this was
found, the in-process venc ring since the 2026-08-29 fold-in) and never get a
`frame_id`: the
wire sequence closes seamlessly over the hole, every FEC/loss counter reads
zero, and a vanished BASE frame silently smears the decoder until an IDR
(rally's natural 2 s GOP is the only healer on this build). Root cause is
CPU famine, not ring depth: above ~12 Mbps at the mcs5 bench op point the
2×A7 SoC starves maburd's hot thread (ring pinned full for 100s of ms),
so the honest knob is `encoder.bitrate_max_kbps` (`waybeam.bitrate_max_kbps`
before the fold-in renamed the section) — the bench runs 10000.
Full findings: `docs/findings/2026-08-12-venc-ring-vanish.md` (committed
with the detection port). The detection (pts-jump, EMA-period,
shed-immune) ships in maburd and exported as
`drone.enc.{vanished_base,vanished_enh,self_idr_refused}` on the sideport
until the 2026-09-30 telem diet removed them (Telem wire grew 61→67, then 67→70 for the venc ring stats below, 70→83
for link-rtt, 83→84 for `roi_qp` and back to 83 the same night when the
never-filled encoder `qp` byte was dropped — a
version-mismatched pair just drops T_TELEM on CRC, so telemetry reads
absent until both ends run the same build; video is unaffected); the 5 s
`frame_ring:` stderr line in `/tmp/mabur.log` still carries them.

**ROI QP, no encoder QP, and the congestion-shed bit (2026-09-03).**
`drone.enc.roi_qp` is RcAgent's ROI QP *override* as commanded (signed
delta, `encoder.roi_qp_low/normal`). Until this date the same value was
exported as `drone.enc.qp`, read 0 for entire flights, and the flight-0011
analysis mistook it for "rate control never moved"
(`docs/findings/2026-09-03-handover-venc-overshoot.md`). For a few hours that day
`drone.enc.qp` carried the encoder's `startQual` instead — which this
firmware never fills — so the key was deleted rather than shipped as a
permanent 0: **there is no encoder-QP readback on this SDK.** (`roi_qp`
itself left the wire in the 2026-09-30 telem diet.) Alongside,
`drone.congestion_shed` (Telem flags bit4) is true while
`RcAgent::run_congestion_guard` holds any shed level — the drone-local
TxQueue-pressure / USB-failure shed (`docs/link-adaptation.md`, "Drone
congestion shed") — distinct from `failsafe_shed` (rung 0 / lost link).
A shed enh layer is silence to the GS ladder, so this bit is the only way
to attribute an enh gap to congestion rather than RF, and the only way a
bench can count sheds at all. maburtop's system row renders the pair as
`shed FS|CONG|off` (`AIR` too until 2026-09-30); `flightreport.py`'s
DRONE TX PATH section counts the periods each held. The drone `stats:`
stderr line carries `enc_pk100=`, the peak 100 ms encoder byte rate
(kbit/s, decimal) inside that stats second — the burst the 1 Hz
`drone.enc.mbps` average (removed 2026-09-30) hid — and, since
2026-09-20, `lp=`/`armed=` (see "2026-09-20 (low-power mode)" below).

**2026-09-23 (pre-FEC loss is late, not lost; and pooled).** `link.pre_fec_loss`
— the OSD's LOSS row — is the ArrivalTracker's "booked missing at the
settle line" fraction with BOTH video layers pooled in one window (until
this date it was the base layer alone); `link.ctl.pre_fec_loss` stays the
base-only sample the ladder acted on, and the player reads it only as the
fallback for a null pooled window. Booked missing means: a seq
counts missing once a later seq arrives `link.arrival_guard_syms` ahead of
it, and a symbol heard after that counts `streams[*].arr_late` and is never
un-booked. The drone's parallel USB TX pool reorders bodies on air, so at
the old compile-time guard of 32 (one FEC window) ~1 % of every burst read
as loss on a bench where `streams[*].recovered` (the symbols FEC actually
had to rebuild) was 0.008 %. Bench sweep at rung 5: guard 64 → 0.67 %, 96 →
0.23 %, 128 → 0.09 %, 192 → 0.02 % OSD loss, repairs flat. Shipped default
192 (`gs/bundle/maburgs.default.toml`). To read real air loss use
`streams[*].recovered` + `abandoned` per second, or `fec.log`; to compare a
pre-2026-09-23 recording's `pre_fec_loss` against a new one, remember the
old number carries the artefact (`docs/data-provenance.md`).
- `GET /venc` → `{"req_bitrate_kbps", "ring_fill_pct", "full_drops",
  "frames", "sensor_mhz"}`. `req_bitrate_kbps` is the REQUESTED rate — what
  RcAgent last successfully commanded — not a readback of what the encoder
  programmed. `sensor_mhz` is the sensor's measured frame rate in milli-Hz
  from the boot-time trim (`docs/airtime-model.md` §3), 0 until its first
  1 s window closes; healthy is a few tens of mHz under `venc.fps`.
  `frames` advancing is the cheapest "is the camera alive" check, and the
  one that distinguishes a stalled encoder (fps flat, ring empty) from a
  starved link. There is no `qp`: this SDK has no encoder-QP readback
  (`h265Info.startQual` is 0 on every frame, no `GetChnStat` QP).
- `GET /snapshot.jpg` → a JPEG straight off the encoder's snapshot channel
  at `venc.snapshot_quality`. Answers 503 on a build without the venc core,
  500 on a capture failure — deliberately distinguishable.
- `POST /venc/set?k=v`, whitelist `bitrate` / `qp_delta` / `roi_qp` /
  `max_ipprop` / `superframe_p_pct` / `min_iqp` / `max_iqp` / `fps` /
  `ov_base_pct` / `ov_enh_pct` (`max_ipprop`/`superframe_p_pct` are
  volatile encoder pokes, `docs/airtime-model.md` §3; `ov_*_pct` is a
  per-layer FEC overhead override, `-1` clears, 0..300 %, and works without
  the venc core). Nothing is persisted. **A bitrate/ROI-QP override is
  bounded:** RcAgent re-asserts its computed values every
  `RcAgent::kReassertMs` (5000 ms) on top of the pushes at every rung
  transition, failsafe/LINKED entry and ROI threshold crossing, so an
  override lasts at most ~5 s on a parked link — a bench procedure must
  re-POST inside that window (`docs/link-adaptation.md`).

**2026-09-23 (carrier sense on).** `drone.radio.rx = {own, foreign,
crcfail}` is the drone's own RX-side view of the channel for the last
telemetry period — the altitude view `cards[].energy` cannot give: every
frame the drone's RX callback saw, split into CRC-clean RC from this GS
(`own`), CRC-clean not-ours (`foreign`, other 802.11 on the channel) and
CRC-failed (`crcfail`, preamble heard, payload undecodable). With carrier
sense ON, `foreign + crcfail` is what the drone's transmitter deferred to.
Software counts, not the chip's CCA/FA registers — that read stalls the
drone's TX pool (`docs/findings/2026-09-23-cca-on.md`). Per period, NOT cumulative:
the exporter repeats the last Telem's values on every record until the
next one, so sample once per `drone.tlm_seq` — `flightreport.py` does
(`DRONE RX` section, silent on older recordings); `maburtop` shows them on
the drone `radio` row. `docs/findings/2026-09-23-cca-on.md`.

**2026-09-26 (VTX recorder).** `drone.rec = {state, err}` is the drone's
onboard SD recorder status, taken from `Telem.rec_status` and sent on both
sideport outputs (8300 maburtop, 8302 player OSD). `state`: 0 off,
1 recording, 2 error. `err`: 0 none, 1 Disabled (`[record] enable =
false` or ch1 failed to bind), 2 NoSlot (no `/sys/class/mmc_host/mmc*`),
3 NoCard (no `/dev/mmcblk0p1`), 4 NotMounted (`record.dir` is not a
mountpoint in `/proc/mounts`), 5 LowSpace (below `record.min_free_mb`),
6 WriteError (a write or fsync failed, so the file was closed). An error
stays until the GS wish goes off, which returns the status to `{0, 0}`.
The next on retries; the drone never retries by itself. The value is repeated on every record until the next Telem.
maburtop shows `VREC` / `VREC!<OFF|NOSLOT|NOCARD|NOMNT|FULL|WRERR>` on
the drone line. The player turns the value into the REC field's VTX leg
(`docs/vtx-recorder.md`). Recordings made before this date have no key.
maburd's own stderr (`/tmp/mabur.log`): the 1 Hz `stats:` line (incl.
`enc_pk100=`, the peak 100 ms encoder byte rate in kbit/s inside that
second, `lp=`/`armed=` for low-power mode, `snr_mhz=`, the trimmed
sensor rate in milli-Hz (same value as `GET /venc`'s `sensor_mhz`), and
`snr_off=`, the phase-lock offset on the sensor in milli-fps (0 = the
trim's own rate); `rc: low_power ENTER/EXIT` lines mark transitions,
`[sensor] cal:` lines the trim's rounds and lock, `[sensor] steer:` lines
the phase lock's first offset, clamps and refusals) and the 5 s
`frame_ring:` line (below).

**2026-09-30 (telem diet, RC_VERSION 13).** `T_TELEM` shrank 98 → 48
bytes: every field only maburtop read is gone (the key list is in
`docs/data-provenance.md`). What stays under `drone.*`: `state`,
`tlm_seq`/`tlm_age_ms`, `failsafe_shed`, `congestion_shed`, `low_power`,
`rec`, `rcf.{age_ms,rx_pps}`, `enc.cmd_kbps`,
`txq_wait_ms`, `txq.{drops,drop_pps}`, `radio.{usb_fail,rx}`, `uplink`
(per drone antenna — the dead-antenna check), `sys.{soc_temp_c,cpu_pct}`.
maburtop's `DEAF` cell is now derived from `radio.rx` (own + foreign +
crcfail = 0) and its encoder-fps / sent→inj cross-check row is gone.
`flightreport.py` gained a DRONE TX PATH section (txq wait, txq drops,
USB fails, CPU, shed periods, once per `tlm_seq`).

**2026-10-06 (software NACK, RC_VERSION 15; `docs/fec-nack.md`).**
`link.nack{requests, repeats, syms_requested, tail_requests, filled, late_fill, wasted, dropped_deadline, suppressed, lead_skipped, fill_pps, fill_ms{p50,p90,max}, settle_ms, late_ms_max}`: the GS NackTracker, counters cumulative, `fill_*`/`late_ms_max` per export window; present only while `[link.nack] enable`. `lead_skipped` (2026-10-06 post-flight-0026): seqs whose request was withheld by `link.nack.min_lead_ms` because the answer could not land before the deadline. Added 2026-10-10 (`docs/fec-nack.md` "Changes on notsudogood/mabur"): `held_covered` (seqs skipped at least once because a repair that already arrived covers them), `held_burst` (seqs whose first request waited for the base burst to end), `urgent` (first requests sent inside a burst because the deadline was near), `on` (the current arm of the `[link.nack] ab_s` A/B; always true without it) and `ab_s`.
`drone.nack{rx, retx_syms, retx_refused}`: the drone's T_NACK answers from Telem, per Telem period (repeated until the next Telem; count once per `drone.tlm_seq`).

**2026-10-10 (H.265 row slices, slice salvage; `docs/slices.md`).**
`link.video` gains five keys from `FrameStream`'s per-AU `SliceAssembler`
result: `slice_salvaged` (AUs rebuilt this window), `slices_kept` /
`slices_filled` (slices that arrived complete vs. were skip-slice fills,
summed over those AUs), `slices_after_hole` (kept slices whose position
is past the first missing one), and `slice_fallback{no_params,
unsupported, islice, no_template, geometry}` (why a damaged split AU fell
back to the old truncated-prefix passthrough instead of salvaging).
maburtop shows `salv`; `flightreport.py` gained a SLICE SALVAGE section.

**2026-09-06 (air clock).** `drone.air_backlog_max_ms` was the per-window
max of the drone's modelled air backlog (`AirClock`, spec
2026-09-06; `docs/link-adaptation.md` "Drone air clock"),
`drone.air_shed_drops` the enh AUs its admission gate has dropped since
link-up, and `drone.air_shed` (Telem flags bit5) whether it dropped any
this window — the third shed tier, below FS and CONG in maburtop's
`shed` cell; the queue row carries the backlog as `air N ms`. With
`air_clock.shed_ms` 0 (the shipped default) the model runs observe-only:
backlog reported, nothing dropped. Per frame, the same backlog rides the
SBI body header (`air_ms`, ver 2) into the AU ring (SlotHdr v3, offset
52) and the AU log's 12th column (`# aulog 3`); `tools/bench/airdrain.py
--model` compares it against the player's measured air excess. The three
sideport keys and the `AIR` shed tier left with the 2026-09-30 telem diet;
the per-frame `air_ms` is the surviving (finer) record.

**2026-09-20 (low-power mode).** `drone.low_power` (Telem flags bit7) is
true while the low-power operating point is in force
(`docs/link-adaptation.md`, "Low-power (disarmed) mode"). maburtop shows
`LP` in TWO places — the summary SYS row, and the DRONE panel's SoC line
(the one carrying `soc`/`radio rx`/`shed`), where it is highlighted the
same way a shed is; the compact bar tints its fps cell caution while set
(see above), but only while the sideport is fresh, since the flag is
link-sourced while the fps number is player-measured. The drone `stats:` line carries `lp=`/`armed=` and the
`rc: low_power ENTER/EXIT` lines mark transitions.

From the venc fold-in (spec 2026-08-28) until the 2026-09-30 telem diet
the drone also reported the PRODUCER side of that ring, straight from
`venc_get_stats()`: `drone.enc.venc_ring_fill_pct` and
`drone.enc.venc_full_drops`, read against the CONSUMER-side
`drone.enc.ring_drops`; all three are gone from the wire (the consumer
side still prints on the `frame_ring:` stderr line).
`drone.enc.idr_gs` (Telem.idr_gs, RC_VERSION 12 until the 2026-09-30 telem
diet removed it) counted IDRs the
drone issued because a GS asked for one over the RCF `idr_epoch` byte. Only
the web GS asks (spec 2026-09-28), so with maburgs flying it stays 0.
maburtop shows it as `idr N` on the encoder row.
`self_idr_refused` counts base vanishes suppressed by the IDR-adjacency
guard — the self-IDR CONSUMER is deliberately not wired: on the parked
`idr-request` branch it amplified CPU overload into an IDR storm (rolling
smear, I-frame-inflated bitrate, `air_pct` low throughout) and needs its
queued redesign (kill switch, GOP-aware suppression, rate-based guard)
before it returns. `tools/bench/ringwatch.c` (branch `bench/loss-sim-v2`)
samples the ring live when attribution is needed.
