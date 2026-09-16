#include <cstdint>

#include "mcs_mode.h"
#include "mtest.h"

using maburgs::McsMode;

TEST(empty_reports_unknown) {
  McsMode m;
  CHECK(m.mode() == -1);
  CHECK(m.samples() == 0);
}

// 255 is devourer's "no usable rate" code (legacy/VHT, or frame-file replay).
// It must be skipped outright, never folded as a value -- a window of nothing
// but unknowns has to read "I don't know", not "mcs255".
TEST(unknown_codes_are_skipped_not_folded) {
  McsMode m;
  for (int i = 0; i < 10; ++i) m.feed(McsMode::kUnknown);
  CHECK(m.mode() == -1);
  CHECK(m.samples() == 0);
  // Anything above HT's mcs7 is equally unusable.
  m.feed(200);
  m.feed(8);
  CHECK(m.mode() == -1);
}

TEST(single_rate_reports_that_rate) {
  McsMode m;
  for (int i = 0; i < 5; ++i) m.feed(4);
  CHECK(m.mode() == 4);
  CHECK(m.samples() == 5);
}

TEST(unknowns_interleaved_with_a_real_rate_do_not_dilute_it) {
  McsMode m;
  for (int i = 0; i < 5; ++i) { m.feed(McsMode::kUnknown); m.feed(2); }
  CHECK(m.mode() == 2);
  CHECK(m.samples() == 5);
}

TEST(mode_picks_the_majority_not_the_latest) {
  McsMode m;
  for (int i = 0; i < 10; ++i) m.feed(5);
  m.feed(0);  // one stray frame, e.g. a mis-decoded descriptor
  CHECK(m.mode() == 5);
}

// The window must fully turn over after a real rate change, so a switch reads
// as a switch rather than blending the two rungs indefinitely.
TEST(window_turns_over_after_a_rate_change) {
  McsMode m;
  for (int i = 0; i < McsMode::kWindow; ++i) m.feed(5);
  REQUIRE(m.mode() == 5);
  for (int i = 0; i < McsMode::kWindow; ++i) m.feed(0);
  CHECK(m.mode() == 0);
  CHECK(m.samples() == McsMode::kWindow);
}

// Mid-change the window holds both rungs. A tie must resolve DOWNWARD: the
// controller then scores against the lower rung's (larger) FEC budget, which
// under-reads utilisation rather than over-reading it. Over-reading is what
// fires a phantom demote, so the safe direction is the low one.
TEST(ties_break_toward_the_lower_mcs) {
  McsMode m;
  for (int i = 0; i < McsMode::kWindow / 2; ++i) { m.feed(7); m.feed(1); }
  REQUIRE(m.samples() == McsMode::kWindow);
  CHECK(m.mode() == 1);
}

TEST(reset_clears_the_window) {
  McsMode m;
  for (int i = 0; i < 8; ++i) m.feed(3);
  REQUIRE(m.mode() == 3);
  m.reset();
  CHECK(m.mode() == -1);
  CHECK(m.samples() == 0);
}

MTEST_MAIN
