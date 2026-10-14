# Ladder transition blanking: the s1/s3 residual settle blank (2026-09-02 / 2026-09-05)

**Status:** shipped — `TransitionEdge` (gs/src/transition_edge.h) settle-blanks both residual decision windows for `kResidSettleMs` 150 ms at every commanded op change; the util windows moved to arrival booking (docs/findings/2026-09-05-arrival-loss.md) and are cleared at the edge since 2026-10-06 (docs/findings/2026-10-06-util-window-clear.md); current mechanism → docs/link-adaptation.md "Demote inputs and transition handling".

Extracted from docs/link-adaptation.md during the 2026-10-11 docs tidy (its former "Tuning invariant" section).

Tuning invariant: the controller's s3 loss/residual
windows are 500 ms wide, while the post-transition blanking
(`s3_settle_ms`, default 300) and probe settle (`probe_settle_ms`, 150)
are shorter — so up to ~200 ms of pre-transition symbols remain in view
after blanking expires. ⚠ SUPERSEDED 2026-09-04 — see "Probe stream"
above: `probe_settle_ms` and the discrete probe state machine it
blanked are DELETED (the key now FAILS BOOT); the probe stream is
always-on and has no settle blackout of its own to tune. The
`s3_settle_ms` half of this paragraph (the ENH/base rung-transition
blanking) is UNCHANGED and still governs real rung changes. Shipped
defaults are safe (stale weight decays
fast against the 250/500 ms confirm windows), but do NOT lower
`s3_settle_ms`/`s3_residual_confirm_ms` toward their floors together: a
rung transition's FEC re-key artifacts could then satisfy the s3-residual
confirm and self-demote on every promote. ⚠ 2026-09-05: that self-demote
DID happen, from the other direction — `s3_settle_ms` only gated the
controller's READ; the 500 ms `s3_resid_cur` window behind it was never
cleared at the edge (the base window got its 150 ms `blank_until` on
2026-09-02, the enh one did not), so the abandonment horizon's ~80 ms
late booking of old-rung loss re-fired an `s3_residual` demote the tick
the 300 ms gate opened, on a 0 ms confirm. Flights 20/21: 13 of 14
s3_residual cascades double-stepped at exactly 300–310 ms and were
promoted straight back ~3 s later. Fixed by `gs/src/transition_edge.h`,
which settle-blanks only the two RESIDUAL decision windows (base and
enh; pinned by `tests/test_transition_edge.cpp`) at every op edge. The
util windows (pre-FEC, base and enh) briefly joined the same blank later
that day (51dd79e), for the same reason: with only the residual windows
blanked the bench still took an `s3_util` step at +400 ms on stale loss,
because every demote opens the fade regime and in-regime the util
confirm is `fade.confirm_ms`, not the 250 ms the "util needs no blank"
paragraph above assumed. That util blank was itself reverted the same
day (2026-09-05) when the two util inputs moved to arrival-time booking
— see "**Since 2026-09-05 (arrival tracker, ctllog 11)…**" further down
this file: the ArrivalTracker never books old-rung loss against the new
rung, so its denominator does not collapse after a re-key and no blank
is needed there any more; `TransitionEdge` today blanks only the two
residual windows. `flightreport.py` prints an
"s3-settle-refire canary" that must read ~0 on any recording after this.
See `docs/findings/2026-09-05-probe-stream-flight.md` §9. ⚠ SUPERSEDED 2026-08-15 — see
the pooled-RF note below: `s3_residual_confirm_ms` is REMOVED and FAILS
BOOT, so there is no longer a config knob to lower — the s3-residual
confirm window this paragraph warns about is now permanently at 0 ms
(its floor), unconditionally, for every deployment. That is deliberately
NOT the unsafe floors-together configuration described here: it is safe
only because attribution is exact rather than fast (the watermark is in
symbol-sequence space, so debris is absent from the input rather than
outrun by a shorter window) — see the 2026-08-15 note for the residual
risk that distinction does not cover.

**Since 2026-09-02 (second wave, same day as ctllog 9) the s1 residual
demote input carries a 150 ms post-transition settle blank**
(`S1LossWindow::blank_until`, wired at `main.cpp`'s sid-0 transition
edge-detect next to `mark_transition`; the constant is `kResidSettleMs`,
not config). Root cause, from flight ctl-0160: the ctllog-9 rewrite fed
block 4 from a 500 ms sliding window over the cumulative abandonment
counters and deleted the old measure's per-step `reset_window()`, so loss
correctly booked as current-rung stayed `> 0` across the demote it caused
and — block 4 being instant, threshold-free and exempt from
`min_between_changes_ms` — re-fired every 50 ms tick until rung 0. Every
`first=residual` episode in that flight ran 4-5 rungs to the floor, one
at 26-32 dB SNR; flightreport's attribution-miss canary read 14.
Attribution itself was innocent (it classifies at booking time; already-
booked current loss is never reclassified). The blank clears the window
at the transition AND swallows deltas booked during the settle — the
~80 ms abandonment-horizon lag books old-rung loss late, and an A-MPDU
burst can kill the in-flight old-rate tail ABOVE the watermark, which
books non-stale. 150 ms = one edge-detect tick + horizon lag + margin,
half of `s3_settle_ms`; the s1 UTIL path keeps its no-blank design
(amplitude decides it) and still carries a genuine sustained fade down at
in-regime speed. This is the same window-outlives-the-blank hazard the
s3 note above bounds with `s3_settle_ms` — s3's window is deliberately
NOT blanked here (its ~2-firing bound is documented, accepted behaviour).
Only the decision input is blanked: S-line `resid`/`resid_cur`, the
sideport and the RungStore observability all still see the loss.
Bench-validated 2026-09-02 (ctl-0165, loss-sim build): a 500 ms
eff=20/burst=4 s0 pulse = ONE residual demote + two measured-util steps
at ~155 ms, floor at rung 2, repromote — where ctl-0160's equivalent was
a 50 ms/rung drop to rung 0; sustained eff=15 still walks 5→0 (util,
150-320 ms/rung) and recovers; ausniff 60.0 fps / 0 gaps. Expect
`residual` E-line pairs closer than 150 ms never again; a demote storm
now shows `util` reasons and real per-rung `u`.
