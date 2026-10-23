#include <cmath>

#include "mtest.h"
#include "transit_window.h"

using maburgs::TransitWindow;

// Median over the AUs that closed in the last window_ms; NaN under min_n.
TEST(transit_window_median_and_expiry) {
  TransitWindow w(200.0, 3);
  CHECK(std::isnan(w.p50(0.0)));
  w.add(0.0, 1.0);
  w.add(20.0, 2.0);
  CHECK(std::isnan(w.p50(20.0)));      // two samples: no verdict
  w.add(40.0, 90.0);
  CHECK(w.p50(40.0) == 2.0);
  w.add(60.0, 120.0);
  w.add(80.0, 130.0);
  CHECK(w.p50(80.0) == 90.0);
  // 230 ms: the samples at 0 and 20 are out; 40, 60, 80 remain.
  CHECK(w.p50(230.0) == 120.0);
  // 400 ms: everything has aged out.
  CHECK(std::isnan(w.p50(400.0)));
}

TEST(transit_window_clear_and_cap) {
  TransitWindow w(1e9, 1);
  for (int i = 0; i < 2000; ++i) w.add(static_cast<double>(i), static_cast<double>(i));
  // Capped at kCap newest samples: 1488..1999, median index 256 -> 1744.
  CHECK(w.p50(2000.0) == 1744.0);
  w.clear();
  CHECK(std::isnan(w.p50(2000.0)));
}

MTEST_MAIN
