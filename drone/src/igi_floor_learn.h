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
// Verification (d19, first-boot calibration): verify_ms of clean sending at
// 40 MHz, and separately at 80 MHz, with rung frames of that width and the
// radio tuned to it, marks that width verified at the current floor; the
// marks are saved with the floor. A stall at a width clears its mark and
// its clean time; the other width keeps its own (the floor only rises, so
// a width that was clean stays clean). The GS reads the marks from the
// drone's telemetry and holds a 40 or 80 MHz rung, while the drone is
// disarmed, until the width it flies is verified (gs/src/floor_cal.h).
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

// One watcher pass's view of the radio.
struct IgiPass {
  uint64_t now_ms = 0;
  int tuned_w = 0;        // the width the radio is tuned to (MHz)
  int rung_w = 0;         // the width of the current rung's frames (MHz)
  bool excused = false;   // a pause TX is allowed (retune, switch, sweep, a spike in the span)
  bool pending = false;   // frames waiting to go out
  uint64_t submitted = 0; // devourer's running TX submission count
};

// What one pass changed: the floor rose, and/or a width's verified mark
// was set or cleared (the caller saves the file on either).
struct IgiEvent {
  std::optional<uint8_t> raised;
  bool marks_changed = false;
};

// The saved per-unit state: the floor and the widths verified at it.
struct IgiSaved {
  uint8_t floor = 0;
  bool v40 = false, v80 = false;
};

class IgiFloorLearner {
 public:
  explicit IgiFloorLearner(IgiSaved start, uint32_t stall_ms = 250, uint32_t window_ms = 1000,
                           uint32_t settle_ms = 1000, uint32_t verify_ms = 10000)
      : floor_(start.floor), stall_ms_(stall_ms), window_ms_(window_ms), settle_ms_(settle_ms),
        verify_ms_(verify_ms) {
    verified_[0] = start.v40;
    verified_[1] = start.v80;
  }

  IgiEvent tick(const IgiPass& p) {
    IgiEvent ev;
    const bool moved = !have_ || p.submitted != last_submitted_;
    // Capped so a watcher that was itself descheduled does not count its
    // own lateness as blocked (or clean) time.
    const uint64_t dt = have_ ? std::min<uint64_t>(p.now_ms - last_tick_ms_, kMaxPassMs) : 0;
    have_ = true;
    last_submitted_ = p.submitted;
    last_tick_ms_ = p.now_ms;
    const bool wide = p.tuned_w >= 40 && !p.excused;
    if (!wide || p.now_ms < settle_until_ms_) {
      clear_();
      return ev;
    }
    const int slot = p.tuned_w >= 80 ? 1 : 0;
    // Clean time: this width's frames going out, nothing held.
    if (moved && p.rung_w == p.tuned_w) {
      clean_ms_[slot] += dt;
      if (!verified_[slot] && clean_ms_[slot] >= verify_ms_) {
        verified_[slot] = true;
        ev.marks_changed = true;
      }
    }
    // Rolling window: the blocked passes that ended in the last window_ms.
    if (p.pending && !moved && dt > 0) {
      blocked_.push_back({p.now_ms, dt});
      blocked_ms_ += dt;
    }
    while (!blocked_.empty() && p.now_ms - blocked_.front().first >= window_ms_) {
      blocked_ms_ -= blocked_.front().second;
      blocked_.pop_front();
    }
    if (blocked_ms_ < stall_ms_) return ev;
    ++stalls_;
    clear_();
    settle_until_ms_ = p.now_ms + settle_ms_;
    clean_ms_[slot] = 0;
    if (verified_[slot]) {
      verified_[slot] = false;
      ev.marks_changed = true;
    }
    if (floor_ >= kIgiFloorTop) {
      top_stalls_ += 1;
      return ev;
    }
    floor_ = igi_floor_next(floor_);
    ev.raised = floor_;
    return ev;
  }

  uint8_t floor() const { return floor_; }
  IgiSaved saved() const { return IgiSaved{floor_, verified_[0], verified_[1]}; }
  bool verified(int width_mhz) const { return verified_[width_mhz >= 80 ? 1 : 0]; }
  // Clean time toward the mark at 40 / 80 MHz since the last stall there.
  uint64_t clean_ms(int width_mhz) const { return clean_ms_[width_mhz >= 80 ? 1 : 0]; }
  uint32_t verify_ms() const { return verify_ms_; }
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
  uint32_t stall_ms_, window_ms_, settle_ms_, verify_ms_;
  bool have_ = false;
  uint64_t last_submitted_ = 0;
  uint64_t last_tick_ms_ = 0;
  std::deque<std::pair<uint64_t, uint64_t>> blocked_;  // (pass end, ms blocked)
  uint64_t blocked_ms_ = 0;
  uint64_t settle_until_ms_ = 0;
  uint64_t stalls_ = 0;
  uint64_t top_stalls_ = 0;
  bool verified_[2] = {false, false};
  uint64_t clean_ms_[2] = {0, 0};
};

// The saved per-unit state: one line, the floor (decimal or 0x hex) and
// then "v40" / "v80" for the widths verified at it ("0x24 v40 v80"). A d18
// file is the floor alone. Missing, unreadable or out of range = nothing
// learned.
inline std::optional<IgiSaved> igi_floor_load(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return std::nullopt;
  char buf[64] = {0};
  const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  buf[n] = 0;
  char* end = nullptr;
  const long v = std::strtol(buf, &end, 0);
  if (end == buf || v < 0 || v > 127) return std::nullopt;
  IgiSaved s;
  s.floor = static_cast<uint8_t>(v);
  const std::string rest(end);
  s.v40 = rest.find("v40") != std::string::npos;
  s.v80 = rest.find("v80") != std::string::npos;
  return s;
}

// Writes `s` atomically (temp file + rename). false on any failure.
inline bool igi_floor_save(const std::string& path, const IgiSaved& s) {
  const std::string tmp = path + ".tmp";
  std::FILE* f = std::fopen(tmp.c_str(), "w");
  if (!f) return false;
  const bool ok = std::fprintf(f, "0x%02x%s%s\n", static_cast<unsigned>(s.floor),
                               s.v40 ? " v40" : "", s.v80 ? " v80" : "") > 0;
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
