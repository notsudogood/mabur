#ifndef MABUR_PLAYER_CONFIG_H_
#define MABUR_PLAYER_CONFIG_H_

#include <string>
#include <vector>

namespace maburplay {

struct DvrCfg {
  // Begin recording as soon as parameters arrive. NOT "the DVR exists":
  // the DVR is always available, and autostart:false is a live, armed
  // player waiting for a press on the input.rec button. There is
  // deliberately no config kill switch -- autostart:false with no button
  // configured is a player that never records, by a simpler route.
  bool autostart = true;
  std::string dir = "/media/dvr";
  int fragment_ms = 1000;
  // "raw"    — remux the received AUs untouched (byte-exact, the default).
  // "burned" — transcode with the MSP OSD composited in by the encoder.
  std::string mode = "raw";
  // Raw mode only (2026-10-10, osd_recorder.h): write the flight
  // controller's OSD next to the video as a Walksnail-format .osd, plus an
  // .srt link summary, same folder and name, for walksnail-osd-tool.
  bool osd_file = true;
  std::string osd_fc = "BTFL";  // .osd header firmware tag: BTFL, INAV or ARDU
  struct BurnedCfg {
    int bitrate_kbps = 12000;
    int fps_cap = 30;   // encode is capped independently of display rate
  } burned;
  // Where the record button records:
  // "gs" = this GS's DVR only, "vtx" = the drone's SD card only, "both".
  std::string target = "gs";
};

// MSP DisplayPort OSD. `port` must match maburgs' msp.out.port -- separate
// daemons, separate config files, so this pairing is a deploy-time
// invariant neither binary can validate on its own.
struct OsdCfg {
  // On by default, like the shipped maburplay.default.toml: a config file
  // that lost its [osd] sections (a FAT /config damaged by a power cut
  // truncated one to 0 bytes, 2026-10-17) must not silently drop the OSD.
  bool enable = true;
  int port = 14560;
  std::string font = "/usr/local/share/mabur/font_btfl.mfont";
  std::string scale = "sharp";  // "sharp" | "fill"
  // Blank the OSD after this much silence; 0 = never. MUST stay several
  // multiples of the drone's msp.update_rate_hz period (default 1 Hz = one
  // snapshot per second, so 5000 = 5 missed snapshots): at ~2x the period a
  // SINGLE dropped snapshot blanks the whole overlay and the next one
  // repaints it, which reads as a strobe rather than as staleness.
  int stale_ms = 5000;

  // GS link-status overlay. Independent of the MSP OSD above: either may be
  // enabled alone, and a GS-only configuration is the natural one for an
  // aircraft with no MSP-capable FC.
  //
  // `port` must equal one of maburgs' stats.out ports -- separate daemons,
  // separate config files, so this pairing is a deploy-time invariant
  // neither binary can validate on its own, exactly like osd.port <->
  // msp.out.port. The 10 s silence warning is the mitigation.
  struct GsCfg {
    bool enable = true;   // same reason as OsdCfg::enable
    int port = 8302;
    std::string font = "/usr/local/share/mabur/gs_osd.gfont";
    // Dim (never blank) after this much sideport silence. 3000 = 6 missed
    // samples at the 500 ms sideport cadence.
    int stale_ms = 3000;
  } gs;
};

// GPIO buttons. One button, one job: toggle the DVR.
struct InputCfg {
  struct RecCfg {
    // False when the config has no input.rec block at all, which is the
    // shipped default -- a ground station with no button wired.
    bool configured = false;
    // Header pin number, resolved to a gpiochip + line offset at startup
    // by matching the kernel's line names (PIN_<n> / GPIO<n> / <n>). The
    // Radxa ZERO 3 names its 40-pin header lines PIN_7..PIN_40 across
    // gpiochip1/3/4.
    int pin = 0;
    // Defaults describe a button between the pin and GND with the kernel's
    // internal pull-up: the line is requested ACTIVE_LOW so "pressed"
    // reads 1. Overridable because goggle builds differ and a silently
    // inverted button is a miserable bug to chase.
    bool active_low = true;
    std::string bias = "pull-up";  // "pull-up" | "pull-down" | "none"
  } rec;
};

struct DisplayCfg {
  // Phase-aware release delay for decoded frames (frame_regulator.h):
  // present at floor(pts) + regulate_ms instead of on arrival. 0 = off
  // (present the instant decode finishes, the pre-regulator behavior).
  // With vsync_lock this is the FALLBACK rule, used while the vblank
  // estimator is cold or stale.
  int regulate_ms = 12;
  // Servo release to next_vblank - vsync_lead_ms instead of the fixed
  // hold. lead covers
  // the present-path submission cost incl. the ~2 ms main-loop tick.
  bool vsync_lock = true;
  // Default 6 (bench-measured): with release-deadline wakeups
  // the submit lands ~0.2 ms after schedule, but decode work can still
  // block the drain for a few ms -- 6 keeps misses at ~0.2/s where 4
  // measured ~12/s and 9 wastes 3 ms of glass latency per frame.
  int vsync_lead_ms = 6;
  // Frames a sequential-slot chain may run before the regulator cuts it
  // with one dropped frame (frame_regulator.h chain_cuts). 0 = unbounded,
  // no chain cut at all. Default 3 (operator choice after the bench A/B in
  // docs/findings/2026-09-02-regulator-chain-budget.md: e2e p50 -3.9 ms for
  // 1.44 drops/s; 6 was -2.4 ms for 0.56/s; p99 unchanged at any value).
  int chain_budget = 3;
  // Genlock (docs/efficient-link-plan.md step 2, genlock.h): steer the
  // drone camera's frame rate onto this screen's refresh grid. The phase is
  // measured (and logged as the 1 Hz `genlock:` line) whenever vsync_lock
  // is on; this switch only decides whether a setpoint is sent. Needs the
  // drone's [genlock] enable too. Off until a bench shows the camera
  // follows.
  bool genlock = false;
  // Share of frames, in percent, allowed to miss the refresh the lock aims
  // them at (they show one refresh later). Lower = a later, safer phase.
  int genlock_miss_pct = 10;
};

// GS-side reverse of the drone's ColorTrans sensor tuning (docs/colortrans.md).
// One knob. The transform's constants are compiled in (colortrans.cpp,
// kColorTrans3); retuning is a rebuild. Default off so a config without the
// block boots unchanged.
struct ColorTransCfg {
  bool enable = false;
};

// [decoder]. stream = true hands a split
// picture to MPP slice by slice while maburgs still receives it (rkvdec2
// stream mode); false submits every AU whole, the non-streamed path. A/B and
// kill switch. Needs the stream-mode GS image (docs/slices.md); anywhere
// else the decoder's probe refuses and maburplay submits whole AUs anyway.
struct DecoderCfg {
  bool stream = true;
};

struct Config {
  std::string ring_path = "/dev/shm/mabur-au";
  std::string socket = "/run/mabur-au.sock";
  std::string backend = "mpp";            // "mpp" | "null"
  std::string screen_mode = "1920x1080@60";
  DvrCfg dvr;
  OsdCfg osd;
  InputCfg input;
  DisplayCfg display;
  ColorTransCfg colortrans;
  DecoderCfg decoder;
};

Config load_config(const std::string& path,
                   std::vector<std::string>* defaulted = nullptr);  // strict; throws like maburgs

}  // namespace maburplay

#endif  // MABUR_PLAYER_CONFIG_H_
