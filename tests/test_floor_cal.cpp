// First-boot floor calibration, GS side (gs/src/floor_cal.h).
#include <string>

#include "floor_cal.h"
#include "mtest.h"
using namespace maburgs;

namespace {
FloorCalRungs rungs() {
  FloorCalRungs r;
  r.r40 = 5;
  r.r80 = 7;
  r.r20 = 0;
  return r;
}
FloorView learning(uint8_t arm = 1) {
  FloorView v;
  v.have = true;
  v.learning = true;
  v.arm = arm;
  return v;
}
}  // namespace

// Unverified, disarmed, linked: hold the 40 MHz rung, then 80, then done.
// Revert (no calibration): tick returns -1 throughout.
TEST(holds_40_then_80_then_finishes) {
  FloorCal fc(FloorCalCfg{}, rungs());
  FloorView v = learning();
  CHECK(fc.tick(0, true, v) == 5);
  CHECK(fc.state() == FloorCal::State::Test && fc.width() == 40);
  CHECK(fc.tick(5000, true, v) == 5);
  v.v40 = true;                                  // the drone marked 40
  CHECK(fc.tick(12000, true, v) == 7);
  CHECK(fc.width() == 80);
  v.v80 = true;
  CHECK(fc.tick(24000, true, v) == -1);
  CHECK(fc.state() == FloorCal::State::Done);
  CHECK(fc.tick(30000, true, v) == -1 && fc.state() == FloorCal::State::Done);
  CHECK(fc.tick(40000, true, v) == -1 && fc.state() == FloorCal::State::Idle);
}

// A stall (the drone's count rises): a 20 MHz rung for the unstick, then
// the same width again with a fresh minute.
TEST(a_stall_unsticks_then_retests) {
  FloorCal fc(FloorCalCfg{}, rungs());
  FloorView v = learning();
  CHECK(fc.tick(0, true, v) == 5);
  v.stalls = 1;
  v.floor = 0x24;
  CHECK(fc.tick(1000, true, v) == 0);
  CHECK(fc.state() == FloorCal::State::Unstick && fc.steps() == 1 && fc.floor() == 0x24);
  CHECK(fc.tick(2000, true, v) == 0);
  CHECK(fc.tick(3600, true, v) == 5);            // 2.5 s later: back on 40
  CHECK(fc.state() == FloorCal::State::Test);
  CHECK(fc.tick(60000, true, v) == 5);           // within the fresh minute
}

// Never on: a drone that is not learning, no floor block (an older drone),
// a 20/40-only GS that already has 40 verified.
TEST(not_learning_or_nothing_to_test_is_off_or_idle) {
  FloorCal fc(FloorCalCfg{}, rungs());
  FloorView none;
  CHECK(fc.tick(0, true, none) == -1 && fc.state() == FloorCal::State::Off);
  FloorView fixed = learning();
  fixed.learning = false;
  CHECK(fc.tick(10, true, fixed) == -1 && fc.state() == FloorCal::State::Off);
  FloorCalRungs no80 = rungs();
  no80.r80 = -1;
  FloorCal narrow(FloorCalCfg{}, no80);
  FloorView v = learning();
  v.v40 = true;
  CHECK(narrow.tick(0, true, v) == -1 && narrow.state() == FloorCal::State::Idle);
  FloorCalCfg off;
  off.enable = false;
  FloorCal disabled(off, rungs());
  CHECK(disabled.tick(0, true, learning()) == -1);
}

// Armed, stale, no link or a spike channel: pause and release the rung;
// resume when it clears.
TEST(pauses_when_armed_unlinked_or_on_a_spike) {
  FloorCal fc(FloorCalCfg{}, rungs());
  FloorView v = learning();
  CHECK(fc.tick(0, true, v) == 5);
  v.arm = 2;
  CHECK(fc.tick(100, true, v) == -1);
  CHECK(fc.state() == FloorCal::State::Paused && std::string(fc.why()) == "armed");
  v.arm = 0;                                     // no FC: allowed
  CHECK(fc.tick(200, true, v) == 5);
  CHECK(fc.tick(300, false, v) == -1 && std::string(fc.why()) == "link");
  v.spike = true;
  CHECK(fc.tick(400, true, v) == -1 && std::string(fc.why()) == "spike");
  v.spike = false;
  v.arm = 3;
  CHECK(fc.tick(500, true, v) == -1 && std::string(fc.why()) == "arm_stale");
}

// Gives up: a minute without the mark, or a stall at the top floor. Stays
// given up (no rung held) for the session.
TEST(gives_up_on_timeout_or_top) {
  FloorCal fc(FloorCalCfg{}, rungs());
  FloorView v = learning();
  CHECK(fc.tick(0, true, v) == 5);
  CHECK(fc.tick(60001, true, v) == -1);
  CHECK(fc.state() == FloorCal::State::Failed && std::string(fc.why()) == "timeout");
  CHECK(fc.tick(70000, true, v) == -1 && fc.state() == FloorCal::State::Failed);
  FloorCal top(FloorCalCfg{}, rungs());
  FloorView t = learning();
  CHECK(top.tick(0, true, t) == 5);
  t.stalls = 3;
  t.top = true;
  t.floor = 0x35;
  CHECK(top.tick(1000, true, t) == -1);
  CHECK(top.state() == FloorCal::State::Failed && std::string(top.why()) == "top");
}

MTEST_MAIN
