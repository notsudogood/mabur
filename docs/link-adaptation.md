# Link adaptation — ladder, attribution, fade

How maburgs decides the operating point and how maburd acts on it. Covers
the measured-loss ladder, the probe-stream promote gate, transition
attribution, the fade regime and its predictive RF trigger, the drone-side
sheds, the RCF drain, and RcAgent's encoder verbs.

Before comparing two recordings from different dates, read
`docs/data-provenance.md`. To record one in the first place, see
`docs/observability.md`. Specs are local and gitignored, under
`docs/superpowers/specs/`: `2026-08-05-s3-probe-promote-design.md`,
`2026-08-15-pooled-rf-and-instant-s3-design.md`,
`2026-09-04-probe-stream-design.md` and
`2026-09-06-air-clock-enh-shed-design.md`.

## Same-rate fixed pairs

The video link has two UEP streams: BASE (sid 0) and ENH (sid 1, the
droppable SVC-T layer). Both ride the same scored rung (`ladder_from` in
`common/src/profile.cpp`). UEP is expressed only through FEC overhead.
That overhead is a fixed **per-rung config pair**,
`[[link.ladder]] overhead_base`/`overhead_enh`, carried in every RCF. The
operator rule `overhead_base >= overhead_enh` is enforced at config load.

A rung is `(bw, mcs)`. `[[link.ladder]] bw` is required on every rung
(`docs/bw40.md`).

`LadderController` (`gs/src/ladder_controller.h`) scores each sid against
its own budget:
- sid 0 (base): `budget_base()` / `budget_base_for(rung)`.
- sid 1 (enh) and the probe: `budget_enh_for(rung)`.

The drone applies the commanded pair to UEP as it arrives. There is no
runtime redistribution between the streams. Bench record of the deleted
`AirBalancer` design: `docs/findings/2026-08-29-airtime-balance-spike.md`.
Measurements behind the same-rate decision:
`docs/findings/2026-08-30-same-rate-uep.md`.

`run_bitrate_policy` is a pure function of the operating point: a held rung
commands a held bitrate. That includes a rung held *under probe*: the probe
stream's MCS is deliberately excluded from the formula
(`docs/airtime-model.md`), so a probe costs zero encoder writes and zero
IDRs.

The drone's `:8301` `ov_base_pct`/`ov_enh_pct` HTTP override is bench
tooling, not a production control. It is the only way applied overhead can
differ from commanded. Telem carries no applied-overhead echo, so the GS
cannot see an armed override. maburtop's `_ov_cmd_cell` shows the commanded
pair only.

Vocabulary: "s1" and "s3" in config keys, demote-input names and ctl-log
reason strings (`s3_settle_ms`, `s3_residual`, `s3_util`) mean sid 0 (BASE)
and sid 1 (ENH). The names were kept rather than renamed (see
`gs/src/ctl_log.h`'s ctllog 7 note). The enh layer always rides the op MCS.
The candidate-MCS canary is the separate probe stream (sid 5, below).

## RCF slotting

The GS holds every RCF and DISC keepalive send until the drone is between
bursts (`RcfSlotter`, `gs/src/rcf_slot.h`, `link.rcf_slot_hold_ms`). The
mechanism, its constants and its limits are documented once, in
`docs/tx-rx-timing.md` §3.2.

Two things matter for the ladder:
- **Control latency.** A commanded op change reaches the drone up to one AU
  period later than an unslotted send would.
- **Probe-arrival release.** While a probe is commanded, the slotter
  releases on the probe body's arrival, not on the AU completion. So the
  probe is the one PPDU the GS uplink blast can never hit.

Sideport: `link.rcf_slot.{au,probe,timeout,passthru,tail_ub_ms}`
(`docs/observability.md`).

## Probe stream — the promote gate

**What it is.** The drone trails every video AU with one extra body: SBI
stream id 5 (`kProbeStreamId`, `common/include/mabur/probe_wire.h`). This
applies to base and enh AUs alike (`probe_follows()` in
`drone/src/probe_source.h`), and never to a shed layer's AU, which never
went on air.
- **Rate.** That is 60 probes/s at 60 fps SVC-T, and the AU rate in
  general.
- **Size.** Each probe body is exactly one video-body length:
  `13 + bpb*(2 + kSwHeaderLen + symbol_size)`, which is 1405 B at the
  shipped 332/bpb 4 geometry. It is derived from the same `FecCfg` as
  video and never configured separately.
- **Profile.** It flies at the profile of rung
  `current + link.probe.rung_offset` (default 1). That profile is commanded
  fresh in every RCF as `Rcf::probe_profile`. `0xFF` (`kNoProbeProfile`)
  means no probe: either `link.probe.enable = false` or the link is already
  on the top rung.

Every sub-block repeats a 9-byte header: magic, seq, profile, `enh_fid`.
So a body with one surviving block is still attributed rather than booked
as `bpb` lost blocks. An FCS-failed probe PPDU still reaches `ProbeTrack`:
`parse_probe_body` salvages the CRC-clean sub-blocks exactly as the video
decoder does. Gating the probe sink on `crc_ok` would book every FCS-failing
PPDU as a full `bpb`-block loss and bias the gate toward Lossy.

**Scoring rule: union across cards, AU count for expected, never seq gaps.**
`ProbeTrack` (`gs/src/probe_track.h`, `on_au(sid, fid)`) books one
expectation of `bpb` blocks per video AU that begins. It does NOT book from
probe sequence numbers. A wholly-lost probe stream carries its `seq` inside
the lost bodies. Scoring from seq gaps would therefore read a 100 %-lost
interval as silence (NoInfo) instead of loss, which is exactly the case
this gate exists to catch.
- **Counters.** `expected_blocks`, `arrived_blocks` and `bodies_rx` are kept
  for the COMMANDED profile only. A body that finalizes carrying a
  different profile (the RCF-lag tail after a rung change) increments one
  monotonic `off_profile` counter. It is "not scored", not "lost".
- **Union.** `arrived_blocks` is the union (OR of survivor bitmaps) across
  GS cards, because that is what video gets through the multi-card FEC
  dedup.
- **No subtraction.** Every counter is non-decreasing. An off-profile body
  cancels its own AU's still-pending expectation. The AU is keyed by frame
  id, which is the id the probe carries as `enh_fid`; base and enh share
  one frame-id counter.
- **Finalize together.** A pending AU is matched to its body on first
  sight and finalizes in the same `tick()` as the body. So only a genuinely
  lost probe opens an `expected` gap.
- **Per card.** Per-card cumulative counters exist alongside the union,
  for "which card collapsed at range".

**Gate (spec §4.4).** Every ordinary promote condition still applies:
- `u < up_util` sustained for `link.clean_ms`;
- `hold_after_down_ms` and `min_between_changes_ms`;
- a next rung that exists and is not penalized.

The probe gate is consulted on top, whenever a probe is commanded:

| gate | action |
|---|---|
| Clean, streak ≥ `link.probe.clean_bodies` | promote now, probation applies, `++promotes_probed`, reason `promote_probed` |
| Clean, streak short | wait; the promote lands on the tick the streak reaches `clean_bodies` |
| Lossy | **hold**, no penalty; `++probe_holds` once per hold episode |
| NoInfo | **hold**, same counter: a commanded-but-absent probe is the blind promote this gate exists to prevent |

Plain reason `promote` happens only when no probe is commanded. Demotes
always win. There is no probe state machine to abort.

**Gate inputs.**
- **Loss.** `u_probe` is the probe stream's union loss over a 500 ms
  `S1LossWindow`, scored against the CANDIDATE rung's enh budget
  (`budget_enh_for(idx + 1)`), never the current rung's.
- **Threshold.** A sample is Clean at `u_probe <= link.probe.max_util`.
  When the key is absent, the threshold is `down_util`.
- **Sample floor.** A sample needs at least `link.probe.min_syms` expected
  symbols to count.
- **Silence.** With no usable sample for `link.probe.silence_ms`, the gate
  reads NoInfo.
- **Streak.** The streak is counted in expected probe BODIES:
  `LinkHealth::probe_bodies_total` (ProbeTrack union
  `expected_blocks / bpb`) feeds `ProbeGate::streak_bodies`, starting from
  the streak's first Clean sample. A body count fixes the gate's confidence
  regardless of fps or layer split. The gate is a zero-loss run test: a
  streak of n bodies passes with probability (1−p)^n.
- **Loss quantum.** One lost body over the 500 ms window reads as 0.1 at
  60 probes/s. The bundle's `max_util` 0.05 therefore means "zero loss
  allowed" at any frame rate. A value between 0.1 and 0.199 would allow
  one loss.
- **RungStore.** `RungStore::observe_probe()` labels each sample by the
  PROBED rung. At the default `rung_offset`, `link.rungs[].probe_u` at
  rung r therefore means loss measured at r against r's own enh budget.
  With `rung_offset > 1` the sample is scored against the budget of
  `r − rung_offset + 1` instead; that is the extra margin the knob buys.
- **Bench pin.** `link.probe.pin_mcs` (bench only) forces the probe MCS.

Shipped values (`gs/bundle/maburgs.default.toml`):

| key | value |
|---|---|
| `link.clean_ms` | 1500 |
| `link.probe.rung_offset` | 1 |
| `link.probe.clean_bodies` | 90 |
| `link.probe.max_util` | 0.05 |
| `link.probe.min_syms` | 16 |
| `link.probe.silence_ms` | 500 |

Promote latency is about 1.5 s plus RCF lag at 60 fps.

Probe air cost:
- One body per AU, so it is a larger share of air at the bottom rungs.
  `link.air_pct` sums streams 0/1 only and cannot see it
  (`docs/airtime-model.md` §6).
- The probe is one short PPDU while video flies as A-MPDU aggregates, so it
  underestimates per-aggregate loss. That bias is the remaining fast-fail
  lever: an SNR floor at the candidate rung, or `rung_offset 2`.

The gate does not drive demotes (spec §10). `flightreport.py`'s probe-lead
report (`docs/observability.md`) is the input a v2 threshold would be tuned
against.

Bench and flight records:
- `docs/findings/2026-09-04-probe-stream.md`
- `docs/findings/2026-09-05-probe-blanking-fix.md`
- `docs/findings/2026-09-05-probe-stream-flight.md` (why `max_util` must
  sit below one loss quantum)
- `docs/findings/2026-09-16-probe-per-au-bench.md` (probe per AU)

## Drone congestion shed

The GS ladder never consumes drone telemetry. `T_TELEM` is display-only,
and every demote input is measured on the GS from the video it receives.
A body the drone's TxQueue throws away before sending therefore looks
exactly like an RF loss, and the ladder would demote for it.

`RcAgent::run_congestion_guard` therefore sheds the enh layer when the
TxQueue is at or past **half its cap**:
- It is level-triggered, sampled on the agent tick, and reads
  `RadioHealth::txq_depth`/`txq_cap` (`kTxqShedNum/kTxqShedDen` = 1/2 in
  `drone/src/rc_agent.cpp`).
- It also fires on a USB-failure trigger.
- The shed is applied at the UEP encoder before anything is queued, so
  there is no frame id and no expected symbol. The GS sees enh silence,
  which `s3_usable()` treats as no information, not loss.
- Recovery is a 2 s clean window per shed level. The level climbs one step
  per tick (100 ms) while the queue stays half full, up to
  `link.shed_max_level` (default 3). Every level sheds the same thing, so
  the setting only sets the tail: 3 = up to 6 s at 30 fps after the queue
  empties, 1 = 2 s. Bench 2026-10-23 (ch 132): each failed 3/40 promote
  left 6 s of 30 fps behind it, 13.5 s when two overlapped. `rc: congestion
  shed ON/OFF` lines in the drone log mark the edges.
- Telem flags bit4 → sideport `drone.congestion_shed`; maburtop shows
  `CONG`.

`txq.drops` is a telemetry counter only: by the time it moves the damage
is done. Flight record of the cascade that motivated the shed:
`docs/findings/2026-09-03-handover-venc-overshoot.md`.

## Drone air clock

`drone/src/air_clock.h` models the drone's air backlog, which sits past
the TxQueue pop where the congestion shed cannot see it.

**Pricing.** Every body pushed to the TxQueue books
`bytes × 8 / (phy_rate(layer) × air_clock.efficiency_<bw>[mcs]) + air_clock.body_us`
on a virtual air clock.
- `efficiency_20`/`efficiency_40` are per-MCS tables of bench-measured
  delivered/nominal rate (`drone/src/air_rate.h`). `run_bitrate_policy`
  prices off the same tables (`docs/airtime-model.md` §7).
- The clock is priced at the APPLIED op's per-layer rate and re-priced on
  every AppliedOp (`apply_op_to_clock`). It drops to the new rate the
  instant a demote lands, and it is leaky at zero.

**Backlog and gate.** `backlog = free_at − now` is read at every frame's
arrival and stamped on the AU's bodies (SBI `air_ms`, carried into
`au.log`). When `air_clock.shed_ms` > 0 and the backlog is at or past it,
the enh AU is dropped in `FramePipeline` before a frame id exists. That
leaves no id gap, and base AUs are never dropped. A dropped AU never goes
on air, so no probe trails it.
- **Defaults.** `shed_ms` 0 (the code default) is observe-only. The bundle
  ships **`shed_ms = 25`**, so the gate is armed in production.
- **Budget pairing.** The gate is calibrated together with
  `encoder.airtime_budget`. With the budget too high, the model runs near
  100 % utilisation and the gate turns into a steady-state enh throttle.

**Shed priority.** The air gate runs before `drop_if_shed`
(`drone/src/frame_pipeline.cpp`). When the congestion shed and the air gate
are both active on sid 1, the drop is booked in `FramePipeline::air_dropped()`,
not `UepEncoder::dropped(1)`. `air_dropped()` is drone-local; the air-clock
shed left the sideport and Telem on 2026-09-30, and `au.log`'s `air_ms` is
how to see it now.

**Dry-run.** The dry-run replay path (`run_dry_run` in
`drone/src/main.cpp`) calls `apply_op_to_uep` only, never
`apply_op_to_clock`. Replayed bodies always carry `air_ms` 0 and the gate
never closes there.

Ladder-side, this is the same contract as the congestion shed: enh silence
is NoInfo to `s3_usable()` and the probe gate, never loss. It ORs with, and
never touches, `failsafe_shed_` / `shed_level_`. `efficiency` and `body_us`
are calibration knobs, fitted with `tools/bench/airdrain.py --model`.
Flight record and the budget/efficiency fit:
`docs/findings/2026-09-06-air-clock-flight.md`.

## Demote inputs and transition handling

`LadderController::update()` checks in this order (a spec contract):
1. **Starved.** No video for `starved_confirm_ms` → straight to rung 0.
2. **s1 residual.** Base post-FEC loss > 0 → instant demote.
3. **Fade predict.** See the next section.
4. **s3 residual.** Enh post-FEC loss > 0 while s3 is live → instant
   demote.
5. **s1 util.** `u = pre_fec_loss / budget_base()` over `down_util`, held
   for the confirm window.
6. **s3 util.** `u3` over `s3_down_util`, held for the confirm window.
7. **Promote.** Clean margin and the probe gate.

The residual demotes ignore `min_between_changes_ms`. During probation, a
util excess demotes at once (reason `probation`). A residual or util
demote from a rung on probation books a penalty; a fade demote does not. A sample with `sample_valid = false` decides
nothing and does not count as feedback; `feedback_timeout_ms` covers that
case.

**Inputs.**
- **Residual inputs** are post-FEC abandonment (`syms_abandoned`, via
  `gs/src/ladder_residual.cpp`): base only for s1, enh for s3. A symbol
  filled by a NACK retransmit still counts as abandoned, so the ladder sees
  the loss even when the video does not (`docs/fec-nack.md`).
- **Util inputs** are pre-FEC source-symbol loss booked at ARRIVAL by
  `SwDecoder`'s `mabur::ArrivalTracker`. Each source seq is booked once,
  when it crosses a settle line `link.arrival_guard_syms` (default 192)
  behind the newest seq heard. Expected comes from the sequence advance,
  arrived means heard. These feed 500 ms `S1LossWindow`s (`s1_loss_cur`,
  `s3_loss_cur`). Arrival booking keeps the denominator independent of
  decoder progress after a re-key, so a post-promote window cannot read a
  near-empty bucket as 100 % loss.
- **s3 liveness.** s3 is live only with at least `link.s3_min_syms`
  expected enh symbols in the window, and only after `s3_settle_ms`
  (default 300) has passed since the last transition. Enh silence (a shed,
  the air gate) is therefore no information, not loss.

**Transition attribution.** Every loss counter is split into current-rung
and pre-transition (stale) parts by a per-stream watermark in
symbol-sequence space. The RX PHY rate is the generation boundary, and the
mark is set by `UepDecoder::mark_transition`. All four demote inputs read
the current-only side, so a rung change's own FEC debris cannot fire a
follow-up demote. This is unconditional; there is no config switch.
- Sideport: `link.streams[].abandoned_stale`,
  `link.attrib.residual_cur`, and `link.attrib.close_ms` (the boundary
  close latency).
- On the ctl-log S line, `resid` is the total and `resid_cur` the
  attributed value.

**The edge (`gs/src/transition_edge.h`).** On every COMMANDED op change
(mcs, bw, or base overhead), `TransitionEdge::on_tick` does three things
(pinned by `tests/test_transition_edge.cpp`):
1. Marks the decoder's per-sid watermark.
2. **Settle-blanks both residual decision windows** (`s1_resid_cur`,
   `s3_resid_cur`) for `kResidSettleMs` = 150 ms. Abandonment books about
   80 ms late, and an A-MPDU burst can kill the old-rate tail above the
   watermark. Without the blank, loss that already produced a demote would
   re-fire the instant path on every tick. The blank both clears the window
   and swallows deltas booked during the settle. 150 ms is one 50 ms
   edge-detect tick, plus the horizon lag, plus margin.
3. **Clears both util decision windows** (`s1_loss_cur`, `s3_loss_cur`,
   `blank_until(now)`, no swallow). Old-rung loss booked before the edge has
   already produced its demote. Without the clear, the fade regime's 100 ms
   confirm would re-decide on it every `min_between_changes_ms`. As a
   result, the new rung is judged on its own entries only, and a genuine
   continuing fade steps about 250 ms per rung.

Only the decision inputs are blanked. The S-line `resid`/`resid_cur`, the
sideport and the RungStore still see the loss. The controller's own
`s3_settle_ms` additionally gates when s3 may be READ after any transition.

**Adaptive open-boundary blank.** While `SwDecoder`'s `wm_open_` is true,
`arr_stale_end()` returns `~0ull`. Every seq booked in that interval is
therefore stale, and `s1_loss_cur`/`s3_loss_cur` get no new entries until
the boundary closes on the first post-transition body. That normally takes
the drone's real switch latency.
- **Degraded case.** If `rx_mcs` is `kMcsUnknown`, no body is ever marked
  post-transition. The boundary then stays open until `kBoundaryExpiryMs`
  (1000 ms, `common/src/uep_decoder.cpp`).
- **What the ladder sees meanwhile.** The 500 ms util window empties and
  `s1_cur_sample.valid` goes false. `gs/src/link_health.cpp` defaults
  `health.pre_fec_loss` to 0.0 (promote-permissive), and `link.pre_fec_loss`
  reads null. The ladder sees `u` = 0 for as long as this lasts, so
  `clean_start` accrues through it.
- **What to watch.** `link.attrib.close_ms` shows how long the adaptive
  blank actually ran.

**Tuning caution.** `s3_settle_ms` and the 500 ms windows interact: a
transition's FEC re-key artifacts must not be able to satisfy an s3 confirm
right after the gate opens. Do not lower `s3_settle_ms` without a bench
A/B. `flightreport.py`'s "s3-settle-refire canary" and attribution-miss
canary (`attribution_misses()`) should both read about zero on any
recording.

History of the blanks and of attribution:
- `docs/findings/2026-08-14-ladder-attribution-fade-wave.md`
- `docs/findings/2026-09-02-ladder-settle-blank.md`
- `docs/findings/2026-09-05-arrival-loss.md`
- `docs/findings/2026-09-05-probe-stream-flight.md` §9
- `docs/findings/2026-10-06-util-window-clear.md`

## Fast fall on a deep partial fade (`link.fast_fall_pre`, off)

Only two paths jumped rungs: `starved` (no completed video for
`starved_confirm_ms`) and `timeout` (no link reports for
`feedback_timeout_ms`), both to rung 0 and both on total silence. A deep fade
that still lets some video through walks down one rung per loss-driven
demote, each at least `min_between_changes_ms` after the last. Bench
2026-10-23 (gs12 + d13, a hand over the drone's antennas): 8 -> 0 in nine
steps at 28-70 % pre-FEC loss, the picture breaking up on the way.

With `fast_fall_pre` > 0, a loss-driven demote (s1 or s3 residual, s1 or s3
util) whose pre-FEC loss is at least that lands on the highest rung below
whose base-layer goodput (PHY rate / (1 + `overhead_base`); VHT at 80 MHz,
HT below) fits what still gets through at the current rung, goodput x
(1 - loss), instead of the next rung down; rung 0 at the floor, never less
than one rung (`LadderController::fall_target`). s3 events use the deeper
of the s1 and s3 losses. Fade, probation, starved and timeout are
unchanged. The E line and the `ctl: rung a->b` line show the skip;
`CtlCounters::fast_falls` counts them.

Counterpart, not yet measured: the fit assumes loss scales with airtime,
while a lower MCS also gains SNR margin, so it tends to land lower than
needed, and the climb back runs at the normal promote pace (`clean_ms` plus
the probe per rung). A short blip that crosses the threshold costs more
picture than one rung would. A hand over the antennas is a sharper fade than
most flights.

**It never fired in flight.** Flights 0007 and 0008 (2026-10-29, gs15, the
concrete-block dips) counted 0 fast falls. Its input is the current rung's
pre-FEC window, which restarts at every rung change; in a fast slide each
rung lasts ~0.2 s, so it read 0-2 % at every step of the worst dip
(0007 487 s, 8 -> 3 in five steps over 0.8 s) while 12-68 % of the video
was lost after repair.

## Fast fall on residual loss (`link.fast_fall_resid`, off)

The same jump and the same goodput fit, keyed on the demote's residual
(post-FEC) loss: the current-rung s1 residual, or the larger of s1 and s3
for an s3 demote. Both keys can be on; the deeper target wins. A
residual-keyed skip adds `fast_fall pre=.. resid=.. skipped=N` to the
`ctl: rung` line.

What the replay says (0007/0008, the residual each step actually saw):
- At the steps of the five video freezes the residual was 1-46 %. At 20 %
  it would have skipped one rung once (0007 487 s: 6 -> 4 instead of
  6 -> 5 -> 4), saving one step, ~0.2 s. The fit is conservative: adjacent
  rungs differ by ~25-35 % in goodput, so 15-30 % residual still lands one
  rung down.
- A blunter rule, "every loss-driven step after the first in a slide skips
  one extra rung", would have cut the 3+-step slides by one or two steps
  (0.2-1 s) but landed one rung lower than needed in 7 of the 12
  multi-step slides, each a longer climb back. Not built.

## Climbing past a rung forgets its failures (`link.penalty_clear_on_climb`, off)

`fail_count_` (the k in `penalty_base_ms << (k - 1)`) resets only when a
rung survives its whole probation (`probation_ms`, 3000). A climb that
passes through a rung in under 3 s never survives it there, so an old
failure stays on the books: 0007 at 496 s penalized rung 4 at k = 2 (10 s)
for a failure 150 s earlier, which cost most of that dip's 24 s climb back.
With the switch on, every promote clears the count of the rung it leaves
and every rung below. Active penalties are not touched.

## Climb back after a dip (`[link.recover]`, off)

A loss-driven demote (residual, util, s3 residual, s3 util or fade) that
is not a failed promote opens an episode: the rung it left, and the RSSI baseline before
the dip (the fade EWMA's slow side, which falls at tau 20 s and so still
holds the pre-dip level). A second dip inside the episode keeps the higher
rung and restarts the clock. While the link is below that rung, the
episode is younger than `window_ms` (30000), and the fast RSSI is back
within `rssi_margin_db` (3) of the baseline, a promote needs `clean_ms`
(750) of clean loss and a probe streak of `clean_bodies` (45, 0.75 s at
60 fps) instead of `link.clean_ms` / `link.probe.clean_bodies`. Probation
and the probe gate still apply. Reaching the rung ends the episode; a
failed promote (anything that books a penalty) ends its fast pace.
Recovery-paced promotes add `recover clean_ms=.. bodies=.. to_rung=..` to
the `ctl: rung` line.

RSSI, not SNR: the GS card's SNR reads several dB higher on a narrower rung
for the same signal, so "SNR back" would trip on a 20 MHz rung while the
signal is still down.

Replay estimate (0007/0008): RSSI was back within 3 dB 0-1.8 s after each
big dip's floor, and the climbs back took 25, 15 and 12 s; at this pace
~5-8 s. Counterparts: that assumes every faster promote would have held,
which nothing here shows; 45 clean probes reject ~6.5 % loss at 95 %
against 90's ~3.3 %, so a marginal rung gets through more often; and a
second dip during the climb restarts the episode, not the measurement.

## Promote latency guard (`link.promote_lat_guard_ms`, off)

A promote to a rung the drone cannot drain shows on the GS first as
transit, then as loss. Bench 2026-10-23 (gs12 + d13, ch 132, 4/20 -> 3/40,
three tries): frames left the drone's queue on time and then sat in its
radio; median transit (the drone's queue wait + radio + air, above the
session's best) crossed 100 ms 0.7-1.2 s after each promote and kept
climbing to 200-300 ms. The residual demote came 1.1-2.2 s after the
promote, by which time the drone had raised its bitrate for the new rung,
and its queue overflowed at the demote (33, 111, 152 bodies dropped).

With `promote_lat_guard_ms` > 0, while the promoted rung is on probation
and at least `lat_guard_blank_ms` (300) after the change, a median transit
over the last 200 ms of AUs (`TransitWindow`, `LinkHealth::transit_ms`) at
or over it, sustained `lat_guard_confirm_ms` (100), sends the link back to
the rung it came from as a probation failure, penalty included. A window
with no AU closed breaks the run. The E line reads `latency`, the
`ctl: rung` line carries `transit=` on every change, and
`CtlCounters::demotes_latency` counts them.

Replayed at 100 ms on au.log (`tools/latguard_replay.py`: the same window,
blank and sustain), it fires 0.8, 1.35 and 0.8 s after the three bad
promotes, against residual demotes at 1.09, 2.20 and 1.62 s: 0.3-0.85 s
sooner, and on the first try still after the drone's bitrate raise. It stays
quiet on the other ~55 promotes in four saved sessions (this one, the 20 Oct
garage run with its 40 -> 80 width steps, s0014, sess8). The worst of those
was 75 ms for ~250 ms (3/20 -> 4/20 here, the encoder stepping up), so the
margin is 25 ms, from one bench. A width switch to 80 MHz without the boost
stalls the drone's TX for 70-130 ms; the blank covers the switch itself, not
a slow drain after it. Off by default.

## Fade regime and predictive trigger

Two independent pieces, both in `[link.fade]` (optional; code defaults
apply when the table is absent).

**Regime and cascade.** Every loss-driven demote opens a fade regime for
`link.fade.hold_ms` (2500). Loss-driven means residual, s3 residual, util,
s3 util, or a fade demote itself; probation, starved and timeout do not
count.
- The regime is always armed, so the exported state stays truthful.
- `link.fade.cascade` (default true) gates only its effect: while the
  regime is open, both util confirm windows drop from `link.confirm_ms`
  (250) to `link.fade.confirm_ms` (100). A real fade therefore steps down
  at fade speed.
- The instant residual paths, `s3_settle_ms` and `min_between_changes_ms`
  are not shortened.
- Sideport `link.ctl.fade.active` is the RAW regime state, not gated on
  `cascade`.

**Predictive trigger.** With `link.fade.predict` on (default true), the
trigger demotes one rung with reason `fade` when both deltas are over
their thresholds and stay there for `trigger_ms` (300):
- the RSSI delta reaches `rssi_db` (code default 8.0), and
- the SNR delta reaches `snr_db` (code default 4.0).

It also respects `min_between_changes_ms`.

**The thresholds are code defaults; the bundle ships `rssi_db = 14.0`,
`snr_db = 6.0`.**

The deltas are slow − fast of a dual-timescale EWMA in
`gs/src/ladder_controller.h` `FadeEwma`. These are structural constants,
not config:
- fast tau 300 ms;
- slow tau 2 s rising, 20 s falling.

So a multi-second fade cannot drag its own baseline down.

The thresholds act on a high-pass response, so they are not trip points:
- **Step response.** A step of depth D gives
  `delta = D·(e^−t/20000 − e^−t/300)`, peaking at 0.92·D at 1.28 s. The
  smallest step that fires is about 1.08× the threshold: 8 dB trips on
  about 8.7 dB.
- **Fading, not faded.** A fade that stops descending falls back under
  threshold as the baseline catches up.
- **Ramps.** On a steady ramp the response is slope-driven, about 19.7 dB
  of delta per dB/s. A ramp below about 0.45 dB/s never reaches an 8 dB
  threshold.
- **Reaction time.** The delta needs about 0.8–1.3 s to peak, so the
  fast tau, not `trigger_ms`, sets the reaction time.

Do not retune these without bench data.

Three properties to know before reading a recording:
1. **Latched.** There is exactly one predictive demote per fade EVENT. The
   latch releases only on an *observed* recovery, a tick where both deltas
   are measurably back under threshold. A NaN window does not release it.
   Further steps during a continuing fade are the cascade's job.
   `link.ctl.counters.demotes_fade` counts fade events, not rungs lost.
2. **`min_rung` is not a floor.** `link.fade.min_rung` (default 2) is the
   lowest rung the trigger fires FROM. The effective floor is
   `min_rung − 1`.
3. **Not rung evidence.** Fade demotes are RF evidence, not evidence about
   the rung. They never book a probation failure or a penalty, and never
   count in the RungStore's `exits_bad`.

**Edge rule (`link.fade.mode = "edge_per"`, off).** The default mode,
`"rssi_snr"`, is the rule above. `"edge_per"` replaces it with three
conditions, all held `trigger_ms`, on rungs at least `edge_min_bw` (80) MHz
wide:
- the SNR delta (the same `dsnr`) reaches `edge_dsnr_db` (6): it is
  falling;
- the SNR itself is at or under `edge_snr_db` (22): it is near the edge.
  A fall from 38 to 30 dB costs nothing; 26 to 20 does;
- the worse GS card's own packet loss, before the cards are combined,
  reaches `edge_card_loss_pct` (2): `LinkHealth::card_loss_max`, each
  card's hardware-sequence gaps over 200 ms. The combined stream stays
  whole while one card struggles, so this is the first real symptom.

RSSI is not read; its 14 dB threshold is what made `rssi_snr` late (0007
487 s: the SNR delta crossed 6 dB at 485.3 s, the RSSI delta reached 14 dB
at 487.7 s, after the loss). An SNR under 5 dB is treated as a broken
reading, not a fade: an 8812EU left in its 10 MHz MAC clock read ~0 for a
whole flight (devourer `src/jaguar3/CLAUDE.md`). Latched like the default
mode; the latch re-arms when the SNR delta is measurably back under
`edge_dsnr_db`. A fire adds `edge_per dsnr=.. snr=.. worst_card=..%` to the
`ctl: rung` line; the E line reads `fade` either way.

Replay, exactly as implemented (`edge_impl.py` over the ctl-log S lines and
the 200 ms per-card `loss_pct` in flight.jsonl; 0007, 9.6 min, and 0008 to
872 s, 4.8 min, before the SNR fault):

| | freezes warned (lead) | false fires/min |
|---|---|---|
| `rssi_snr` (latched) | 0 of 5 | 0.94 / 1.47 |
| `edge_per` | 3 of 5 (2.2, 0.1, 0.6 s) | 0.42 / 1.05 |

A freeze is a loss episode with >= 20 % residual within 2 s of its first
step; a false fire is one with no loss-driven step in the next 2.5 s.
Counterparts: five freezes is no evidence of a rate; the 2.2 s lead at
0007 459 s is probably the 455 s dip before it; the replay cannot show what
an earlier step would have changed; and most loss at the top rung comes with
no warning in any signal (rung 8 lost packets in 3-14 % of half-second
windows at 28-38 dB SNR), so no predictor catches those. Per-card loss
alone (worst card >= 10-20 %) warned of 1 of 5: it moves in the same 200 ms
window as the combined loss, so it is a tie-breaker here, not a trigger. A
card back from a scout dwell reads its whole absence as loss for a window;
scouting is off on the 40/80 MHz rungs by default (`scout_fast_rungs`).

**RF labels.** `rf_snr_db`, `rf_evm_db` and `rf_rssi_dbm` come from ONE
card, as a coherent snapshot. That card's per-card pooled sid0+sid1 track
supplies all three. The card is chosen by `select_label_card()`
(`gs/src/rf_labels.h`, `tests/test_rf_labels.cpp`): the SNR argmax over
cards whose pooled frame count advanced in the current feedback window.
With no fresh card, all three read NaN (`nan` in the ctl log), which is
inert for the trigger.

**Card-hop false fades.** The argmax does not stick. A front-end that
wedges for about 1 s hands the labels to a weaker sibling. RSSI then steps
by the card gap, and nothing re-baselines on a hop. If a `fade` demote has
no fade behind it, correlate the ctl log's `drssi`/`dsnr` step against
per-card `classes.*` in the sideport:
- a hop moves them by the card gap in one window;
- a real fade moves them along the transfer function above.

Observability:
- Sideport `link.ctl.fade` = {active, drssi, dsnr}, plus
  `counters.demotes_fade`. drssi and dsnr are `null` when NaN.
- Ctl-log S line `drssi dsnr`.
- `flightreport.py`'s episode analyzer (`find_episodes()`,
  `print_episode_report()`) gives first-demote reason per episode, fade
  lead times, false fades, and the attribution-miss canary.

## Drone RCF drain

On the drone, the agent loop wakes every `link.rc_drain_ms` (default 5,
bounds 1–1000) to drain queued RC frames. All per-tick housekeeping sits
behind a `TickGate` deadline of `link.tick_ms` (default 100, bounds
1–1000, `drone/src/tick_gate.h`): USB health polls, `RcAgent::tick()`, the
watchdog, and 1 Hz stats/telem. Op actuation latency is therefore
U(0, `rc_drain_ms`).

`rc_drain_ms > tick_ms` fails boot: it would silently retime every
per-tick job to the drain period. A `tick_ms` under 5 therefore needs an
explicit `rc_drain_ms`. `rc_drain_ms == tick_ms` reproduces a
single-cadence loop.

End-to-end op-change latency (`link.attrib.close_ms`) is dominated by
uplink delivery, not the drain. A lost RCF costs one `feedback_ms`.
Records:
- `docs/findings/2026-08-14-rcf-uplink-loss.md`
- `docs/findings/2026-08-14-handover-rc-drain-close-ms.md`
- uplink timing: `docs/tx-rx-timing.md`

## RcAgent owns the encoder

The encoder runs inside `maburd`. `RcAgent`'s three verbs
(`set_bitrate_kbps`, `set_roi_qp`, `request_idr`) are direct calls into
`venc_core` on the agent thread. The venc core is a pure mechanism with no
encoder-local policy, and there is no `venc.bitrate` config key. The
commanded rate is only ever the output of `run_bitrate_policy()`
(`drone/src/rc_agent.cpp`):

    kbps = 1000 × encoder.airtime_budget
           / [ 0.60·(1+ov_base)/rate_b + 0.40·(1+ov_enh)/rate_e ]

where:
- `rate_b`/`rate_e` are each layer's DELIVERED rate in Mb/s
  (`delivered_mbps()`, nominal × `air_clock.efficiency_<bw>[mcs]`);
- 0.60 is the fixed base byte share `kShareBase`;
- `ov_base`/`ov_enh` is the commanded pair, or the :8301 override when it
  is armed.

The result is clamped to `encoder.bitrate_min_kbps`/`bitrate_max_kbps`,
`min()`-capped by low-power mode, and rounded to 100. Bundle values are
budget 0.65 and 1000–24000 kbps. Code defaults are 0.60 and 2000–10000.
See `docs/airtime-model.md` §1 and §7.

- **Failed verbs are visible and retried.** Each verb returns real status,
  and RcAgent latches "what the encoder is running" only on success. One
  dropped MI call is therefore re-issued on the next policy run instead of
  wedging the rate for the rest of the flight.
- **RcAgent is the only IDR authority, and it paces.** Every producer goes
  through one pacer: GS-requested IDRs, the entering-LINKED heal, and the
  encoder's own chain-break signal.
  - The pacer enforces a 100 ms floor between any two IDRs, plus a 1 s
    holdoff between chain-break IDRs.
  - The chain-break path is an atomic flag set from the venc callback and
    consumed at the top of `tick()`. It is evaluated against the state as
    of tick entry, so a break arriving on the same tick as a missed
    feedback deadline still heals.
  - A refused chain-break request is deferred, not dropped: with no
    periodic refresh (`venc.gop_s` 0, the bundle default) there is no GOP
    to self-clear an unhealed break. It is served on the first tick past
    the floor and the holdoff; session edges clear it, the link-up IDR
    covers whatever broke while unlinked. Served count:
    `RcAgent::idr_chain_total()`.
  - The one exception is a **GS-requested** IDR (RCF `idr_epoch`). An epoch
    change raises a pending request that survives a pacer refusal and goes
    out on the first tick past the floor. maburgs's `RefreshRequester` and
    the web GS page both drive `idr_epoch`; each retries every 300 ms.
    Only the web page actually freezes waiting on it — maburplay/MPP
    conceals and keeps decoding instead (`docs/slices.md`,
    `docs/web-gs.md` "Known limits"). See `docs/fec-nack.md` "Demand
    refresh".
  - Session edges (DISC, unconfirmed move, FAILSAFE) clear the pending
    request. The entering-LINKED IDR on a return from FAILSAFE/RENDEZVOUS
    also clears it, so no redundant second IDR follows.
  - Served count: `RcAgent::idr_gs_total()`, drone-local.

### The bitrate policy pushes on CHANGE, plus a 5 s re-assert

`run_bitrate_policy()` only calls the encoder when its computed target
differs from the last value actually applied:
- decreases always go out immediately;
- non-decreases are throttled to 1 Hz;
- state transitions force.

On top of that, `RcAgent::tick()` **re-asserts** the current computed target
every `RcAgent::kReassertMs` = **5000 ms**, with `force=true`. A
`force=false` re-assert would be a no-op by construction.
- **Clock.** The clock runs from the last bitrate the encoder actually
  *accepted*. A link that keeps genuinely changing rung never adds a
  re-assert on top of its own pushes; a parked one gets one every 5 s.
- **States.** The re-assert is gated to LINKED and FAILSAFE. RENDEZVOUS is
  the pre-link state, with nothing to defend the value against.

This closes two holes:

- **A failed verb in FAILSAFE is retried.** Policy runs happen only on
  RCF, DISC and max-range entry, and FAILSAFE has no RCFs. A verb that
  failed on the failsafe entry itself would otherwise go unrepaired for up
  to `rendezvous_ms` (30 s) while the encoder floods an mcs0-sized pipe. A
  failed apply short-circuits the interval and retries on the next tick.
- **Overrides are bounded.** Anything that moves the encoder rate behind
  RcAgent's back would otherwise win until the ladder changed rung. That
  includes the debug endpoint's `POST /venc/set?bitrate=`. Such an
  override survives **at most one re-assert interval (5 s)**. Bench
  procedures that need an override to stick must re-POST inside that
  window.

### Encoder faults are process faults — there is no in-process rebuild

If the MI pipeline dies or has to be rebuilt (a resolution/sensor-mode
change, a wedged ISP), `maburd` must **exit and let its init wrapper
(`/etc/init.d/S00mabur`, repo file `bundle/S96mabur`) respawn it**. `venc_core_stop()` + `venc_core_start()` in the same PID can never
recover it.

Upstream implemented and bench-tested every in-process reset lever on this
SoC, and each one either wedges or comes back with a dead stream
(`ISP channel readiness timeout after 2000 ms` → `CmdLoadBinFile failed -1`
→ `not sync err` floods → no frames). The levers were:
- disabling userspace 3A before VPE destroy;
- `MI_SYS_Exit`/`MI_SYS_Init` in the same PID;
- closing the `/dev/mi_vif` and `/dev/mi_vpe` fds;
- `dlclose`/`dlopen` of the whole MI vendor library set.

The residual state is per-task VIF/VPE/ISP channel state in the kernel
driver, released only by `execv`. The negative result is recorded in the
sibling checkout,
`../waybeam_venc/documentation/STAR6E_SINGLE_PID_REINIT_FINDINGS.md`. Do
not re-attempt without new SigmaStar SDK/kernel insight. This is why a
fold-in bring-up failure is fatal by design: exit, respawn, cold bring-up
in about 14–17 s.

### Low-power (disarmed) mode

While the FC reports DISARMED, `RcAgent` runs the encoder at
`low_power.bitrate_kbps` / `low_power.fps`. The bundle ships the mode
**off** (`low_power.enable = false`) with 1 Mb/s and 30 fps ready for when
it is turned on; the code defaults are 1000 kbps and 15 fps. An ARMED
report returns full power.

The mode tracks the FC's **current** arm state both ways. It is not a
one-shot pre-arm latch, so a DISARMED report re-enters it however many
times the aircraft has armed before. Spike:
`docs/findings/2026-09-19-low-power-spike.md` (bitrate is the thermal
lever, fps is second-order, CPU clock is not a lever).

The second case this covers, after pre-arm thermal, is **recovery video**.
An aircraft that is down but still powered keeps answering MSP_STATUS with
DISARMED, so it drops back to the thin stream. At the bottom rung, the
thin stream is far likelier to reach the GS than full rate. `armed_latched()`
records whether this process ever saw the FC armed, which separates a
pre-flight drone from a downed one on the `stats:` line. It is
observability only and does not gate the mode.

- **Trigger.** The MSP thread polls `MSP_STATUS` (cmd 101) every 500 ms
  (`kArmPollMs`) on the OSD UART; the FC pushes only DisplayPort on its
  own. It hands BOXARM (flightModeFlags bit 0) to
  `RcAgent::note_arm_state()`, an atomic consumed on the tick.
- **Fail open.** `low_power_active_` = enabled ∧ the last arm report says
  DISARMED ∧ that report is fresher than `low_power.stale_ms` (2000).
  - Silence, a dead UART, or `msp.enable = false` all mean full power. A
    maburd respawn in flight boots at full power and follows the first
    reply.
  - **A crash that kills the FC or its UART gives full-rate video**,
    because the reports stop.
  - A spurious DISARMED (an MSP desync, a corrupt frame that passes the
    XOR checksum) throttles to the low-power rate in flight until the next
    poll corrects it. There is deliberately no debounce.
- **What changes.** `run_bitrate_policy()` computes a target fps next to
  the target bitrate, and the bitrate is additionally `min()`-capped.
  - fps goes out first, then the bitrate. The bitrate write's `SetChnAttr`
    IDR seeds the stream at the new rate, so the fps verb never requests
    one.
  - Both are latched on success only and retried on refusal, in every
    state including RENDEZVOUS. Both are restated by the 5 s re-assert.
- **The fps verb** (`venc_set_fps`, `drone/venc/venc_core.c`) is three
  steps:
  1. a VPE→VENC unbind/rebind at `sensor_fps:dst`, where dst is `fps`
     below the sensor's set rate (the decimation that makes the thin
     stream) and `sensor_fps` at or above it, so full rate never puts the
     SDK's frame-rate limiter in the path (`sensor_cal_bind_dst_fps`,
     `docs/airtime-model.md` §3);
  2. an RC `fpsNum` and GOP rewrite;
  3. a SuperFrame re-derive.

  The RC `fpsNum` alone drops nothing: it is only the CBR budget divisor.
  The verb logs `> FPS delivered N ... in M us`. `set_fps` also raises a
  flag the hot thread consumes to `FramePipeline::note_rate_change()`
  before scoring the next pts. That re-anchors the vanish period instead
  of booking every longer frame interval as a hole
  (`frame_pipeline_rate_change_reanchors_the_period_instead_of_booking_holes`).
- **What the ladder needs under the thin stream.**
  1. **The probe gate's sample floor must stay under the thin stream's
     window.** At `low_power.fps` × bpb 4 the gate's 500 ms window holds
     `fps × 2` expected probe symbols. Keep `link.probe.min_syms` (16)
     below `2 × low_power.fps`, or the gate reads NoInfo and holds every
     promote while disarmed. The 90-body clean streak simply takes longer
     at the lower AU rate. Pinned by
     `probe_gate_has_information_at_the_low_power_au_rate`.
  2. **The loss ratios are more sensitive.** A thin stream puts fewer
     symbols in each 500 ms window, so one lost aggregate reads as a
     larger `u`/`u3`. On a lossy link the low-power ladder can saw-tooth
     rather than hold. This is open: the fix would be a window measured in
     AUs, or a per-loss-event floor. 30 fps rather than 15 halves the
     effect.
  3. **The arm step is one write.** At ARM, `run_bitrate_policy(force=true)`
     raises the encoder from the cap to the current rung's full budget in
     a single write. Whether that provokes a demote is unmeasured. Read the
     rung at the `drone.low_power` false-edge (or `rc: low_power EXIT
     (armed)` in the drone log) and check for a demote within about 5 s.

  Bench record: `docs/findings/2026-09-20-low-power-ladder-bench.md`.
- **ROI interaction.** Entering low power trips `roi_low_`, because
  1000 kbps is under `encoder.roi_threshold_kbps` (3000). RcAgent therefore
  issues `set_roi_qp(-24)` on the way in and `set_roi_qp(0)` on the way
  out.
  - This is harmless only because the bundle's `[venc.roi] enabled = false`
    makes the ROI apply a no-op.
  - Re-enable ROI and the low-power transition inherits the
    rung-0-demote-IDR hazard: an IDR encoded at `roi_qp_low`.
  - Telem/maburtop read `roi -24` for the whole disarmed period while ROI
    does nothing.
- **Observability.**
  - Telem flags bit7 → sideport `drone.low_power`.
  - maburtop shows `LP` on the SYS row and the DRONE panel's SoC line.
  - The compact OSD's fps cell turns caution colour while the sideport is
    fresh.
  - stderr logs `rc: low_power ENTER fps=N cap=N kbps` and
    `rc: low_power EXIT (armed|stale)`.
  - The `stats:` line carries `lp=` and `armed=`.
  - Every recording opens with a low-power segment
    (`docs/data-provenance.md`, 2026-09-20).
