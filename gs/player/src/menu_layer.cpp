#include "menu_layer.h"

#include <algorithm>

#include "gs_draw.h"

namespace maburplay {

namespace {
// The widest row the model can produce, to size the box once.
constexpr const char* kWidest = "> Channel       < 233 >  (hop picks)";
constexpr int kRowsMax = 7;   // title + 5 items + status
constexpr uint32_t kBoxRgb = 0x1C1F22u;
}  // namespace

bool MenuLayer::layout(int screen_w, int screen_h, std::string* err) {
  if (!inner_->layout(screen_w, screen_h, err)) return false;
  // About a 30th of the height per row: readable at arm's length on the
  // goggles' panel, and the box stays inside the middle half of the screen.
  atlas_ = font_.nearest(std::max(12, screen_h / 30));
  if (!atlas_) return true;   // no font: the menu simply never draws
  pad_ = std::max(4, atlas_->glyph_h / 3);
  line_h_ = atlas_->glyph_h + pad_ / 2;
  const int w = std::min(screen_w, text_width(*atlas_, kWidest) + 2 * pad_);
  const int h = std::min(screen_h, kRowsMax * line_h_ + 2 * pad_);
  box_ = DirtyRect{(screen_w - w) / 2, (screen_h - h) / 3, w, h};
  drawn_version_ = 0;
  return true;
}

int MenuLayer::draw_box_(const Surface& s, std::vector<DirtyRect>* out) {
  if (!atlas_ || box_.w <= 0) return 0;
  const bool vis = model_->visible();
  drawn_version_ = model_->version();
  if (!vis && !drawn_visible_) return 0;
  clear_region(s, box_);
  out->push_back(box_);
  drawn_visible_ = vis;
  if (!vis) return 1;
  fill_rect(s, box_.x, box_.y, box_.w, box_.h, kBoxRgb, 220);
  const std::vector<std::string> rows = model_->lines();
  int y = box_.y + pad_ + atlas_->baseline;
  for (size_t i = 0; i < rows.size() && static_cast<int>(i) < kRowsMax; ++i) {
    const bool title = model_->open() && i == 0;
    const bool sel = model_->open() && static_cast<int>(i) == model_->selected() + 1;
    const uint32_t rgb = title ? tok::kTextLabel : sel ? tok::kStatusOk : tok::kTextPrimary;
    draw_text(s, *atlas_, box_.x + pad_, y, rows[i].c_str(), rgb);
    y += line_h_;
  }
  return 1;
}

int MenuLayer::update(const GsSnapshot& snap, bool stale, const GsPlayerState& ps,
                      const Surface& s, std::vector<DirtyRect>* out) {
  int n = inner_->update(snap, stale, ps, s, out);
  if (model_->version() != drawn_version_) {
    const bool was = drawn_visible_;
    n += draw_box_(s, out);
    // Closing cleared the box: give the bar back any field it covered.
    if (was && !drawn_visible_) n += inner_->repaint_intersecting(&box_, 1, s, out);
  }
  return n;
}

int MenuLayer::repaint_intersecting(const DirtyRect* rects, size_t n, const Surface& s,
                                    std::vector<DirtyRect>* out) {
  int k = inner_->repaint_intersecting(rects, n, s, out);
  if (!drawn_visible_) return k;
  for (size_t i = 0; i < n; ++i) {
    const DirtyRect& r = rects[i];
    if (r.x < box_.x + box_.w && box_.x < r.x + r.w && r.y < box_.y + box_.h &&
        box_.y < r.y + r.h) {
      drawn_visible_ = false;   // force the redraw below to paint
      return k + draw_box_(s, out);
    }
  }
  return k;
}

void MenuLayer::invalidate() {
  inner_->invalidate();
  drawn_version_ = 0;
}

DirtyRect MenuLayer::bounds() const {
  const DirtyRect b = inner_->bounds();
  if (box_.w <= 0) return b;
  if (b.w <= 0) return box_;
  const int x0 = std::min(b.x, box_.x), y0 = std::min(b.y, box_.y);
  const int x1 = std::max(b.x + b.w, box_.x + box_.w), y1 = std::max(b.y + b.h, box_.y + box_.h);
  return DirtyRect{x0, y0, x1 - x0, y1 - y0};
}

}  // namespace maburplay
