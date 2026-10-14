# VTX onboard SD recorder: bench record (2026-09-26/27)

**Status:** the recorder shipped as measured here (RC_VERSION 11, `[record]`, bind-before-link); the drain rewrite and the port-1 `record.size` path are live. Current reference → docs/vtx-recorder.md.

Extracted from docs/vtx-recorder.md during the 2026-10-11 docs tidy. "Above"/"below" refer to that page as it was then.

## Recording size — `record.size` (bench 2026-09-27)

Measured on the drone, rung 5 / mcs 4 / 24 Mb/s, `low_power.enable = false`:

| link | recording | link `enc` p50/p99 idle → recording | recording |
|---|---|---|---|
| 1920x1080@30 (4K sensor mode) | 3840x2160@30, port 1, 40 Mb/s | 9.1/9.2 → **40.8/50.5 ms** | valid HEVC 4K, 38.7 Mb/s, **28.4 fps** (181 of 1366 gaps are 62 ms) |
| 1280x720@60 | 1920x1080@60, port 1 | 4.6/4.8 → 4.2/5.0 ms | recorded, no writer drops |
| 1920x1080@60 | 1920x1080@60, port 0 | 7.8 ms idle (unchanged topology) | see above |

- **4K is not usable next to the link.** One 4K encode is ~30 ms against a
  31 ms frame period (the 4K mode runs 32 fps), so the single-task H.265
  engine is over budget: the link queues behind the 4K encode (+32 ms glass
  latency) and the recorder drops ~12 % of its frames. Bind order cannot
  help once the engine is saturated. 2560x1440 from the 4K mode (~13 ms +
  ~8 ms) is the untested size that should fit.
- **720p link + 1080p recording works and costs the link nothing.** The
  port-1 1080p recorder did not move the link's `enc`.
- **A 720p link under-delivers CBR on the bench scene: 9.8 Mb/s against a
  24 Mb/s command, recorder on or off**, where 1080p60 on the same scene
  and rung delivered 23.8. That is ~ the 2.25x pixel ratio, consistent with
  the RC sitting at the `MinQp 12` floor on a static scene (the live QP is
  not exposed; not confirmed). No config key moves `MinQp`.
- Switching `venc.size` needs no other key change: the rolling-refresh
  key is `venc.intra_refresh_frames` (sweep length), whose rows per P the
  pipeline derives from the encoded height. The raw `intra_refresh_rows`
  it replaced (2026-09-27) failed boot at 720p with the 1080p value 34.

## Bench results (2026-09-26, Runcam WiFiLink 2 + bench GS)

The binaries were built against devourer master 56eabe4. The link sat at
mcs4/40 MHz, 24 Mb/s, 1080p60, with `low_power.enable = false` for the
runs. MI numbers are averages over 60 s after `reset_counter`, in µs on
the MI origin. `lat.log` `enc` is the GS player's per-second p50/p99.

| run | VPE `FinDMADispatch` | ch0 enqueue | ch0 dequeue | ch0 GetStream | ch1 enqueue | ch1 GetStream | link `enc` p50/p99 (60 windows) |
|---|---|---|---|---|---|---|---|
| `[record] enable=false` | 16 744 | 16 832 (+0.09 ms) | 24 002 | 24 439 | — | — | 7/8 ×49, 8/8 ×12 |
| enable=true, idle | 16 747 | 16 832 (+0.09 ms) | 23 986 | 24 431 | — | — | 7/8 ×49, 8/8 ×11 |
| recording 40 Mb/s @ 60 | 16 723 | 16 807 (+0.08 ms) | 24 010 | 24 447 | 24 113 (+7.39 ms) | 31 941 | 7/8 ×55, 7/9 ×5 |

- **Bind order holds.** While recording, the link enqueues at VPE dispatch
  and ch1 enqueues one link encode later. The link's `enc` is unchanged.
  An idle ch1 costs the link nothing. With ch1 created, `MbRate %` reads
  147 even while ch1 is idle, so it counts configured channels, not work.
- **ausniff** during recording (30 s): 1816 AUs, 908/908 complete,
  `fid_gaps=0 resyncs=0`, 60.5 fps.
- **CPU** (`drone.sys.cpu_pct`, 60 s): 39.4 % off → 59.1 % recording,
  **+19.7 points**. This is higher than the spike's +14. Per thread:
  `mbr-rec` (writer) 12.4 % and `mbr-recdrain` 6.2 % of one core. The rest
  is kernel time for vfat/SD. A repeat after a fresh boot gave 39.9 → 56.7.
- **CPU after the drain rewrite** (4704246, same day): the drain now copies
  each NAL straight into MP4 layout from the SDK's NAL table
  (`drone/venc/rec_frame.h`) and the writer no longer rescans or re-copies
  frames. Per-thread user time, % of one core, 20 s windows at 40 Mb/s:
  `mbr-rec` 5.5 → 0.4, `mbr-recdrain` 6.0 → 1.7. System-wide user time
  added by recording (of 200 % on two cores) fell from +14.4 to +3.9. What
  remains is `fwrite` + one `fsync` per fragment (`mbr-rec` sys ~7 %),
  kernel vfat/SD writeback, and ~20 % iowait behind the fsyncs. A 33.6 s
  recording made with it decodes cleanly (2003 frames, 1080p60).
- **Memory** at 40 Mb/s: `maburd` VmRSS peaked at 18.8 MB and MemAvailable
  never fell below 58.8 MB. With a parallel `dd` of 300 MB `conv=fsync` to
  the card, the peaks were 22.5 MB RSS and 53.3 MB MemAvailable, with 18
  queue-full drop events and no OOM. At `bitrate_kbps = 80000` with the
  same `dd`: 21.1 MB RSS, 55.2 MB MemAvailable, 18 drops, no OOM.
- **Delivered bitrate.** At 40000 the file carries about 32 Mb/s
  (`ffprobe` 31.8 Mb/s). At 80000 the sizes on the card worked out to only
  about 21–25 Mb/s, *less* than at 40000. Not investigated. Keep 40000.
- **Content.** A stopped 61 s recording decodes cleanly: HEVC 1920x1080,
  61.32 s. ffmpeg's null muxer prints 5 "non monotonically increasing dts"
  warnings per file, with no decode errors.
- **Battery pull** (plug cut after about 65 s): after reboot the file is
  listed at 255 341 831 B, and `ffprobe` gives 63.32 s, decoding cleanly to
  the end. The time from the press to the last ping before the cut was
  64.7–65.3 s, so the loss is **≤ 2.0 s**. That is an upper bound, because
  the time from the press to the first IDR was not subtracted. A second,
  less precisely timed pull gave the same result.
- **`kill -9 maburd` while recording.** The wrapper respawns it. The new
  instance is not Disabled, and because the button was still on it
  resumed straight into a new file. The pre-kill file plays to 20.14 s, as
  large as it was listed just before the kill. dmesg shows the usual MI
  teardown noise (CMDQ `WAIT_TRIG_TIMEOUT`, `FlushInputPortTasks`, a
  `GetChnDevid CH 1 is not created` warning from the ch1 pre-init
  teardown) and no MMU callback storm.
- **`S00mabur stop` while recording**: a clean `rec: STOP`. The file
  (57.27 s) plays to the end.
- **Re-link resumes.** After a power cut with the button still on, the
  drone was recording into a new file within about 7 s of kernel start.
- **Link outage** (`maburgs` stopped for about 10 s): the drone went to
  state 3 and did a rendezvous retune, and there was no STOP. The file
  (78.66 s) is one continuous file with no gap > 40 ms.
- **GS restarts.** `S96maburgs restart` while recording: no STOP.
  `S97maburplay stop` for 18 s: the drone kept recording. Starting the
  player again with the button off made the drone log `rec: STOP` about 3 s
  later, including player startup. `killall maburplay` does not leave the
  player down, because the S97 loop respawned it within about 2–2.5 s. The drone
  then stopped 1–2 s after the new player appeared (3 runs, 3.3–4.8 s from
  the kill). This is the accepted "a restarted player with its button off
  stops the drone" behaviour.
- **Error states** (read from `drone.rec`, not from the screen):
  `dir = "/tmp/nocard"` gave err 4 NotMounted (`VTX NO CARD`).
  `min_free_mb = 1000000` gave err 5 LowSpace (`VTX FULL`).
  `enable = false` gave err 1 Disabled (`VTX OFF`). Pressing off clears
  each of them back to state 0, err 0.

### Open

- **Recording starts at 40 fps for about 0.9 s.** In every file, every
  third frame is missing for the first ~0.9 s after `StartRecvPic`. ch1 is
  bound `src 90 → dst 60` (the sensor mode's 90 fps), and its frame-rate
  control seems to start out dropping 1 in 3. The link channel gets the
  cold-boot `SetFps(60)` re-kick, but ch1 does not.
- **One missing frame every ~10.15 s** in the recording. The link's own
  `au.log` shows the same cadence (sessions 0250, 0253 and 0259). This is
  a VPE/sensor frame-rate effect that predates the recorder.
- **Writer drops in one instance.** A fresh boot drops 0–1 AU burst per
  minute. One `maburd` instance, started with `S00mabur start` after the
  GS restart tests and a rendezvous retune, dropped 3–22 bursts per
  recording, with 3.4 s of 29 s missing, and its idle CPU was 45 % instead
  of 40 %. A power cycle cleared it, and a repeated `kill -9` did not
  bring it back. Cause unknown. Each drop costs the frames up to the next
  requested IDR (67–383 ms).
- Not tested on hardware: pulling the card mid-recording, unplugging the
  GS radio, the OSD pixels themselves (the states were read from the
  sideport), and rotation at 3.9 GB (the longest file was 1.13 GB).

## Deploy and rollback (2026-09-26)

This is a flag day (RC_VERSION 11). Install the **binary first, then the
config**. The new `maburd` runs without `[record]` (recorder disabled),
and an old binary refuses the key. The same applies to `maburplay` and
`dvr.target`. `docs/deploy.md` has the details. The rollbacks on the bench
are `/usr/bin/maburd.pre-vtxrec` + `/etc/mabur.toml.pre-vtxrec` (drone),
and `/usr/local/bin/maburgs.pre-vtxrec`,
`/usr/local/bin/maburplay.pre-vtxrec` + `/etc/maburplay.toml.pre-vtxrec`
(GS). Roll back both ends together, each binary with its config.
