// mabur/spur_channels.h: the 8812EU channels that stall the drone's radio.
#include "mabur/spur_channels.h"
#include "mtest.h"
using namespace mabur;

TEST(stall_channels_at_20_are_the_four_spike_channels) {
  for (uint8_t ch : {153, 161, 169, 177}) CHECK(e8822_stall_reason(ch, 20, false).has_value());
  // 173's 20 MHz (5855-5875) holds no spike; its 40 MHz pair holds 5880.
  CHECK(!e8822_stall_reason(173, 20, false));
  CHECK(e8822_stall_reason(173, 40, false).has_value());
  CHECK(e8822_stall_reason(173, 80, false).has_value());
  // With the notch it still warns, saying untested.
  CHECK(e8822_stall_reason(173, 40, true)->find("untested") != std::string::npos);
}

TEST(bench_clean_channels_do_not_warn) {
  for (uint8_t ch : {36, 100, 104, 108, 112, 116, 120, 124, 132, 144, 149, 157})
    for (int w : {20, 40, 80}) CHECK(!e8822_stall_reason(ch, w, false));
}

TEST(warnings_cover_the_set_and_a_pin_outside_it) {
  CHECK(e8822_stall_warnings({36, 100, 116, 132}, std::nullopt, 80, false).empty());
  auto w = e8822_stall_warnings({132, 153}, std::optional<uint8_t>(161), 80, false);
  REQUIRE(w.size() == 2);
  CHECK(w[0].rfind("channel 153:", 0) == 0 && w[1].rfind("channel 161:", 0) == 0);
  // A pin that is a member is not listed twice.
  CHECK(e8822_stall_warnings({153}, std::optional<uint8_t>(153), 20, false).size() == 1);
}

MTEST_MAIN
