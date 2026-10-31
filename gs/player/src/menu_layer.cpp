#include "menu_layer.h"

#include <algorithm>
#include <string>

#include "gs_draw.h"

namespace maburplay {

namespace {
// Mock-up v5 colours: dark grey cells on a darker panel, the open tab and
// the live channel light grey, white text, a white outline on the selection.
constexpr uint32_t kPanelRgb = 0x1E2023u;
constexpr uint32_t kTitleRgb = 0x26282Bu;
constexpr uint32_t kCellRgb = 0x2F3134u;
constexpr uint32_t kActiveRgb = 0x8E9194u;
constexpr uint32_t kOutlineRgb = 0xFFFFFFu;
constexpr uint32_t kKnobOffRgb = 0x1A1C1Eu;
constexpr uint32_t kTextRgb = 0xF2F3F5u;
constexpr int kMaxCellRows = 2;   // radio.channels holds at most 8, 4 per row
constexpr int kHelpLines = 2;

int scaled(int v1080, int screen_h) { return std::max(1, v1080 * screen_h / 1080); }

void outline(const Surface& s, const DirtyRect& r, int t, uint32_t rgb) {
  fill_rect(s, r.x, r.y, r.w, t, rgb);
  fill_rect(s, r.x, r.y + r.h - t, r.w, t, rgb);
  fill_rect(s, r.x, r.y, t, r.h, rgb);
  fill_rect(s, r.x + r.w - t, r.y, t, r.h, rgb);
}

int baseline_in(const MaskAtlas& a, const DirtyRect& r) {
  return r.y + (r.h - a.glyph_h) / 2 + a.baseline;
}

// Greedy word wrap to `w` pixels, at most `max_lines` (the last one keeps
// whatever is left).
std::vector<std::string> wrap(const MaskAtlas& a, const std::string& text, int w, int max_lines) {
  std::vector<std::string> out;
  std::string line, word;
  auto flush_word = [&] {
    if (word.empty()) return;
    const std::string cand = line.empty() ? word : line + " " + word;
    if (!line.empty() && text_width(a, cand.c_str()) > w &&
        static_cast<int>(out.size()) + 1 < max_lines) {
      out.push_back(line);
      line = word;
    } else {
      line = cand;
    }
    word.clear();
  };
  for (char c : text) {
    if (c == ' ') flush_word();
    else word += c;
  }
  flush_word();
  if (!line.empty()) out.push_back(line);
  return out;
}
}  // namespace

bool MenuLayer::layout(int screen_w, int screen_h, std::string* err) {
  if (!inner_->layout(screen_w, screen_h, err)) return false;
  // About a 30th of the height for labels, a 45th for values and help:
  // readable at arm's length on the goggles' panel (mock-up v5 at 1080p).
  atlas_ = font_.nearest(std::max(12, screen_h / 30));
  small_ = font_.nearest(std::max(10, screen_h / 45));
  if (!atlas_ || !small_) return true;   // no font: the menu simply never draws
  pad_ = scaled(26, screen_h);
  outline_ = std::max(2, scaled(3, screen_h));
  panel_w_ = std::min(screen_w - 2 * pad_, scaled(1280, screen_h));
  panel_x_ = (screen_w - panel_w_) / 2;
  tab_w_ = std::min(panel_w_ / 2, scaled(270, screen_h));
  tab_h_ = std::max(atlas_->glyph_h + pad_, scaled(96, screen_h));
  tabs_y_ = scaled(110, screen_h);
  panel_y_ = tabs_y_ + tab_h_ + scaled(40, screen_h);
  title_h_ = std::max(atlas_->glyph_h + pad_ / 2, scaled(62, screen_h));
  cell_h_ = std::max(atlas_->glyph_h + pad_, scaled(96, screen_h));
  btn_h_ = std::max(atlas_->glyph_h + pad_ / 2, scaled(72, screen_h));
  line_ = small_->glyph_h + scaled(6, screen_h);
  const int h = panel_y_ + title_h_ + kMaxCellRows * cell_h_ + btn_h_ + pad_ / 2 +
                (kHelpLines + 1) * line_ + pad_ - tabs_y_;
  box_ = DirtyRect{panel_x_ - pad_ / 2, tabs_y_, panel_w_ + pad_, std::min(h, screen_h - tabs_y_)};
  drawn_version_ = 0;
  return true;
}

void MenuLayer::draw_cell_(const Surface& s, const DirtyRect& r, const GsMenuCell& c,
                           bool centred) {
  const int gap = std::max(1, outline_ / 2);
  const DirtyRect in{r.x, r.y, r.w - gap, r.h - gap};
  fill_rect(s, in.x, in.y, in.w, in.h, c.active ? kActiveRgb : kCellRgb);
  // The Scan rating: a bar down the cell's left edge, inside the text inset.
  if (c.bar >= 0) {
    const uint32_t bc = c.bar == 0 ? tok::kStatusOk : c.bar == 1 ? tok::kStatusCaution : tok::kChannelBusy;
    fill_rect(s, in.x, in.y, std::max(4, pad_ / 3), in.h, bc);
  }
  const uint32_t txt = c.dim ? tok::kTextLabel : kTextRgb;
  const int by = baseline_in(*atlas_, in);
  if (centred) {
    const int tw = text_width(*atlas_, c.label.c_str());
    draw_text_over(s, *atlas_, in.x + (in.w - tw) / 2, by, c.label.c_str(), txt);
  } else {
    // Label left and value right on one line; when they do not fit (the
    // dot, a narrow screen), the value goes under the label.
    const char* dot = "● ";
    const int lw = (c.marked ? text_width(*atlas_, dot) : 0) + text_width(*atlas_, c.label.c_str());
    const int vw = c.value.empty() ? 0 : text_width(*small_, c.value.c_str());
    const bool one_line = pad_ + lw + pad_ / 2 + vw + pad_ / 2 <= in.w;
    const int lby = one_line ? by : in.y + (in.h - atlas_->glyph_h - small_->glyph_h) / 2 + atlas_->baseline;
    int x = in.x + pad_;
    if (c.marked) x += draw_text_over(s, *atlas_, x, lby, dot, tok::kStatusOk);
    draw_text_over(s, *atlas_, x, lby, c.label.c_str(), txt);
    if (vw > 0) {
      const uint32_t vc = c.active ? kTextRgb : tok::kTextSecondary;
      if (one_line)
        draw_text_over(s, *small_, in.x + in.w - pad_ / 2 - vw, baseline_in(*small_, in), c.value.c_str(), vc);
      else
        draw_text_over(s, *small_, in.x + pad_, lby - atlas_->baseline + atlas_->glyph_h + small_->baseline,
                  c.value.c_str(), vc);
    }
  }
  if (c.focus) outline(s, in, outline_, kOutlineRgb);
}

void MenuLayer::draw_open_(const Surface& s, const GsMenuView& v) {
  // Tab bar, centred over the panel.
  const int n_tabs = static_cast<int>(v.tabs.size());
  const int tabs_x = panel_x_ + (panel_w_ - n_tabs * tab_w_) / 2;
  for (int t = 0; t < n_tabs; ++t)
    draw_cell_(s, DirtyRect{tabs_x + t * tab_w_, tabs_y_, tab_w_, tab_h_},
               v.tabs[static_cast<size_t>(t)], /*centred=*/true);

  // Title row: "Channel  <" left, the Hop switch right.
  int y = panel_y_;
  const DirtyRect title_r{panel_x_, y, panel_w_, title_h_};
  fill_rect(s, title_r.x, title_r.y, title_r.w, title_r.h, kTitleRgb);
  const std::string title = v.title + "  <";
  draw_text_over(s, *atlas_, panel_x_ + pad_, baseline_in(*atlas_, title_r), title.c_str(), kTextRgb);
  if (v.has_toggle) {
    const int track_w = std::max(8, atlas_->glyph_h * 3 / 2);
    const int track_h = std::max(6, atlas_->glyph_h * 2 / 3);
    const int lw = text_width(*atlas_, v.toggle.label.c_str());
    const int tw = pad_ + lw + pad_ / 2 + track_w + pad_;
    const DirtyRect tr{panel_x_ + panel_w_ - tw, y, tw, title_h_};
    draw_text_over(s, *atlas_, tr.x + pad_, baseline_in(*atlas_, tr), v.toggle.label.c_str(), kTextRgb);
    const int kx = tr.x + pad_ + lw + pad_ / 2, ky = y + (title_h_ - track_h) / 2;
    fill_rect(s, kx, ky, track_w, track_h, v.toggle_on ? tok::kStatusOk : tok::kTrack);
    const int inset = std::max(1, track_h / 6), knob = track_h - 2 * inset;
    const int knob_x = v.toggle_on ? kx + track_w - knob - inset : kx + inset;
    fill_rect(s, knob_x, ky + inset, knob, knob, v.toggle_on ? kOutlineRgb : kKnobOffRgb);
    if (v.toggle.focus) outline(s, tr, outline_, kOutlineRgb);
  }
  y += title_h_;

  // Cells: channels four to a row, or the settings grid.
  const int cols = std::max(1, v.cols);
  const int cw = panel_w_ / cols;
  const size_t ncols = static_cast<size_t>(cols);
  const int rows = static_cast<int>((v.cells.size() + ncols - 1) / ncols);
  for (size_t i = 0; i < v.cells.size(); ++i) {
    const int r = static_cast<int>(i / ncols), c = static_cast<int>(i % ncols);
    draw_cell_(s, DirtyRect{panel_x_ + c * cw, y + r * cell_h_, cw, cell_h_}, v.cells[i], false);
  }
  y += std::max(1, rows) * cell_h_;

  // Apply and Close, a quarter of the panel each.
  const int bw = panel_w_ / 4;
  for (size_t i = 0; i < v.buttons.size(); ++i)
    draw_cell_(s, DirtyRect{panel_x_ + static_cast<int>(i) * bw, y, bw, btn_h_}, v.buttons[i],
               /*centred=*/true);
  y += btn_h_ + pad_ / 2;

  // Help, then any status, under the panel on a dark strip.
  const std::vector<std::string> help = wrap(*small_, v.help, panel_w_ - 2 * pad_, kHelpLines);
  const int lines = static_cast<int>(help.size()) + (v.status.empty() ? 0 : 1);
  if (lines > 0)
    fill_rect(s, panel_x_, y - pad_ / 4, panel_w_, lines * line_ + pad_ / 2, kPanelRgb, 210);
  int by = y + small_->baseline;
  for (const std::string& l : help) {
    draw_text_over(s, *small_, panel_x_ + pad_, by, l.c_str(), tok::kTextSecondary);
    by += line_;
  }
  if (!v.status.empty())
    draw_text_over(s, *small_, panel_x_ + pad_, by, v.status.c_str(), tok::kStatusCaution);
}

int MenuLayer::draw_box_(const Surface& s, std::vector<DirtyRect>* out) {
  if (!atlas_ || !small_ || box_.w <= 0) return 0;
  const bool vis = model_->visible();
  drawn_version_ = model_->version();
  drawn_live_ = live_ch_;
  if (!vis && !drawn_visible_) return 0;
  clear_region(s, box_);
  // Null on most composer updates (gs_layer.h): gs11 pushed through it and
  // crashed maburplay on the first centre press.
  if (out) out->push_back(box_);
  drawn_visible_ = vis;
  if (!vis) return 1;
  const GsMenuView v = model_->view(live_ch_);
  if (v.open) {
    draw_open_(s, v);   // the cells are opaque; the video shows around them
  } else {
    // Closed with a status: one line in a small box where the panel sits.
    const int w = std::min(box_.w, text_width(*small_, v.status.c_str()) + 2 * pad_);
    const DirtyRect r{panel_x_ + (panel_w_ - w) / 2, panel_y_, w, btn_h_};
    fill_rect(s, r.x, r.y, r.w, r.h, kPanelRgb, 220);
    draw_text_over(s, *small_, r.x + pad_, baseline_in(*small_, r), v.status.c_str(), kTextRgb);
  }
  return 1;
}

int MenuLayer::update(const GsSnapshot& snap, bool stale, const GsPlayerState& ps,
                      const Surface& s, std::vector<DirtyRect>* out) {
  int n = inner_->update(snap, stale, ps, s, out);
  live_ch_ = snap.channel && *snap.channel > 0 && *snap.channel < 256
                 ? static_cast<uint8_t>(*snap.channel) : 0;
  const bool live_moved = model_->open() && live_ch_ != drawn_live_;
  if (model_->version() != drawn_version_ || live_moved) {
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
