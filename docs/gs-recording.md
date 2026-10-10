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
genlock on). Raw recording only copies the received bitstream into the
file. It has not flown yet, so its own cost in flight is unmeasured.
`lat.log` now carries `rec=off|raw|burned` on every 1 Hz `lat:` line, and
`flightreport`'s GS RECORDING section splits latency by it, so the next
flight measures it.

Existing installs keep their saved config: `/config/maburplay.toml` from an
older image still says `mode = "burned"` until you change it.

## What lands on the card

All in `[dvr] dir` (default `/media/dvr`), one set per recording, same
base name:

| file | what |
|---|---|
| `record-NNNN.mp4` | the video as received (HEVC, fragmented MP4, 1 s fragments) |
| `record-NNNN.osd` | the flight controller's OSD, Walksnail format |
| `record-NNNN.srt` | one subtitle a second: channel, MCS/width, RSSI, loss, VTX temp, latency |

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
`.osd`/`.srt`, matching the video's 1 s fragments.

## Stitching the OSD back on

1. Copy the three files of a recording off the card, keeping them in one
   folder.
2. Open the `.mp4` in walksnail-osd-tool (github.com/avsaase/walksnail-osd-tool).
   It finds the `.osd` with the same name; otherwise, load it by hand.
3. Pick the font for your flight controller firmware (the `osd_fc` you
   set, Betaflight by default) and render.

The `.srt` opens in any video player as a subtitle track.

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
