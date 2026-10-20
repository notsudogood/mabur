# GS recording: raw video plus OSD sidecars

Since 2026-10-10 a fresh GS install records **raw** (`[dvr] mode = "raw"`
in `maburplay.toml`). The video is saved exactly as it arrived from the
drone, with no re-encode. The flight controller's OSD and a link summary
are saved next to it, so the OSD can be drawn back on afterwards with the
tools made for Walksnail goggle recordings.

## Why not "burned"

`mode = "burned"` decodes, draws the OSD and re-encodes on the GS while
it is also decoding and displaying the live picture. On the 2026-10-10
flight that cost latency: with burned recording on, **58 % of seconds** had
an e2e p99 of 70 ms or more, against **5 %** with it off (same flight,
genlock on). `lat.log` carries `rec=off|raw|burned` on every 1 Hz `lat:`
line, and `flightreport`'s GS RECORDING section splits latency by it.

## Raw recording's cost, and the writer thread (2026-10-11)

Raw recording first flew on 2026-10-11 and was **not** free. Armed
seconds, grouped by the encoder's bitrate so the comparison is like for
like (three sessions: two flights and a bench run):

| encoder | recorder | seconds | decode p99 | e2e p99 | seconds with e2e p99 ≥ 70 ms |
|---|---|---|---|---|---|
| 24 Mbps | off | 132 | 14 ms | 55 ms | 11 % |
| 24 Mbps | raw | 185 | 41 ms | 82 ms | 91 % |
| 13.8 Mbps | off | 62 | 16 ms | 58 ms | 34 % |
| 13.8 Mbps | raw | 21 | 25 ms | 66 ms | 38 % |

The typical frame decoded as fast either way (p50 8–9 ms); only the worst
frame or two of each second slowed, and the cost grew with bitrate. The
cause was in the code, not the card: the recorder writes each 1 s
fragment (about 3 MB at 24 Mbps) and `fflush()`es it inside `feed()`, which
ran on the player loop that also feeds the decoder. *Against it:* most of
the 24 Mbps "off" seconds come from a different flight and channel, and
no log times individual frames, so the once-a-second stall is inferred
from the code and the per-second pattern; the radio link can't explain
it, though (0.06 % loss while recording, and the stage that grew was
decoding, not waiting for repairs).

Since then maburplay runs the recorder on a **writer thread**
(`gs/player/src/async_raw_dvr.h`): the loop only copies each complete
frame into a queue, and the thread does the sync gate, the muxing and all
file I/O at a lower priority. The file is byte-identical to before
(`tests/test_async_raw_dvr.cpp`). If the card falls more than 16 MB
(about 5 s at 24 Mbps) behind, the recording **stops with an error**
(the OSD's REC turns to a fault, the file so far is kept, the log says
`the SD card fell N MB behind`) rather than stalling the picture.
**Flown 2026-10-11 (GS eMMC session 0027, firmware efficient-link-nack-gs4):**
armed at 24 Mbps with raw recording on for 252 s, decode p99 15 ms and e2e
p99 56 ms, 14 % of seconds at or above 70 ms -- against 41 ms / 82 ms /
91 % with the inline writer and 14 ms / 55 ms / 11 % with recording off
(table above). *Against it:* that flight recorded to the GS's on-board
eMMC (no SD card inserted), which is faster than a typical SD card; the
thread takes the write off the loop on any card, but a card that cannot
sustain the bitrate now ends the recording with an error instead, and
that path has not been seen in flight. The player log (`the SD card fell
… behind`) was not captured, and the file itself has not been played
back yet.

## Pressing record

The recorder starts a file only at a real keyframe (IRAP), and the
drone's GDR encoder sends one almost only at session start and at arming.
Since 2026-10-11, while a raw recording waits for one the player asks the
drone for one every 500 ms (`maburplay: rec: waiting for a keyframe --
asked the drone for one`): maburplay sends `idr` to maburgs (UDP 8401,
`rec_control.h`), maburgs bumps the RCF's `idr_epoch`, and the drone
serves one paced IDR per change -- the web GS's request path. A press
mid-flight should now start recording within well under a second (design
estimate: the next RCF, the drone's 100 ms IDR floor, one frame); unflown. If a recording starts while genlock is calibrating, the
calibration starts over (`genlock-cal: restarted` in lat.log): on
2026-10-11 a recording that began 6 s into calibration was the likely
reason it measured the camera at 60.274 fps instead of 60.08.

Existing installs keep their saved config: `/config/maburplay.toml` from an
older image still says `mode = "burned"` until you change it.

## What lands on the card

All in `[dvr] dir` (default `/media/dvr`), one set per recording, same
base name:

| file | what |
|---|---|
| `record-NNNN.mp4` | the video as received (HEVC, fragmented MP4, 1 s fragments) |
| `record-NNNN.osd` | the flight controller's OSD, Walksnail format |
| `record-NNNN.srt` | one subtitle a second in Walksnail's own format, the only one walksnail-osd-tool reads: `Signal:N CH:N FlightTime:N SBat:0.0V GBat:0.0V Delay:Nms Bitrate:N.NMbps Distance:0m` |
| `record-NNNN.link.srt` | the same cues with our detail: channel, MCS/width, RSSI, loss, VTX temp, latency |

The `.osd` starts when the video file gets its first frame (time zero is
that frame), and records every complete DisplayPort screen. Format: a
40-byte header whose first four bytes name the firmware (`osd_fc`:
`BTFL`, `INAV` or `ARDU`), then one 2124-byte record per screen: a u32
LE time in ms, then a 53 × 20 grid of u16 LE glyph indices, row by row
(`gs/player/src/osd_recorder.h`). The font page rides in the glyph's high
byte. A smaller canvas (the usual 50 × 18) is padded with blanks.

Both sidecars are buffered in memory and written once a second from the
player's idle slot, never between a frame and its refresh: an SD write can
stall for hundreds of ms. A power cut loses at most the last second of
`.osd`/`.srt`/`.link.srt`, matching the video's 1 s fragments.

In the `.srt`, Signal is 0-4 bars from the best card's RSSI (4 at -60 dBm
or better, one bar per 10 dB, 1 at -90), CH is our channel, FlightTime
the seconds since the recording started, Delay the e2e p50 and Bitrate
the received video rate. The GS has no battery or GPS feed, so SBat, GBat
and Distance are 0.

## Stitching the OSD back on

1. Copy the files of a recording off the card, keeping them in one
   folder.
2. Open the `.mp4` in walksnail-osd-tool (github.com/avsaase/walksnail-osd-tool).
   It finds the `.osd` with the same name; otherwise, load it by hand.
3. Pick the font for your flight controller firmware (the `osd_fc` you
   set, Betaflight by default) and render.

Both subtitle files open in any video player as a subtitle track; the
`.link.srt` is the one with our link detail.

**Not verified yet:** no file from this recorder has been through
walksnail-osd-tool. The layout matches what that tool reads (and what
msposd writes), and the unit tests pin it byte for byte, but the first real
recording is the test. If the glyphs come out wrong, the likely cause is
the font page mapping; say which firmware and font you used.

## Config (`maburplay.toml`, `[dvr]`)

```toml
mode     = "raw"    # "raw" = video exactly as received; "burned" = re-encoded with the OSD drawn in
osd_file = true     # raw: also save .osd and .srt next to the video
osd_fc   = "BTFL"   # .osd firmware tag: "BTFL", "INAV" or "ARDU"
```

`osd_file` does nothing in burned mode (the OSD is already in the video).
The drone's own SD recording (`target = "vtx"` or `"both"`) has no `.osd`
file yet; a drone-side writer is planned, not built.
