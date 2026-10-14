# Deploy flag days, 2026-08 to 2026-10

**Status:** historical deploy runbooks, one per wire/config change; every change here is live on both ends now. The current deploy rules, pairing procedure and flag-day checklist are in docs/deploy.md.

Extracted from docs/deploy.md during the 2026-10-11 docs tidy. "Above", "below" and "this page" in the text refer to docs/deploy.md as it was then. Rollback file names (`*.pre-*`) are bench state at the time and may no longer exist on the devices.

## Earlier RC_VERSION bumps (from the page intro)

The 2026-08-12 constant-TX-power change (`docs/data-provenance.md`)
bumped `RC_VERSION` 1 -> 2, the first bump the protocol has ever had; the
2026-08-15 RCF shrink (dropping the write-only `ack_seq`, `score` and
`layer_delivery` fields, none of which `maburd` ever read) bumped it
2 -> 3. mabur owns the RC wire outright now: the goldens live in
`tests/test_rc.cpp` rather than mirrored from devourer's frozen
`tools/precoder/rc_proto.py` (pinned at version 1, no longer an oracle),
so further bumps are cheap and need no deprecation path — the deploy
window is their entire cost, and that cost is real.

## ssh and the respawn wrapper (from the page intro, 2026-08-28/29)

**Restarting the drone daemon over ssh: use `setsid`.** `S96mabur`'s respawn
loop is a background subshell of the shell that started it, so a plain
`ssh root@drone '/etc/init.d/S96mabur start'` gives you a `maburd` that dies
with the ssh session — the daemon comes up, ssh returns, and the link drops
seconds later for no visible reason. Detach it:
`setsid /etc/init.d/S96mabur start </dev/null >/dev/null 2>&1 &`. (Measured
during the 2026-08-28 Part A gate; it applies to any wrapper started from a
non-interactive ssh command, not just this one.) The mirror image also happens: on
2026-08-29 an `ssh root@drone '...; /etc/init.d/S96mabur stop; mv ...'`
one-liner died at the `stop` and never ran the rest — the daemons stopped,
the ssh command silently returned, and the swap that was supposed to follow
it had not happened. `S96mabur stop` kills a PID read from
`/var/run/mabur-loop.pid`, and after hours of uptime that PID can name
something else. **Never chain work after a `stop` in the same ssh command:
stop in one invocation, verify with `ps`, then swap in the next.**

## JSON to TOML cutover

All three configs moved from JSON to TOML on 2026-09-07. There is
deliberately no converter script: the operator reads each device's old
`.json` and hand-writes the new `.toml` against it, so a tuned value
never survives a mechanical transform that might silently drop or
reshape it.

**Per device, independent, no wire impact.** Config never crosses the
air — it only ever configures the local daemon — so a TOML drone against
a JSON GS is a perfectly fine pair mid-cutover; this is nothing like the
`RC_VERSION` flag days above. What is NOT independent is binary and
config on the SAME device: they swap **together**. An old binary cannot
read `.toml` (it doesn't know the format exists) and a new binary will
not read the old `.json` (parsing is TOML-only, no fallback), so the two
must change in the same window or that one device fails to boot.

1. **Drone.** `df -h /` first — the 5.8 MB rootfs fits at most two
   `maburd` binaries, so prune old `.pre-*` copies before staging a new
   one. Read the live config, hand-write the TOML against it, then swap
   both together:
   ```sh
   ssh root@<drone> 'cat /etc/mabur.json'                 # read the tuned values
   # hand-write /tmp/mabur.toml against that output
   ssh root@<drone> '/etc/init.d/S96mabur stop'
   scp -O /tmp/mabur.toml       root@<drone>:/etc/mabur.toml
   ssh root@<drone> 'mv /usr/bin/maburd /usr/bin/maburd.pre-toml'
   scp -O out/arm/maburd        root@<drone>:/usr/bin/maburd
   ssh root@<drone> '/etc/init.d/S96mabur start'
   ```
2. **GS.** Same shape: read `/etc/maburgs.json`, hand-write
   `/etc/maburgs.toml`, stop `S96maburgs`, swap config and binary
   together (rollback copy `maburgs.pre-toml`), start.
3. **Player.** Same shape again for `maburplay`/`maburplay.json`/
   `maburplay.toml`. There is no `gs/player/bundle/install.sh` to do this
   step for you — copy `out/arm64/maburplay.default.toml` (staged by
   `tools/build-arm64.sh` next to the fonts and splash) to
   `/etc/maburplay.toml` by hand, or hand-write it against the live
   `maburplay.json` the same way as the other two. Do not skip this: with
   no `/etc/maburplay.toml`, `maburplay -c /etc/maburplay.toml` exits 2,
   and `S97maburplay` treats exit 2 as terminal — no respawn, permanently
   black GS screen, not a crash-loop you'd notice in a log tail.
4. **Verify with the defaulted-keys report**, not by eye. Each daemon
   prints `config: N key(s) defaulted:` on boot — that line, read
   immediately after each swap, is the only safety net against a knob
   the hand-conversion dropped. An unexpected name in it means a value
   that should have carried over from the old `.json` fell back to the
   compiled default instead.
5. **Rollback is paired and per device**, same rule as everywhere else in
   this page: restore the old `.json` **and** the `.pre-toml` binary
   together, never one alone — a `.pre-toml` binary started against the
   new `.toml`, or vice versa, just fails to boot again.
6. **Delete the old `.json` files only after a clean flight** on the new
   config, per device.

Two holes in that safety net, worth knowing before you trust it blind:

- The defaulted-keys report checks for whole keys that are absent from
  the file; it says nothing about a key that IS present but short. Drop
  one `[[stats.out]]` entry or one `[[link.ladder]]` rung while
  hand-converting and the report stays silent — the array key was
  supplied, just with fewer elements than before.
- An absent `[[stats.out]]` array does not follow the `(section absent)`
  convention the rest of the report uses — it reports as its scalar
  fallback fields, `stats.host`/`stats.port`, defaulted individually,
  which reads like two ordinary scalar defaults rather than "the whole
  export list is gone."

## Stale-caps restart deadlock — retired

The old failure mode: either daemon restarting (drone reboot, `maburgs`
restart, GS reboot — anything that resets one side's session state while
the other side keeps running) left a rebooted GS stuck at
`peer_caps_ == 0`, refusing video forever (`REFUSING video: peer session
did not advertise CAP_FRAME_WIRE`) because it had no fast path back to
re-learning the live drone's caps, and a rebooted drone likewise had no
way to re-teach a GS that was still nominally LINKED. The only known
recovery was the manual dance: `waybeam stop` on the drone, wait for the
GS log to print `video tail -> frame wire`, then `waybeam start`.

The caps-reteach pair retires this dance for same-version pairs:
- the drone acks a keep-alive DISC while already LINKED, instead of
  ignoring it, so a GS that forgot its caps gets them re-taught without
  needing to re-enter rendezvous;
- a GS whose peer's caps are unknown holds no session either (since link
  pairing, 2026-10-01, SESSION requires an accepted DiscAck, which
  carries the caps), so it is still beaconing a DISC every 20 ms, not the
  slow steady-state `beacon_keepalive_ms`, and the re-teach happens on
  the first ack that gets through. (The 2026-08-28 build used a separate
  fast 250 ms keep-alive for this, since deleted.)

Gate-verified on hardware 2026-08-28: 5x drone `maburd` restart and 5x GS
`maburgs` restart, each under a live peer, all 10 recovered unaided
(`fps` back to ~60, `frame_id_gaps` flat, no `REFUSING`/`chip_caps=0x0000`
in the GS log) within the standard ~25 s post-restart wait — no
`waybeam stop`/`start`, no manual restart of either daemon.

**Both binaries must carry the fix for it to self-heal.** An old-GS +
new-drone pair (or new-GS + old-drone) is *safe* — same `RC_VERSION`, no
crash-loop, no frame rejection — but **un-healed**: the side still
running the old binary lacks its half of the re-teach, so a restart on a
half-deployed pair still needs the manual `waybeam stop` -> wait for
`video tail -> frame wire` -> `waybeam start` dance until the deploy is
finished on both ends. ⚠ Since the 2026-08-29 venc fold-in that
escape hatch no longer exists on the drone at all — there is no `waybeam` to
stop — so the equivalent is `S96mabur stop` / `start`, which restarts the
encoder along with the link. Keeping both ends on the same build is what
makes that never necessary.

## 2026-09-16 colortrans (maburplay glibc-dynamic, `[colortrans]` key)

`maburplay` is a glibc-DYNAMIC binary from this build on (Mesa EGL/GLESv2/GBM +
librga for the burned-DVR colortrans stage; `docs/colortrans.md`). Check with
`readelf -d out/arm64/maburplay | grep NEEDED`: libEGL.so.1, libGLESv2.so.2,
libgbm.so.1, librga.so.2, libdrm.so.2, librockchip_mpp.so.1 (plus the usual
libstdc++.so.6, libm.so.6, libgcc_s.so.1, libc.so.6 and the loader). The GS
image must carry them — the current one does; an image built from the radxa
defconfig between sbc-groundstations `ef55018` and the 2026-09-16 mesa3d/librga
re-add does not. That `readelf` check only proves what the *build host*
produced; also check the *GS* before swapping, e.g.
`ssh $GS 'ls /usr/lib/libEGL.so.1 /usr/lib/libGLESv2.so.2 /usr/lib/libgbm.so.1 /usr/lib/librga.so.2'`
— miss it and the new binary will not exec at all.

Binary BEFORE config: `[colortrans] enable` is a new key (in-code default
false). Rollback: `maburplay.pre-colortrans` (the last musl-static binary) and
`/etc/maburplay.toml.pre-colortrans`, or strip the `[colortrans]` block.
**Rollback order matters: strip the `[colortrans]` block from the config
FIRST, then swap the binary back** — the old strict-parsing binary hits the
unknown key and exits 2, and `S97maburplay` treats exit 2 as terminal, no
respawn (same rule as the vsync rollback above). Push
`out/arm64/maburplay-static` to the GS alongside the new binary at deploy
time, so this rollback works even if the image turns out to lack Mesa.

## 2026-09-16 probe per AU (no wire change, both ends together)

The drone trails every video AU with a probe (60/s at 60 fps, was
enh-only 30/s) and the GS books one expectation per video AU. No RCF or
SBI byte changes, but the pair is still mismatched between the two swaps:
new `maburd` + old `maburgs` books half the expectations (loss clamps to
0, the promote gate is blind); old `maburd` + new `maburgs` reads 50 %
probe loss and never promotes. Neither state has a control-link symptom —
finish the deploy.

**GS config first, then both binaries.** `/etc/maburgs.toml`:
`link.probe.clean_ms` is gone and fails boot; replace it with
`link.probe.clean_bodies = 90`; set `link.probe.max_util = 0.05` (the loss
quantum halved to 0.1 per lost body at 60/s, 0.15 would now allow one
loss) and `link.clean_ms = 1500`. `tools/maburtop.py` reads
`link.probe.streak_bodies` (was `streak_ms`) — swap it with `maburgs`.

## 2026-09-04 RC_VERSION 6 (probe stream)

The discrete s3 probe is replaced by an always-on probe-stream canary
(`docs/link-adaptation.md` "Probe stream"); the RCF head gains a fixed
`probe_profile` byte and `RCF_F_PROBE_ENH`/`CAP_ENH_PROBE`/`CAP_S3_PROBE`
are deleted — RC_VERSION 5 → 6, so this is a version-mismatch flag day
like the 2026-08-12/2026-08-15 bumps above: a mismatched pair rejects
each other's frames in both directions and, since DISC_ACK carries
`CAP_FRAME_WIRE`, looks like no video at all until both ends match.

**GS config first.** `/etc/maburgs.toml` gains the optional `link.probe`
block (live defaults if omitted: `enable true, rung_offset 1, clean_ms
2000, max_util <0 ⇒ down_util, min_syms 40, silence_ms 500, pin_mcs -1`)
and loses five flat keys that now FAIL BOOT — delete them before the
binary swap:

```sh
grep -nE '^[[:space:]]*probe_(ms|settle_ms|max_util|s3_min_syms|s3_silence_ms)[[:space:]]*=' /etc/maburgs.toml
```

`probe_s3_min_syms` has a successor, `link.s3_min_syms` (default 50) —
carry over a non-default value before deleting the old key.

Drone config (`/etc/mabur.toml`) is untouched — there is no drone-side
probe config; the RCF byte is the only switch. Sequence:

1. Edit `/etc/maburgs.toml` on the GS (delete the five keys above; add
   `link.probe` or rely on defaults).
2. Swap `maburgs` AND `tools/maburtop.py` together (the sideport's
   `classes` keys change shape — `s2`/`s3` gone, `probe` added — and an
   old `maburtop` against a new `maburgs` mis-renders the signal panel).
3. Swap `maburd`.

Between steps 2 and 3 the pair is mismatched (RC_VERSION 5 drone vs 6
GS): no control link, no video, exactly the deploy-window symptom this
page opens with. Finish the deploy; do not restart either daemon hoping
to fix it.

Rollback is paired, as always: `maburgs.pre-probe` / `maburd.pre-probe`
with `maburgs.json.pre-probe` (the five flat keys restored, `link.probe`
removed) alongside the GS binary — an old GS binary against a config
carrying `link.probe` fails strict keys at boot just as surely as the
reverse.

## 2026-09-06 debug-log consolidation

The five separate debug files (`ctl-NNNN_<date>.log`, `probe-NNNN_<date>.log`,
`au-NNNN.log`, `flight-NNNN.jsonl`, `lat-NNNN.log`, the last three written by
the standalone `flightrec.py` recorder) collapse into one per-session
directory, `<debug_log.dir>/NNNN/` holding `ctl.log`, `probe.log`, `au.log`,
`flight.jsonl` and `lat.log`, gated by one knob: `debug_log.enable`. This is
**config-before-binary on BOTH ground-station daemons**, not just one — an
unknown key fails boot into the usual 2 s respawn loop (see the top of this
page).

- `/etc/maburgs.toml` loses four keys that now FAIL BOOT — delete them
  before swapping `maburgs`: `link.ctl_log`, `link.ctl_log_dir`,
  `link.ctl_log_period_ms`, `link.rung_stats.rung_log_period_s`. Their
  replacements live under the new `debug_log` block (`enable`, `dir`,
  `ctl_period_ms`, `rung_period_s`) — additive, its own key, not a rename
  in place.
- `/etc/maburplay.toml` loses `display.lat_log_dir` — also now FAIL BOOT.
  maburplay holds no logging config of its own any more: it follows the
  `/tmp/mabur-session` marker maburgs writes and needs no key at all.
- **After both binaries land, delete `/root/flightrec.py` and
  `/etc/init.d/S95flightrec` from the ground station.** A leftover S95
  keeps boot-starting the old recorder, which still binds UDP `:8300` —
  starving `maburtop`, which now binds that port directly — and still
  attaches to the AU ring as a second outside reader, racing maburgs'
  own in-process ring publish for no reason.
- **Half-deployed signature:** a session directory containing every file
  EXCEPT `lat.log` (`ctl.log`, `probe.log`, `au.log`, `flight.jsonl` all
  present) means `maburgs` was updated but `maburplay` was not — the old
  player is still writing (or failing to write) `lat-NNNN.log` the old
  way, or not writing anything if it never had `lat_log_dir` set. Swap
  `maburplay` to close the gap; there is no wire-format risk in doing so
  on its own schedule, since this is config/logging only, not an
  RC_VERSION bump.

## 2026-09-09 GS card auto-scan

`maburgs` discovers its radios itself. With no `[[radio.cards]]` in
`/etc/maburgs.toml` it probes the USB bus at startup and receives on every
supported card it finds, identifying each by chip-id — one device-recipient
vendor read of `SYS_CFG2`, no claim and no reset, so a non-radio device that
shares the Realtek VID is asked one question, STALLs it, and is left alone.
Cards are ordered by physical port (`bus-port.path`, logged at startup as
`cards: card 0 = 0bda:a81a at usb 2-1.1`), so `card 0` in the stats and the
OSD is the same antenna across reboots.

Deploy notes, none of which are wire-format:

- **This is a GS-only change.** No RC_VERSION bump, no drone deploy, no flag
  day. Swap `maburgs` alone.
- **Binary before config, unusually.** The new binary reads an old config
  (explicit `[[radio.cards]]` blocks) exactly as before — the list still
  pins, and skips the probe entirely. So swap the binary, confirm, then
  delete the card blocks.
- **The rollback trap is silent.** An OLD `maburgs` with the NEW card-less
  config does not fail boot: absent `radio.cards` used to mean *one default
  card*, so it comes up receiving on a single radio and looks exactly like a
  dead antenna. Rolling back the binary means restoring
  `/etc/maburgs.toml.pre-autoscan` alongside it — the standing paired-rollback
  rule, with no error message to remind you.
- **An empty bus exits 1** after a 15 s wait rather than running blind; the
  `S96maburgs` wrapper respawns at 2 s, which is also the retry a card that
  enumerates late needs. The 2 s per-card reopen retry is unchanged.
- **`radio.tx_card` is no longer range-checked at config load** under
  auto-scan — the count is hardware, not config. A pin past the cards found
  logs `warning: radio.tx_card N but only M card(s) found; falling back to
  auto-select` and runs on auto, rather than costing the uplink because one
  card did not enumerate.

## 2026-09-10 RC_VERSION 7 (TX-power calibration kit)

Two new frame types, `T_CAL_CMD` and `T_CAL_RESULT`, carry the
`maburcal` calibration protocol between `maburgs` and `maburd`
(`docs/calibration.md`) — `RC_VERSION` 6 → 7, a version-mismatch flag day
like the 2026-08-12/2026-08-15/2026-09-04 bumps above. A mismatched pair
rejects each other's frames in both directions and, since `DISC_ACK`
carries `CAP_FRAME_WIRE`, **looks like no video at all** between the two
swaps — exactly the stale-caps restart deadlock's symptom. Restarting
either daemon will not fix it; finish the deploy.

No config keys move. This is a binary-only flag day on both ends —
deploy `maburd` and `maburgs` together, in either order, and confirm
video before treating the deploy as done. Rollback is the usual paired
one: an old `maburd`/`maburgs` pair (`.pre-cal` binaries, if kept) talks
`RC_VERSION` 6 to itself and needs no config change to go with it, since
none of this bump touches config.

`ausniff` is the standing gate for this change (`tools/bench/ausniff.py`)
— run it once both binaries are up, expecting ~59.8 fps and 0 gaps; a
`frame_id_gap` on the very first post-deploy pass can be a phantom from
the restart itself, so take a second pass before treating it as real.

## 2026-09-13 RC_VERSION 8 (relative TX-power walls)

Calibration indices are signed and relative to the chip's efuse anchor;
`Telem` drops `cal_base_ref_idx` (88 → 87 bytes) — `RC_VERSION` 7 → 8, a
version-mismatch flag day like the ones above (no control link and no
video between the two swaps; finish the deploy, do not restart).

**Config keys move on the drone:** `radio.rate_walls_idx`,
`radio.legacy_wall_idx` and `radio.base_ref_idx` are gone;
`radio.rate_walls_rel` and `radio.legacy_wall_rel` replace them. Old
binary + new config, or new binary + old config, both fail boot into the
2 s respawn loop — swap config and binary together on the drone (stop
`S00mabur`, copy both, start with `setsid`). The GS has no config change.
Rollback is paired: `maburd.pre-relwalls` + `mabur.toml.pre-relwalls`
on the drone with `maburgs.pre-relwalls` on the GS.

## 2026-09-20 low-power mode

`[low_power]` is a new drone section: **binary BEFORE config** on the
drone — the previous strict-parsing binary exits on the unknown table and
`S00mabur` respawns it forever. `low_power.enable = true` requires
`msp.enable = true` (boot failure otherwise, by design). The GS side is
additive (Telem flags bit7, sideport `drone.low_power`): deploy maburgs +
maburplay + `tools/maburtop.py` together. Rollback: `maburd.pre-lowpower`
+ strip the `[low_power]` block first, then swap the binary.

Same day, follow-up: the GS's `link.probe.min_syms` must be **16** (was
40) or the ladder never promotes while the drone is disarmed — config
value only, no wire or binary change, restart `S96maburgs` to take it
(docs/link-adaptation.md, "Low-power (disarmed) mode" item 1). The drone
binary from the same commit re-anchors the vanish tracker on the fps
verb (item 3); rollback copy `maburd.pre-ratechange`.

**2026-09-21 `drone.sys.load` → `drone.sys.cpu_pct`.** Telem keeps its
size and layout — the old `load_x100` slot now carries `cpu_busy_x100`
(65535 = unavailable) — so a mismatched pair still links; an old
maburgs just exports the new number under the old key (`load 0.14` for
14 %). Deploy maburd + maburgs + `tools/maburtop.py` together anyway; the
sideport key is renamed, so maburtop from before this reads `--`.

## 2026-09-23 RC_VERSION 10 (carrier sense ON, drone RX energy telemetry)

`RC_VERSION` 9 → 10: `Telem` grows 89 → 95 bytes with the drone's RX-side
channel view (`rx_own`, `rx_foreign`, `rx_crcfail`), and both daemons
now leave the MAC carrier-sense gate at the chip default
(`disable_cca = false`; `docs/findings/2026-09-23-cca-on.md`). **One new GS
key, `link.arrival_guard_syms` (default 192, in the bundle)** — the
ArrivalTracker settle line that decides when a symbol is booked missing
(`docs/observability.md` "pre-FEC loss is late, not lost"). It has a
default, so an old `maburgs.toml` without it boots fine on the new binary,
but a config that carries it fails on an old binary: binary first, then
the key. **No drone config change.** Otherwise this flag day is binary
swaps only: `maburgs` first, then `maburd` (or the reverse; the
mismatched-pair window between them has no video and no control link
exactly as every `RC_VERSION` bump, and clears once both are swapped).
The carrier-sense flip is compiled in, not configured, and the two ends
must never be split: a GS with carrier sense on against a blind drone is
the measured-worse arm of `docs/findings/2026-08-14-rcf-uplink-loss.md`
§6. Verify after the swap: both bring-up lines read `requested ON`, the
sideport shows `drone.radio.rx`, the drone `stats:` line keeps
`txq_drop=0`, and `streams[*].recovered` sits near 0/s on a clean bench
(it read 0.2–0.3/s blind).

## 2026-09-26 RC_VERSION 11 (VTX onboard SD recorder)

`RC_VERSION` 10 → 11: the RCF gains the one-byte `rec` wish and `Telem`
gains `rec_status` (`docs/vtx-recorder.md`). This is a flag day like every
bump. Between the two swaps there is no control link and no video. Finish
the deploy.

**Order is binary first, then config.** This reverses the usual
config-before-binary rule, and the reason is the direction of the new keys.
The new `maburd` boots without `[record]` (recorder disabled, presses
report Disabled), but an old `maburd` refuses the unknown `[record]`
section and crash-loops. The same holds for `maburplay` and
`dvr.target`. So:

1. Drone: `df -h /` (keep one rollback: `maburd.pre-vtxrec`), stop
   `S00mabur` in its own ssh call, and wait until no process with comm
   `maburd` remains in `/proc`. Swap the binary, save
   `/etc/mabur.toml.pre-vtxrec`, append the `[record]` block from
   `bundle/mabur.default.toml`, then start with `setsid`.
2. GS: stop `S96maburgs`, swap `maburgs` and `maburplay` (keeping
   `.pre-vtxrec` copies), add `target = "gs" | "vtx" | "both"` under
   `[dvr]` in `/etc/maburplay.toml` (keeping `.pre-vtxrec`), then start
   `S96maburgs` and restart `S97maburplay`.

Verify: the drone log has
`[record] ch1 … bound before the link (idle)` before the link comes up.
The sideport carries `drone.rec`. After one press and about 5 s, the
drone log shows `rec: recording -> /mnt/mmcblk0p1/record-NNNN.mp4`. GS
`lat.log` `enc` still reads 7/8. `ausniff` is clean. Rollback is paired
per device: `maburd.pre-vtxrec` + `mabur.toml.pre-vtxrec`,
`maburgs.pre-vtxrec` + `maburplay.pre-vtxrec` +
`maburplay.toml.pre-vtxrec`, on both ends together.

## 2026-10-01 RC_VERSION 14 (link pairing)

`RC_VERSION` goes 13 → 14: every GS→drone control frame (DISC, RCF,
CAL_CMD, CAL_RESULT) gains an 8-byte SipHash-2-4 tag before its CRC;
`link.vtx_id` is gone from every frame and both configs; `DISC_ACK`
gains `vtx_nonce` + a flags byte; `Telem` flags bit1 is `auth_reject`.
A version-mismatch flag day like every `RC_VERSION` bump above: between
the two swaps there is no control link and no video (`DISC_ACK` carries
`CAP_FRAME_WIRE`) — finish the deploy, do not restart either daemon
hoping to fix it. `docs/link-pairing.md` is the as-built page.

**Config moves on both ends.** `link.vtx_id` is replaced by
`link.key_file` — an old config fails boot (strict keys), so this is
config-before-binary on both the drone and the GS, same as every config
move in this doc:

```toml
[link]
key_file = "/etc/mabur.key"
```

### Pairing

The key-generation and fingerprint procedure is current reference: docs/deploy.md "Pairing".

`ausniff` is the standing gate once both ends are up:
`tools/bench/ausniff.py`.

Bench gate 2026-10-01 (branch `link-pairing` at 08a7b60, both ends
deployed with `key_file`, rollbacks `maburd.pre-pairing` /
`maburgs.pre-pairing` beside `mabur.toml.pre-pairing` /
`maburgs.toml.pre-pairing`): ausniff 30 s at mcs4/40 — 1815 AUs,
60.5 fps, 0 incomplete, 0 gaps, 0 resyncs (identical to the telem-diet
run); sideport `link.state = session`, `key_fp = default`,
`drone.auth_reject = false`. Wrong key on the drone only → GS
`KEY MISMATCH` within 1.2 s of the first rejected RCF; same key on the
GS + `restart maburgs` → `session`, both fingerprints `2263`, drone not
restarted. Timed recoveries in `docs/findings/2026-10-01-link-pairing-bench.md`.

## 2026-09-30 telem diet (RC_VERSION 13)

`T_TELEM` shrinks 98 → 48 bytes and `RC_VERSION` goes 12 → 13 — a
version-mismatch flag day (no control link, no video between the two
swaps; finish the deploy, do not restart). **No config change on either
device**: swap `maburd` and `maburgs` only. `maburplay` is unaffected
(it reads only sideport keys that stayed). The web GS speaks the same RC
wire, so the hosted page must be rebuilt and redeployed with it.
`tools/maburtop.py` and `tools/flightreport.py` must be the same commit
to render the new `drone.*` block. Rollback is binary-only and paired:
`maburd.pre-telemdiet` on the drone with `maburgs.pre-telemdiet` on the
GS. Bench gate 2026-10-01, both the 53-byte first cut and the final
48-byte build (1d27796): ausniff 30 s at mcs4/40 — 1815 AUs, 60.5 fps,
0 incomplete, 0 frame_id gaps, 0 resyncs.

## 2026-10-02 relay cards (`radio.relays`)

`maburgs` gains `[radio] relays = [...]`: each `"ipv4:port"` entry (numeric
dotted IPv4 — a hostname fails boot; the CPE has no DNS anyway) adds a
CPE510 `mabur-relay` unit as a card after the USB cards
(`docs/cpe510-relay.md`, "maburgs RemoteCard"). **GS only — no drone
change, no wire change, no flag day.** `maburplay` reads the new sideport
keys (`cards[i].kind`) and ships in the same deploy; `tools/maburtop.py`
must be the same commit to draw the `r<id>` rows.

**Config and binary move together, with the daemon stopped** (the
config-before-binary rule): an old `maburgs` fails boot on `relays`, the new
one boots without it (no relays). Stop `S96maburgs`, swap `maburgs` (and
`maburplay`) keeping `maburgs.pre-relay` / `maburplay.pre-relay`, save
`/etc/maburgs.toml.pre-relay`, add `relays = ["10.83.11.1:8310"]` under
`[radio]`, start `S96maburgs`, restart `S97maburplay`. Never start the old
binary against the edited config.

**Network.** The Radxa ZERO 3 has no Ethernet: the CPE needs a
USB-Ethernet adapter on the GS. The CPE serves DHCP on its LAN,
10.83.11.100-199, with **no router and no DNS** (mabur-openwrt
`90-mabur-lan`). After a CPE `sysupgrade -n` its SSH host key changes:
`ssh-keygen -R 10.83.11.1`.

**`tx_card` pin.** A pin may name the relay, but under auto-scan its index
is `n_usb + k` — a second USB card appearing shifts it; pin a relay only
with an explicit `[[radio.cards]]` list.

Verify: the GS log prints `cards: card N = relay 10.83.11.1:8310` and then
`maburgs relay card N (…): owned and tuned`; maburtop shows an `r<N>` row
with a relay strip reading `own=1`, `gaps` flat. `ausniff` is the gate.
Rollback: `maburgs.pre-relay` + `maburplay.pre-relay` +
`maburgs.toml.pre-relay` together (the old binary refuses the `relays`
key).

## 2026-10-03 channel set (no RC_VERSION bump)

Home channel + candidates is replaced by one shared channel set,
`radio.channels`, on both ends (`docs/channel-select.md`). **Binary THEN
config, on each device** — the exception this page's intro already flags
for exactly this shape of change: the new binary still boots on an old
config (every new key has a default), but the new config fails the OLD
binary at the unknown-key check (GS: `radio.channel` as a bare number,
`radio.scan.enable`, `candidates`, `home_window_ms`, `split_after_ms`,
`home_margin` are gone; drone: `radio.channel`, `radio.follow_gs` are
gone). So on each device: swap the binary, confirm it is up, then push
`gs/bundle/maburgs.default.toml` → GS `/etc/maburgs.toml` and
`bundle/mabur.default.toml` → drone `/etc/mabur.toml`.

**No `RC_VERSION` bump.** `Disc.op_channel`/`DiscAck.agreed_channel` and
`Rcf.hop_ch`/`hop_epoch` are unchanged wire fields that both ends now mean
literally over the whole set rather than one home channel, so a
half-deployed pair (old binary one end, new the other, for however long
the swap takes) just links on whichever channel both happen to be parked
on — drone first or GS first does not matter, and there is no flag-day
window of no video at all the way a wire bump produces.

**Two new state files**, plain decimal text, written via a temp file +
`rename()`: `/etc/mabur.channel` (drone) and `/etc/maburgs.channel` (GS).
`rm` either one to make that end forget its remembered channel; on the
next boot it falls back to `radio.channels[0]` (drone) or searches from
`channels[0]`/the configured pin (GS) instead of re-finding whatever
channel it last parked on. Deleting the GS's file is also how you force a
fresh boot-time search after changing `radio.channels` itself, rather than
the GS trusting a now-stale remembered member.

Verify after the swap: drone stderr
`maburd: channel set [40,64,112,144], parking on 40` (gains
`(remembered)` after the first confirmed move); GS stderr
`maburgs channel: set [40,64,112,144] mode auto start 40` (likewise) —
both print once, at boot, before anything else. `ausniff` is the standing
gate once both ends are up.

Rollback is paired, as always: the old binary needs its old config
(`radio.channel`/`follow_gs` etc.) restored alongside it on each device.
Keep `maburd.pre-chanset` / `maburgs.pre-chanset` binary copies (with
their old configs saved alongside) before swapping, the same convention
as every other dated section on this page.

## 2026-10-10 FrameHdr byte 3 codec→slice_rows (H.265 row slices, `docs/slices.md`)

FrameHdr byte 3 was an always-H.265 codec id; it is now `slice_rows` (64-px
CTU rows per slice of the AU, 0 = one slice). Unlike the `CAP_FRAME_WIRE`
cases above, a mismatched pair still has video: an old maburd sends byte 3
= 1, which a new maburgs reads as `slice_rows` 1 (damaged AUs then pass
through as `no_template` instead of being salvaged); a new maburd's
`slice_rows` is ignored by an old maburgs. Video continues, salvage is off
or miscounted until both ends match — still deploy maburd and maburgs
together. `[venc] slices` is
a new drone key (default 1 = off, so an old config still boots the new
binary unchanged) — binary before config, as always.

## 2026-10-10 AU ring v4 + stream-mode GS image + `[decoder] stream` (`docs/slices.md` Part 2)

GS-only flag day; the drone is untouched.

1. **Image first** ("GS image: p1 squashfs swap" above): the image-repo
   `slice-stream` build (`output/gs-p1-stream/rootfs.default.squashfs`).
   The old maburgs/maburplay keep running on it (ring v3 both, whole-AU
   decode with link mode off) — check video before going on.
2. **maburgs and maburplay together.** Stop `S97maburplay` and
   `S96maburgs` (each in its own ssh call), keep `.pre-ringv4` copies of
   both binaries and of `/config/maburplay.toml`, then swap both binaries.
   `/etc/maburplay.toml` is a symlink to `/config/maburplay.toml` (the vfat
   partition) — back up and edit the **target**, `/config/maburplay.toml`,
   not the link: BusyBox `sed -i` on the symlink itself replaces it with a
   regular overlay file, after which the `/config` copy goes silently dead
   (it is no longer what `/etc/maburplay.toml` resolves to), and `cp -a`
   on the link backs up only the link, not the config. Write every config
   path in this entry as `/config/maburplay.toml` for that reason. A
   mismatched maburgs/maburplay pair shows no video and no crash: either
   maburplay keeps waiting on a ring of the other version. Finish the
   swap; restarting either side does not help.
3. **Binary before config:** add `[decoder]` / `stream = true` to
   `/config/maburplay.toml` only once the new maburplay is in place — an
   old maburplay exits 2 on the unknown key and `S97maburplay` does not
   respawn on exit 2.
4. Start `S96maburgs`, then `S97maburplay`. Verify in
   `/tmp/maburplay.log`: `maburplay: decoder stream: on`,
   `MppBackend: stream mode available`, and `stream:` lines with
   `aborted=0`; `ausniff.py` (v4) clean.

Kill switch without a rollback: `stream = false` in `/config/maburplay.toml`
plus a restart of `S97maburplay`. Rollback: both `.pre-ringv4` binaries and
the `.pre-ringv4` copy of `/config/maburplay.toml` together; the image can
stay, or p1 rolls back to `output/gs-p1-backup-2026-10-10/p1.orig.img`. The
bench GS additionally carries `.pre-feed` copies of maburplay (from before
Task 11g's decoder-input thread, `docs/findings/2026-10-10-slice-salvage.md` "Part 2 bench — streamed
decode"): don't confuse the two — `.pre-ringv4` is the pre-Part-2 baseline,
`.pre-feed` is the pre-FeedLoop one, one step later.

## GS p1 backups of record (2026-10-09 / 2026-10-10)

From docs/deploy.md "GS image: p1 squashfs swap": the backups of record are
`output/gs-p1-backup-2026-10-09/p1.orig.img` (stock CI image, before the
spike) and `output/gs-p1-backup-2026-10-10/p1.orig.img` (stock, before Part
2; byte-identical to the 10-09 one, md5 `8e38695a…`). `/root/mpp-stream/` on
the GS holds the 2026-10-09 spike's private MPP copies, used only through
`LD_LIBRARY_PATH` by its `run.sh`; they shadow nothing. On 2026-10-10 a
read-back attempted after `echo 3 > /proc/sys/vm/drop_caches` on a rewritten
p1 reset every new ssh connection.
