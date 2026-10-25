// Host test for gs/player/src/rate_match.h: the measured source rate, and
// whether the panel is close enough for the phase lock to reach.
#include <cmath>
#include <cstdint>

#include "mtest.h"
#include "rate_match.h"

using maburplay::RateMatch;
using maburplay::SourceRate;

namespace {

void windows(RateMatch* m, int n, double src, double panel,
             RateMatch::LowPower lp = RateMatch::LowPower::kNo) {
  for (int i = 0; i < n; ++i) m->on_window(src, panel, lp);
}

}  // namespace

TEST(source_rate_reads_the_median_step) {
  SourceRate r;
  CHECK(r.hz() == 0.0);
  uint64_t now = 0;
  uint32_t pts = 1000;
  for (int i = 0; i < 300; ++i) {
    pts += (i == 150) ? 20000 : 10000;  // one hole: a frame never arrived
    now += 10000;
    r.on_pts(pts, now);
  }
  CHECK(std::fabs(r.hz() - 100.0) < 0.01);
}

TEST(rate_match_flags_a_100fps_source_on_a_60hz_panel) {
  RateMatch m;
  windows(&m, 3, 99.96, 60.0);
  CHECK(m.mismatch());
}

TEST(rate_match_flags_a_60fps_source_on_a_100hz_panel) {
  RateMatch m;
  windows(&m, 3, 59.99, 100.0);
  CHECK(m.mismatch());
}

TEST(rate_match_accepts_trimmed_rates) {
  RateMatch a;
  windows(&a, 10, 99.96, 99.946);
  CHECK(!a.mismatch());
  RateMatch b;
  windows(&b, 10, 59.99, 60.0);
  CHECK(!b.mismatch());
}

TEST(rate_match_flags_a_ceiling_short_of_the_panel) {
  RateMatch m;
  windows(&m, 3, 119.66, 120.0);  // 0.34 Hz: past the steer's reach
  CHECK(m.mismatch());
}

TEST(rate_match_is_silent_in_low_power) {
  RateMatch m;
  windows(&m, 10, 30.0, 100.0, RateMatch::LowPower::kYes);
  CHECK(!m.mismatch());
  // A mismatch already raised clears once low-power reports in.
  RateMatch n;
  windows(&n, 3, 99.96, 60.0);
  REQUIRE(n.mismatch());
  windows(&n, 1, 30.0, 60.0, RateMatch::LowPower::kYes);
  CHECK(!n.mismatch());
}

TEST(rate_match_needs_consecutive_windows_to_flip) {
  RateMatch m;
  for (int i = 0; i < 10; ++i) {
    m.on_window(99.96, 60.0, RateMatch::LowPower::kNo);
    m.on_window(99.96, 99.946, RateMatch::LowPower::kNo);
  }
  CHECK(!m.mismatch());
  windows(&m, 2, 99.96, 60.0);
  CHECK(!m.mismatch());
  windows(&m, 1, 99.96, 60.0);
  CHECK(m.mismatch());
  windows(&m, 2, 99.96, 99.946);
  CHECK(m.mismatch());
  windows(&m, 1, 99.96, 99.946);
  CHECK(!m.mismatch());
}

TEST(rate_match_ignores_an_unmeasured_side) {
  RateMatch m;
  windows(&m, 5, 0.0, 60.0);   // no source rate yet
  CHECK(!m.mismatch());
  windows(&m, 5, 99.96, 0.0);  // no panel rate yet
  CHECK(!m.mismatch());
}

// A quiet sideport says nothing about low-power: a disarmed drone whose
// stats went silent must not be judged at its (deliberately low) rate, and
// a mismatch already raised is not cleared on no evidence either.
TEST(rate_match_holds_while_low_power_is_unknown) {
  RateMatch m;
  windows(&m, 10, 30.0, 100.0, RateMatch::LowPower::kUnknown);
  CHECK(!m.mismatch());
  windows(&m, 3, 99.96, 60.0);
  REQUIRE(m.mismatch());
  windows(&m, 10, 99.96, 99.946, RateMatch::LowPower::kUnknown);
  CHECK(m.mismatch());
}

// What the player knows about low-power: no sideport reader at all (GS OSD
// stats off) means no report will ever come, so the drone is taken as
// armed and the warning still works; a reader whose sideport went quiet
// knows nothing.
TEST(low_power_report_from_the_sideport) {
  using LP = RateMatch::LowPower;
  CHECK(maburplay::low_power_report(false, false, false) == LP::kNo);
  CHECK(maburplay::low_power_report(true, true, true) == LP::kYes);
  CHECK(maburplay::low_power_report(true, true, false) == LP::kNo);
  CHECK(maburplay::low_power_report(true, false, true) == LP::kUnknown);
  CHECK(maburplay::low_power_report(true, false, false) == LP::kUnknown);
}

MTEST_MAIN
