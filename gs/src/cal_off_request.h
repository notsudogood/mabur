#pragma once
// The GS menu's "Remove calibration", maburgs side: one request from the
// player ("cal off" on RecControl's port), held level-triggered in every RCF
// (Rcf::cal_off) until the drone's Telem stops reporting kTelemCalibrated.
// The drone side is drone/src/main.cpp (cal_apply.h remove_calibration);
// acting on it twice changes nothing, so no grace window is needed: any
// Telem without the flag settles the request. Pure, I/O-free.
#include <cstdint>

namespace maburgs {

class CalOffRequest {
 public:
  static constexpr uint64_t kTimeoutMs = 20000;

  void request(uint64_t now_ms) {
    pending_ = true;
    since_ms_ = now_ms;
    result_ = "sent";
  }

  void on_telem(bool calibrated, uint64_t /*now_ms*/) {
    if (pending_ && !calibrated) finish("removed");
  }

  // The RCF byte; ends a request that has run out of time.
  uint8_t wire(uint64_t now_ms) {
    if (pending_ && now_ms > since_ms_ && now_ms - since_ms_ > kTimeoutMs) finish("timeout");
    return pending_ ? 1 : 0;
  }

  bool pending() const { return pending_; }
  // "" (never asked), "sent", then "removed" or "timeout".
  const char* result() const { return result_; }

 private:
  void finish(const char* r) {
    pending_ = false;
    result_ = r;
  }

  bool pending_ = false;
  uint64_t since_ms_ = 0;
  const char* result_ = "";
};

}  // namespace maburgs
