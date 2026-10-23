#ifndef MABURGS_TRANSIT_WINDOW_H_
#define MABURGS_TRANSIT_WINDOW_H_

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <limits>
#include <vector>

namespace maburgs {

// Rolling median of per-AU drone-to-GS transit (ms above the session's best:
// the drone's TX queue wait + radio + air), over the last window_ms of AU
// closures. The promote latency guard's input (LinkHealth::transit_ms).
// Unlike LatWindow it is time-bounded, not flush-bounded, so the ladder can
// read it at its own (RCF) cadence while the sideport keeps its own window.
// Single-threaded (core loop).
class TransitWindow {
 public:
  explicit TransitWindow(double window_ms = 200.0, std::size_t min_n = 3)
      : window_ms_(window_ms), min_n_(min_n) {}

  void add(double now_ms, double transit_ms) {
    s_.push_back({now_ms, transit_ms});
    while (s_.size() > kCap) s_.pop_front();
  }

  // NaN when fewer than min_n samples closed inside the window: no
  // measurement, which the guard reads as "not over".
  double p50(double now_ms) {
    while (!s_.empty() && s_.front().t < now_ms - window_ms_) s_.pop_front();
    if (s_.size() < min_n_) return std::numeric_limits<double>::quiet_NaN();
    v_.clear();
    for (const auto& e : s_) v_.push_back(e.v);
    const std::size_t mid = v_.size() / 2;
    std::nth_element(v_.begin(), v_.begin() + static_cast<long>(mid), v_.end());
    return v_[mid];
  }

  void clear() { s_.clear(); }

  static constexpr std::size_t kCap = 512;

 private:
  struct E { double t, v; };
  double window_ms_;
  std::size_t min_n_;
  std::deque<E> s_;
  std::vector<double> v_;
};

}  // namespace maburgs

#endif  // MABURGS_TRANSIT_WINDOW_H_
