#include "mtest.h"
#include "width_hold.h"
using namespace mabur;

// radio.width 80, home 20, hold 3 s: the radio stays wide for the hold after
// the last wide rung, then narrows to the need.
TEST(narrows_only_after_the_hold_while_linked) {
  uint64_t need[3] = {10000, 9500, 9000};   // 20 now, 40 at 9.5 s, 80 at 9.0 s
  CHECK(width_narrow_target(80, 20, 20, 80, need, 10000, 3000, true, true) == 0);   // 80 still held
  CHECK(width_narrow_target(80, 20, 20, 80, need, 12100, 3000, true, true) == 40);  // 80 lapsed, 40 held
  CHECK(width_narrow_target(80, 20, 20, 80, need, 12600, 3000, true, true) == 20);  // both lapsed
  CHECK(width_narrow_target(20, 20, 20, 80, need, 12600, 3000, true, true) == 0);   // already there
}

// d18, flight 0009: in failsafe the drone was deaf at a wide tune with the
// IGI floor on. Not linked, it narrows to the need at once. Revert (no
// linked argument / narrow_unlinked ignored): the first CHECK returns 0.
TEST(not_linked_narrows_at_once) {
  uint64_t need[3] = {10000, 9900, 9950};
  CHECK(width_narrow_target(80, 20, 20, 80, need, 10000, 3000, false, true) == 20);
  CHECK(width_narrow_target(40, 20, 20, 80, need, 10000, 3000, false, true) == 20);
  CHECK(width_narrow_target(20, 20, 20, 80, need, 10000, 3000, false, true) == 0);
  // a 40 home: the need is 40, so 80 -> 40 and no further
  CHECK(width_narrow_target(80, 40, 40, 80, need, 10000, 3000, false, true) == 40);
  // radio.narrow_unlinked = false: the hold applies (d17)
  CHECK(width_narrow_target(80, 20, 20, 80, need, 10000, 3000, false, false) == 0);
}

// The old home: with home 40 nothing goes below 40 (why d17 sat at 40 with
// the floor on at MCS0/20).
TEST(a_40_home_never_narrows_below_40) {
  uint64_t need[3] = {20000, 0, 0};
  CHECK(width_narrow_target(80, 40, 40, 80, need, 20000, 3000, true, true) == 40);
  CHECK(width_narrow_target(40, 40, 40, 80, need, 20000, 3000, true, true) == 0);
}

MTEST_MAIN
