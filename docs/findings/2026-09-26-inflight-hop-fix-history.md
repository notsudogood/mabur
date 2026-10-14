# In-flight channel hop: how the fixes were found (2026-09-14 → 2026-10-04)

**Status:** historical — the bench/flight narratives (GS sessions 0207, 0232, the O4 split, the review probes), the old home fallback, the `hop.enable` shadow mode and the 2026-09-14 config shape. All fixes described here shipped; `home`, `split_after_ms`, `hop.enable` and `scout_when_disabled` no longer exist. What is current now → `docs/inflight-channel-hop.md` (drone side: `docs/channel-select.md`).

Extracted from docs/inflight-channel-hop.md during the 2026-10-11 docs tidy. Section numbers (§N) refer to that page.

## Wire: RC_VERSION 8 → 9 and the readback Telem fields (§1)

`RC_VERSION` 8 → 9 (`common/include/mabur/rc_proto.h`), a flag day like
every prior bump: an old/new pair rejects each other in both directions and
has no video, since `DISC_ACK` carries `CAP_FRAME_WIRE`. See
`docs/deploy.md` for the binary-swap sequence that applies to every
`RC_VERSION` bump.

`Telem` gained `channel` (u8, `RcAgent::channel()` at build) and `hop_epoch`
(u8, echo of the last pair applied) — observability only, nothing waited on
either, and nothing read them; both left the wire in the 2026-09-30 telem
diet (RC_VERSION 13). The drone's channel is whichever channel its video
arrives on, and maburd's own `maburd: retune A -> B (hop)` stderr line
records what it did during a split.

## Confirmation stamped at the source: the queued-body bug (§1)

   (`M ... hop_follow`). "Genuinely received on" is
   `RxBody::rx_channel` (`common/include/mabur/node.h`) — the channel the
   producing `RadioFrontend` was known to be tuned to at the instant it
   lifted the frame off the card, stamped there, on the producer thread,
   and carried with the body. It is **not** "where is this card tuned
   now": the core loop drains `BodyQueue` up to a full control tick behind
   the RX threads and `RadioFrontend::retune()` does not flush that queue,
   so bodies received on the OLD channel and still queued when the lead
   card's retune landed were being stamped with the target and confirming
   the hop ~10 ms after the order — before the order's RCF had even left
   the slotter (held to the next end-of-AU, up to ~17 ms) on a link that
   documents 30–50 % RCF uplink loss. The TX card then followed to a
   channel the drone had never been told about, the drone's
   `move_confirm_ms` never armed because it had never moved, and recovery
   was `SplitHome` at 5 s or the drone's own 30 s rendezvous. Because the
   stamp is taken at the source, no amount of queueing downstream can
   change it. `retune()` additionally blanks the stamp to 0 = unknown for
   the duration of the move, so a frame delivered across the retune is
   attributed to neither side of it; 0 never equals a hop target, so it
   simply fails to confirm.

## Confirm timeout and the 60 s split (§1, 2026-09-26)

4. No video within `hop.confirm_ms` (code default 500 ms; the bundle ships
   1000 since 2026-09-26 — hops to or from the 8822E spur pairs 149+153 /
   157+161 confirmed in 423–460 ms, and two 506/510 ms withdraws against
   an O4 split the link): `HopController`
   withdraws — epoch bumps again, `hop_ch()` reverts to the old channel,
   the lead card returns, the target is backed off (§5). A drone that had
   already moved sees the withdrawal RCF or times out on its own
   `move_confirm_ms`, which returns it to the channel it hopped FROM
   (`RcAgent::move_from_ch_`) and stays there (no home since 2026-10-03,
   `docs/channel-select.md`). Until 2026-09-26 the
   timeout went straight HOME, so with the hop target == home the drone
   stayed put while the GS sat on the old op — a 60 s split on the bench
   (GS on 112 in a `hold_cap`, drone on home 153). The GS side of the same
   split: the scout's home dwells trickled the drone's frames into
   `feed_video`, so the GS never counted the link lost and
   `split_after_ms` never engaged. Since 2026-09-26 only video received on
   op (or on the hop target while a hop is in flight) refreshes the
   rendezvous silence timer (`ChannelPlan::is_link_video`, keyed on the
   body's `rx_channel`; 0 = unknown still counts).

## Verdict units and session gating (§2, 2026-09-15 bench)

`HopVerdict::window()` runs every `hop.window_ms` (150 ms) while the link
is in `SESSION` and the boot scout owns no card (`hop_active()` in
`hop_burst_gate.h`; the engine is reset on the falling edge — run
unconditionally, the boot rendezvous read as `interfered` and the shadow
controller ordered every candidate before the boot pick had committed),
fed straight from a dedicated read block in `gs/src/main.cpp` (OFDM
FA/CCA via `read_energy_scout()`, foreign-frame and CRC-fail deltas from
the aggregator, RSSI/SNR EMAs converted from devourer raw units by
`rssi_raw_to_dbm()`/`snr_raw_to_db()` in `hop_verdict.h` — the first bench
run fed the raw values and `weak` could never trip) — **a card mid-dwell
that window is skipped**
(`VerdictCardIn::valid = false`). Link-level: the s1 (BASE) pre-FEC loss
window and the FEC recovered-symbol delta. The loss input is the verdict's
own 500 ms window (`s1_hop_loss` in `gs/src/main.cpp`, fed like `s1_loss`),
blanked when a hop lands (`HopAction::Confirm` + 150 ms settle,
`hop_verdict_loss_blank_until` in `gs/src/hop_blank.h`) so a verify is
judged on the channel it verifies; `s1_loss` itself, which feeds the
sideport/ctl-log/OSD gauge, is never blanked.

## recovered_min and starved (§2, 2026-09-26)

**`recovered_min` and `starved` (2026-09-26).** Two holes in `impaired`
found on the long-frame jam bench run. (1) On a clean channel the
recovered term's trailing mean sits near 0.2/window, so 1–5 recovered
symbols read "3× the reference" at 0 % loss; on 112, whose ambient FA
background reads `raised` (240–460/s), that made `interfered` (evidence
0x11) and hopped a link that had 0.0 % pre-FEC loss into a blocked
channel. Session 0232's scan.log had 529 windows impaired only via the
recovered term at <= 3 % loss: recovered 1: 251, 2: 154, 3: 42, 4: 36,
5: 18, 6–8: 10, >8: ~18 — a floor of 8 removes ~97 %. The loss term is
unchanged. (2) A drone starved by a jam (zero frames) read `healthy`: no
frames, no loss. `starved` now counts as impaired; with `blocked` it
classifies `interfered`, alone it falls through to `unknown` like any
unexplained impairment (never a trigger by itself). A starved window is
impaired, so it freezes the references and — like every impaired window —
pushes nothing into the trailing RSSI/recovered histories (`HopVerdict`
only pushes while `!frozen_`, and the freeze happens before the push in
the same window); pinned by `starved_windows_do_not_feed_the_trailing_references`.

**Near-starved (2026-09-26, Task 12 (e)).** A long-frame jam does not
starve the link to zero: session 0232's second jam window saw 5–30 own
frames/s (vs ~3200/s), so the zero-frames rule read "not starved", the
loss tracker had nothing to measure yet, and the window read `healthy` —
breaking the 2-of-3 persistence (loss only showed at +1.05 s, the order
at +1.36 s). `starved` now also fires when the AUs published this window
fall below `starved_frac` (0.25) of the trailing per-window AU mean. The
AU reference follows the recovered one exactly: a 5 s history pushed only
while `!frozen_`, captured at the freeze and used while frozen (so a long
collapse never drags its own baseline down), cleared by `reset()` with the
other snapshots. The AU history and reference (only those) are also
dropped by `HopVerdict::new_session()`, which `main.cpp` calls on the
`hop_active` edge where it re-primes the `au_seq`/recovered baselines —
the frame rate before an outage is no baseline for the session after it.
A mean under 2 AUs/window is no baseline and disables the
term; 60→30 fps low-power is a 0.5 drop, so keep `starved_frac` below it.

## blocked: min across cards (§2, final-review fix 2026-09-25)

`blocked` takes the MINIMUM foreign-busy reading across cards with a
reading, not any single card's (final-review fix, 2026-09-25). A weakly-
receiving diversity card's NHM busy counts our own frames too, but its
`own_air_pct` is reconstructed only from the frames it actually decoded —
so a card that hears little of our own video reads "foreign" close to its
own busy % even with no interferer present, and would false-block alone.
No card with a busy reading this window -> not blocked, same as before;
one-card GS -> the minimum over one card is that card, unchanged.

## Scout and burst: bugs caught before shipping (§3)

`InflightScout::dwell()` checks **both** retunes' results, not just the
first: if the retune to the candidate fails, or the observation completes
but the return retune to `back` itself fails, the card's true position is
uncertain — a stranded scout card means the aircraft is down to one radio
with no diversity — so `dwell()` sets `kFlagRetuneFailed` on the record and
**returns `false` without populating the `HopVisit`**, rather than booking
a visit gathered around an unknown card position. Recovery is not this
module's job: `gs/src/main.cpp`'s mechanical per-card retune loop (driven
off `plan.desired(card)` every tick, resynced from live hardware —
`cur_ch[card] = fe.channel()` — on every drained dwell) is what notices
and pulls a stranded card back. A failed dwell also does not leak a
phantom zero-score `HopVisit` into the ranker — an early bug (a
default-constructed `HopVisit{ch=0}` pushed unconditionally, which would
have ranked channel 0 as artificially best) was caught and fixed before
this shipped.

**Freshness burst.** `gs/src/main.cpp`: rather than act on a ranking that
may be up to `hop.rank_max_age_ms` old, every channel of the dwell set
except the op channel (home included) is swept once,
back to back, via `InflightScout::burst()` whenever the controller has
no hop in flight (`HopState::Idle` **or** `HopState::Hold`) and the
verdict's trigger is set — **on both card counts**, matching the spec's
§3 "the only card on a one-card GS since the link is already impaired."
The gate does not require `ht.best` to already hold a value (an earlier
build's gate did, which on a one-card GS is circular: the burst is the
only source of visits, so requiring a ranked candidate before running it
made the ranker permanently empty — fixed to run the sweep unconditionally
whenever hop-free-and-triggered). Two cards are unaffected either way,
since the periodic scout thread already keeps the ranker warm before any
burst runs.

**Ranking** (`gs/src/hop_ranker.{h,cpp}`, pure). Per candidate, over the
last `hop.rank_visits` (5) visits not older than `hop.rank_max_age_ms`
(10 000 ms): `score = Σ (fa + max(cca − own, 0) + 4·foreign)`. Fewer than
2 fresh visits = unranked, never chosen. Lowest score wins; deterministic
tiebreak is **boot-time pick, then config order** (spec
2026-10-03-auto-channel-set §5: "ties → boot-time pick, then config
order" — no home; `tests/test_hop_ranker.cpp`'s
`tie_break_falls_through_to_config_order`). An earlier build's tiebreak
was boot-time pick, then the configured home, then config order; there is
no home any more, so that middle term is gone — ranked-clean ties among
candidates fall straight to config order now, same as an unranked entry
always did. The **boot-time pick is published after construction**
(`HopRanker::set_boot_pick()`, called from `gs/src/main.cpp` at the first
DiscAck, next to the `K` scan.log pick line): the boot scan has not
resolved when the ranker is built, so until it is set the tiebreak falls
straight through to config order
(`no_boot_pick_falls_through_to_config_order`). The pick is published only
when the scan actually measured something (some channel reached
`min_rounds`); with no boot scan at all, or a drone that appeared before
any channel ranked, `boot_pick` stays 0 — never a real channel.

## Store blank: why it starts at the first interfered window (§4)

  Previously only (2) existed, so the ~300–450 ms of detection windows
  before the persistence gate tripped an order — including the demotes the
  spec explicitly expects, "a demote or two, each an IDR" — were written
  into the per-rung EWMA store against the interfered channel, which is
  exactly the pollution `blank_store` exists to prevent.

## Verify, roll back, rate limits: the bench narratives (§5)

  - **Only a verdict measured entirely after the hop landed can fail the
    verify.** `verifying_tick` compares `VerdictOut::t_start_ms` against
    its own `verify_start_` and ignores anything older. Without this the
    machine failed the verify of a perfectly good channel ~10 ms after
    landing on it: `gs/src/main.cpp` recomputes a verdict every
    `hop.window_ms` (150 ms) but ticks the controller every ~10 ms, so the
    cached `VerdictOut` immediately after a `Confirm` is always the one
    measured before or during the hop, on the old channel — `interfered`
    by construction, since that is why we hopped. It backed the
    just-landed channel off for 30 s, ordered the next candidate, and
    repeated: four orders in ~0.5 s, then `hold_cap`, with the backoff
    doubling 30 → 60 → 120 s, so after two interference events the feature
    had disabled itself for minutes having blacklisted the good channels.
    `t_start_ms` rather than `t_ms` because a window that merely *ends*
    after the confirm gathered most of its deltas on the old channel. The
    cost is one window of detection latency; the first eligible window
    lands ~2 × `window_ms` after the confirm, leaving five inside a
    1000 ms verify. Since 2026-09-24 the bound is the confirm **plus the
    150 ms landing settle** (`kHopSettleBlankMs`): the hop's own retune gap
    is still being repaired then, and those repairs read as `recovered`
    symbols — 40 failed a verify on 85 then 32 of them at 0 % loss, in a
    window starting 27 ms after landing. The first eligible window now
    lands ~3 × `window_ms` after the confirm, leaving four.

- **Confirm extension: undelivered orders are not failures** (2026-09-26,
  Task 12 (f)). In GS session 0232 the jammer sat next to the drone, so it
  jammed the uplink too: the order (carried by every RCF, ~20/s) never
  reached the drone, the GS withdrew at `confirm_ms` and backed the clean
  target (112) off as **failed**, the escape (which skips failed) had
  nowhere to go, and the link held 30 s on 144 at 95–99 % loss. Now, once
  `confirm_ms` has passed without a confirm, if `hop.confirm_extend_ms`
  (3000) > `confirm_ms` **and** the current verdict carries `kEvBlocked`
  **and** the order is younger than `confirm_extend_ms`, the controller
  stays `Ordered` — no action, the RCFs keep carrying the order — and logs
  one H `confirm_extend` (elapsed = time since the order) the first time.
  When it finally withdraws, an order that entered the extension logs
  `withdraw_undelivered` and backs the target off as **undelivered**;
  otherwise it is today's `withdraw` / failed. The extension is engaged
  only by a blocked verdict at the `confirm_ms` expiry, but once engaged it
  holds to `confirm_extend_ms` whatever later windows say — the jam lifting
  is exactly when the order lands, and withdrawing then would race the
  drone's retune into a `move_unconfirmed` split. A confirm during the
  extension proceeds to `Verifying` as usual. A session loss during the
  extension backs the target off as **failed** (the unchanged
  `on_session_lost` rule). `confirm_extend_ms <= confirm_ms` (e.g. 400
  with `confirm_ms` 500, or 0) never extends. Undelivered is
  in `backed_off()` (the normal ranker still skips it) but not in
  `backed_off_failed()`, so the escape may target it; a later
  `verify_pass` on it erases the back-off like any other.
  `on_session_lost` still records failed, and the one-card
  `OneCardRetune` path is unchanged.

- **The op verdict runs on op-channel cards only** (2026-09-26, found
  with the extension). `main.cpp`'s verdict loop skips a card whose tuned
  channel is not `plan.op()` exactly like a card mid-dwell
  (`verdict_card_usable`, `gs/src/hop_burst_gate.h`): no foreign/FA/CCA,
  busy or starved input. Before, a hop's lead card parked on the target
  during `Ordered` fed its clean-target readings into the op verdict — a
  latent bug that, through `blocked`'s min-across-cards rule, cleared
  `kEvBlocked` within ~3 windows of every order, which would have
  disabled the extension on a two-card GS. Consequences: during `Ordered`
  the op verdict runs on the TX card alone; on a one-card GS after
  `OneCardRetune` there is no valid card (`unknown`, no `kEvBlocked`), so
  the extension does not engage there and the order withdraws at
  `confirm_ms` as before.

- **The channel a trigger flees is backed off too** (same schedule as a
  failed target, `HopController::flee`). Before 2026-09-24 only failed
  targets were, so when the first target failed its verify the retry could
  go straight back to the channel just fled — and the in-flight ranker
  (event counts over 5 ms dwells) scores a long-frame jammer low, so it
  did (bench, GS session 0207: 144 → 128 → 144). A withdraw still returns
  to it: that path restores op, it does not consult the ranker. At the
  time this bug was found the fallback for "nothing ranked" was to order
  home, with its own backed-off/blocked checks
  (`HopController::home_available`, `HopTick::home_blocked`). Since
  2026-10-03 there is no home to fall back to at all
  (`docs/channel-select.md`): both members are gone, and "nothing ranked"
  simply holds (`hold_exhausted`, below) — see the Exhaustion bullet.

- **Never hop into a blocked channel** (2026-09-26). Every
  `ranker.best()` that feeds `HopTick::best` passes `require_unblocked`,
  so a channel whose fresh dwells average `>= radio.scan.blocked_pct` busy is never
  ordered — `ranking()` keeps the blocked tier ("least busy first among
  blocked") for display and logs only. Before, "all blocked → least busy
  wins" handed back 136, which every in-flight dwell had read 99.6–100 %
  busy (the jammer on 144 leaked into 132 and 136); its verify failed and
  the link sat ~33 s on it in `hold_exhausted`. When every option is
  blocked the controller now holds on the existing paths instead.

- **Back-off reasons and the escape** (2026-09-26). Each back-off entry
  records why: `fled` (`flee()`), `failed` (verify fail, withdraw,
  session lost) or `undelivered` (a withdraw after a confirm extension,
  see above). A later back-off of the same channel overwrites the
  reason and keeps doubling. `backed_off()` still returns both;
  `backed_off_failed()` only the failed ones. `main.cpp` fills
  `HopTick::escape` = `ranker.best(op, backed_off_failed(), require_unblocked)`
  — fled channels allowed, verify-failed and blocked never. The controller
  orders it (H kind `escape`) only when there is no `best`, the current
  verdict carries `kEvBlocked` (the channel the link is on is itself
  blocked) and `max_hops_per_min` allows it (`HopController::
  escape_allowed`; there is no home-unavailable condition to check any
  more, `docs/channel-select.md`): in
  `idle_tick` before `hold_exhausted` (after the existing cooldown check),
  and in `verifying_tick`'s verify-fail branch before its `verify_fail`
  hold (cooldown exempt, hop cap counted, like any retry — a full cap logs
  `hold_cap`). The escape target then runs the normal Ordered/Verifying
  flow. On the bench this is the missing move: 112 had carried video and
  was only backed off as fled, while the link sat on the blocked 136.

- **Session lost mid-hop** (`HopController::on_session_lost`, called on
  `hop_active`'s falling edge). The controller is only ticked in SESSION,
  and `ChannelPlan::tick` ignores link loss while a hop is in flight, so an
  order still waiting for its confirm when the session dropped used to
  freeze both: lead card on the target, trailing card on the old op, no
  `split_home`, the drone's DiscAcks ignored (`plan.hopping()`), while the
  drone had gone home on `move_confirm_ms` — link down until a GS restart
  (bench 2026-09-24, session 0207). Now an `Ordered` hop is withdrawn at
  that edge like a `confirm_ms` timeout (target backed off, epoch bumped,
  `session_lost` logged), which frees the plan's own link-loss path: since
  2026-10-03 that is `ChannelPlan::release_scout()` handing the spare card
  to the search scout after `search_after_ms`, not a move to home
  (`docs/channel-select.md` — there is no home to send a card to any
  more). A `Verifying` hop (already confirmed, so the plan's op has moved)
  just drops its stale verify.

- **Exhaustion.** All candidates backed off or unranked: hold
  (`hold_exhausted`, no retune — the ladder copes). Before 2026-10-03 the
  fallback was home if not already there; there is no home now, so this is
  the only outcome. Automatically retried once a shorter backoff expires
  and a new trigger fires (there is no timer of its own; the next
  `interfered` window re-evaluates `ranker.best()`).

- **A hold is a state, and only its EDGES are logged and counted.**
  `idle_tick()` runs from `Hold` as well as `Idle`, so a held controller
  with the trigger still latched re-enters the hold branch on every ~10 ms
  control tick. Logging per tick pushed an `H` line into `scan.log` **and**
  a line to stderr at ~100 Hz (~10 KB/s each) and turned `hop.holds` on
  the sideport into a meaningless six-digit ramp — worst on the Sept
  observe-only flights (`hop.enable = false`, deleted 2026-10-04), where
  the then-shadow FSM ran the same loop and filled the log with
  `would_hold_cap`. As built: entering logs one event naming the
  reason (`hold_cap`, `hold_exhausted`, or `verify_fail`) and bumps
  `holds()` once; re-entering while already held logs nothing; leaving
  logs one `hold_end` whose `elapsed_ms` is how long the episode lasted.
  `hop.holds` therefore counts hold **episodes**. A hold also now ends
  when the trigger clears — every other way out runs through `order()`,
  which needs a live trigger, so without that exit `hop.state` read
  `hold` for the rest of the flight after a single exhausted episode and
  no `hold_end` ever closed it.

- **Rate limits.** `hop.max_hops_per_min` (4) **includes verify-fail
  retries** — only `hop.cooldown_ms` (2000 ms, between a confirmed hop and
  the next fresh trigger) exempts them. This is a deliberate reading of
  the spec's own sentence structure ("including retries, then hold" vs.
  "verify-window retries exempt" applying only to the cooldown clause,
  not the cap) — the original implementation generalised the cooldown
  exemption to the cap too, letting a genuinely bad link retry unbounded
  (8 orders in under a second against a cap of 4, in one review probe);
  fixed and pinned by `verify_fail_retries_count_against_rate_cap`. Hitting
  the cap mid-verify-retry logs `hold_cap` and does not self-perpetuate —
  the 60 s trailing window (`hop_times_`) slides and the next trigger
  after it clears retries normally.

- **Kill switch (history).** Until 2026-10-04 `hop.enable = false` ran
  the whole machine in shadow — verdict, ranker, controller ticks,
  scan.log — with `tick()` zeroing the action and every event
  `would_`-prefixed (`would_order`, `would_withdraw`, …); the Sept flights
  flew that way. Deleted: the mode knob is `radio.channel` alone
  (`docs/channel-select.md` "Which knob drives which piece"), and
  `HopController` is mode-agnostic — it acts on what `ChannelCore` feeds it,
  which pinned is relocations only. Recordings from those flights carry
  `would_` rows and `flightreport.py` still prints them as SHADOW; no
  later build emits one.
- **Convergence.** The drone obeys only the newest `(hop_epoch, hop_ch)`
  pair and homes on `move_confirm_ms`; the GS waits only for video and
  withdraws on `confirm_ms`; the boot-time rendezvous machinery is
  untouched. Worst-case disagreement window is bounded by
  `move_confirm_ms + split_after_ms`, same as at boot.

## devourer (§6)

`GetRxEnergyScout()` (OFDM FA + CCA, delta-on-read like `GetRxEnergy`) and
a batched-control-transfer path (precedent: the InitWrite async write
queue) landed on `../devourer` ahead of this plan and are consumed
unmodified — no devourer change was part of this task's scope. The
spike's "~10 ms with the batched path, 26 ms without" planning figures are
in the design spec; the measured figure from the real GS path is one of
the numbers `docs/findings/2026-09-15-handover-inflight-hop-bench.md` still owes.

## Config as shipped 2026-09-14 (§7)

Since 2026-10-03 (`docs/channel-select.md`) `HopController` takes no home
parameter at all — its constructor is just `HopController(HopCfg cfg)`,
and `gs/src/hop_controller.h`'s own comment says so ("No home: this
feature has no fallback channel any more"). The same date adds a second
`H` (hop event) kind, `relocate` (named `boot_order` until 2026-10-04) —
the order that moves the link from where the drone was found to where it
should live (`ChannelPlan::want()`: the pin, or the boot pick — the boot
hop is a relocation), placed through this same controller
(`docs/channel-select.md` "Finding the drone"). `HopTick::relocate`: no
flee of the channel left, exempt from `cooldown_ms`, counted against
`max_hops_per_min`. No config key changed for either of these.

**`[hop]` has no on/off switch** (2026-10-04). The reactive hop runs in
auto mode only; `radio.channel = N` is static. `busy_dbm`/`blocked_pct`
are `radio.scan.busy_dbm`/`blocked_pct` since the same date: every
measurer reads them, so they left `[hop.verdict]`
(`docs/channel-select.md` "Which knob drives which piece"). The block
below is the shape that shipped 2026-09-14, kept for the recordings made
under it.

`gs/bundle/maburgs.default.toml`, `[hop]`/`[hop.verdict]` (drone config
unchanged — the drone has no hop config of its own, it just obeys
whatever `hop_ch`/`hop_epoch` arrive on the RCF):

```toml
[hop]
enable               = false   # first flights observe-only -- DELETED 2026-10-04
scout_when_disabled  = true    # DELETED 2026-10-04
window_ms            = 150
persist              = 2
dwell_observe_ms     = 5
dwell_period_ms      = 333
rank_visits          = 5
rank_max_age_ms      = 10000
confirm_ms           = 500
confirm_extend_ms    = 3000
verify_ms            = 1000
cooldown_ms          = 2000
max_hops_per_min     = 4
backoff_ms           = 30000
one_card_repeats     = 5

[hop.verdict]
loss_pct             = 3.0
recovered_x          = 3.0
weak_rssi_dbm        = -78
weak_snr_db          = 12
fading_drop_db       = 6
foreign_pps          = 50
fa_pps               = 100
busy_dbm             = -83     # now radio.scan.busy_dbm (2026-10-04)
blocked_pct          = 50      # now radio.scan.blocked_pct (2026-10-04)
recovered_min        = 8
starved_frac         = 0.25
```

`recovered_min` (added 2026-09-26, §2) is the floor under the recovered
term of `impaired`; 0 disables it. `starved_frac` (added 2026-09-26, §2)
is the AU-rate collapse that reads `starved`; 0 disables the AU term.
`[hop] confirm_extend_ms` (added 2026-09-26, §5) is how long an
unconfirmed order is kept while the op reads blocked; <= `confirm_ms`
(e.g. 0) disables the extension.

`busy_dbm`/`blocked_pct` (added 2026-09-25, spec
`docs/superpowers/specs/2026-09-25-nhm-airtime-design.md`) feed the
`blocked` evidence bit (§2) and are shared unchanged by both rankers
(§3); the spec's own draft carried a provisional `blocked_pct = 30`, the
hw spike (`docs/findings/2026-09-25-nhm-airtime-spike.md`) replaced it
with 50 — midway between the worst clean-channel error (~10 in a single
window) and a real interferer's reading (95–99).


`[hop]`/`[hop.verdict]` were wholly new sections with defaults for every
key, so an old config without them booted unchanged on the new binary
(`enable` then defaulted `false`) — but `RC_VERSION` still forced the binary
flag day described in §1, since the RCF/Telem shapes changed regardless of
config.

## Observability history (§8)

**`scan.log`, marker `scanlog 4`** (`gs/src/scan_log.h/.cpp`; formats
locked by `tests/test_scan_log.cpp`; bumped from `scanlog 3` by the NHM
airtime work, 2026-09-25 — see `docs/data-provenance.md` for the break).
The marker has since moved on to `scanlog 5` (2026-10-03, the channel set
replacing home + candidates — §7 above and `docs/channel-select.md`), then
`scanlog 6` (2026-10-05, the CPE510 relay's interference sweep/hop,
`docs/cpe510-relay.md`); every record kind this section documents is
unchanged by those bumps except `M` (loses the `split_home`/`reunite`
reasons; gains `link_found` 2026-10-04), `H` (gains `relocate`, below) and
`D` (gains a trailing `<rx|->` — a relay sweep entry's rx % of the
observe, `-` for a USB dwell or an invalid relay reading; `scanlog 6`).
The `A` (1 Hz in-flight energy) record and `radio.scan.energy_period_ms`
are **gone** — the verdict engine's window reads replace them, feeding
`cards[i].energy` on the sideport continuously in-session instead of once
a second (§below). Two new record kinds:

- **H** — one per `HopController` state transition or logged decision.
  `kind` is always a single snake_case token — `order`, `lead_confirm`,
  `one_card_retune`, `verify_pass`, `verify_fail`, `escape`, `withdraw`, `session_lost`, `hold_cap`,
  `hold_exhausted`, `hold_end`, `confirm_extend`, `withdraw_undelivered`,
  `relocate`
  (`escape`, added 2026-09-26 under the same
  `scanlog 4` marker, is an order placed to leave a blocked channel, §5;
  `confirm_extend` / `withdraw_undelivered`, added the same day, same
  marker, are the confirm extension's entry and its expiry, §5;
  `relocate`, added 2026-10-04 under `scanlog 5` (the pre-merge
  2026-10-03 bench builds logged it as `boot_order`), is the order that
  moves the link to `ChannelPlan::want()` — the pin, or the boot pick, the
  boot hop being one — placed through this same controller; a relocation
  is the one hop a pinned GS places — §7 above and
  `docs/channel-select.md` "Finding the drone"; the hold pair used to be the two-word C++ strings
  `"hold cap"`/`"hold exhausted"`, a space-delimited field containing the
  delimiter — fixed at the emitter rather than kept as a parser
  workaround, since scan.log is designed to outlive the code that wrote
  it). Before 2026-10-04 `hop.enable = false` prefixed every kind with
  `would_` (shadow mode, deleted); such rows exist only in older
  recordings. Note:
  there is no `trail_follow` H-kind — the trailing card following the lead
  is an `M` line (`hop_follow`), not an `H` line; the spec's sketch listed
  it as an H event. `hold_end` closes a hold episode opened by
  `hold_cap`/`hold_exhausted`/`verify_fail`, and its `elapsed_ms` is the
  episode's duration (§5). `tools/flightreport.py` treats it as a
  **terminal** kind, defensively: the controller does not currently emit a
  `hold_end` while a hop-attempt row is open (a hold entry closes the row
  first, and `verifying_tick`'s terminal `verify_fail` carries an
  *unbumped* epoch, so `build_hop_rows`' epoch-match branch reads it as
  the outcome rather than as a retry), but an unrecognised kind arriving
  with a row open falls through to `unterminated` — "the log ends
  mid-attempt" — which would misreport a flight that in fact ended in a
  hold.

**Player OSD:** the compact bar's `ch:` field appends `(h)` while a hop's
target is the live channel and that target is not `scan.pick`
(`GsSnapshot::hopped`, `gs/player/src/gs_snapshot.cpp`) — **not**
`hop.hops > 0`, which is a monotonic counter that never resets on
withdrawal or on hopping back to the pick and would stay lit forever after
the first confirmed hop. `scan.pick` is the channel the GS froze its boot
pick on (or the pin, in pinned mode) for the process lifetime — latched in
`gs/src/main.cpp`'s `frozen_pick` at the freeze, never the live `op`
(until 2026-10-04 it was published live as `plan.op()`, so after a hop it
followed the target and `(h)` never lit); `hopped` is
false while the pick is still open (`scan.pick` null, boot phase) — that
case is the boot-scan's own `(a)`/`moving` mark, not a reactive hop. `(h)`
and `(a)` share one suffix slot (worst_case() reserves exactly `(a)`'s
width) with `(h)` taking priority, since a mid-flight hop is the more
actionable of the two for the pilot. `(h)` reads false, not stale-true, on
an older maburgs missing `scan` or `hop`.

In a pre-2026-10-04 recording flown with `hop.enable = false` every row is
`would_`-prefixed and printed as
"SHADOW hop(s)" in a separate table rather than being silently folded into
"zero hops" — video never confirms while disabled, so a shadow row's
outcome is structurally almost always a `would_withdraw` timeout, which is
expected, not a bug. On a session with **zero real hops** (the expected
shape of every observe-only flight), the report instead prints:

- the verdict histogram (`healthy`/`fade`/`interfered`/`unknown` counts),
- an evidence-bit tally (`impaired`/`weak`/`fading`/`contended`/`raised`,
  counted over **all** windows, since a window can carry a bit without
  that bit driving the top-level verdict),
- per-card medians (not means — these are counter deltas with outliers)
  of `foreign`/`fa`/`rssi_dbm`/`snr_db` over non-healthy windows —

this was the whole point of the first (Sept, `hop.enable = false`) flights: the
spec's own Open Items call them "the calibration" for five threshold
defaults that are spike numbers, not measurements, and a verdict name
alone cannot say whether `foreign_pps` or `weak_rssi_dbm` was the one
that nearly tripped.
