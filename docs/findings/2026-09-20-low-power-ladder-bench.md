# Low-power mode: what the ladder did under the thin stream (bench 2026-09-20)

**Status:** item 1 fixed (`link.probe.min_syms` 16), item 3 fixed (`note_rate_change` on a live fps change), item 2 mitigated by `low_power.fps` 30 (window design still open), the arm step still unmeasured; current mechanism → docs/link-adaptation.md "Low-power (disarmed) mode".

Extracted from docs/link-adaptation.md during the 2026-10-11 docs tidy.

- **What the ladder does under the thin stream — measured on the bench,
  2026-09-20 (GS session 0147, drone log).** Three things, in the order
  they were found:
  1. **The probe gate held every promote for the whole pre-arm period.**
     The per-AU probe books 15 AU/s × bpb 4 = 30 expected symbols in the
     gate's 500 ms window (S lines read `probe_n` 28-32), and
     `link.probe.min_syms` was 40 — sized against 60 fps = 120 per window.
     Under the floor every sample is unusable, the gate reads NoInfo, and
     NoInfo *holds* the promote (spec §4.4). The first gate edge of the
     session is `P 9614278 1 clean … 115446`: 115 s of NoInfo from boot,
     ending the instant the FC armed and the stream went to 60 fps, after
     which the ladder climbed 0→5 in 8 s. **Fixed by lowering the floor
     to 16** (four AUs' worth — the true no-traffic floor; code default,
     `gs/bundle`, and the GS's `/etc/maburgs.toml`). With it the promote
     needs the same 90-body clean streak, which takes ~6 s at 15
     bodies/s instead of 1.5 s (`probe_gate_has_information_at_the_low_
     power_au_rate` pins both halves). Keep `min_syms` under
     `low_power.fps × 2` if either moves.
  2. **The thin stream makes the loss ratios hypersensitive.** After a
     disarm from rung 5 the ladder cascaded 4→3→2→1→0 on `s3_util` in
     50 s (dwells 27/32/11/6 s) with the bench's usual ~2 FEC repair
     episodes/s (`fec.log`: 44 in that window, the GS-uplink self-blanking
     class). At 60 fps one lost aggregate is ~1-2 % of a 500 ms window; at
     15 fps × 1 Mb/s the enh window holds ~15-30 symbols, so the same
     single loss reads `u3` 0.22-0.29 against `s3_down_util 0.15` and
     demotes after `confirm_ms`. Expect the low-power ladder to saw-tooth
     on a lossy bench rather than hold a rung; NOT fixed — it is the
     window/threshold design meeting a 4-8x thinner stream, and the
     right answer (a window in AUs, or a per-loss-event floor) is a
     separate spec.
  3. **A live fps drop booked phantom vanishes.** `FramePipeline`'s vanish
     period is an EMA over "normal" deltas only, so after the 60→15 fps
     rebind every 66.7 ms step read as a 4x hole: `vanished` ran 3/4 →
     5578/5582 in four minutes at a flat 15 reads/s (3 per frame), and
     the Telem counters saturate. Link-inert (the self-IDR latch has no
     consumer), telemetry-corrupting. **Fixed:** `set_fps` raises a
     flag the hot thread consumes to `note_rate_change()` before scoring
     the next pts (`frame_pipeline_rate_change_reanchors_the_period_
     instead_of_booking_holes`).

  Follow-up the same night: `low_power.fps` raised **15 → 30** in the
  bundle (and on the drone). At 30 fps the windows hold twice the symbols,
  the single-loss quantum halves, and the bench ladder climbed to mcs 5
  and held instead of saw-toothing — item 2 mitigated by config, the
  windowing question itself still open.

  What is still unmeasured is the arm step itself: at ARM,
  `run_bitrate_policy(force=true)` raises the encoder from 1 Mb/s to the
  current rung's full budget in one write. On the bench the ladder was at
  rung 0 at arm (item 1), so the step was small; with item 1 fixed the
  rung at arm can be anything the thin stream converged on, never loaded
  at full rate. Read post-flight: the rung at the `drone.low_power`
  false-edge (or `rc: low_power EXIT (armed)` in the drone log) and
  whether a demote follows within ~5 s. If real, the fix is a ramp, or
  holding the ladder down until the first full-rate windows have scored.
