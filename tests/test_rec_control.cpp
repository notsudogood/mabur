#include "mtest.h"
#include "rec_control.h"
#include "mabur/rc_proto.h"

using maburgs::RecControl;

TEST(rec_control_starts_unknown) {
  RecControl c;
  CHECK(c.wire() == 0);   // unknown: the drone keeps whatever it is doing
}

TEST(rec_control_on_off_and_holds_without_decay) {
  RecControl c;
  CHECK(c.apply("vtx_rec on"));
  CHECK(c.wire() == (mabur::rc::kRecKnown | mabur::rc::kRecOn));
  CHECK(!c.apply("vtx_rec on\n"));   // same wish: no change
  CHECK(c.wire() == (mabur::rc::kRecKnown | mabur::rc::kRecOn));  // held, no timer
  CHECK(c.apply("vtx_rec off"));
  CHECK(c.wire() == mabur::rc::kRecKnown);
}

TEST(rec_control_ignores_garbage) {
  RecControl c;
  CHECK(!c.apply(""));
  CHECK(!c.apply("vtx_rec maybe"));
  CHECK(!c.apply("start"));
  CHECK(c.wire() == 0);
}

// `refresh <reason>` lines are queued verbatim and never touch the VTX
// record wish; malformed lines are ignored.
TEST(rec_control_refresh_lines) {
  maburgs::RecControl c;
  CHECK(!c.apply("refresh start"));         // not a wish change ...
  CHECK(!c.apply("refresh dvr\n"));         // ... trailing newline tolerated
  CHECK(!c.apply("refresh"));               // no reason: ignored
  CHECK(!c.apply("refresh  resync"));       // two spaces: ignored, the line is exact
  CHECK(!c.apply("refreshstart"));
  auto got = c.take_refresh();
  REQUIRE(got.size() == 2);
  CHECK(got[0] == "start" && got[1] == "dvr");
  CHECK(c.take_refresh().empty());
  CHECK(c.wire() == 0);                     // the VTX wish is still unknown
  CHECK(c.apply("vtx_rec on"));
  CHECK(!c.apply("refresh decoder"));
  CHECK(c.wire() == (mabur::rc::kRecKnown | mabur::rc::kRecOn));
}

// `phase <mfps>`: maburplay's phase-lock offset, held until the player goes
// quiet for kPhaseTimeoutMs, then 0 (the drone falls back to its trim).
TEST(rec_control_phase_line_held_with_timeout) {
  RecControl c;
  CHECK(c.snr_off(0) == 0);                   // nothing heard yet: no steering
  CHECK(!c.apply("phase -37", 1000));         // never a VTX wish change
  CHECK(c.snr_off(1000) == -37);
  CHECK(c.snr_off(1000 + RecControl::kPhaseTimeoutMs) == -37);
  CHECK(c.snr_off(1001 + RecControl::kPhaseTimeoutMs) == 0);
  CHECK(!c.apply("phase 200\n", 5000));       // the range edge, newline tolerated
  CHECK(c.snr_off(5000) == 200);
  CHECK(c.wire() == 0);                       // the VTX wish is untouched
}

TEST(rec_control_phase_rejects_bad_values) {
  RecControl c;
  c.apply("phase 12", 0);
  c.apply("phase 201", 10);
  c.apply("phase -201", 10);
  c.apply("phase abc", 10);
  c.apply("phase 5x", 10);
  CHECK(c.snr_off(10) == 12);   // the last good value stays
  CHECK(c.phase_rejects() == 4);
}

MTEST_MAIN
