# Raw DVR sync point: parameter-set AU → IRAP only (2026-09-29)

**Status:** Fixed on 2026-09-29. `RawDvr`'s sync point is now the first complete IRAP once VPS+SPS+PPS are known, and only IRAPs are flagged sync. The current behaviour is in `docs/web-gs.md` ("Record") and `docs/observability.md` (maburplay raw DVR). This page records the files written before the fix and how to repair them.

Extracted from docs/web-gs.md during the 2026-10-11 docs tidy.

## Files written before 2026-09-29

**Before 2026-09-29** the sync point was any AU carrying VPS+SPS+PPS. On the
GDR encoder that is the TRAIL_R refresh start every `gop_s`, which is a P
slice. Those files therefore begin on a P slice and flag every refresh start
as sync. ffmpeg decodes them, but Apple's decoder (QuickTime, VLC on macOS)
refuses the file. Their fragment seams could also step the timeline up to
~130 ms backwards.

The same window applies to `maburplay`'s raw DVR mode (shared `RawDvr`) from
2026-09-28 to 2026-09-29; see `docs/data-provenance.md` "2026-09-28 / 2026-09-29 — raw DVR sync points".

## Repair recipe

To repair one of those files:

1. Drop every fragment before the first IRAP.
2. Re-flag sync on IRAPs only.
3. Rewrite tfdt as the running sum of trun durations.

## The pre-fix expectation (former Known limit)

The web GS page carried this known limit while the bench was still pending.
The 2026-09-29 fix made it moot:

> **A local recording may not play in Chrome's own `<video>` element**
> (pending the live browser bench, plan Task 9 Step 2 — this is expected,
> not yet confirmed): the file starts on the drone's GDR parameter-set
> refresh, not a real IRAP. Browsers commonly refuse to start playback on a
> file whose first frame isn't an IRAP, and Chrome's `<video>` element may
> do the same here (unconfirmed pending the bench). `mpv`/`ffmpeg` decode a
> GDR-first stream without issue.
