#include "phase_lock.h"

#include <algorithm>
#include <cmath>

namespace maburplay {

const char* phase_lock_state_name(PhaseLock::State s) {
  switch (s) {
    case PhaseLock::State::Waiting: return "wait";
    case PhaseLock::State::Acquiring: return "acq";
    case PhaseLock::State::Locked: return "lock";
    case PhaseLock::State::OutOfRange: return "oor";
  }
  return "?";
}

void PhaseLock::on_margin(double margin_us, double period_us, uint64_t now_us) {
  if (period_us <= 0.0 || state_ == State::OutOfRange) return;
  double e = margin_us - target_us_;
  e -= period_us * std::round(e / period_us);
  if (!samples_.empty()) {
    // Unwrap against the window's first sample, so a window that straddles
    // the +-P/2 seam stays one continuous line for the slope fit.
    const double ref = samples_.front().e_us;
    e -= period_us * std::round((e - ref) / period_us);
  }
  samples_.push_back({now_us, e});
  period_us_ = period_us;
}

void PhaseLock::on_discontinuity(uint64_t now_us) {
  centre_ = 0.0;
  offset_ = 0;
  window_offset_ = 0;
  samples_.clear();
  window_open_ = true;
  win_start_us_ = now_us;
  enter(State::Waiting, now_us);
}

bool PhaseLock::tick(uint64_t now_us) {
  const State s0 = state_;
  const int32_t o0 = offset_;
  if (!window_open_) {
    window_open_ = true;
    win_start_us_ = now_us;
  }
  if (state_ == State::OutOfRange) {
    samples_.clear();
    if (now_us - oor_since_us_ >= kOorRetryUs) {
      centre_ = 0.0;
      enter(State::Waiting, now_us);
      win_start_us_ = now_us;
      window_offset_ = offset_;
    }
  } else if (now_us - win_start_us_ >= kWindowUs) {
    if (static_cast<int>(samples_.size()) >= kMinSamples) close_window(now_us);
    samples_.clear();
    win_start_us_ = now_us;
    window_offset_ = offset_;
  }
  if ((state_ == State::Acquiring || state_ == State::Locked) &&
      now_us - last_window_us_ >= kQuietUs)
    enter(State::Waiting, now_us);  // the offset is held: the drone keeps its rate
  if (state_ == State::Acquiring && now_us - acq_since_us_ >= kAcquireLimitUs)
    enter(State::OutOfRange, now_us);
  return state_ != s0 || offset_ != o0;
}

void PhaseLock::close_window(uint64_t now_us) {
  std::vector<double> es;
  es.reserve(samples_.size());
  for (const Sample& s : samples_) es.push_back(s.e_us);
  std::sort(es.begin(), es.end());
  const size_t n = es.size();
  double med = es[n / 2];
  med -= period_us_ * std::round(med / period_us_);
  err_ms_ = med / 1000.0;
  last_window_us_ = now_us;

  // A margin that does not hold still inside a second (an unreachable panel
  // sweeps a whole period per second) has no meaningful median or slope:
  // never lock on it, count it against a lock, and leave the offset alone.
  if (es[(9 * n) / 10] - es[n / 10] > kCoherentUs) {
    if (state_ == State::Waiting) {
      enter(State::Acquiring, now_us);
    } else if (state_ == State::Acquiring) {
      in_count_ = 0;
    } else if (state_ == State::Locked && ++out_count_ >= kLockOutWindows) {
      enter(State::Acquiring, now_us);
    }
    return;
  }

  update_centre();
  const double ae = std::fabs(err_ms_);
  switch (state_) {
    case State::Waiting:
      enter(State::Acquiring, now_us);
      break;
    case State::Acquiring:
      if (ae < kLockInMs) {
        if (++in_count_ >= kLockInWindows) enter(State::Locked, now_us);
      } else {
        in_count_ = 0;
      }
      break;
    case State::Locked:
      if (ae > kLockOutMs) {
        if (++out_count_ >= kLockOutWindows) enter(State::Acquiring, now_us);
      } else {
        out_count_ = 0;
      }
      break;
    case State::OutOfRange:
      break;
  }
  if (state_ == State::Acquiring || state_ == State::Locked) {
    const double lim = state_ == State::Locked ? kCorrLockMfps : kCorrAcqMfps;
    const double corr = std::clamp(kKpMfpsPerMs * err_ms_, -lim, lim);
    const double r = static_cast<double>(kRangeMfps);
    offset_ = static_cast<int32_t>(std::lround(std::clamp(centre_ - corr, -r, r)));
  }
}

void PhaseLock::update_centre() {
  // Least-squares slope (µs of error per s) over the samples after the
  // offset sent at the window's start has reached the drone.
  const uint64_t t0 = win_start_us_ + kSlopeSkipUs;
  double n = 0, st = 0, se = 0, stt = 0, ste = 0;
  for (const Sample& s : samples_) {
    if (s.t_us < t0) continue;
    const double t = static_cast<double>(s.t_us - t0) / 1e6;
    n += 1.0;
    st += t;
    se += s.e_us;
    stt += t * t;
    ste += t * s.e_us;
  }
  if (n < kMinSamples) return;
  const double den = n * stt - st * st;
  if (den <= 0.0) return;
  const double slope = (n * ste - st * se) / den;
  const double icpt = (se - slope * st) / n;
  double rss = 0.0;
  for (const Sample& s : samples_) {
    if (s.t_us < t0) continue;
    const double t = static_cast<double>(s.t_us - t0) / 1e6;
    const double res = s.e_us - (icpt + slope * t);
    rss += res * res;
  }
  if (std::sqrt(rss / n) > kSlopeResidUs) return;  // a step inside the window
  const double neutral =
      static_cast<double>(window_offset_) - (slope / 1000.0) / kSlopeMsPerSecPerMfps;
  const double r = static_cast<double>(kRangeMfps);
  centre_ = std::clamp(centre_ + kCentreAlpha * (neutral - centre_), -r, r);
}

void PhaseLock::enter(State s, uint64_t now_us) {
  state_ = s;
  in_count_ = 0;
  out_count_ = 0;
  if (s == State::Acquiring) acq_since_us_ = now_us;
  if (s == State::OutOfRange) {
    oor_since_us_ = now_us;
    offset_ = 0;
  }
}

}  // namespace maburplay
