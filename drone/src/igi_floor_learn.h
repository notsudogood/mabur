#pragma once
// Per-unit wide-TX IGI floor, learned (radio.igi_floor_auto, d18).
//
// devourer's 40 MHz stall: on a quiet channel, at full receiver gain, a
// Jaguar3 radio can park where its secondary 20 MHz never reads idle and the
// MAC holds the first 40/80 MHz frame forever. Raising the DIG floor (IGI)
// clears it, and the edge is PER UNIT (docs/experiments/8822c-ht40-tx-stall.md
// in devourer: four radios, edges from "never" to above 0x2c; here drone 1
// stalled on 7 of 20 promotes onto 40 MHz with no floor and drone 2 never).
// Flying every unit at 0x35, the worst unit's fix, costs the others receiver
// sensitivity at 40/80 MHz for nothing.
//
// So the floor starts where this unit last needed it (0 = off on a unit
// that never stalled) and climbs one step each time a stall is seen:
// 0 -> 0x24 -> 0x2c -> 0x35 (0x35 cleared every unit measured; nothing
// above it is tried). It never comes down by itself: deleting the saved
// value relearns from off.
//
// A stall, as maburd sees it: tuned 40/80 MHz, frames waiting to go out,
// and devourer's TX submission count not moving. maburd sends with
// no_cancel_multipkt, so a held frame blocks its sender rather than timing
// out and the failure count never moves; with every sender blocked,
// submissions stop. The measure is blocked time, not one long gap: on the
// bench another station's traffic released a hang at once, and in the air
// the GS's own uplink frames (~19 a second) may do the same, so a stalled
// unit can send in short bursts between hangs instead of stopping. Each
// watcher pass (~25 ms) that finds frames waiting and no submission since
// the last pass adds its time; stall_ms of that inside the last window_ms
// is a stall. A full freeze takes stall_ms; bursts at half duty about twice that.
// A healthy sender at 40/80 MHz frees a chip FIFO slot in about a
// millisecond, so it is not blocked for a whole pass. The caller excuses the
// moments TX legitimately pauses (a width switch or retune in flight or just
// done, a calibration sweep) by passing wide = false.
//
// Not measured: whether raising the floor releases a frame the MAC is
// already holding (devourer's bench pinned IGI before TX; there, traffic
// from another station released a hang at once, which suggests a gain
// change does). If it does not, the GS's latency guard demotes the rung as
// it did before the floor existed, and the next 40 MHz promote flies at the
// new floor. And not measured: how often a busy channel (the drone deferring
// to other 802.11 traffic) reads as a stall here; each false one costs a
// step of uplink sensitivity at 40/80 MHz on that unit until the saved file
// is deleted.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <optional>
#include <string>
#include <utility>

namespace mabur {

inline constexpr uint8_t kIgiFloorTop = 0x35;

// The next step up from `cur`; kIgiFloorTop stays put.
inline uint8_t igi_floor_next(uint8_t cur) {
  if (cur < 0x24) return 0x24;
  if (cur < 0x2c) return 0x2c;
  return kIgiFloorTop;
}

class IgiFloorLearner {
 public:
  IgiFloorLearner(uint8_t start, uint32_t stall_ms = 250, uint32_t window_ms = 1000,
                  uint32_t settle_ms = 1000)
      : floor_(start), stall_ms_(stall_ms), window_ms_(window_ms), settle_ms_(settle_ms) {}

  // One watcher pass. wide: tuned 40/80 MHz and nothing excuses a TX
  // pause; pending: frames are waiting to go out; submitted: devourer's
  // running TX submission count. Returns the new floor when it rises.
  std::optional<uint8_t> tick(uint64_t now_ms, bool wide, bool pending, uint64_t submitted) {
    const bool moved = !have_ || submitted != last_submitted_;
    // Capped so a watcher that was itself descheduled does not count its
    // own lateness as blocked time.
    const uint64_t dt = have_ ? std::min<uint64_t>(now_ms - last_tick_ms_, kMaxPassMs) : 0;
    have_ = true;
    last_submitted_ = submitted;
    last_tick_ms_ = now_ms;
    if (!wide || now_ms < settle_until_ms_) {
      clear_();
      return std::nullopt;
    }
    // Rolling window: the blocked passes that ended in the last window_ms.
    if (pending && !moved && dt > 0) {
      blocked_.push_back({now_ms, dt});
      blocked_ms_ += dt;
    }
    while (!blocked_.empty() && now_ms - blocked_.front().first >= window_ms_) {
      blocked_ms_ -= blocked_.front().second;
      blocked_.pop_front();
    }
    if (blocked_ms_ < stall_ms_) return std::nullopt;
    ++stalls_;
    clear_();
    settle_until_ms_ = now_ms + settle_ms_;
    if (floor_ >= kIgiFloorTop) {
      top_stalls_ += 1;
      return std::nullopt;
    }
    floor_ = igi_floor_next(floor_);
    return floor_;
  }

  uint8_t floor() const { return floor_; }
  // Stalls seen, and those seen with the floor already at the top step.
  uint64_t stalls() const { return stalls_; }
  uint64_t top_stalls() const { return top_stalls_; }

 private:
  static constexpr uint64_t kMaxPassMs = 100;
  void clear_() {
    blocked_.clear();
    blocked_ms_ = 0;
  }
  uint8_t floor_;
  uint32_t stall_ms_, window_ms_, settle_ms_;
  bool have_ = false;
  uint64_t last_submitted_ = 0;
  uint64_t last_tick_ms_ = 0;
  std::deque<std::pair<uint64_t, uint64_t>> blocked_;  // (pass end, ms blocked)
  uint64_t blocked_ms_ = 0;
  uint64_t settle_until_ms_ = 0;
  uint64_t stalls_ = 0;
  uint64_t top_stalls_ = 0;
};

// The saved per-unit value: one number (decimal or 0x hex) in a small file
// next to mabur.toml. Missing, unreadable or out of range = nothing learned.
inline std::optional<uint8_t> igi_floor_load(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return std::nullopt;
  char buf[32] = {0};
  const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  buf[n] = 0;
  char* end = nullptr;
  const long v = std::strtol(buf, &end, 0);
  if (end == buf || v < 0 || v > 127) return std::nullopt;
  return static_cast<uint8_t>(v);
}

// Writes `v` atomically (temp file + rename). false on any failure.
inline bool igi_floor_save(const std::string& path, uint8_t v) {
  const std::string tmp = path + ".tmp";
  std::FILE* f = std::fopen(tmp.c_str(), "w");
  if (!f) return false;
  const bool ok = std::fprintf(f, "0x%02x\n", static_cast<unsigned>(v)) > 0;
  if (std::fclose(f) != 0 || !ok) {
    std::remove(tmp.c_str());
    return false;
  }
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

// "<dir of cfg_path>/mabur.igi_floor" ("mabur.igi_floor" for a bare name).
inline std::string igi_floor_path(const std::string& cfg_path) {
  const size_t slash = cfg_path.find_last_of('/');
  return slash == std::string::npos ? std::string("mabur.igi_floor")
                                    : cfg_path.substr(0, slash + 1) + "mabur.igi_floor";
}

}  // namespace mabur
