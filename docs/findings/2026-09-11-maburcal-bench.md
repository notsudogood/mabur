# maburcal bench validation and relative walls (2026-09-11 → 2026-09-14)

**Status:** The kit works end to end; walls are stored relative to the chip's per-channel anchor, no-dip rows park at the +63 rail, the EVM recording was reverted, and the noisy-coarse-cell defect is still open. Current mechanism → `docs/calibration.md`.

Extracted from docs/calibration.md during the 2026-10-11 docs tidy.

## Cross-channel sweep, 2026-09-13 — why walls are stored relative

Measured 2026-09-13 (six runs, two interleaved passes over ch136 / 149 /
165 at one geometry, GS `/media/dvr/log/0074/cal.log`):

| rate | abs 136 (anchor 39) | abs 149 (53) | abs 165 (57) | abs spread | rel 136 | rel 149 | rel 165 | rel spread |
|---|---|---|---|---|---|---|---|---|
| mcs3 | 84 | 95 | 92.5 | 11 | 45 | 42 | 35.5 | 9.5 |
| mcs4 | 61 | 73 | 73 | 12 | 22 | 20 | 16 | 6 |
| mcs5 | 45 | 61 | 62 | 17 | 6 | 8 | 5 | 3 |
| mcs6 | 48 | 57 | 61 | 13 | 9 | 4 | 4 | 5 |
| mcs7 | 44 | 57 | 57 | 13 | 5 | 4 | 0 | 5 |

Same-channel repeatability is 0-9 indices (mcs3/4 are the noisy rows).
On the rows with a real PA wall the relative wall is flat to 3-5 indices
while the absolute one moves 13-17 with the anchor. The residual is a
~1 dB slope toward ch165 on mcs6/7, inside the default 1 dB
`wall_margin_db`; calibrate on home and fly the candidates.

## Bench validation, 2026-09-11 — what four real runs showed

The kit was deployed to the bench pair (`RC_VERSION` 7 both ends) and run
four times: three on channel 136, once on channel 149. Raw data is on the
GS at `/media/dvr/log/0057/cal.log` (+ `cal-run1-headerless.log`) and
`/media/dvr/log/0058/cal.log` (+ `cal.log.callog1` for the ch149 run).

**The mechanism works.** Every structural check passed: the run drives
itself end to end, `/etc/mabur.toml` is patched surgically (three lines,
every comment and unrelated key intact) with a byte-identical `.pre-cal`
backup, the verify pass reads 96-100% at every parked index, video resumes
with no restart (`ausniff` 60.3 fps / 0 gaps after every session), and
killing `maburgs` mid-sweep leaves the drone flying with a **bit-identical**
config — nothing written. Radio silence is corroborated statistically
rather than by capture: MCS 0-2 delivered 3840/3840 coarse frames with zero
loss across the first three runs, and GS uplink self-blanking would have
cost ~0.35% of them.

**Two measurement results are NOT yet trustworthy. Read the numbers for
MCS 0-2 as advisory, whatever flags they carry.**

### 2026-09-11: walls looked per-channel — resolved 2026-09-13

The 2026-09-11 runs found the anchor and the absolute walls moving with
the channel, and this page told you to calibrate on the channel you fly.
The controlled 2026-09-13 sweep (table at the top of this page) showed
the walls move *with the anchor*: relative to it they are flat within run
noise. Walls are now stored relative, which retires the rule. Note the
earlier claim that "a ch149 table flown on ch136 parks every rate ~1.5 dB
high" had the sign wrong — under the diff formula the chip adds the
ch136 anchor, so the old absolute table parked *low* there.

### Defect: one noisy coarse cell reroutes a no-dip row

MCS 0-2 never compress, so they are supposed to take the no-dip path.
In two runs of four, a *single* coarse cell in the mcs2 row read below
90% — 4 frames lost out of 20 — and that one cell ended the "first
contiguous ≥90% run", putting the row on the delivery path instead. It
reported 101 (ch136) and 111 (ch149) against a no-dip rail of 102, and
**both were written to the flight config**: mcs2 parked at 97 is roughly
6 dB above where run 1 put it, in the overdriving direction.

With 20 frames per coarse cell a 90% threshold has no noise margin, and a
run has 256 cells, so an outlier is likely *every* run. The `narrow` flag
fires on the resulting row and is reported — but flags never block, so the
number is applied anyway.

The rail change above shrank the damage **in the observed cases only**.
Both outliers happened to sit near the top of the sweep (idx 112), so the
rerouted row reported 101 against a rail of 102 — near-harmless. That is
luck, not a fix: an outlier at idx 40 would report a wall of 36 and park
that rate about 16 dB low. The failure mode is still live; it is the
`no_dip` flag going missing that tells you it happened.

### EVM was tried as a second instrument and the sweep frames carry none

EVM is the direct observable of PA compression — it degrades under drive
whether or not the frame still decodes, which is exactly what delivery
cannot see on the rows above. It arrives on the same `RxBody` as RSSI, so
it was recorded per cell (`callog 2`) and the bench re-run on
2026-09-11.

**It came back empty.** Every sweep frame, every rate, every index
reported rxevm `0x80` on both streams — the Jaguar3 type1 phy-status
page's "this stream was not measured" (`FrameParserJaguar3.h`), which
reaches `RxBody` as raw −128 and reads as an impossible −64 dB if taken
at face value. Ordinary video on the same link at the same moment
reported −14.5 dB, so the chip measures EVM fine. RSSI on those same
sweep frames is valid and tracks the index ramp cleanly, so the
phy-status page is present and parsed — it is the per-stream EVM field
specifically that is blank.

The code was **reverted**; `cal.log` is `callog 1` again with no EVM
columns. `maburcal` still accepts a `callog 2` file because bench runs in
that shape are on the DVR.

The plausible difference is the frames themselves: 64-byte single
(non-aggregated) probe-request frames, versus the large A-MPDU-aggregated
data frames video sends. **Untested.** If EVM is worth another attempt,
that is the experiment — a longer sweep payload, or sweep frames sent as
aggregated data frames — and it changes `cal_wire.h`, so it needs both
binaries redeployed together. Do not re-add the recording without
changing the frames first; it produced nothing but `-999` columns.

### Fixed: the RSSI knee is gone, no-dip rows park at the rail

The original rule for a row with no compression wall was the *RSSI
saturation knee* — the lowest index whose median RSSI was within 1 dB of
the row's peak. It was not reproducible: mcs0 read **72, 56, 84 and 56**
across four runs of one unit, and run 4 put mcs0 at 56 and mcs1 at 68 —
same PA, same modulation class, same run, 3 dB apart. The rule cannot do
better. The transfer curve creeps at ~0.2 dB/idx, so a 1 dB tolerance
band already spans ~5 indices before 1 dB of RSSI quantization moves it
further, and `peak` is a maximum over 32 quantized samples, which is both
upward-biased and jumpy. The design's claim that coarse resolution puts
the knee within ±2 indices "because the curve is flat there" does not
hold — near the tolerance boundary the curve is still climbing.

A no-dip row parks at the rail: `+63`, the top of the diff field.

Three things make this better than the knee, not merely more stable:

- **It is exact and identical every run.** No scatter to reason about.
- **It is provably inside measured-good territory.** The rail is the top
  cell of the sweep (the coarse grid runs −41..63 step 4 for exactly this
  reason), so a no-dip row just delivered ≥90% *at* the index it parks on.
  The knee was never validated by delivery anywhere.
- **It does not cost range.** On the measured curve the last real gain
  lands by idx ~80; everything above is flat to within quantization. The
  knee, firing early by construction, was giving up ~1-1.5 dB on
  precisely the rates the link falls back to when it is struggling.

One defect remains -- the noisy-cell reroute above. MCS 3-7 are the
measured product; a no-dip MCS 0-2 row is now a deterministic constant.

## Hardware acceptance, flag-day deploy (RC_VERSION 6 → 7)

### Two-device flag-day deploy

`RC_VERSION` bumped 6 → 7 for the two new frame types (`T_CAL_CMD`,
`T_CAL_RESULT`). Deploy config-before-binary on both devices as usual
(`docs/deploy.md`); no config keys move for this bump, so the flag day is
binary-only. **Between swapping `maburd` and swapping `maburgs` the pair
has no control link and no video** — indistinguishable from the
stale-caps restart deadlock. Do not restart either daemon trying to fix
it; finish the deploy. Confirm video resumes once both binaries match.

## Relative-walls deploy and A/B, 2026-09-14

### 2026-09-14 relative walls

Deployed commit `07d0df2` (branch `relative-walls`; supersedes the earlier `3d1bbe1` deploy — see the follow-up below). `RC_VERSION` 7 → 8,
plus `/etc/mabur.toml` swapped from `rate_walls_idx`/`legacy_wall_idx`/
`base_ref_idx` to `rate_walls_rel`/`legacy_wall_rel` — a config-key flag
day on top of the binary flag day, drone stopped/config+binary
swapped/started together as usual. Both device binaries rotated
(`maburd.pre-relwalls`, `maburgs.pre-relwalls`, `maburcal.pre-relwalls`);
md5s of the deployed files matched the local build outputs exactly.
Drone daemon PID (926) stayed constant across the whole session — no
respawn observed at any point.

`maburcal start` on ch136 (nonce 1651516276, margin 1.00 dB):

| rate | wall(rel) | park | verify | flags |
|---|---|---|---|---|
| mcs0 | 63 | 59 | 100% | no_dip |
| mcs1 | 63 | 59 | 100% | no_dip |
| mcs2 | 63 | 59 | 100% | no_dip |
| mcs3 | 41 | 37 | 100% | narrow,drift |
| mcs4 | 24 | 20 | 100% | card_disagree |
| mcs5 | 9 | 5 | 100% | drift |
| mcs6 | 9 | 5 | 100% | |
| mcs7 | 6 | 2 | 100% | drift |

`legacy_wall_rel = 63` (derived from mcs0). `written: /etc/mabur.toml`.
mcs5/6/7 landed at 9/9/6 — within ±5 of the 2026-09-13 reference
(6/9/5) — and mcs0-2 parked `no_dip` at the 63 rail, matching
expectations. Drone confirmed post-write: `rate_walls_rel = [63, 63,
63, 41, 24, 9, 9, 6]`, `legacy_wall_rel = 63`, no `base_ref` key,
`power_mode = "offset"`.

`ausniff` (15 s passes, `/dev/shm/mabur-au`), with the `incomplete`
count that the first write of this section omitted — **most passes
below are not fully clean**; only the two rows marked `{}` + `fid_gaps
0` are:

| point | aus | incomplete | fid_gaps | resyncs | fps |
|---|---|---|---|---|---|
| post-deploy, pass 1 | 907 | `{'1': 1}` | 0 | 0 | 60.5 |
| post-deploy, pass 2 | 909 | `{'1': 3}` | 0 | 0 | 60.6 |
| post-calibration, pass 1 | 895 | `{}` | 3 | 0 | 59.7 |
| post-calibration, pass 2 | 907 | `{'1': 1}` | 1 | 0 | 60.5 |
| post-calibration, pass 3 | 907 | `{'1': 2}` | 0 | 0 | 60.5 |
| home-scan (candidates default) | 908 | `{'1': 2}` | 0 | 0 | 60.6 |
| forced `candidates=[149]`, `home_margin=0`, pass 1 | 892 | `{'1': 2}` | 1 | 1 | 59.5 |
| forced `candidates=[149]`, `home_margin=0`, pass 2 | 909 | `{'1': 3}` | 0 | 0 | 60.6 |
| config restored, final | 908 | `{}` | 0 | 0 | 60.6 |

`fid_gaps` resolved to 0 on a follow-up pass every time it went
nonzero, in line with the documented first-pass-after-restart phantom
— but `incomplete` on stream `'1'` did not: only the post-calibration
pass 1 row and the final config-restored row show `{}`, and those two
still disagree with each other on `fid_gaps` (3 vs 0). The other seven
rows all carry a nonzero `incomplete['1']`; see the steady-state and
TX-power A/B passes below for the clean-bench characterization of that
residual.

Channel auto-select: with the shipped defaults
(`candidates=[120,149,165]`, `home_margin=20`) the scan committed to
home 136 (`K none 0` / `M all 136 136 commit`) — expected on a quiet
bench. Forcing `candidates=[149]`, `home_margin=0` and restarting three
times still produced `K none 0` / `M all 136 136 commit` every time:
136 was never worse than 149 by any margin at this bench's noise
floor. `pgrep -a maburd` on the drone during this window returned
`926 /usr/bin/maburd` unchanged on both a check taken mid-wait for the
first forced-candidate restart and a second taken mid-wait for the
third — no respawn. The GS config was restored to
`candidates=[120,149,165]`, `home_margin=20` and confirmed live via
grep; **the pair ran the whole session, and finished it, on channel
136**. Step 8 (ch149 cross-channel re-check) was skipped by controller
ruling.

### 2026-09-14 follow-up: steady-state and TX-power A/B

The passes above were taken while the operator was also exercising
channel-select restarts on the GS, so they are not a steady-state
baseline. With the pair left untouched, three clean passes at the
calibrated table, three at an old-power control (the pre-deploy
absolute parks re-expressed relative to the ch136 anchor 39 —
`rate_walls_rel = [59, 59, 59, 25, 26, 2, 5, -1]`, applied via a drone
restart), and two after restoring the calibrated table (md5
`91aac883f215c2d975d00db420f37bb8` on the drone):

| point | aus | incomplete | fid_gaps | resyncs | fps |
|---|---|---|---|---|---|
| calibrated `[63,63,63,41,24,9,9,6]`, pass 1 | 1205 | `{'1': 1}` | 1 | 0 | 60.3 |
| calibrated, pass 2 | 1205 | `{'1': 1}` | 0 | 0 | 60.3 |
| calibrated, pass 3 | 1206 | `{'1': 1}` | 0 | 0 | 60.3 |
| old-power control `[59,59,59,25,26,2,5,-1]`, pass 1 | 1205 | `{'1': 1}` | 0 | 0 | 60.3 |
| old-power control, pass 2 | 1205 | `{'1': 1}` | 1 | 0 | 60.3 |
| old-power control, pass 3 | 1204 | `{'1': 1}` | 1 | 0 | 60.2 |
| calibrated table restored, pass 1 | 1205 | `{}` | 0 | 0 | 60.3 |
| calibrated table restored, pass 2 | 1204 | `{'1': 2}` | 1 | 0 | 60.2 |

The incomplete-enhancement-AU residual (~0.08% of enh AUs) is the same
at the old and new TX-power tables, so it is bench background loss and
not attributable to relative walls.

### 2026-09-14 follow-up: live retune re-applies the anchor (07d0df2)

The final review found and fixed a real gap: devourer does not
re-derive the per-channel efuse anchor on a plain retune, so `maburd`
now calls `ReApplyTxPower()` after every `FastRetune` (commit
`07d0df2`); the coarse sweep is also now rel −41..63, 27 cells/row, 216
total. Redeployed both ends at `07d0df2` (maburd md5
`8ed0a5e90dadcc668e29ab2fd450d06f`, maburgs md5
`7c554ea0964ec08e4b241390381cf76f`); config unchanged (ch136-calibrated
rel table `[63, 63, 63, 41, 24, 9, 9, 6]`).

Cross-channel by reboot: both ends home 149 (anchor 53) with the
ch136-calibrated table:

```
aus=907 complete={'1': 450, '0': 456} incomplete={'1': 1} fid_gaps=1 resyncs=0 fps=60.5
```

Live retune: home 48 (the operator's house WiFi channel) on both ends,
GS candidates `[136]`, `home_margin 0`. GS `scan.log`:

```
K 18114850 136 16 48:24:-95 136:4:-95
M 18114850 all 48 136 commit
```

Drone `/tmp/mabur.log`:

```
maburd: retune 48 -> 136 (disc)
maburd: retune 48 -> 136: tx power re-applied (ok)
```

`ausniff` on 136 after the live move:

```
aus=907 complete={'0': 456, '1': 450} incomplete={'1': 1} fid_gaps=1 resyncs=0 fps=60.5
```

Why 48 was needed (note for the auto-channel-select scanner, not this
change): on this bench the scanner ranks home quieter from either side
— 136 as home busy 2 vs as candidate 88; 149 as home 12 vs as candidate
13 — so with home 136 or 149 it never left home even at
`home_margin 0`; a genuinely busy home (48) was required to provoke a
move.

Restored afterwards: both ends home 136, GS scan config
`candidates [120, 149, 165]`, `home_margin 20`:

```
aus=908 … incomplete={'1': 1} fid_gaps=0 fps=60.5
aus=906 … incomplete={'1': 1} fid_gaps=1 fps=60.5
```

## Addendum 2026-10-03: pairing broke the result frame

A result frame that arrived but failed its pairing tag is counted, not
logged: `cal_auth_rej=N` on the drone's `maburd tx_send:` stats line (it is
cumulative, and only prints while video is flowing, so compare a line from
before the run with one after). From the pairing merge (8f75a19,
2026-10-01) until the session-latch fix (2026-10-03) this was every run:
failsafe cleared the drone's session mid-sweep and the result never
verified -- see "The drone will be in `RENDEZVOUS`" in `docs/calibration.md`.
