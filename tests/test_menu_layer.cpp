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

// OsdComposer passes no rect list unless the card block moved (gs_layer.h):
// gs11 opened the menu on the operator's GS by pushing through that null
// and segfaulting (2026-10-22, exit 139 right after "menu key centre").
TEST(menu_box_draws_with_no_rect_list) {
  GsFont font;
  std::string err;
  REQUIRE(font.load(GSFONT_SCALED, &err));
  auto model = std::make_shared<GsMenuModel>(std::vector<uint8_t>{36, 132}, true,
                                             GsMenuSettings{true, 36, true});
  MenuLayer ml(std::make_unique<StubLayer>(), font, model);
  REQUIRE(ml.layout(1920, 1080, &err));
  std::vector<uint32_t> px(1920 * 1080, 0);
  const Surface s{px.data(), 1920, 1080, 1920};
  GsSnapshot snap;
  GsPlayerState ps;
  model->key(GsMenuModel::Key::Center, 0);
  CHECK(ml.update(snap, false, ps, s, nullptr) == 1);
  CHECK(any_pixel(px, ml.bounds(), 1920));
  model->key(GsMenuModel::Key::Up, 1);
  model->key(GsMenuModel::Key::Center, 2);   // Close
  ml.update(snap, false, ps, s, nullptr);
}

// gs18: the Playback tab (8 recordings, four rows) is the tallest; the box
// the layer clears was sized for the Channel tab's two rows, so the list's
// lower rows and the help strip were drawn below it and stayed on screen
// after a tab switch, through playback and back to live (operator video,
// 2026-11-01). Everything a tab draws must lie inside the box, so closing
// leaves no pixel behind. Revert (kMaxCellRows 2): pixels remain after close.
TEST(menu_box_covers_the_playback_tab) {
  GsFont font;
  std::string err;
  REQUIRE(font.load(GSFONT_SCALED, &err));
  auto model = std::make_shared<GsMenuModel>(std::vector<uint8_t>{36, 100, 116, 132}, true,
                                             GsMenuSettings{true, 36, true});
  MenuLayer ml(std::make_unique<StubLayer>(), font, model);
  REQUIRE(ml.layout(1920, 1080, &err));
  std::vector<uint32_t> px(1920 * 1080, 0);
  const Surface s{px.data(), 1920, 1080, 1920};
  GsSnapshot snap;
  GsPlayerState ps;
  std::vector<GsMenuModel::RecItem> recs;
  for (int i = 0; i < GsMenuModel::kMaxRecordings + 2; ++i)   // more than fit: capped
    recs.push_back({"record-00" + std::to_string(10 + i), 60000000});
  model->set_playback_allowed(true, "");
  model->key(GsMenuModel::Key::Center, 0);   // open, on the tab row
  model->key(GsMenuModel::Key::Left, 1);     // Channel -> Playback
  model->set_recordings(recs);
  std::vector<DirtyRect> out;
  REQUIRE(ml.update(snap, false, ps, s, &out) == 1);
  REQUIRE(out.size() == 1);
  const DirtyRect box = out[0];
  for (int y = 0; y < 1080; ++y)
    for (int x = 0; x < 1920; ++x)
      if (px[static_cast<size_t>(y) * 1920 + x] != 0) {
        CHECK(x >= box.x && x < box.x + box.w && y >= box.y && y < box.y + box.h);
        if (!(x >= box.x && x < box.x + box.w && y >= box.y && y < box.y + box.h)) {
          y = 1080;
          break;
        }
      }
  model->key(GsMenuModel::Key::Right, 2);    // Playback -> Channel: a shorter tab
  out.clear();
  ml.update(snap, false, ps, s, &out);
  model->tick(10 * 60 * 1000);               // idle: the menu closes
  REQUIRE(!model->open());
  ml.update(snap, false, ps, s, &out);
  CHECK(!any_pixel(px, DirtyRect{0, 0, 1920, 1080}, 1920));
}
// gs19: during playback the live bar goes and only the box draws (operator,
// 2026-11-01: "hide the live OSD during playback too"). The stub's bounds
// stand in for the bar; a pixel planted there plays the bar's last frame.
TEST(playback_clears_the_bar_and_keeps_the_banner) {
  GsFont font;
  std::string err;
  REQUIRE(font.load(GSFONT_SCALED, &err));
  auto model = std::make_shared<GsMenuModel>(std::vector<uint8_t>{36, 132}, true,
                                             GsMenuSettings{true, 36, true});
  auto stub = std::make_unique<StubLayer>();
  StubLayer* sp = stub.get();
  MenuLayer ml(std::move(stub), font, model);
  REQUIRE(ml.layout(1920, 1080, &err));
  std::vector<uint32_t> px(1920 * 1080, 0);
  const Surface s{px.data(), 1920, 1080, 1920};
  GsSnapshot snap;
  GsPlayerState live, play;
  play.live_hidden = true;
  const DirtyRect bar{0, 1000, 1920, 80};
  ml.update(snap, false, live, s, nullptr);
  REQUIRE(sp->updates == 1);
  px[static_cast<size_t>(1040) * 1920 + 960] = 0xFFFFFFFFu;   // the bar's last frame
  model->set_banner("Playing record-0012  0:05 / 1:00");
  std::vector<DirtyRect> out;
  ml.update(snap, false, play, s, &out);
  CHECK(sp->updates == 1);                    // the bar is not drawn
  CHECK(!any_pixel(px, bar, 1920));           // and what it showed is gone
  REQUIRE(!out.empty());
  CHECK(any_pixel(px, DirtyRect{0, 0, 1920, 1000}, 1920));   // the banner is up
  // Hidden: no repaint reaches the bar, and a second update clears nothing.
  ml.repaint_intersecting(&bar, 1, s, nullptr);
  CHECK(sp->repaints == 0);
  out.clear();
  CHECK(ml.update(snap, false, play, s, &out) == 0 && out.empty());
  // Back to live: the bar updates again (invalidated: a whole repaint).
  model->set_banner("");
  ml.update(snap, false, live, s, nullptr);
  CHECK(sp->updates == 2);
}
MTEST_MAIN
