# Link pairing: hardware gate (2026-10-01)

**Status:** pairing shipped (RC_VERSION 14) and links on hardware; the timed recoveries below were the gate. Current reference → docs/link-pairing.md.

Extracted from docs/link-pairing.md during the 2026-10-11 docs tidy. "The spec's Restart cases" and "above" refer to that page's "Restart cases" section.

## Bench results

Filled in by the operator during the Task 9 Step 6 hardware gate — the
four timed recoveries, mirroring the spec's "Restart cases" above
(cold start / drone restart / GS restart / takeover), stopwatched from
`maburtop`.

| case | expected | measured |
|---|---|---|
| cold rendezvous, default keys on both ends | ~1 s | 2026-10-01: `session` on the first sideport sample after both daemons came up (not stopwatched); ladder 0→4 in ~8 s; ausniff 30 s 1815 AUs, 60.5 fps, 0 incomplete, 0 gaps, 0 resyncs |
| drone power cycle with GS running | <= 1.2 s | 2026-10-01: `session` 6.5 s after power-on, of which ~4 s is the drone's boot to maburd; `drone.auth_reject` set for exactly one Telem period at link-up, then 0 of 100 samples |
| `restart maburgs` with drone running | <= 1.2 s | 2026-10-01: `session` 0.2 s after the first beacon (≈6 s after the restart command, the rest is the GS radio bring-up); drone untouched (same PID) |
| maburgs stopped, phone page connected with the key loaded | <= 1.2 s | not run 2026-10-01 (no phone on the bench session) |

Wrong-key run, 2026-10-01 (key `2263` on the drone only, `restart maburd`):
the GS went BEACONING when the drone's video stopped, re-entered SESSION
on its held nonce when video returned, had its RCFs rejected
(`drone.auth_reject = true`, drone `rendezvous`) and showed
`link.state = key_mismatch` 1.2 s after the first rejected period, with
the `maburgs: link KEY MISMATCH` stderr line. Copying the same key to
the GS and `restart maburgs` recovered to `session` with `key_fp = 2263`
on both boot logs without touching the drone. Not run that day: the
stranger drone (the second VTX is on vendor firmware), the web spotter
row, and the cold-boot `vtx_nonce` check (nothing logs the nonce).

Also on the hardware gate: confirm the drone's first `vtx_nonce` differs
across cold boots (`RcAgent::fresh_vtx_nonce_` seeds from
`std::random_device` very early in boot on the 4.9 kernel). Nothing logs
the nonce today: capture the first DISC_ACK off the air on two
consecutive power cycles, or add a temporary print.
Also confirm the drone links at all — the host suite feeds exact-length
frames, and the FCS-tail bug fixed in `verify_control` was invisible to it.
