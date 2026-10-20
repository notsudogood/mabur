// MenuLayer (gs/player/src/menu_layer.h): the GS menu box drawn over the
// compact bar, driven only through GsLayer's five calls.
#include "mtest.h"

#include <memory>
#include <vector>

#include "gs_font.h"
#include "gs_layer.h"
#include "gs_menu_model.h"
#include "menu_layer.h"

using namespace maburplay;

namespace {
// Records calls; draws nothing.
struct StubLayer : GsLayer {
  int updates = 0, repaints = 0;
  bool layout(int, int, std::string*) override { return true; }
  int update(const GsSnapshot&, bool, const GsPlayerState&, const Surface&,
             std::vector<DirtyRect>*) override { ++updates; return 0; }
  int repaint_intersecting(const DirtyRect*, size_t, const Surface&,
                           std::vector<DirtyRect>*) override { ++repaints; return 0; }
  void invalidate() override {}
  DirtyRect bounds() const override { return DirtyRect{0, 1000, 1920, 80}; }
};

bool any_pixel(const std::vector<uint32_t>& px, const DirtyRect& r, int stride) {
  for (int y = r.y; y < r.y + r.h; ++y)
    for (int x = r.x; x < r.x + r.w; ++x)
      if (px[static_cast<size_t>(y) * stride + x] != 0) return true;
  return false;
}
}  // namespace

TEST(menu_box_draws_when_open_and_clears_on_close) {
  GsFont font;
  std::string err;
  REQUIRE(font.load(GSFONT_SCALED, &err));
  auto model = std::make_shared<GsMenuModel>(std::vector<uint8_t>{36, 132}, true,
                                             GsMenuSettings{true, 36, true});
  auto stub = std::make_unique<StubLayer>();
  StubLayer* sp = stub.get();
  MenuLayer ml(std::move(stub), font, model);
  REQUIRE(ml.layout(1920, 1080, &err));
  const DirtyRect b = ml.bounds();
  CHECK(b.y < 1000 && b.y + b.h >= 1080);   // the bar plus the box above it

  std::vector<uint32_t> px(1920 * 1080, 0);
  const Surface s{px.data(), 1920, 1080, 1920};
  GsSnapshot snap;
  GsPlayerState ps;
  std::vector<DirtyRect> out;
  // Closed: the bar updates, the box draws nothing.
  ml.update(snap, false, ps, s, &out);
  CHECK(sp->updates == 1 && out.empty());
  // Open: one dirty rect, pixels inside it.
  model->key(GsMenuModel::Key::Center, 0);
  out.clear();
  CHECK(ml.update(snap, false, ps, s, &out) == 1);
  REQUIRE(out.size() == 1);
  CHECK(any_pixel(px, out[0], 1920));
  // Unchanged: nothing redrawn.
  out.clear();
  CHECK(ml.update(snap, false, ps, s, &out) == 0 && out.empty());
  // Close: the box is cleared and the bar asked to repaint under it.
  model->key(GsMenuModel::Key::Up, 1);
  model->key(GsMenuModel::Key::Center, 2);   // Close
  out.clear();
  ml.update(snap, false, ps, s, &out);
  REQUIRE(!out.empty());
  CHECK(!any_pixel(px, out[0], 1920));
  CHECK(sp->repaints == 1);
}
MTEST_MAIN
