#ifndef MABUR_PLAYER_MENU_LAYER_H_
#define MABUR_PLAYER_MENU_LAYER_H_

// Draws the GS menu (gs_menu_model.h) in the Ascent-style layout of mock-up
// v5 -- tab bar, title row with the Hop switch, a grid of cells, Apply and
// Close, a help line -- on top of another GsLayer (the compact bar), so the
// OSD composer drives it through the same five calls and needs no change.
// One per buffer, like the layer it wraps: each remembers which menu
// version (and live channel) its own buffer shows.
//
// Known limit: the box clears what is under it, and an MSP (flight
// controller) cell there is only redrawn when it changes. The menu is a
// ground-side tool; the FC OSD under its box comes back cell by cell.

#include <memory>

#include "gs_font.h"
#include "gs_layer.h"
#include "gs_menu_model.h"

namespace maburplay {

class MenuLayer final : public GsLayer {
 public:
  MenuLayer(std::unique_ptr<GsLayer> inner, GsFont& font,
            std::shared_ptr<const GsMenuModel> model)
      : inner_(std::move(inner)), font_(font), model_(std::move(model)) {}

  bool layout(int screen_w, int screen_h, std::string* err) override;
  int update(const GsSnapshot& snap, bool stale, const GsPlayerState& ps, const Surface& s,
             std::vector<DirtyRect>* out) override;
  int repaint_intersecting(const DirtyRect* rects, size_t n, const Surface& s,
                           std::vector<DirtyRect>* out) override;
  void invalidate() override;
  DirtyRect bounds() const override;

 private:
  // Clears the box and, while the menu is visible, draws it. Returns 1 when
  // something was written.
  int draw_box_(const Surface& s, std::vector<DirtyRect>* out);
  void draw_open_(const Surface& s, const GsMenuView& v);
  // A cell: fill (light grey when active), label left, value right, the
  // green dot when marked, a white outline when focused.
  void draw_cell_(const Surface& s, const DirtyRect& r, const GsMenuCell& c, bool centred);

  std::unique_ptr<GsLayer> inner_;
  GsFont& font_;
  std::shared_ptr<const GsMenuModel> model_;
  const MaskAtlas* atlas_ = nullptr;   // labels
  const MaskAtlas* small_ = nullptr;   // values, help
  DirtyRect box_{};
  // Geometry, from layout(): the panel, the tab bar above it, row heights.
  int panel_x_ = 0, panel_w_ = 0, tabs_y_ = 0, tab_w_ = 0, tab_h_ = 0, panel_y_ = 0;
  int title_h_ = 0, cell_h_ = 0, btn_h_ = 0, pad_ = 0, line_ = 0, outline_ = 2;
  uint64_t drawn_version_ = 0;   // 0 = nothing drawn on this buffer yet
  bool drawn_visible_ = false;
  uint8_t live_ch_ = 0, drawn_live_ = 0;
};

}  // namespace maburplay

#endif  // MABUR_PLAYER_MENU_LAYER_H_
