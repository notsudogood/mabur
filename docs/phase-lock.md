# Phase lock

The GS steers the drone's sensor frame rate so that every frame's
capture-time slot sits a fixed margin before the vblank release it lands
on. Locked, glass latency is flat (no sensor/panel beat sawtooth) and the
regulator places frames by capture time, so late-tail frames keep their
slot. The evidence is `docs/findings/2026-10-17-phase-lock-spike.md` and
`docs/findings/2026-10-17-phase-lock-spike-2.md`.

## The loop

1. **maburplay measures.** For every frame, `FrameRegulator` computes
   `want = pts + delivery floor + allowance` (the lateness window, p1 and
   p99 − p1 + 1 ms capped at `display.sched_cap_ms`) and the release of
   the first vblank at or after it. The margin is release − want, in
   [0, P). It depends only on the capture clock, the window and the vblank
   grid, not on the frame's own arrival. It is measured in both placement
   modes.
2. **maburplay decides** (`gs/player/src/phase_lock.h`). Each 1 s window:
   - error = median of (margin − `display.phase_margin_ms`), wrapped into
     ±P/2;
   - a window with fewer than 10 margins is empty; one whose error spread
     (p90 − p10) exceeds 4 ms is incoherent: it never counts toward
     locking, counts toward unlocking and leaves the offset alone;
   - the phase slope (least squares, skipping the first 150 ms while the
     last offset reaches the drone) names the offset that would match the
     panel, n = o − slope / G with G = 1/60 ms/s per mfps (a rate error of
     D mfps at ~60 fps moves the phase D/60000 s per s); windows whose fit
     residual exceeds 1 ms RMS give no slope;
   - `centre` = EMA of n (α 0.3), the offset sent = centre − 30 mfps per
     ms of error, the correction clamped to ±60 mfps while acquiring and
     ±20 while locked, the whole clamped to ±200 mfps.
   The sensor only takes whole lines, so near lock the line count toggles
   between neighbours.
3. **maburgs forwards.** maburplay sends `phase <mfps>` to RecControl
   (UDP 127.0.0.1:8401) on change and every second. maburgs puts the value
   in every RCF; after 3 s without a `phase` line it sends 0.
4. **The drone applies.** `RcAgent` hands each new `snr_off` to venc. The
   encoder thread sets the sensor to the trim request plus the offset,
   clamped to ±200 mfps and never above the sensor mode's ceiling (maxFps
   × 1000: a mode whose maximum is its nominal rate, like 1728x972@100,
   has the driver refuse anything faster), once the boot trim has locked
   (or given up): one sensor writer. 0 restores the trim's own request. Link loss keeps the
   last value. A refused value is not retried until another request succeeds.

## States

| state | entered when | offset | placement |
|---|---|---|---|
| wait | start; 2 s without a non-empty window; a pts discontinuity (centre and offset reset to 0) | held | arrival |
| acq | the first non-empty window after wait; lock lost | centre − clamp(30·e, ±60) | arrival |
| lock | \|e\| < 1 ms for 3 coherent windows | centre − clamp(30·e, ±20) | capture |
| oor | acquiring for 60 s without locking | 0 (the drone runs on its trim) | arrival |

lock → acq after 2 windows with \|e\| > 3 ms or incoherent. oor → wait
after 60 s. An unreachable panel (50 Hz, 59 Hz) never produces a coherent
window and ends in oor.

## Rate mismatch

The player measures the drone's frame rate (median pts step over 1 s,
`SourceRate`) and compares it with the panel's refresh (`RateMatch`,
`gs/player/src/rate_match.h`). More than 0.25 Hz apart (the steer reaches
±0.2 fps) for 3 consecutive seconds is a mismatch: a drone `venc.fps` that
does not match `screen_mode`, or a display that fell back to another mode
because the sink does not offer the requested one. While it lasts the lock
is held in wait and sends nothing (maburgs' RCF `snr_off` falls to 0 after
its 3 s timeout, so the drone runs on its trim), capture-time placement is
off, the OSD fps cell reads `src/panel` in caution, and the player logs
`maburplay: rate mismatch: source X Hz, panel Y Hz -- phase lock paused`
(and `... cleared` once 3 matched seconds follow). A drone reporting
low-power is never a mismatch: its rate is low on purpose. The check runs
whether or not the GS OSD is up; the low-power flag comes from its sideport
reader and counts only while fresh. With the reader up but the sideport
quiet the check holds its last decision; with no reader at all the drone is
taken as armed. A pair close enough to call a match that the lock still
gives up on (out of range: the steer cannot reach the panel, e.g. a mode
whose ceiling sits just under it) tints the OSD fps cell caution too.

A sensor that refuses every `SetFps` (or a drone build with no venc
steering at all) never moves, so the centre walks to the range limit
without ever producing a coherent window: the lock unlocks, goes oor
after 60 s, and retries a minute later -- a steady acq/oor cycle visible
in `reg.log`, paired with a `[sensor] steer: ... refused` line on the
drone.

## Wire

`Rcf::snr_off`: int16 little-endian at RCF head offset 15, milli-fps on the
drone's trim request, 0 = no steering. `RC_VERSION` 16. The web GS and
spotters always send 0.

## Deploy

`RC_VERSION` 16 is a flag day for `maburd`, `maburgs` and the hosted web GS
page: finish the deploy on both devices (and republish the web page) before
judging the link, per `docs/deploy.md`. On the GS, the `maburplay` binary
goes in before its config -- an older binary exits on the new
`[display]` keys (`phase_lock`, `phase_margin_ms`).

## Configuration

`maburplay.toml` `[display]`: `phase_lock` (code false, bundle true),
`phase_margin_ms` (code and bundle 4, range 1-10), `sched_cap_ms` (bundle
16; with the lock on, capture placement runs only while locked).
`vsync_lead_ms` (code and bundle 5) is the release's lead before the
vblank, the only one -- recording runs under the same lead as live view.
It covers the release thread's wake lateness (`reg.log` `rlate=`), the
present() path and the kernel's latch deadline. `rt_priority` (code and
bundle 10, range 0-50) is the release thread's `SCHED_FIFO` priority; 0
runs it at ordinary priority. A refused request (the process lacks
`CAP_SYS_NICE`) is logged once and the thread runs at ordinary priority
(`rt=0`).
`phase_lock` needs `vsync_lock = true`, `sched_cap_ms > 0` and
`regulate_ms > 0`, or the player refuses to start.

`screen_mode` is `WIDTHxHEIGHT@RATE`, RATE an integer or a decimal. The
presenter picks, among non-interlaced modes of that size within 0.5 Hz of
RATE, the one with the closest exact rate (from the pixel clock and
totals): a sink offering 60.000 and 59.94 under "@60" gets 60.000, `@59.94`
forces 59.94, a 59.94-only sink still matches "@60" and the lock steers
the camera there (~3 lines, centre near −50 mfps). It logs
`DrmPresenter: mode 1920x1080 @60.000 Hz (requested 1920x1080@60)`.

The drone has no phase-lock config.

## Display actuator (`display.phase_actuator = "display"`, the default since gs23)

The same controller can move the GS's HDMI pixel clock instead of the
camera: the panel follows the camera, and the RCF's `snr_off` stays 0, so
the drone runs on its boot trim. It exists for camera modes whose ceiling
is their nominal rate, which the sensor steer can only slow down: the
Camera tab's Crop 60 (2816x1584 crop, ceiling 60.000 fps) and 1728x972@100.
Flights 0019-0021 flew Crop 60: the lock held 2-3 % of armed seconds,
gave up (`oor`) for 20-45 %, and its offset sat at the +200 mfps limit.
It was opt-in in gs22 and is the default in code and bundle since gs23;
wherever it cannot run, the sensor steer runs instead, as before gs22.

At start and after every display hotplug maburplay names the attached
sink from its EDID (`edid.h`: the connector's EDID property, else sysfs)
and logs it with the mode:

    maburplay: display sink: "<monitor name>" <MFR>-<product>, HDMI, 1080p60 also as YCbCr 4:2:0 (at start, 60.000 Hz, 148500 kHz)

The name is the EDID's monitor-name descriptor, the id its maker code and
product code (present even without a name), then HDMI or DVI, then what
the EDID's 4:2:0 blocks say about 1080p60. A sink listed in
`display.trim_skip_sinks` (name or id, any case) stays on the sensor
steer; every other sink gets the trim. That list is the per-sink escape
for a display that blanks or flickers under the trim, which the GS cannot
see itself.

How it works (`hdmi_trim.h`; evidence in
`docs/findings/2026-10-22-hdmi-clock-trim-spike.md` and
`2026-10-24-display-trim-servo-spike.md`):

- HPLL in the RK3566's PMUCRU clocks only the HDMI port and the HDMI PHY.
  Right after each presenter init, maburplay moves it from the kernel's
  integer 148.5 MHz setting (fbdiv 99, postdivs 4x4) to the same rate at
  postdivs 4x3, fbdiv 74 + 0.25, just before the presenter's own modeset
  (so the sink's one re-sync rides the modeset's). A PLL already at that
  point (a player restart) is kept, with no re-sync.
- Once that first modeset is in (splash or first frame), the trim is
  checked again: the PLL now holds the kernel's setting for the mode
  actually set. Still at the point: nothing happens. Reprogrammed by the
  modeset (the boot or parked mode differed, e.g. the 123.75 MHz mode the
  kernel parks the port on while the player is down): it moves now (one
  more re-sync) or is refused, and the log says
  `... after the first modeset`.
- Some sinks get the same 1080p60 mode with HPLL at half that rate,
  74.25 MHz (the kernel's setting with refdiv 2); HDZero goggles do. The
  video port then runs at half the pixel clock, which is how the kernel
  drives a YCbCr 4:2:0 link (not confirmed on those goggles). The refresh
  still follows HPLL 1:1, so the same trim applies at a second point:
  fbdiv 74 + 0.25 (the same VCO, 1782 MHz) over postdivs 6x4. Which point
  is used follows what the PLL reads; the engage line on stderr names it
  (`HPLL 74.25 MHz (half rate ...)`).
- Every trim after that writes the 24-bit fraction alone: a step inside
  one feedback divider does not make the sink re-sync (bench: up to
  2500 ppm in one write); crossing a divider does.
- offset (mfps of camera rate) -> ppm of pixel clock = -offset x 1e6 /
  (panel Hz x 1000): a faster panel moves the phase like a slower camera.
  16.7 ppm per mfps at 60 Hz.
- Bounded by `display.trim_max_ppm` (2000 = +-120 mfps at 60 Hz; 1000
  in gs22, raised because a bench lock against Crop 60 needed -1050). A
  clamped request is fed back to the lock (`PhaseLock::set_applied`), so
  the window is read against what is in force and the centre settles on
  the real need instead of winding up (the 10-24 spike's 22 s clamp wound
  it to 139 mfps); `maburplay: display trim at its bound` on stderr.
- 720p60/50 modes (74.25 MHz pixel clock) use the 74.25 MHz point at
  full rate; 1080p50 shares 1080p60's 148.5 MHz clock and points.
- Refused, with the sensor steer kept and the reason on stderr: a sink
  in `display.trim_skip_sinks`, not an RK3566, `/dev/mem` unavailable, a
  mode other than 148.5 or 74.25 MHz (100 Hz, 59.94 Hz), a PLL setting
  other than the kernel's two (148.5 and 74.25 MHz) or their trim points. A trim point the PLL does not take is
  undone to the kernel's setting it came from and not retried.
- Rate mismatch, playback or a lost source: back to 0 ppm. Exit: 0 ppm
  at the trim point (the same rate as the kernel's setting).

Against it:

- Sink tolerance is per sink and invisible to the GS. Avatar Goggles X at
  1080p60 held a 164 s run at -70..-300 ppm with no blank beyond the one
  at the point move. At 100 Hz the same goggles blanked repeatedly after
  excursions of -1000..-1800 ppm, cause never found; 100 Hz modes have no
  trim point here. If a display blanks, add its name or id from the
  `display sink:` line to `trim_skip_sinks`, or set
  `phase_actuator = "sensor"` for every display.
- Default on means every GS display gets the trim, including ones never
  tested with it. The bound doubled to 2000 ppm; Avatar Goggles X at 60 Hz
  were only ever run at -70..-300 ppm.
- The EDID can be missing (the Avatar Goggles X's sysfs `edid` read back
  empty in the 10-22 spike); the line then says `no EDID` and the skip
  list cannot name that sink. The trim itself does not need the EDID.
- The 74.25 MHz point has not run on hardware. Its arithmetic is the
  148.5 MHz point's (unit-tested on the register values the HDZero
  goggles read), but nothing on the bench has shown how a 4:2:0 sink
  takes a moving clock. The bench lock (an HDMI capture card, full rate)
  needed -1050 ppm against Crop 60. The 720p point is the same setting,
  untested at 720p.
- How far Crop 60 sits from the panel is unmeasured: the drone's
  `[sensor] cal:` line gives its trim, and `steer: offset ... clamped to N`
  how much room the sensor had. A need past the bound leaves the lock
  saturated and acquiring, as unlocked as before.
- The controller's gains are sized for a sensor that steps in whole lines;
  on the continuous panel they dither +-170..400 ppm second to second
  (bench: nothing visible).

## Threads

maburplay runs four loops, so the release deadline never waits on
anything slower than itself:

- **Feed thread** (`FeedLoop`, `feed_loop.h`): blocks on the AU doorbell,
  reads the ring, and makes every decoder *input* call (`AuRouter`,
  `StreamFeeder`). It posts submit notes and delivery notes.
- **Decode-output thread** (`DecodePump`, `decode_pump.h`): drives
  `backend->poll()` (MPP blocks in it up to 4 ms waiting for a picture)
  and returns released frames to the decoder -- the decoder's whole output
  side. Per decoded frame, in order: the frame's submit notes (`lat`
  submit stamps, incomplete-AU flags for the lateness window),
  `lat.on_decoded`, the source-rate sample, the hand-over to the release
  thread, and then the framebuffer prime, so the release thread's
  present() finds the frame's framebuffer cached. It also takes the submit
  notes after every poll round, frame or not, so they never queue up in
  the feed while the decoder emits nothing.
- **Release thread** (`ReleaseLoop`, `release_loop.h`, `SCHED_FIFO` at
  `rt_priority`): owns the `FrameRegulator`'s decisions and every call
  into the display. It sleeps on an absolute timer armed to the next
  release (or a 10 ms idle tick), the hand-over queue and the display's
  flip events. Each wake: reap flips, release due frames, offer newly
  handed-over frames (a late one is presented at once), set the
  presenter's paced mode, run the chain-break heal, and commit a pending
  OSD on its own when no frame is held. A completed flip also feeds the
  burned recorder: the flip sink runs `RecorderTap::on_flip`
  (`recorder_tap.h`), which calls `BurnRecorder::submit()` on each
  recorded slot of the vblank grid (one buffer reference and a mailbox
  post). It never calls the decoder: displaced frames and frames the
  display is done with go back through a release queue the decode-output
  thread drains. Each offer's phase margin is queued for housekeeping.
- **Housekeeping** (the main loop, 2 ms): delivery notes (raw DVR, OSD
  figures), the record button, the phase lock itself (it takes the release
  thread's margins while the source and panel rates match, ticks and sends
  the offset), the rate match, OSD composition, the 1 Hz stats,
  `lat.log`/`frame.log`/`reg.log`, the decode watchdog and the display
  hotplug retry. It composes the screen's OSD only while no publish is
  pending, so it never draws into a buffer that is on screen or about to
  be; the burned recording's overlay, a canvas of its own at the record
  size, composes from the same input whether a publish is pending or not.
  It also installs and removes the recorder tap's hook (`RecorderTap::set`
  after `BurnRecorder::start()`, `clear()` before `stop()`), so no record
  start or stop runs on the release thread. A flush, a watchdog reset or
  re-creation, a hotplug reacquire and shutdown park the decode-output
  thread and then the release thread (which drops everything the display
  and the regulator hold), return every frame to the decoder, and resume
  them after; a thread that does not park within 2 s exits the player for
  respawn.

`--decode-only` (the hardware decode gate) and the host build have no
decode-output or release thread: one loop polls the decoder and releases
each frame on the spot.

## Observability

- `reg.log` (`# reglog 5`) regulator line: `lock=<off|wait|acq|lock|oor>
  lerr=<ms>ms loff=<mfps> lctr=<mfps>`, the release thread's
  `rlate= hand= rt= flushes= recstarts=`, the burned recorder's
  `burn_in= burn_enc= burn_drop=`, and `act=<sensor|display> trim=<ppm>`
  (plus `trim_sat=1` while the display trim is at its bound)
  (docs/observability.md "Regulator line").
- maburplay stderr: `maburplay: display sink: ...` once per presenter
  init (start and each hotplug), whatever the actuator.
- maburplay stderr: `maburplay: phase lock actuator: display|sensor -- ...`
  once per presenter init, with the reason when the display trim is
  refused or skipped, and again `after the first modeset` when that
  modeset changed the outcome.
- maburplay stderr: `phase-lock: <state> err= off= centre=` on each state
  change.
- Drone stats line: `snr_off=` (the offset on the sensor); `/tmp/mabur.log`
  `[sensor] steer:` lines for the first steer, clamps and refusals.
- `tools/flightreport.py` (session mode): PHASE LOCK — share of rows
  locked, acquire time, offset changes per minute, offset range, |lerr|
  while locked.
