#include <algorithm>
#include "ta_bench.h"
#include "mtest.h"

static maburgs::TurnaroundCfg cfg(double hz, std::vector<int> lanes) {
  maburgs::TurnaroundCfg c;
  c.rate_hz = hz;
  c.lanes = std::move(lanes);
  c.frames = 3;
  c.bytes = 500;
  return c;
}

TEST(off_by_default_and_with_rate_zero) {
  maburgs::TaPinger p(maburgs::TurnaroundCfg{}, 1, 42);
  CHECK(!p.enabled());
  for (uint64_t t = 0; t < 10'000'000; t += 1000) CHECK(!p.due(t).has_value());
}

TEST(first_call_schedules_then_pings_at_jittered_intervals) {
  maburgs::TaPinger p(cfg(10, {0, 4}), 7, 42);  // 10 Hz: gaps in [50, 150] ms
  REQUIRE(p.enabled());
  CHECK(!p.due(0).has_value());
  std::vector<uint64_t> at;
  for (uint64_t t = 0; t < 20'000'000; t += 1000)
    if (auto g = p.due(t)) {
      at.push_back(t);
      CHECK(g->vtx_id == 7);
      CHECK(g->n_frames == 3);
      CHECK(g->frame_bytes == 500);
    }
  // ~10/s over 20 s; jittered, so not a fixed comb.
  CHECK(at.size() > 150 && at.size() < 260);
  uint64_t lo = ~0ull, hi = 0;
  for (size_t i = 1; i < at.size(); ++i) {
    const uint64_t g = at[i] - at[i - 1];
    lo = std::min(lo, g);
    hi = std::max(hi, g);
  }
  CHECK(lo >= 49'000 && hi <= 151'000);
  CHECK(hi - lo > 50'000);
}

TEST(lanes_round_robin_and_seq_increments) {
  maburgs::TaPinger p(cfg(50, {0, 4, 6}), 1, 1);
  std::vector<mabur::rc::TaPing> got;
  for (uint64_t t = 0; got.size() < 7; t += 1000)
    if (auto g = p.due(t)) got.push_back(*g);
  const int want_lane[7] = {0, 4, 6, 0, 4, 6, 0};
  for (size_t i = 0; i < got.size(); ++i) {
    CHECK(got[i].lane == want_lane[i]);
    CHECK(got[i].seq == i);
  }
}

TEST(pause_restarts_the_schedule_without_catch_up) {
  maburgs::TaPinger p(cfg(10, {0}), 1, 3);
  p.due(0);
  p.pause();
  // A long pause, then resume: the first call only schedules, so there is
  // no burst of overdue pings.
  CHECK(!p.due(60'000'000).has_value());
  int n = 0;
  for (uint64_t t = 60'000'000; t < 60'040'000; t += 1000) n += p.due(t) ? 1 : 0;
  CHECK(n == 0);  // nothing inside the first 40 ms (min gap is 50 ms)
}

MTEST_MAIN
