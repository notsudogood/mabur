#pragma once
// Camera presets the GS menu's Camera tab offers, and the wire's camera
// bytes (Rcf::cam, Telem::cam_*).
//
// A preset is a (venc.size, venc.fps) pair the drone writes into its own
// /etc/mabur.toml and boots into: a sensor mode change needs the encoder
// pipeline rebuilt, and the encoder stays claimed until a reboot, so a
// switch costs ~30 s without video and is refused while the FC reports
// ARMED. Which sensor mode serves each rate is camera_mode_for()
// (drone/venc/camera_modes.c, docs/camera-modes.md):
//
//   1080p 60 / 90   1920x1080 binned full view, either IMX415 driver
//   720p 100        1728x972 binned crop (1.11x zoom), waybeam's driver only
//   544p 120 wide   1728x816 binned crop (2.12:1), waybeam's driver only
//
// 1440p is not a preset: the IMX415's 1440-class modes are crops (stock
// 2560x1440@60, waybeam 2816x1584@60, ~1.4x zoom) and 2560x1440@60 asks the
// H.265 engine for ~2x its rated macroblock rate, past the 158 % that
// 1728x972@100 held on the bench (docs/findings/2026-10-19-waybeam-imx415-
// 1728x972.md). Encoder load per preset, same yardstick: 1080p60 ~117 %,
// 1080p90 ~176 % (above anything benched), 720p100 86 %, 544p120 ~71 %.
//
// Pure, header-only; the drone and the GS player both include it.
#include <cstddef>
#include <cstdint>

namespace mabur::cam {

struct Preset {
  uint8_t id;
  const char* label;  // the menu cell
  const char* view;   // the menu cell's second line
  uint16_t w, h, fps;
  bool needs_star6e;  // only waybeam's IMX415 driver has the sensor mode
};

inline constexpr Preset kPresets[] = {
    {0, "1080p 60", "full view", 1920, 1080, 60, false},
    {1, "1080p 90", "full view", 1920, 1080, 90, false},
    {2, "720p 100", "1.1x zoom", 1280, 720, 100, true},
    {3, "544p 120", "wide crop", 1152, 544, 120, true},
};
inline constexpr size_t kNumPresets = sizeof(kPresets) / sizeof(kPresets[0]);

// Rcf::cam: no camera request (the drone leaves its mode alone).
inline constexpr uint8_t kNoWish = 0xFF;
// Telem::cam_cur: the drone runs a size/fps that is no preset.
inline constexpr uint8_t kCustom = 0xFE;
// Telem::cam_cur: the drone has not reported (or predates the wire).
inline constexpr uint8_t kUnknown = 0xFF;

// Telem::cam_state: the outcome of the last request the drone handled,
// kept until the next one. Values are wire bytes; append only.
enum class State : uint8_t {
  Idle = 0,         // no request this boot
  Applying = 1,     // written, rebooting into it
  NoArmState = 2,   // refused: armed, or the FC's arm state went stale
  Unavailable = 3,  // refused: the loaded driver lacks the mode and none can be switched in
  BadConfig = 4,    // refused: the rest of mabur.toml does not load with it (record.*, ...)
  RolledBack = 5,   // the switch this boot failed to start video; back on the previous mode
  Switched = 6,     // this boot runs the mode a switch asked for
};

// Telem::cam_state carries the outcome in its low nibble and the preset it
// is about in its high nibble (kNoPreset: none), so the GS can tell the
// answer to its own request from an older one.
inline constexpr uint8_t kNoPreset = 0x0F;
inline constexpr uint8_t pack_state(State s, uint8_t preset_id) {
  return static_cast<uint8_t>(((preset_id & 0x0F) << 4) | (static_cast<uint8_t>(s) & 0x0F));
}
inline constexpr State state_of(uint8_t wire) { return static_cast<State>(wire & 0x0F); }
inline constexpr uint8_t state_preset(uint8_t wire) { return static_cast<uint8_t>(wire >> 4); }

inline const Preset* preset(uint8_t id) {
  return id < kNumPresets ? &kPresets[id] : nullptr;
}

// The preset a running size/fps is, or kCustom.
inline uint8_t preset_of(uint16_t w, uint16_t h, uint16_t fps) {
  for (const Preset& p : kPresets)
    if (p.w == w && p.h == h && p.fps == fps) return p.id;
  return kCustom;
}

inline const char* state_name(State s) {
  switch (s) {
    case State::Idle: return "idle";
    case State::Applying: return "applying";
    case State::NoArmState: return "armed";
    case State::Unavailable: return "unavailable";
    case State::BadConfig: return "bad_config";
    case State::RolledBack: return "rolled_back";
    case State::Switched: return "switched";
  }
  return "unknown";
}

}  // namespace mabur::cam
