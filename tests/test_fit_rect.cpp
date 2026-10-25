// Host test for gs/player/src/fit_rect.h: a picture fitted inside the
// panel at its own aspect ratio (letterbox / pillarbox), never stretched.
#include "fit_rect.h"
#include "mtest.h"

using maburplay::fit_rect;
using maburplay::Rect;

namespace {
bool eq(Rect r, int x, int y, int w, int h) {
  return r.x == x && r.y == y && r.w == w && r.h == h;
}
}  // namespace

TEST(fit_rect_same_aspect_fills_the_panel) {
  CHECK(eq(fit_rect(1728, 972, 2560, 1440), 0, 0, 2560, 1440));
  CHECK(eq(fit_rect(1280, 720, 1920, 1080), 0, 0, 1920, 1080));
  CHECK(eq(fit_rect(1920, 1080, 1920, 1080), 0, 0, 1920, 1080));
}

TEST(fit_rect_wide_source_gets_bars_top_and_bottom) {
  CHECK(eq(fit_rect(1728, 816, 1920, 1080), 0, 86, 1920, 906));
}

TEST(fit_rect_tall_source_gets_bars_left_and_right) {
  const Rect r = fit_rect(1440, 1080, 1920, 1080);  // 4:3 on 16:9
  CHECK(eq(r, 240, 0, 1440, 1080));
}

TEST(fit_rect_is_even_aligned) {
  const Rect r = fit_rect(1728, 816, 2560, 1440);
  CHECK(r.x % 2 == 0 && r.y % 2 == 0 && r.w % 2 == 0 && r.h % 2 == 0);
  CHECK(r.w == 2560);
  CHECK(r.y + r.h <= 1440);
}

TEST(fit_rect_without_a_picture_fills_the_panel) {
  CHECK(eq(fit_rect(0, 0, 1920, 1080), 0, 0, 1920, 1080));
}

MTEST_MAIN
