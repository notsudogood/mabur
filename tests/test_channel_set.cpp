#include <vector>
#include "mtest.h"
#include "mabur/channel_set.h"
using namespace mabur;

TEST(default_set_is_valid_at_both_widths) {
  const std::vector<uint8_t> s = {40, 64, 112, 144};
  CHECK(!channel_set_issue(s, 40, "radio.channels").has_value());
  CHECK(!channel_set_issue(s, 20, "radio.channels").has_value());
}

TEST(size_and_uniqueness_and_range) {
  CHECK(channel_set_issue({}, 20, "x")->why.find("1 to 8") != std::string::npos);
  CHECK(channel_set_issue({40, 64, 112, 144, 36, 44, 60, 108, 124}, 20, "x").has_value());  // 9
  CHECK(channel_set_issue({40, 40}, 20, "x")->why.find("duplicate") != std::string::npos);
  CHECK(channel_set_issue({0}, 20, "x").has_value());
  CHECK(channel_set_issue({178}, 20, "x").has_value());
  CHECK(channel_set_issue({40, 64, 112, 144, 36, 44, 60, 108}, 20, "x") == std::nullopt);  // 8 ok
}

TEST(width_40_needs_pair_primaries_sharing_one_offset) {
  CHECK(channel_set_issue({165}, 40, "x")->why.find("no 40 MHz pair") != std::string::npos);
  CHECK(channel_set_issue({40, 36}, 40, "x")->why.find("offset") != std::string::npos);  // 36 is lower half
  CHECK(channel_set_issue({136, 144}, 40, "x") == std::nullopt);                         // both upper
  CHECK(channel_set_issue({132, 140}, 40, "x") == std::nullopt);                         // both lower
  CHECK(channel_set_issue({165}, 20, "x") == std::nullopt);                              // 20 MHz: any channel
}

// Width 80: every member is an 80 MHz block's lowest 20, the VHT80 primary
// devourer tunes (36 52 100 116 132 149); the size/duplicate rules still hold.
TEST(width_80_needs_block_lowest_channels) {
  CHECK(channel_set_issue({36, 132}, 80, "x") == std::nullopt);
  CHECK(channel_set_issue({36, 52, 100, 116, 132, 149}, 80, "x") == std::nullopt);
  CHECK(channel_set_issue({36, 40}, 80, "x")->why.find("80 MHz block") != std::string::npos);
  CHECK(channel_set_issue({165}, 80, "x").has_value());
  CHECK(channel_set_issue({36, 36}, 80, "x")->why.find("duplicate") != std::string::npos);
}

TEST(field_name_rides_through) {
  CHECK(channel_set_issue({}, 20, "radio.channels")->field == "radio.channels");
}

TEST(membership) {
  const std::vector<uint8_t> s = {40, 64, 112, 144};
  CHECK(channel_set_member(s, 112));
  CHECK(!channel_set_member(s, 136));
  CHECK(!channel_set_member(s, 0));
}

MTEST_MAIN
