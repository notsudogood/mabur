#ifndef MABUR_PLAYER_RATE_MATCH_H_
#define MABUR_PLAYER_RATE_MATCH_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace maburplay {

// The drone's frame rate as the player sees it: the median pts step over
// 1 s windows, so a dropped frame (a doubled step) does not move it.
// Pure logic: the caller feeds every frame's pts and its own mono clock.
class SourceRate {
 public:
  static constexpr uint64_t kWindowUs = 1'000'000;
  static constexpr uint32_t kMaxStepUs = 1'000'000;  // longer = a restart
  static constexpr size_t kMinSteps = 10;

  void on_pts(uint32_t pts_us, uint64_t now_us) {
    if (have_prev_) {
      const uint32_t step = pts_us - prev_pts_;
      if (step > 0 && step <= kMaxStepUs) steps_.push_back(step);
    } else {
      win_start_us_ = now_us;
    }
    have_prev_ = true;
    prev_pts_ = pts_us;
    if (now_us - win_start_us_ >= kWindowUs) {
      if (steps_.size() >= kMinSteps) {
        auto mid = steps_.begin() + static_cast<long>(steps_.size() / 2);
        std::nth_element(steps_.begin(), mid, steps_.end());
        hz_ = 1e6 / static_cast<double>(*mid);
      }
      steps_.clear();
      win_start_us_ = now_us;
    }
  }
  // 0 until the first window closes.
  double hz() const { return hz_; }

 private:
  std::vector<uint32_t> steps_;
  bool have_prev_ = false;
  uint32_t prev_pts_ = 0;
  uint64_t win_start_us_ = 0;
  double hz_ = 0.0;
};

// Whether the panel rate is within the phase lock's reach of the source
// rate. The steer moves the sensor at most +-0.2 fps around its trim, so a
// source further than kReachHz from the panel can never lock: a drone fps
// that does not match the display (or a display that fell back to another
// rate). It flips only after kFlipWindows consecutive windows agree, a
// drone in low-power (lower fps on purpose) is never a mismatch, and with
// no current low-power report it holds whatever it last decided.
class RateMatch {
 public:
  static constexpr double kReachHz = 0.25;
  static constexpr int kFlipWindows = 3;

  // The drone's low-power report: kUnknown when its sideport is quiet.
  enum class LowPower { kNo, kYes, kUnknown };

  // Once per second; 0 for a side not measured yet.
  void on_window(double src_hz, double panel_hz, LowPower low_power) {
    if (low_power == LowPower::kYes) {
      mismatch_ = false;
      run_ = 0;
      return;
    }
    // No fresh low-power report, or a side not measured yet: no evidence
    // either way, so hold the current state.
    if (low_power == LowPower::kUnknown || src_hz <= 0.0 || panel_hz <= 0.0) {
      run_ = 0;
      return;
    }
    const bool out = std::fabs(src_hz - panel_hz) > kReachHz;
    if (out == mismatch_) {
      run_ = 0;
      return;
    }
    if (++run_ >= kFlipWindows) {
      mismatch_ = out;
      run_ = 0;
    }
  }
  bool mismatch() const { return mismatch_; }

 private:
  bool mismatch_ = false;
  int run_ = 0;
};

// What the player knows about the drone's low-power state. reader: a
// sideport reader exists (the GS OSD's stats source); fresh: it heard the
// drone recently; low_power: the flag it last heard. Without any reader no
// report can ever arrive, so the drone is taken as armed and the mismatch
// check still runs; a reader gone quiet knows nothing.
inline RateMatch::LowPower low_power_report(bool reader, bool fresh, bool low_power) {
  if (!reader) return RateMatch::LowPower::kNo;
  if (!fresh) return RateMatch::LowPower::kUnknown;
  return low_power ? RateMatch::LowPower::kYes : RateMatch::LowPower::kNo;
}

}  // namespace maburplay

#endif  // MABUR_PLAYER_RATE_MATCH_H_
