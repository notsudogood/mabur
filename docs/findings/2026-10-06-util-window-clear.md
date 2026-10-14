# Util decision windows cleared at an op change (flight 0026, 2026-10-06)

**Status:** shipped in gs/src/transition_edge.h (`s1_loss_cur`/`s3_loss_cur` `blank_until(now)`); a fade-arm bench A/B on cascade depth and re-promote time was the stated pre-flight gate; current mechanism → docs/link-adaptation.md "Demote inputs and transition handling".

Extracted from docs/link-adaptation.md during the 2026-10-11 docs tidy.

**2026-10-06 (flight 0026, first NACK flight): the util decision windows
are CLEARED at a commanded op change again** (`gs/src/transition_edge.h`,
`s1_loss_cur`/`s3_loss_cur`, `blank_until(now)` with no swallow). The
adaptive blank below keeps post-edge old-rung bookings out of them, but
nothing emptied the 500 ms of old-rung loss legitimately booked before the
edge, and in the fade regime (100 ms confirm, 150 ms
`min_between_changes_ms`) one real demote kept re-deciding on it every
150 ms: in all four 5→0 cascades traced, `u` collapsed to 0 in ONE tick
exactly 500 ms after the booking that started the cascade, two or three
rungs below where the fade stopped, with SNR already back at 14–17 dB and
the re-promote 1.8 s later; 17 of the flight's 82 cascade steps rode on a
booking at least two steps old. That loss already produced its demote; the
new rung is judged on its own entries only. Cost: a genuine continuing fade
steps ~250 ms/rung (confirm on fresh entries) instead of 150. Bench gate
before flight: a fade-arm A/B on cascade depth and re-promote time.
