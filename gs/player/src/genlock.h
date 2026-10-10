#ifndef MABUR_PLAYER_GENLOCK_H_
#define MABUR_PLAYER_GENLOCK_H_

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace maburplay {

// Genlock (docs/efficient-link-plan.md step 2): steer the drone camera's
// frame rate so its frames land on the screen's refresh grid at a fixed
// phase, instead of beating against it.
//
// Why: the vsync-locked regulator already releases each frame at the first
// refresh after it is ready. But the camera and the screen run on separate
// clocks (flights 2026-10-09: camera 60.078 fps from the drone's pts, screen
// 60.000 Hz), so the wait for that refresh sweeps 0..one period and wraps
// every ~13 s: a 17 ms latency sawtooth, and one frame thrown away per wrap
// because the camera makes more frames than the screen can show.
//
// What it measures, per frame, all on the GS clock:
//   c     the frame's capture time, i.e. its pts mapped through the
//         regulator's floor anchor (capture + the fastest transit seen);
//   x     ready - c: how long after that the frame was ready to show;
//   delta the time from c to the next release deadline (refresh - lead).
// delta depends only on the two clocks, not on the link, so it is a clean
// phase signal. The target phase is the (1 - miss_frac) quantile of x: with
// the camera locked there, that share of frames is ready by its deadline and
// shows at c + target, the rest one refresh later.
//
// The camera's rate comes from the same phase: delta drifts by (P - T_cam) per
// frame, so its slope against capture time is f_cam / f_screen - 1. That is
// a least-squares fit over the last few seconds of (capture, phase) points,
// sorted by capture time -- it needs no frame order and survives repeated
// timestamps (bench 2026-10-10: the per-frame pts steps the player sees did
// not give the camera's period, while the phase slope read the beat exactly).
// Whether the camera runs one frame per refresh at all (low power's 30 fps
// does not) is judged from frames per real second between ticks.
//
// What it does: once a tick (~1 s), a PI loop on the wrapped phase error
// turns into a camera-rate setpoint in milli-fps, sent to the drone, which
// trims its sensor's frame length. The first setpoint is a feed-forward from
// the measured camera and screen rates (assuming the drone is at its
// nominal rate), so lock takes seconds rather than the integrator's minute.
//
// Pure arithmetic: no clocks, no I/O, no threads.
class Genlock {
 public:
  struct Params {
    double miss_frac = 0.10;     // share of frames allowed to miss their slot
    double kp_mfps_per_ms = 8.0;  // proportional: setpoint per ms of error
    double ki_mfps_per_ms = 1.0;  // integral: per ms of error, per tick
    double max_step_mfps = 40.0;  // slew limit per tick
    double band = 0.01;           // setpoints stay within +-1% of nominal
    double target_alpha = 0.2;    // smoothing of the target phase per tick
  };

  static constexpr int kMinFrames = 20;       // per tick, to say anything
  static constexpr size_t kReadyRing = 600;   // ~10 s of x for the quantile
  static constexpr size_t kSlopeRing = 240;   // ~4 s of (capture, phase) points

  Genlock() : Genlock(Params{}) {}
  explicit Genlock(Params p) : p_(p) {}

  // One frame on the servo path (the vblank grid was valid). pts64_us is the
  // drone's unwrapped pts; capture_us its anchor-mapped GS time; ready_us
  // when the frame was offered; flip_phase_us/period_us the estimator's grid;
  // lead_us the regulator's release lead.
  void on_frame(uint64_t pts64_us, uint64_t capture_us, uint64_t ready_us,
                uint64_t flip_phase_us, double period_us, uint64_t lead_us) {
    if (period_us <= 0) return;
    period_us_ = period_us;
    // Diagnostic only (the `pstep=` / `pback=` log fields): what the raw
    // pts sequence looks like from here.
    if (have_pts_) {
      const int64_t d = static_cast<int64_t>(pts64_us - last_pts_);
      if (d > 0) push_ring(pts_steps_, static_cast<double>(d), kSlopeRing, pts_pos_);
      else ++pts_back_;
    }
    last_pts_ = pts64_us;
    have_pts_ = true;
    const double x = static_cast<double>(static_cast<int64_t>(ready_us - capture_us));
    // A frame later than two periods misses whatever the phase; keeping it
    // out stops a loss burst from dragging the target around.
    push_ring(ready_, std::clamp(x, 0.0, 2.0 * period_us), kReadyRing, ready_pos_);
    const double d = static_cast<double>(static_cast<int64_t>(flip_phase_us - lead_us)) -
                     static_cast<double>(capture_us);
    const double delta = wrap0p(d, period_us);
    phases_.push_back(delta);
    if (slope_.size() < kSlopeRing) {
      slope_.push_back({static_cast<double>(capture_us), delta});
    } else {
      slope_[slope_pos_] = {static_cast<double>(capture_us), delta};
      slope_pos_ = (slope_pos_ + 1) % kSlopeRing;
    }
  }

  struct Tick {
    bool valid = false;     // enough frames this tick to measure the phase
    double cam_hz = 0;      // camera rate on the drone clock, 0 = unknown
    double panel_hz = 0;    // screen rate
    double fps = 0;         // frames per real second since the last tick
    double pstep_us = 0;    // diagnostic: median forward pts step seen
    int pts_back = 0;       // diagnostic: pts steps <= 0 this tick
    double phase_ms = 0;    // capture -> next deadline (median)
    double target_ms = 0;   // where the phase is being held (mod one period)
    double err_ms = 0;      // phase - target, wrapped to +-half a period
    bool steering = false;  // a setpoint is being produced
    uint32_t cmd_mfps = 0;  // the setpoint to send (valid when steering)
    int n = 0;              // frames this tick
  };

  // ~1 Hz, now_us on the same clock as on_frame's times. steer=false
  // measures only (no setpoint, integrator untouched).
  Tick tick(bool steer, uint64_t now_us) {
    Tick t;
    t.n = static_cast<int>(phases_.size());
    if (have_tick_ && now_us > last_tick_us_)
      t.fps = t.n * 1e6 / static_cast<double>(now_us - last_tick_us_);
    last_tick_us_ = now_us;
    have_tick_ = true;
    t.pts_back = pts_back_;
    pts_back_ = 0;
    if (!pts_steps_.empty()) t.pstep_us = median(pts_steps_);
    const double P = period_us_;
    if (P <= 0 || t.n < kMinFrames) {
      phases_.clear();
      return t;
    }
    t.valid = true;
    t.panel_hz = 1e6 / P;
    const double slope = phase_slope(P);
    if (slope_.size() >= static_cast<size_t>(kMinFrames)) t.cam_hz = t.panel_hz * (1.0 + slope);

    const double target_raw = quantile(ready_, 1.0 - p_.miss_frac);
    if (!have_target_) {
      target_us_ = target_raw;
      have_target_ = true;
    } else {
      target_us_ += p_.target_alpha * (target_raw - target_us_);
    }
    const double target_mod = wrap0p(target_us_, P);
    std::vector<double> errs;
    errs.reserve(phases_.size());
    for (double d : phases_) errs.push_back(wrap_half(d - target_mod, P));
    const double err_us = median(errs);
    t.err_ms = err_us / 1000.0;
    t.target_ms = target_mod / 1000.0;
    t.phase_ms = wrap0p(target_mod + err_us, P) / 1000.0;
    phases_.clear();

    // Steer only a camera that runs 1:1 against the screen: one frame per
    // refresh by the clock (low power's 30 fps is not), and a rate within 1%
    // of the screen's. Anything else holds whatever the drone has.
    const bool one_to_one = t.fps >= 0.8 * t.panel_hz && t.fps <= 1.2 * t.panel_hz;
    const bool steerable = one_to_one && t.cam_hz > 0 &&
                           std::fabs(t.cam_hz - t.panel_hz) <= 0.01 * t.panel_hz;
    if (!steer || !steerable) return t;

    const double nominal = std::round(t.cam_hz) * 1000.0;
    if (nominal <= 0) return t;
    const double lo = nominal * (1.0 - p_.band), hi = nominal * (1.0 + p_.band);
    if (!have_cmd_) {
      // Feed-forward: assume the drone runs at its nominal setpoint and the
      // sensor's real rate is that times a fixed scale.
      const double scale = t.cam_hz * 1000.0 / nominal;
      integ_ = std::clamp(t.panel_hz * 1000.0 / scale, lo, hi);
      cmd_ = integ_;
      have_cmd_ = true;
    }
    integ_ = std::clamp(integ_ - p_.ki_mfps_per_ms * t.err_ms, lo, hi);
    double u = integ_ - p_.kp_mfps_per_ms * t.err_ms;
    u = std::clamp(u, cmd_ - p_.max_step_mfps, cmd_ + p_.max_step_mfps);
    cmd_ = std::clamp(u, lo, hi);
    t.steering = true;
    t.cmd_mfps = static_cast<uint32_t>(std::lround(cmd_));
    return t;
  }

  // Forget the setpoint history (the next steering tick re-seeds from the
  // feed-forward) and the old pts space's phase points. For a drone restart.
  void reset_steering() {
    have_cmd_ = false;
    slope_.clear();
    slope_pos_ = 0;
  }

 private:
  // d(phase)/d(capture) by least squares over the ring, sorted by capture
  // time and unwrapped point to point; one pass drops points more than 3x
  // the median residual (an anchor snap, a stray frame) and refits.
  double phase_slope(double P) const {
    if (slope_.size() < 3) return 0;
    std::vector<std::pair<double, double>> pts(slope_.begin(), slope_.end());
    std::sort(pts.begin(), pts.end());
    for (size_t i = 1; i < pts.size(); ++i)
      pts[i].second = pts[i - 1].second + wrap_half(pts[i].second - pts[i - 1].second, P);
    auto fit = [](const std::vector<std::pair<double, double>>& v, double* a, double* b) {
      double mx = 0, my = 0;
      for (const auto& p : v) { mx += p.first; my += p.second; }
      mx /= v.size();
      my /= v.size();
      double sxx = 0, sxy = 0;
      for (const auto& p : v) {
        sxx += (p.first - mx) * (p.first - mx);
        sxy += (p.first - mx) * (p.second - my);
      }
      *b = sxx > 0 ? sxy / sxx : 0;
      *a = my - *b * mx;
    };
    double a = 0, b = 0;
    fit(pts, &a, &b);
    std::vector<double> res;
    res.reserve(pts.size());
    for (const auto& p : pts) res.push_back(std::fabs(p.second - (a + b * p.first)));
    const double cut = 3.0 * median(res) + 1.0;
    std::vector<std::pair<double, double>> keep;
    for (size_t i = 0; i < pts.size(); ++i)
      if (res[i] <= cut) keep.push_back(pts[i]);
    if (keep.size() >= 3 && keep.size() < pts.size()) fit(keep, &a, &b);
    return b;
  }

  static void push_ring(std::vector<double>& r, double v, size_t cap, size_t& pos) {
    if (r.size() < cap) {
      r.push_back(v);
    } else {
      r[pos] = v;
      pos = (pos + 1) % cap;
    }
  }
  static double wrap0p(double v, double P) {
    double m = std::fmod(v, P);
    if (m < 0) m += P;
    return m;
  }
  static double wrap_half(double v, double P) {
    double m = wrap0p(v + P / 2, P);
    return m - P / 2;
  }
  static double median(std::vector<double> v) {
    if (v.empty()) return 0;
    const size_t k = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
  }
  static double quantile(std::vector<double> v, double q) {
    if (v.empty()) return 0;
    q = std::clamp(q, 0.0, 1.0);
    const size_t k = static_cast<size_t>(q * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
  }

  Params p_;
  double period_us_ = 0;
  std::vector<double> phases_;
  std::vector<double> ready_;
  size_t ready_pos_ = 0;
  std::vector<std::pair<double, double>> slope_;  // (capture_us, phase_us)
  size_t slope_pos_ = 0;
  std::vector<double> pts_steps_;  // diagnostic
  size_t pts_pos_ = 0;
  int pts_back_ = 0;
  uint64_t last_pts_ = 0;
  bool have_pts_ = false;
  uint64_t last_tick_us_ = 0;
  bool have_tick_ = false;
  bool have_target_ = false;
  double target_us_ = 0;
  bool have_cmd_ = false;
  double integ_ = 0;
  double cmd_ = 0;
};

}  // namespace maburplay

#endif  // MABUR_PLAYER_GENLOCK_H_
