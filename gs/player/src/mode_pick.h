#ifndef MABUR_PLAYER_MODE_PICK_H_
#define MABUR_PLAYER_MODE_PICK_H_

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "vblank_estimator.h"  // mode_period_us

namespace maburplay {

// screen_mode "WIDTHxHEIGHT@RATE", RATE an integer or a decimal (59.94).
struct ScreenWant {
  int w = 0;
  int h = 0;
  double hz = 0.0;
  bool ok = false;
};

inline ScreenWant parse_screen_want(const std::string& s) {
  ScreenWant r;
  int n = 0;
  if (std::sscanf(s.c_str(), "%dx%d@%lf%n", &r.w, &r.h, &r.hz, &n) == 3 &&
      n == static_cast<int>(s.size()) && r.w > 0 && r.h > 0 && r.hz > 0.0)
    r.ok = true;
  return r;
}

// One connector mode, reduced to what the pick needs (no DRM types, so the
// rule is host-testable).
struct ModeCand {
  int w = 0;
  int h = 0;
  int clock_khz = 0;
  int htotal = 0;
  int vtotal = 0;
  bool interlaced = false;
  bool preferred = false;
};

// The mode's exact refresh rate: DRM's integer vrefresh says "60" for both
// 60.000 and 59.94 Hz.
inline double mode_hz(const ModeCand& m) {
  return 1e6 / mode_period_us(m.clock_khz, m.htotal, m.vtotal);
}

constexpr double kModeMatchHz = 0.5;

// Among non-interlaced modes of the wanted size whose exact rate is within
// kModeMatchHz of the wanted rate, the closest (first on a tie): a sink that
// lists 60.000 and 59.94 under "@60" gets 60.000, "@59.94" gets 59.94, and a
// 59.94-only sink still matches "@60" -- the phase lock steers the camera to
// whichever rate the panel runs. No match: the preferred non-interlaced mode,
// else the first. -1 for an empty list.
inline int pick_mode(const std::vector<ModeCand>& modes, const ScreenWant& want,
                     bool* matched) {
  *matched = false;
  int best = -1;
  double best_d = 0.0;
  if (want.ok) {
    for (size_t i = 0; i < modes.size(); ++i) {
      const ModeCand& m = modes[i];
      if (m.interlaced || m.w != want.w || m.h != want.h) continue;
      const double d = std::fabs(mode_hz(m) - want.hz);
      if (d > kModeMatchHz) continue;
      if (best < 0 || d < best_d) {
        best = static_cast<int>(i);
        best_d = d;
      }
    }
  }
  if (best >= 0) {
    *matched = true;
    return best;
  }
  for (size_t i = 0; i < modes.size(); ++i)
    if (!modes[i].interlaced && modes[i].preferred) return static_cast<int>(i);
  return modes.empty() ? -1 : 0;
}

}  // namespace maburplay

#endif  // MABUR_PLAYER_MODE_PICK_H_
