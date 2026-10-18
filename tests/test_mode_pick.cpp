#include <vector>

#include "mode_pick.h"
#include "mtest.h"

using maburplay::ModeCand;
using maburplay::parse_screen_want;
using maburplay::pick_mode;

namespace {
// CEA timings: 1080p60.000 (148.5 MHz), 1080p59.94 (148.352 MHz), 1080i60,
// 720p60.
const ModeCand k1080p5994{1920, 1080, 148352, 2200, 1125, false, false};
const ModeCand k1080p60{1920, 1080, 148500, 2200, 1125, false, true};
const ModeCand k1080i60{1920, 1080, 74250, 2200, 1125, true, false};
const ModeCand k720p60{1280, 720, 74250, 1650, 750, false, false};
}  // namespace

TEST(parse_accepts_integer_and_decimal_rates) {
  auto a = parse_screen_want("1920x1080@60");
  CHECK(a.ok && a.w == 1920 && a.h == 1080 && a.hz == 60.0);
  auto b = parse_screen_want("1920x1080@59.94");
  CHECK(b.ok && b.hz > 59.93 && b.hz < 59.95);
  CHECK(!parse_screen_want("1920x1080@60mhz").ok);
  CHECK(!parse_screen_want("1080p").ok);
  CHECK(!parse_screen_want("1920x1080@0").ok);
}

TEST(at60_prefers_60000_over_5994) {
  const std::vector<ModeCand> modes{k1080p5994, k1080i60, k1080p60, k720p60};
  bool matched = false;
  CHECK(pick_mode(modes, parse_screen_want("1920x1080@60"), &matched) == 2);
  CHECK(matched);
}

TEST(at5994_forces_5994) {
  const std::vector<ModeCand> modes{k1080p60, k1080p5994};
  bool matched = false;
  CHECK(pick_mode(modes, parse_screen_want("1920x1080@59.94"), &matched) == 1);
  CHECK(matched);
}

TEST(a_5994_only_sink_still_matches_at60) {
  const std::vector<ModeCand> modes{k1080i60, k1080p5994};
  bool matched = false;
  CHECK(pick_mode(modes, parse_screen_want("1920x1080@60"), &matched) == 1);
  CHECK(matched);
}

TEST(no_match_falls_back_to_preferred_then_first) {
  bool matched = true;
  const std::vector<ModeCand> modes{k720p60, k1080p60};
  CHECK(pick_mode(modes, parse_screen_want("1920x1080@50"), &matched) == 1);
  CHECK(!matched);
  const std::vector<ModeCand> no_pref{k720p60, k1080p5994};
  CHECK(pick_mode(no_pref, parse_screen_want("3840x2160@60"), &matched) == 0);
  CHECK(pick_mode({}, parse_screen_want("1920x1080@60"), &matched) == -1);
}

TEST(interlaced_never_matches) {
  const std::vector<ModeCand> modes{k1080i60};
  bool matched = true;
  pick_mode(modes, parse_screen_want("1920x1080@60"), &matched);
  CHECK(!matched);
}

MTEST_MAIN
