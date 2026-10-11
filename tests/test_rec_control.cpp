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

// The raw recorder's keyframe ask (2026-10-11): counted, never a wish.
TEST(rec_control_counts_keyframe_requests_without_touching_the_wish) {
  RecControl c;
  CHECK(c.idr_requests() == 0);   // epoch 0: a link-up asks for nothing
  CHECK(!c.apply("idr"));
  CHECK(!c.apply("idr\n"));
  CHECK(c.idr_requests() == 2);
  CHECK(c.wire() == 0);           // still unknown: an ask is not a VTX wish
  CHECK(c.apply("vtx_rec on"));
  CHECK(!c.apply("idr"));
  CHECK(c.idr_requests() == 3);
  CHECK(c.wire() == (mabur::rc::kRecKnown | mabur::rc::kRecOn));
}

MTEST_MAIN
