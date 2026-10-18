#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <utility>
#include <vector>

#include "frame_regulator.h"
#include "mtest.h"
#include "phase_lock.h"

using maburplay::DmaFrame;
using maburplay::FrameRegulator;
using maburplay::PhaseLock;

namespace {

// The sensor driver turns a milli-fps request into a whole line count; the
// real rate is the line rate over that count. These constants reproduce the
// bench IMX415: the trim request 59718 mfps lands on 3391 lines = 59.987 Hz,
// and 3390 lines = 60.005 Hz, so a 60.000 Hz panel sits between two lines.
struct Sensor {
  static constexpr double kDriverLinesPerS = 202503.0;  // what the driver assumes
  static constexpr double kLinesPerS = 203416.4;        // 74.247 MHz / HMAX 365
  static constexpr uint32_t kTrimMfps = 59718;
  static double period_us(int32_t off_mfps) {
    const double req = static_cast<double>(static_cast<int64_t>(kTrimMfps) + off_mfps) / 1000.0;
    const double vmax = std::round(kDriverLinesPerS / req);
    return vmax / kLinesPerS * 1e6;
  }
};

constexpr double kPanel60 = 1e6 / 60.0;
constexpr double kPanel5994 = 1e6 * 1001.0 / 60000.0;
constexpr double kTargetUs = 4000.0;

double pos_mod(double v, double p) {
  const double r = std::fmod(v, p);
  return r < 0 ? r + p : r;
}
double wrapped(double v, double p) { return v - p * std::round(v / p); }

// Deterministic noise in [-amp, +amp].
double noise(uint64_t i, double amp) {
  const uint64_t h = (i + 1) * 2654435761u;
  return (static_cast<double>(h % 2001) / 1000.0 - 1.0) * amp;
}

struct RunResult {
  double locked_at_s = -1.0;  // first time the lock reported Locked
  double worst_us = 0.0;      // max |true margin - target| from lock + 5 s on
  int unlocks = 0;
  PhaseLock::State final_state = PhaseLock::State::Waiting;
  int32_t final_offset = 0;
  double final_centre = 0.0;
};

// Margin plant: each capture moves `want` by the sensor's period while the
// vblank grid repeats every panel period, so the margin shrinks by the
// difference (mod the panel period). Offsets reach the sensor 100 ms after
// the lock sends them. frame_div N feeds every Nth frame (low-power
// decimation: the sensor still runs at full rate). A jump shifts the margin
// once (an allowance step); the 30 s after it are not scored. neutral_step_at_s
// stretches the sensor's own line rate (e.g. auto-exposure), not a steering
// offset, by 70/60000 from that time on -- a disturbance the lock has to
// absorb rather than cause; the 30 s after it are likewise not scored.
RunResult run(double panel_us, double seconds, double margin0_us, int frame_div = 1,
              double jump_at_s = -1.0, double jump_us = 0.0, double noise_us = 300.0,
              double neutral_step_at_s = -1.0) {
  PhaseLock lock(kTargetUs / 1000.0);
  RunResult r;
  std::deque<std::pair<double, int32_t>> cmd;
  int32_t applied = 0;
  double t = 0.0, m = margin0_us;
  uint64_t i = 0;
  while (t < seconds * 1e6) {
    while (!cmd.empty() && cmd.front().first <= t) {
      applied = cmd.front().second;
      cmd.pop_front();
    }
    double pc = Sensor::period_us(applied);
    if (neutral_step_at_s >= 0 && t >= neutral_step_at_s * 1e6)
      pc /= (1.0 - 70.0 / 60000.0);
    const double t_prev = t;
    t += pc;
    m = pos_mod(m - pc, panel_us);
    if (jump_at_s >= 0 && t_prev < jump_at_s * 1e6 && t >= jump_at_s * 1e6)
      m = pos_mod(m - jump_us, panel_us);
    ++i;
    const uint64_t now = 1'000'000 + static_cast<uint64_t>(t);
    if (i % static_cast<uint64_t>(frame_div) == 0)
      lock.on_margin(pos_mod(m + noise(i, noise_us), panel_us), panel_us, now);
    const PhaseLock::State before = lock.state();
    if (lock.tick(now)) cmd.push_back({t + 100'000.0, lock.offset_mfps()});
    if (lock.locked() && r.locked_at_s < 0) r.locked_at_s = t / 1e6;
    if (before == PhaseLock::State::Locked && !lock.locked()) ++r.unlocks;
    const double ts = t / 1e6;
    const bool excluded = (jump_at_s >= 0 && ts >= jump_at_s && ts <= jump_at_s + 30.0) ||
                          (neutral_step_at_s >= 0 && ts >= neutral_step_at_s &&
                           ts <= neutral_step_at_s + 30.0);
    if (r.locked_at_s >= 0 && ts > r.locked_at_s + 5.0 && !excluded)
      r.worst_us = std::max(r.worst_us, std::fabs(wrapped(m - kTargetUs, panel_us)));
  }
  r.final_state = lock.state();
  r.final_offset = lock.offset_mfps();
  r.final_centre = lock.centre_mfps();
  return r;
}

}  // namespace

TEST(starts_waiting_and_sends_zero) {
  PhaseLock lock(4.0);
  CHECK(lock.state() == PhaseLock::State::Waiting);
  CHECK(lock.offset_mfps() == 0);
  CHECK(!lock.tick(1'000'000));
  CHECK(lock.state() == PhaseLock::State::Waiting);
}

TEST(locks_on_60hz_within_30s_and_holds) {
  const RunResult r = run(kPanel60, 180, 12'000);   // starts 8 ms off target
  CHECK(r.locked_at_s >= 0 && r.locked_at_s < 30.0);
  CHECK(r.worst_us <= 1'000.0);
  CHECK(r.unlocks == 0);
  CHECK(r.final_state == PhaseLock::State::Locked);
  CHECK(r.final_centre > -15.0 && r.final_centre < 30.0);  // the panel sits ~+12 mfps from the trim
}

TEST(locks_across_the_half_period_seam) {
  const RunResult r = run(kPanel60, 120, kTargetUs + 8'300);  // error right at +-P/2
  CHECK(r.locked_at_s >= 0 && r.locked_at_s < 30.0);
  CHECK(r.worst_us <= 1'000.0);
}

TEST(locks_on_5994_panel) {
  // About three lines slower than the trim: the centre has to walk there.
  const RunResult r = run(kPanel5994, 180, 12'000);
  CHECK(r.locked_at_s >= 0 && r.locked_at_s < 30.0);
  CHECK(r.worst_us <= 1'000.0);
  CHECK(r.final_centre > -70.0 && r.final_centre < -30.0);
}

TEST(locks_on_120hz_panel) {
  // 60 fps camera, 8.33 ms panel: the error wraps at +-4.17 ms, just past
  // the 4 ms target.
  const RunResult r = run(1e6 / 120.0, 180, 6'000);
  CHECK(r.locked_at_s >= 0 && r.locked_at_s < 30.0);
  CHECK(r.worst_us <= 1'000.0);
}

TEST(locks_with_15fps_decimation) {
  const RunResult r = run(kPanel60, 180, 12'000, /*frame_div=*/4);
  CHECK(r.locked_at_s >= 0 && r.locked_at_s < 30.0);
  CHECK(r.worst_us <= 1'000.0);
}

TEST(holds_under_heavy_margin_noise) {
  const RunResult r = run(kPanel60, 180, 12'000, 1, -1.0, 0.0, /*noise_us=*/1'000.0);
  CHECK(r.locked_at_s >= 0 && r.locked_at_s < 30.0);
  CHECK(r.worst_us <= 1'500.0);
  CHECK(r.unlocks == 0);
}

TEST(rides_out_an_allowance_jump) {
  const RunResult r = run(kPanel60, 200, 12'000, 1, /*jump_at_s=*/100.0, /*jump_us=*/3'000.0);
  CHECK(r.final_state == PhaseLock::State::Locked);
  CHECK(r.worst_us <= 1'000.0);
}

TEST(rides_out_a_neutral_rate_step) {
  // Auto-exposure stretching the frame mid-flight: the sensor's own line
  // rate moves on its own, not from a steering request. The lock should
  // absorb it without ever unlocking.
  const RunResult r = run(kPanel60, 240, 12'000, 1, -1.0, 0.0, 300.0,
                           /*neutral_step_at_s=*/150.0);
  CHECK(r.unlocks == 0);
  CHECK(r.final_state == PhaseLock::State::Locked);
  CHECK(r.worst_us <= 1'000.0);
}

TEST(unreachable_panels_go_out_of_range) {
  // 50 Hz and 59 Hz: the phase sweeps a whole period every second, so no
  // window is coherent, nothing locks, and after 60 s of acquiring the lock
  // gives up and hands the sensor back to its trim.
  for (double panel : {20'000.0, 1e6 / 59.0}) {
    const RunResult r = run(panel, 100, 5'000);
    CHECK(r.locked_at_s < 0);
    CHECK(r.final_state == PhaseLock::State::OutOfRange);
    CHECK(r.final_offset == 0);
  }
}

TEST(out_of_range_retries_after_a_minute) {
  // Acquiring from ~1 s, out of range at ~61 s, waiting at ~121 s, the next
  // window re-acquires.
  const RunResult r = run(1e6 / 59.0, 125, 5'000);
  CHECK(r.final_state == PhaseLock::State::Acquiring);
  CHECK(r.final_offset == 0);
}

TEST(discontinuity_resets_to_waiting_and_zero) {
  PhaseLock lock(4.0);
  uint64_t now = 1'000'000;
  for (int k = 0; k < 6 * 60; ++k) {          // 6 s of margins 0.5 ms over target:
    now += 16'667;                            // close enough to lock, and the
    lock.on_margin(4'500.0, kPanel60, now);   // correction moves the offset off 0
    lock.tick(now);
  }
  CHECK(lock.locked());
  CHECK(lock.offset_mfps() != 0);
  lock.on_discontinuity(now);
  CHECK(lock.state() == PhaseLock::State::Waiting);
  CHECK(lock.offset_mfps() == 0);
  CHECK(lock.centre_mfps() == 0.0);
}

TEST(quiet_stream_goes_waiting_and_holds_the_offset) {
  PhaseLock lock(4.0);
  uint64_t now = 1'000'000;
  for (int k = 0; k < 6 * 60; ++k) {
    now += 16'667;
    lock.on_margin(4'500.0, kPanel60, now);
    lock.tick(now);
  }
  REQUIRE(lock.locked());
  lock.tick(now + 1'000'000);                 // closes the window in progress
  REQUIRE(lock.locked());
  const int32_t held = lock.offset_mfps();
  CHECK(lock.tick(now + 3'100'000));          // > 2 s since the last window
  CHECK(lock.state() == PhaseLock::State::Waiting);
  CHECK(lock.offset_mfps() == held);          // the drone keeps its rate
}

TEST(locked_to_acquiring_on_sustained_error) {
  PhaseLock lock(4.0);
  uint64_t now = 1'000'000;
  for (int k = 0; k < 6 * 60; ++k) {          // lock on margins 0.5 ms over target
    now += 16'667;
    lock.on_margin(4'500.0, kPanel60, now);
    lock.tick(now);
  }
  REQUIRE(lock.locked());
  now += 1'000'000;                           // flush the window in progress
  lock.tick(now);
  REQUIRE(lock.locked());

  // |e| = 5.5 ms > kLockOutMs: the first bad window only brings out_count to
  // 1, short of kLockOutWindows (2) -- the lock rides it out.
  for (int k = 0; k < 60; ++k) {
    now += 16'667;
    lock.on_margin(9'500.0, kPanel60, now);
    lock.tick(now);
  }
  CHECK(lock.state() == PhaseLock::State::Locked);

  // Second consecutive bad window reaches kLockOutWindows.
  for (int k = 0; k < 60; ++k) {
    now += 16'667;
    lock.on_margin(9'500.0, kPanel60, now);
    lock.tick(now);
  }
  CHECK(lock.state() == PhaseLock::State::Acquiring);
}

TEST(locked_to_acquiring_on_incoherent_windows) {
  PhaseLock lock(4.0);
  uint64_t now = 1'000'000;
  for (int k = 0; k < 6 * 60; ++k) {          // lock on margins 0.5 ms over target
    now += 16'667;
    lock.on_margin(4'500.0, kPanel60, now);
    lock.tick(now);
  }
  REQUIRE(lock.locked());
  now += 1'000'000;                           // flush the window in progress
  lock.tick(now);
  REQUIRE(lock.locked());
  const int32_t held = lock.offset_mfps();

  // A margin that sweeps the whole panel period inside one second (p90-p10
  // way past kCoherentUs) has no meaningful median: it never updates the
  // offset, only counts against the lock.
  for (int k = 0; k < 60; ++k) {
    now += 16'667;
    lock.on_margin(std::fmod(k * 3'333.0, kPanel60), kPanel60, now);
    lock.tick(now);
  }
  CHECK(lock.state() == PhaseLock::State::Locked);
  CHECK(lock.offset_mfps() == held);

  for (int k = 0; k < 60; ++k) {
    now += 16'667;
    lock.on_margin(std::fmod(k * 3'333.0, kPanel60), kPanel60, now);
    lock.tick(now);
  }
  CHECK(lock.state() == PhaseLock::State::Acquiring);
  CHECK(lock.offset_mfps() == held);          // incoherent windows never touch it
}

TEST(acquiring_to_waiting_on_quiet_holds_offset) {
  PhaseLock lock(4.0);
  uint64_t now = 1'000'000;
  // One window, 9 ms off the 4 ms target. 61 ticks: the first one opens the
  // window (spending its own interval setting win_start_us_, not counting
  // toward it), so closing needs an extra tick past the usual 60.
  for (int k = 0; k < 61; ++k) {
    now += 16'667;
    lock.on_margin(13'000.0, kPanel60, now);
    lock.tick(now);
  }
  REQUIRE(lock.state() == PhaseLock::State::Acquiring);
  const int32_t held = lock.offset_mfps();
  CHECK(lock.tick(now + 2'100'000));          // > 2 s since the last window
  CHECK(lock.state() == PhaseLock::State::Waiting);
  CHECK(lock.offset_mfps() == held);          // the drone keeps its rate
}

TEST(state_names) {
  CHECK(std::strcmp(maburplay::phase_lock_state_name(PhaseLock::State::Waiting), "wait") == 0);
  CHECK(std::strcmp(maburplay::phase_lock_state_name(PhaseLock::State::Acquiring), "acq") == 0);
  CHECK(std::strcmp(maburplay::phase_lock_state_name(PhaseLock::State::Locked), "lock") == 0);
  CHECK(std::strcmp(maburplay::phase_lock_state_name(PhaseLock::State::OutOfRange), "oor") == 0);
}

// ---- closed loop: lock + regulator + a line-quantised sensor ----

namespace {
struct LoopResult {
  uint64_t repeats = 0, skips = 0, backwards = 0;
  std::vector<double> lat_us;   // latch - capture per newly shown frame, last 100 s
  bool locked = false;
};

// The panel at 60.000 Hz, delivery = capture + 20 ms + 0-2 ms noise with 2 %
// of frames 6 ms late. with_lock: the PhaseLock steers the sensor and owns
// capture placement; without: arrival placement (sched_cap_ms 0), today's
// unlocked stream, whose latency rides the sensor/panel beat.
LoopResult run_closed_loop(bool with_lock, double seconds) {
  FrameRegulator reg(12, true, 5, 0, with_lock ? 16 : 0);
  reg.set_phase_lock_mode(with_lock);
  reg.set_panel_period(kPanel60);
  PhaseLock lock(4.0);
  FrameRegulator::Displaced disp;
  DmaFrame out;
  LoopResult res;
  double flip_t = 5'000'000.0;
  for (int i = 0; i < 8; ++i) {
    reg.on_flip(static_cast<uint64_t>(flip_t), true);
    flip_t += kPanel60;
  }
  auto jitter = [](uint64_t i) {
    const uint64_t h = i * 2654435761u;
    double j = static_cast<double>(h % 2000);
    if ((h >> 13) % 50 == 0) j += 6'000.0;
    return j;
  };
  auto index_of = [](const DmaFrame& f) {
    return static_cast<int64_t>(reinterpret_cast<uintptr_t>(f.opaque) >> 1);
  };
  std::vector<std::pair<double, int64_t>> pending;
  std::vector<double> cap{flip_t + 3'000.0};
  std::deque<std::pair<double, int32_t>> cmd;
  int32_t applied = 0;
  int64_t shown = -1;
  uint64_t idx = 0;
  double prev_dec = cap[0] + 20'000.0 + jitter(0);
  double next_dec = prev_dec;
  const double t_end = 5'000'000.0 + seconds * 1e6;
  const double t_meas = t_end - 100e6;
  while (flip_t < t_end) {
    const double t = std::min(flip_t, next_dec);
    while (reg.release_due(static_cast<uint64_t>(t), &out)) pending.push_back({t, index_of(out)});
    if (flip_t <= next_dec) {
      int64_t best = -1;
      for (const auto& p : pending) if (p.first <= flip_t && p.second > best) best = p.second;
      pending.erase(std::remove_if(pending.begin(), pending.end(),
                                   [&](const auto& p) { return p.first <= flip_t; }),
                    pending.end());
      if (flip_t >= t_meas && shown >= 0) {
        if (best < 0) ++res.repeats;
        else if (best > shown + 1) res.skips += static_cast<uint64_t>(best - shown - 1);
        else if (best < shown) ++res.backwards;
      }
      if (best > shown) {
        shown = best;
        if (flip_t >= t_meas) res.lat_us.push_back(flip_t - cap[static_cast<size_t>(best)]);
      }
      reg.on_flip(static_cast<uint64_t>(flip_t), true);
      flip_t += kPanel60;
      if (with_lock) {
        if (lock.tick(static_cast<uint64_t>(flip_t))) cmd.push_back({flip_t + 100'000.0, lock.offset_mfps()});
        reg.set_capture_placement(lock.locked());
      }
    } else {
      DmaFrame f;
      f.pts_us = static_cast<uint32_t>(static_cast<uint64_t>(cap[idx] - cap[0]));
      f.opaque = reinterpret_cast<void*>(static_cast<uintptr_t>((idx << 1) | 1u));
      if (reg.offer(f, static_cast<uint64_t>(next_dec), &disp))
        pending.push_back({next_dec, static_cast<int64_t>(idx)});
      double m = 0.0;
      if (with_lock && reg.take_margin(&m))
        lock.on_margin(m, reg.panel_period_us(), static_cast<uint64_t>(next_dec));
      ++idx;
      while (!cmd.empty() && cmd.front().first <= cap.back()) {
        applied = cmd.front().second;
        cmd.pop_front();
      }
      cap.push_back(cap.back() + Sensor::period_us(applied));
      const double d = cap.back() + 20'000.0 + jitter(idx);
      prev_dec = d > prev_dec + 300.0 ? d : prev_dec + 300.0;
      next_dec = prev_dec;
    }
  }
  res.locked = lock.locked();
  return res;
}

double pct(std::vector<double> v, double q) {
  std::sort(v.begin(), v.end());
  return v[static_cast<size_t>(q * static_cast<double>(v.size() - 1))];
}
}  // namespace

TEST(closed_loop_lock_is_flat_and_smooth) {
  const LoopResult locked = run_closed_loop(true, 200);
  CHECK(locked.locked);
  REQUIRE(locked.lat_us.size() > 5000);
  CHECK(pct(locked.lat_us, 0.9) - pct(locked.lat_us, 0.5) <= 2'500.0);
  CHECK(locked.repeats + locked.skips <= 3);
  CHECK(locked.backwards == 0);
  // The unlocked stream rides the beat: its latency spread is most of a period.
  const LoopResult free_run = run_closed_loop(false, 200);
  REQUIRE(free_run.lat_us.size() > 5000);
  CHECK(pct(free_run.lat_us, 0.9) - pct(free_run.lat_us, 0.5) > 4'000.0);
}

MTEST_MAIN
