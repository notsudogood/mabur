#pragma once
// The GS menu's Camera tab, drone side: the file half. camera_switch.h
// decides; this writes the decision and settles it on the next boot.
//
// A switch is a two-phase commit across a reboot:
//   apply    <conf>.cam-new is validated with load_config, the running file
//            is kept as <conf>.cam-prev, the new one replaces <conf>, and
//            <conf>.cam-pending names the preset (plus the driver flag the
//            switch set, if any). The caller then reboots.
//   commit   the next boot runs video: the marker goes (cam::State::Switched).
//   rollback the next boot cannot start the encoder, or encodes nothing for
//            kCamCommitWindowMs: <conf>.cam-prev goes back, the driver flag
//            the switch set goes, <conf>.cam-result says rolled_back for the
//            boot after (the caller reboots again).
#include <cstdint>
#include <optional>
#include <string>

#include "camera_switch.h"
#include "config.h"
#include "mabur/camera_presets.h"

namespace mabur {

// load_sigmastar insmods waybeam's IMX415 driver instead of the stock one
// while this file exists (openipc-builder's load_sigmastar).
inline constexpr const char* kStar6eFlag = "/etc/sensor_star6e";
inline constexpr const char* kStar6eKo = "/lib/modules/4.9.84/sigmastar/sensor_imx415_star6e.ko";

// A switched boot must encode this many frames before this long passes.
inline constexpr uint64_t kCamCommitFrames = 100;
inline constexpr uint64_t kCamCommitWindowMs = 25000;

struct CamFiles {
  std::string conf, next, prev, pending, result, star6e_flag;
  explicit CamFiles(const std::string& conf_path, const std::string& flag = kStar6eFlag)
      : conf(conf_path),
        next(conf_path + ".cam-new"),
        prev(conf_path + ".cam-prev"),
        pending(conf_path + ".cam-pending"),
        result(conf_path + ".cam-result"),
        star6e_flag(flag) {}
};

// The marker a switch left, or nullopt (none, or unreadable -- then it is
// removed so it cannot outlive its boot).
std::optional<CamPending> cam_take_pending(const CamFiles& f);

// The outcome a rollback left for this boot, read and removed: the wire
// byte cam::pack_state(RolledBack, preset), or pack_state(Idle, kNoPreset).
uint8_t cam_take_result(const CamFiles& f);

// Writes a switch to `p` over the running file (which loaded as `cur`).
// Applying on success; BadConfig with *err set when the result does not load
// or a write fails -- then nothing on disk has changed.
cam::State cam_apply(const CamFiles& f, const Config& cur, const cam::Preset& p,
                     bool driver_switch, std::string* err);

void cam_commit(const CamFiles& f);
void cam_roll_back(const CamFiles& f, const CamPending& p);

// Presets a driver switch would add: waybeam's list minus what is offered
// now, when its module is in the image (`star6e_ko_present`) and the stock
// IMX415 driver is the one loaded (`proc_modules`, /proc/modules' text).
uint8_t cam_switch_mask(uint8_t now_mask, const std::string& proc_modules, bool star6e_ko_present);

}  // namespace mabur
