#include <cmath>
#include <cstdint>
#include "glass_cadence.h"
#include "mtest.h"
using maburplay::GlassCadence;

TEST(every_1_records_every_flip_on_the_grid) {
  GlassCadence c; c.reset(10000.0, 1);
  CHECK(c.feed(5'000'000).record);
  auto s = c.feed(5'010'003); CHECK(s.record && s.pts_us == 10000);
  s = c.feed(5'019'996);      CHECK(s.record && s.pts_us == 20000);
}
TEST(every_2_records_the_first_flip_of_each_pair) {
  GlassCadence c; c.reset(10000.0, 2);
  CHECK(c.feed(0).record);              // index 0, slot 0
  CHECK(!c.feed(10000).record);         // index 1, slot 0 again
  auto s = c.feed(20000); CHECK(s.record && s.pts_us == 20000);   // index 2, slot 1
  CHECK(!c.feed(30000).record);
}
TEST(a_flip_on_the_second_vblank_of_an_empty_pair_is_recorded) {
  GlassCadence c; c.reset(10000.0, 2);
  CHECK(c.feed(0).record);
  auto s = c.feed(30000);               // index 3: pair 1 had no flip at index 2
  CHECK(s.record && s.pts_us == 20000); // stamped at the pair's grid time
}
TEST(a_hold_extends_the_previous_sample) {
  GlassCadence c; c.reset(16666.667, 1);
  CHECK(c.feed(0).record);
  auto s = c.feed(50000);               // 3 periods later
  CHECK(s.record && s.pts_us == 50000);
  CHECK(c.index() == 3);
}
TEST(two_flips_inside_one_period_still_advance) {
  GlassCadence c; c.reset(10000.0, 1);
  CHECK(c.feed(0).record);
  auto s = c.feed(2000);                // step rounds to 0 -> forced to 1
  CHECK(s.record && s.pts_us == 10000);
}
TEST(ppm_drift_never_moves_the_index_off_the_grid) {
  GlassCadence c; c.reset(10000.0, 1);
  const double actual = 10000.0 * (1 + 40e-6);
  uint64_t t = 0; c.feed(0);
  for (int i = 1; i <= 60000; ++i) {
    t = (uint64_t)llround(i * actual);
    auto s = c.feed(t);
    CHECK(s.record && s.pts_us == (uint64_t)i * 10000);
  }
  CHECK(c.index() == 60000);
}
TEST(jitter_of_a_quarter_period_never_changes_the_index) {
  GlassCadence c; c.reset(10000.0, 1);
  c.feed(0);
  int sign = 1;
  for (int i = 1; i <= 1000; ++i, sign = -sign) {
    auto s = c.feed((uint64_t)(i * 10000 + sign * 2400));
    CHECK(s.record && s.pts_us == (uint64_t)i * 10000);
  }
}
MTEST_MAIN
