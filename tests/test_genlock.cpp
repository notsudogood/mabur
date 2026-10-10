#include <cmath>
#include <cstdint>
#include <deque>
#include <vector>

#include "genlock.h"
#include "mtest.h"

using maburplay::Genlock;

namespace {

// Deterministic jitter source (LCG) so a failure reproduces exactly.
struct Lcg {
  uint32_t s = 12345;
  double uni() {
    s = s * 1664525u + 1013904223u;
    return static_cast<double>(s >> 8) / static_cast<double>(1u << 24);
  }
};

// The bench camera (IMX415 on the SSC338Q, open-loop holds 2026-10-10): a
// milli-fps request becomes a whole number of lines per frame,
// floor(K / mfps), one line ~7.4 us / ~27 mfps, fitted to 59.400 -> 16758 us,
// 59.725 -> 16669.5, 59.650 -> 16689.5. But a settled frame is never
// shorter than 16645 us, so 0 (the configured 60) and everything from
// ~59.80 up run at 60.078 fps (the flights' camera); for about a second
// after a change the bare, shorter length shows through (60.17-60.20 on the
// bench). `ignores` is a driver that accepts a request and does nothing.
struct Sensor {
  double line_us = 16669.5 / 2247.0;
  double K = 2247.5 * 59725.0;
  double floor_us = 16645.0;
  bool ignores = false;
  double period_us(uint32_t mfps, uint64_t frames_since_change) const {
    if (ignores) return floor_us;
    const double m = mfps ? static_cast<double>(mfps) : 60000.0;
    const double bare = std::floor(K / m) * line_us;
    return frames_since_change < 60 ? bare : std::max(bare, floor_us);
  }
};

struct SimResult {
  std::vector<double> errs_ms;  // per tick after settling
  double mean_latency_ms = 0;   // deadline - c over the settled frames
  double ready_by_target = 0;   // share of settled frames ready by c + target
  std::vector<Genlock::Tick> ticks;
};

// Panel 60.000 Hz; drone clock 10 ppm off the GS's; uplink delay `delay`
// ticks; every `lose_every`-th setpoint lost (0 = none).
// order: 0 = frames reach the player in capture order; 1 = each pair swapped
// (t+1 before t); 2 = every second frame repeats the previous frame's pts.
SimResult simulate(bool steer, int seconds, int settle_s, int delay, int lose_every,
                   Genlock::Params params = {}, int order = 0, Sensor sensor = {}) {
  Genlock g(params);
  Lcg rng;
  const double P = 1e6 / 60.0;
  const double grid0 = 1234.0;
  const double lead = 6000.0;
  const double skew = 1.0 + 10e-6;
  uint32_t applied = 0;  // the drone's configured rate
  uint64_t since_change = 1000;
  struct Send { bool sent; uint32_t mfps; };
  std::deque<Send> in_flight;
  double t_drone = 1'000'000.0;
  double next_tick = 2'000'000.0;
  int tick_no = 0;
  SimResult r;
  double lat_sum = 0;
  int lat_n = 0, ready_ok = 0;
  double target_ms = 0;
  uint64_t frame_no = 0;
  struct Pending { uint64_t pts, c, ready, flip; bool have = false; } held;
  double prev_t_drone = 0, prev_c = 0;
  while (t_drone < 1e6 * (seconds + 1)) {
    t_drone += sensor.period_us(applied, since_change++);
    const double cap_gs = t_drone * skew;
    const double c = cap_gs + 3000.0;
    // Transit + FEC + decode above the floor: mostly 6-14 ms, 10% tail.
    double x = 6000.0 + 8000.0 * rng.uni();
    if (rng.uni() < 0.10) x += 10000.0 * rng.uni();
    const double ready = c + x;
    const double k = std::floor((ready - grid0) / P);
    const double flip_phase = grid0 + k * P;
    double pts_d = t_drone, c_d = c;
    if (order == 2 && (frame_no % 2) == 1) {  // repeated timestamp
      pts_d = prev_t_drone;
      c_d = prev_c;
    }
    prev_t_drone = t_drone;
    prev_c = c;
    const Pending cur{static_cast<uint64_t>(pts_d), static_cast<uint64_t>(c_d),
                      static_cast<uint64_t>(ready), static_cast<uint64_t>(flip_phase), true};
    auto feed = [&](const Pending& f) { g.on_frame(f.pts, f.c, f.ready, f.flip, P, static_cast<uint64_t>(lead)); };
    if (order == 1) {
      if (!held.have) {
        held = cur;
      } else {
        feed(cur);   // the later frame first
        feed(held);
        held.have = false;
      }
    } else {
      feed(cur);
    }
    ++frame_no;
    if (tick_no >= settle_s) {
      // Shown at the first deadline (refresh - lead) at or after ready.
      const double n = std::ceil((ready - (grid0 - lead)) / P);
      const double deadline = grid0 - lead + n * P;
      lat_sum += deadline - c;
      ++lat_n;
      if (x <= target_ms * 1000.0 + 1.0) ++ready_ok;
    }
    if (cap_gs >= next_tick) {
      next_tick += 1e6;
      ++tick_no;
      const auto t = g.tick(steer, static_cast<uint64_t>(cap_gs));
      r.ticks.push_back(t);
      target_ms = t.target_ms;
      const bool lost = lose_every > 0 && tick_no % lose_every == 0;
      in_flight.push_back(Send{t.steering && !lost, t.cmd_mfps});
      if (static_cast<int>(in_flight.size()) > delay) {
        const Send v = in_flight.front();
        in_flight.pop_front();
        if (v.sent && v.mfps != applied) {
          applied = v.mfps;
          since_change = 0;
        }
      }
      if (tick_no > settle_s && t.valid) r.errs_ms.push_back(t.err_ms);
    }
  }
  r.mean_latency_ms = lat_n ? lat_sum / lat_n / 1000.0 : 0;
  r.ready_by_target = lat_n ? static_cast<double>(ready_ok) / lat_n : 0;
  return r;
}

double max_abs(const std::vector<double>& v) {
  double m = 0;
  for (double e : v) m = std::max(m, std::fabs(e));
  return m;
}

}  // namespace

TEST(measures_camera_and_panel_rates_without_steering) {
  auto r = simulate(false, 20, 5, 2, 0);
  REQUIRE(r.ticks.size() > 10);
  const auto& t = r.ticks.back();
  CHECK(t.valid);
  CHECK(!t.steering);
  CHECK(t.cmd_mfps == 0);
  CHECK(std::fabs(t.cam_hz - 60.078) < 0.01);
  CHECK(std::fabs(t.panel_hz - 60.0) < 0.001);
  // Free-running, the phase sweeps: errors cover most of a period.
  CHECK(max_abs(r.errs_ms) > 6.0);
}

TEST(locks_through_whole_line_steps_delay_and_loss) {
  auto r = simulate(true, 240, 60, 2, 5);
  REQUIRE(r.errs_ms.size() > 100);
  CHECK(max_abs(r.errs_ms) < 3.0);
  for (const auto& t : r.ticks)
    if (t.steering && t.cmd_mfps != 0) {
      // Inside the drone's +-1% band...
      CHECK(t.cmd_mfps >= 59400u);
      // ...and never up where this camera sits on its floor (~59.80+):
      // the region the first loop hunted in.
      CHECK(t.cmd_mfps < 59790u);
    }
}

TEST(calibrates_before_steering) {
  auto r = simulate(true, 60, 30, 2, 0);
  int first_steer = -1, calibrated = -1;
  for (size_t i = 0; i < r.ticks.size(); ++i) {
    const auto& t = r.ticks[i];
    if (t.steering && first_steer < 0) {
      first_steer = static_cast<int>(i);
      // The first thing sent puts the camera at its configured rate.
      CHECK(t.cmd_mfps == 0u);
      CHECK(t.stage == Genlock::kCalNative);
    }
    if (t.calibrated) calibrated = static_cast<int>(i);
  }
  REQUIRE(first_steer >= 0);
  REQUIRE(calibrated > first_steer);
  // Two steps of settle + measure: about 18 s.
  CHECK(calibrated - first_steer <= 20);
  const auto& t = r.ticks[static_cast<size_t>(calibrated)];
  CHECK(t.stage == Genlock::kRun);
  // The bench's numbers: native 60.078, the 59.580 probe ~59.86, and the
  // request that gives 60.000 is ~59.72 (59.725 ran at 59.990 on the bench).
  CHECK(t.cmd_mfps >= 59700u && t.cmd_mfps <= 59740u);
}

TEST(refuses_a_camera_that_ignores_the_request) {
  Sensor deaf;
  deaf.ignores = true;
  auto r = simulate(true, 90, 30, 2, 0, Genlock::Params{}, 0, deaf);
  int refused = -1;
  for (size_t i = 0; i < r.ticks.size(); ++i)
    if (r.ticks[i].refused) refused = static_cast<int>(i);
  REQUIRE(refused >= 0);
  // Its last word restores the configured rate; then nothing more is sent.
  CHECK(r.ticks[static_cast<size_t>(refused)].steering);
  CHECK(r.ticks[static_cast<size_t>(refused)].cmd_mfps == 0u);
  for (size_t i = static_cast<size_t>(refused) + 1; i < r.ticks.size(); ++i) {
    CHECK(!r.ticks[i].steering);
    CHECK(r.ticks[i].stage == Genlock::kRefused);
  }
}

TEST(recalibrates_after_a_drone_restart) {
  Genlock g;
  const double P = 1e6 / 60.0;
  const double T = 1e6 / 59.990;  // camera a little slower than the screen
  uint64_t frame = 0;
  auto second = [&](int s) {
    for (int i = 0; i < 60; ++i, ++frame) {
      const uint64_t pts = 1'000'000 + static_cast<uint64_t>(frame * T);
      g.on_frame(pts, pts + 3000, pts + 13000, 1'000'000 + static_cast<uint64_t>(frame * P), P, 6000);
    }
    return g.tick(true, 1'000'000 + static_cast<uint64_t>((s + 1) * 1e6));
  };
  int s = 0;
  auto t = second(s++);
  for (; s < 6 && !t.steering; ++s) t = second(s);
  REQUIRE(t.steering);
  CHECK(t.stage == Genlock::kCalNative);
  g.reset_steering();
  t = second(s++);
  for (; s < 12 && !t.steering; ++s) t = second(s);
  REQUIRE(t.steering);
  CHECK(t.stage == Genlock::kCalNative);
  CHECK(t.cmd_mfps == 0u);
}

TEST(locked_is_faster_on_average_than_free_running) {
  auto free_run = simulate(false, 240, 60, 2, 0);
  auto locked = simulate(true, 240, 60, 2, 0);
  // Free-running waits a uniform 0..one period for its refresh; locked waits
  // only for the slower frames. Both include the same transit.
  CHECK(locked.mean_latency_ms < free_run.mean_latency_ms - 1.0);
  // About the configured share is ready by the target phase.
  CHECK(locked.ready_by_target > 0.85);
  CHECK(locked.ready_by_target < 0.95);
}

TEST(does_not_steer_a_camera_off_the_screen_rate) {
  // Low power delivers every other frame: 30 a second against a 60 Hz
  // screen. The sensor itself still runs at 60, and the phase slope says so.
  Genlock g;
  const double P = 1e6 / 60.0;
  g.tick(true, 1'000'000);
  for (int s = 0; s < 3; ++s) {
    for (int i = 0; i < 30; ++i) {
      const uint64_t pts = 1'000'000 + static_cast<uint64_t>((s * 30 + i) * 2 * P);
      g.on_frame(pts, pts + 3000, pts + 13000, pts, P, 6000);
    }
    auto t = g.tick(true, 1'000'000 + static_cast<uint64_t>((s + 1) * 1e6));
    CHECK(t.valid);
    CHECK(std::fabs(t.fps - 30.0) < 1.0);
    CHECK(!t.steering);
  }
}

TEST(camera_rate_survives_reordered_and_repeated_timestamps) {
  // Bench 2026-10-10: the player's per-frame pts steps did not give the
  // camera's period. The phase slope must not care about frame order or a
  // repeated timestamp.
  for (int order = 1; order <= 2; ++order) {
    auto r = simulate(false, 20, 5, 2, 0, Genlock::Params{}, order);
    REQUIRE(r.ticks.size() > 10);
    const auto& t = r.ticks.back();
    CHECK(t.valid);
    CHECK(std::fabs(t.cam_hz - 60.078) < 0.005);
    CHECK(std::fabs(t.fps - 60.0) < 2.0);
  }
  auto locked = simulate(true, 240, 60, 2, 5, Genlock::Params{}, 1);
  CHECK(max_abs(locked.errs_ms) < 3.0);
}

TEST(too_few_frames_is_not_a_measurement) {
  Genlock g;
  const double P = 1e6 / 60.0;
  for (int i = 0; i < Genlock::kMinFrames - 1; ++i) {
    const uint64_t pts = 1'000'000 + static_cast<uint64_t>(i * P);
    g.on_frame(pts, pts, pts + 10000, pts, P, 6000);
  }
  auto t = g.tick(true, 2'000'000);
  CHECK(!t.valid);
  CHECK(!t.steering);
}

MTEST_MAIN
