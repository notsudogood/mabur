# GS on-screen menu (minimal, gs10)

Backlog item 9, first cut. Press the 5-way stick's centre: a box opens
over the video.

| Row | Left / Right (or Centre) |
|---|---|
| Channel hop | On (auto channel + in-flight hop) / Off (stay on one channel) |
| Channel | the channel to stay on while hop is Off (members of `radio.channels`) |
| Bandwidth | Wide (80 MHz rungs on) / Normal (up to MCS4/40); fixed when `radio.width` is not 80 |
| Apply | saves and restarts the link (~5 s of no video) |
| Close | leaves without changes (it also closes after 20 s untouched) |

Apply writes `/etc/maburgs.menu` (`hop=`, `channel=`, `bandwidth=`) and
stops maburgs; its respawn loop starts it again in ~2 s and maburgs applies
the file over `/etc/maburgs.toml`, logging `maburgs: menu (...): ...` per
change. A choice that would not validate is logged and ignored. Delete the
file to go back to the TOML as written.

Setup: maburplay's `[input.menu]` names the five header pins. The bundle
carries the RunCam WiFiLink's (PixelPilot's map: up 16, down 18, left 13,
right 38, centre 11, active high); delete the section for no menu. The
menu needs the GS overlay (`osd.gs.enable`) to be seen.

`bias = "none"` leaves each pin's pull as the board set it, as PixelPilot's
libgpiod request does. On gs10 it switched the pull off instead, and the
stick did nothing on the operator's RunCam (2026-10-21): a switch to 3.3 V
with no pull-down floats. `"disable"` is that old behaviour, kept as an
explicit value. At startup each line logs
`maburplay: menu up: PIN_16 -> /dev/gpiochip3 line 9, bias none, idle level 0`
(1 = it reads pressed untouched: wrong polarity or pull), and every
accepted press logs `maburplay: menu key <name>`.

Limits: the box clears what is under it, and a flight-controller OSD cell
there comes back only when it next changes. Resolution, frame rate,
Spectrum and the rest of the v5 mock-up are not in this cut.
