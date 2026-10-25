#pragma once
// The GS menu's Camera tab, maburgs side: one preset request from the
// player ("camera <id>" on RecControl's port), held level-triggered in every
// RCF (Rcf::cam) until the drone's Telem settles it. The drone side is
// drone/src/camera_switch.h; the presets mabur/camera_presets.h.
//
// Settled by: the drone running the preset (switched), or an outcome ABOUT
// this preset (cam::state_preset) that refuses it or rolls it back. An
// outcome from the first kGraceMs is not trusted to refuse: it may be the
// drone's answer to an older request for the same preset, sent before this
// RCF reached it. A switch reboots the drone, so kTimeoutMs is generous.
// A clock reading older than the request (a replay's synthetic stamps)
// counts as inside the grace and never as a timeout.
// Pure, I/O-free.
#include <cstdint>

#include "mabur/camera_presets.h"

namespace maburgs {

class CameraRequest {
 public:
  static constexpr uint64_t kGraceMs = 2000;
  static constexpr uint64_t kTimeoutMs = 120000;

  // A new request replaces any pending one.
  void request(uint8_t preset, uint64_t now_ms) {
    wish_ = preset;
    since_ms_ = now_ms;
    result_ = "sent";
  }

  void on_telem(uint8_t cam_cur, uint8_t cam_state, uint64_t now_ms) {
    if (wish_ == mabur::cam::kNoWish) return;
    if (cam_cur == wish_) {
      finish("switched");
      return;
    }
    if (mabur::cam::state_preset(cam_state) != wish_) return;
    using S = mabur::cam::State;
    const S s = mabur::cam::state_of(cam_state);
    if (s == S::Applying) {
      result_ = "applying";
      return;
    }
    if (now_ms < since_ms_ || now_ms - since_ms_ < kGraceMs) return;
    if (s == S::NoArmState || s == S::Unavailable || s == S::BadConfig || s == S::RolledBack)
      finish(mabur::cam::state_name(s));
  }

  // The RCF byte; ends a request that has run out of time.
  uint8_t wire(uint64_t now_ms) {
    if (wish_ != mabur::cam::kNoWish && now_ms > since_ms_ && now_ms - since_ms_ > kTimeoutMs)
      finish("timeout");
    return wish_;
  }

  uint8_t wish() const { return wish_; }
  // "" (never asked), "sent", "applying", then the settled outcome:
  // "switched", "armed", "unavailable", "bad_config", "rolled_back", "timeout".
  const char* result() const { return result_; }
  // The preset the last request was for (kNoWish: none yet).
  uint8_t last() const { return last_; }

 private:
  void finish(const char* r) {
    result_ = r;
    last_ = wish_;
    wish_ = mabur::cam::kNoWish;
  }

  uint8_t wish_ = mabur::cam::kNoWish;
  uint8_t last_ = mabur::cam::kNoWish;
  uint64_t since_ms_ = 0;
  const char* result_ = "";
};

}  // namespace maburgs
