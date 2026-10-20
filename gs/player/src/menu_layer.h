#ifndef MABUR_PLAYER_MENU_LAYER_H_
#define MABUR_PLAYER_MENU_LAYER_H_

// Draws the GS menu (gs_menu_model.h) as a box in the middle of the GS
// overlay, on top of another GsLayer (the compact bar), so the OSD
// composer drives it through the same five calls and needs no change.
// One per buffer, like the layer it wraps: each remembers which menu
// version its own buffer shows.
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

  std::unique_ptr<GsLayer> inner_;
  GsFont& font_;
  std::shared_ptr<const GsMenuModel> model_;
  const MaskAtlas* atlas_ = nullptr;
  DirtyRect box_{};
  int line_h_ = 0, pad_ = 0;
  uint64_t drawn_version_ = 0;   // 0 = nothing drawn on this buffer yet
  bool drawn_visible_ = false;
};

}  // namespace maburplay

#endif  // MABUR_PLAYER_MENU_LAYER_H_
