#include "gs_compact.h"

#include <algorithm>
#include <cmath>

#include "gs_draw.h"
#include "gs_font.h"

namespace maburplay {
namespace {

// Design geometry at 1080p. Deliberately tighter than a broadcast safe
// area: this line is a single strip along the bottom whose whole point is
// to fit the most figures at the largest readable size. 32/24 still clears
// the panel bezel on the GS's own display.
constexpr int kInsetX = 32, kInsetY = 24;

// The eleven items, in draw order, and the row each one lands on. This IS
// the reading order on the glass: row 0 is the radio (what the link is
// doing), row 1 the picture (what came out of it). Split by source rather
// than by width -- an even split would put `snr` next to `bitrate`, which
// reads as one continuous line of unrelated figures.
//
// The two rows share columns: an item's column is its position within its
// row, so kOrder pairs ch/bitrate, mcs/res, air/fps, rssi/jit, snr/lat and
// temp/loss. The worst-case grid is 72 cells plus 5 gaps, inside the
// sizing reference row's 76 + 5, so the reference -- not the grid --
// decides the type size (gs_compact.h). REC sits in the corner because in
// the grid it would eat that slack and shift a column whenever the
// recorder was merely armed.
constexpr GsBarField kOrder[] = {
    GsBarField::kCh,   GsBarField::kMcs, GsBarField::kAir,     GsBarField::kRssi,
    GsBarField::kSnr,  GsBarField::kTemp, GsBarField::kRec,    GsBarField::kBitrate,
    GsBarField::kRes,  GsBarField::kFps, GsBarField::kJit,     GsBarField::kLat,
    GsBarField::kLoss,
};
constexpr int kRow[] = {0, 0, 0, 0, 0, 0, GsCompactBar::kCorner, 1, 1, 1, 1, 1, 1};
static_assert(sizeof(kOrder) / sizeof(kOrder[0]) == (size_t)GsBarField::kCount,
              "every field must appear exactly once in the draw order");
static_assert(sizeof(kRow) / sizeof(kRow[0]) == (size_t)GsBarField::kCount,
              "every field needs a row");
constexpr int items_in_row(int row) {
  int n = 0;
  for (int r : kRow) n += (r == row);
  return n;
}
static_assert(items_in_row(0) == GsCompactBar::kCols && items_in_row(1) == GsCompactBar::kCols,
              "both rows fill every column of the shared grid");

// Gap between the two rows, on top of the cell height. The boxes span the
// padded CELL, so stacking by glyph_h alone already separates them; this is
// margin, not clearance.
constexpr int kRowGap = 4;

// Drone SoC caution threshold. The disarmed fan-off spike climbed to 84 C
// in 15 min flattening at ~1 C/min
// (docs/findings/2026-09-19-low-power-spike.md); 70 leaves the pilot a
// margin to act before that.
constexpr int kTempCautionC = 70;

// Top inset for the corner-anchored recording indicator. Deeper than the
// bar's own 24 px bottom inset: the bottom strip is a deliberate band of
// instrumentation, while this one sits alone in the picture and wants the
// clearance a title-safe area gives it.
constexpr int kInsetTop = 40;

bool intersects(const DirtyRect& a, const DirtyRect& b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

DirtyRect union_of(const DirtyRect& a, const DirtyRect& b) {
  if (a.w <= 0 || a.h <= 0) return b;
  if (b.w <= 0 || b.h <= 0) return a;
  const int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
  const int x1 = std::max(a.x + a.w, b.x + b.w), y1 = std::max(a.y + a.h, b.y + b.h);
  return DirtyRect{x0, y0, x1 - x0, y1 - y0};
}

// Per-side shadow pad an atlas bakes in (gs_font.h: glyph_w/glyph_h include
// it, advance_x does not). A field's box spans the padded CELL -- it has to,
// because clear_region must erase the shadow with the glyph -- so the box is
// this much wider than its text on each side.
int pad_h(const MaskAtlas* a) { return (a->glyph_w - a->advance_x) / 2; }

// Space between two items. One character of advance reads as the single
// space the format string implies, but it must also keep two padded boxes
// apart: at 2*pad the boxes ABUT, and any less and one item's clear erases
// its neighbour's last column. The +2 buys a margin the no-overlap test can
// actually assert on.
int item_gap(const MaskAtlas* a) {
  return std::max(a->advance_x, (a->glyph_w - a->advance_x) + 2);
}

// Plain ASCII integer, hyphen-minus for negatives. Not U+2212: a true
// minus is a typographic refinement this line does without, and the ASCII
// form is a character narrower.
std::string ascii_int(double v) { return fmt_int(v); }

// An item's lead: its icon and one space.
std::string tag(const char* icon) { return std::string(icon) + " "; }

std::string join_cards(const char* icon, const GsSnapshot& snap, bool snr) {
  const int n = std::min((int)snap.cards.size(), kMaxCards);
  if (n <= 0) return tag(icon) + "--";
  std::string out = tag(icon);
  for (int i = 0; i < n; ++i) {
    if (i) out += "/";
    const GsCard& c = snap.cards[(size_t)i];
    if (!c.heard) {
      out += "--";
      continue;
    }
    if (snr) {
      out += c.snr_db ? ascii_int(std::clamp(*c.snr_db, -99.0, 999.0)) : "--";
    } else {
      out += c.rssi_dbm ? ascii_int(std::clamp(*c.rssi_dbm, -999.0, 999.0)) : "--";
    }
  }
  return out;
}

std::string repeat_joined(const char* icon, const char* per, int n) {
  if (n <= 0) return tag(icon) + "--";
  std::string out = tag(icon);
  for (int i = 0; i < n; ++i) {
    if (i) out += "/";
    out += per;
  }
  return out;
}

}  // namespace

std::string GsCompactBar::worst_case(GsBarField id, int n_cards,
                                     RecTarget rec_target) {
  const int n = std::clamp(n_cards, 0, kMaxCards);
  switch (id) {
    // "--" is narrower than the numeric form, so the number sizes the box;
    // "(a)" is the auto-channel-select marker (GsSnapshot::scan_auto). The
    // in-flight hop marker (GsSnapshot::hopped, "(h)") shares this same
    // slot -- the two are mutually exclusive (see the render switch) and
    // exactly as wide, so no separate worst case is needed for it. "KEY?"
    // (link key mismatch) replaces the whole item and is narrower still.
    case GsBarField::kCh:      return tag(gs_icon::kCh) + "999(a)";
    // 40 MHz rungs: the width suffix makes the live form the WIDER one
    // ("9/40" beats the dash form "--") -- an explicit worst case rather
    // than whatever the live value happens to be.
    case GsBarField::kMcs:     return tag(gs_icon::kMcs) + "9/40";
    case GsBarField::kAir:     return tag(gs_icon::kAir) + "100%";
    case GsBarField::kRssi:    return repeat_joined(gs_icon::kRssi, "-999", n);
    case GsBarField::kSnr:     return repeat_joined(gs_icon::kSnr, "-99", n);
    // int8 on the wire, rendered clamped to [0, 127]: "127" beats "--" by
    // one glyph.
    case GsBarField::kTemp:    return tag(gs_icon::kTemp) + "127";
    case GsBarField::kBitrate: return tag(gs_icon::kBitrate) + "999.9";
    // The decoded height plus "p", clamped to four digits.
    case GsBarField::kRes:     return tag(gs_icon::kRes) + "9999p";
    case GsBarField::kFps:     return tag(gs_icon::kFps) + "999";
    case GsBarField::kJit:     return tag(gs_icon::kJit) + "999.9";
    case GsBarField::kLat:     return tag(gs_icon::kLat) + "999/999";
    case GsBarField::kLoss:    return tag(gs_icon::kLoss) + "100.0/100.0";
    // Sized on rec_worst(target) (gs_layer.h): the widest REC text this
    // dvr.target can produce -- "● REC FAULT" for "gs", as it always was.
    // Armed renders nothing and leaves the box blank -- the same
    // fixed-width reservation every other item makes.
    case GsBarField::kRec:     return rec_worst(rec_target);
    case GsBarField::kCount:   break;
  }
  return "";
}

int GsCompactBar::row_of(GsBarField id) {
  for (int i = 0; i < (int)GsBarField::kCount; ++i)
    if (kOrder[i] == id) return kRow[i];
  return 0;
}

int GsCompactBar::col_of(GsBarField id) {
  int col[kRows] = {};
  for (int i = 0; i < (int)GsBarField::kCount; ++i) {
    if (kRow[i] == kCorner) {
      if (kOrder[i] == id) return -1;
      continue;
    }
    if (kOrder[i] == id) return col[kRow[i]];
    ++col[kRow[i]];
  }
  return -1;
}

// Each column's width in `a`: the wider of its items' worst cases.
static void column_widths(const MaskAtlas& a, int n_cards, int* cw) {
  for (int k = 0; k < GsCompactBar::kCols; ++k) cw[k] = 0;
  for (int i = 0; i < (int)GsBarField::kCount; ++i) {
    const int k = GsCompactBar::col_of(kOrder[i]);
    if (k < 0) continue;
    cw[k] = std::max(cw[k], text_width(a, GsCompactBar::worst_case(kOrder[i], n_cards).c_str()));
  }
}

int GsCompactBar::worst_grid_width(const MaskAtlas& a, int n_cards) {
  int cw[kCols];
  column_widths(a, n_cards, cw);
  int w = 0;
  for (int k = 0; k < kCols; ++k) w += cw[k];
  // Gaps between columns, then the first box's left pad and the last one's
  // right.
  return w + (kCols - 1) * item_gap(&a) + 2 * pad_h(&a);
}

int GsCompactBar::sizing_row_width(const MaskAtlas& a) {
  return kSizingChars * a.advance_x + (kSizingItems - 1) * item_gap(&a) + 2 * pad_h(&a);
}

bool GsCompactBar::layout(int screen_w, int screen_h, std::string* err) {
  laid_out_ = false;
  atlas_ = nullptr;
  for (Field& f : fields_) f = Field{};
  bounds_ = DirtyRect{0, 0, 0, 0};
  // A re-layout must force the next update() to reconcile the card count
  // again, even if it reports what it did before this layout() threw the
  // boxes away: a stale n_cards_ would make that reconciliation a no-op.
  n_cards_ = -1;

  if (screen_w <= 0 || screen_h <= 0) {
    if (err) *err = "gs osd: bad screen size";
    return false;
  }
  const double scale = (double)screen_h / 1080.0;
  const int inset_x = (int)(kInsetX * scale + 0.5);
  const int inset_y = (int)(kInsetY * scale + 0.5);
  const int avail_w = screen_w - 2 * inset_x;

  // Largest baked size at which the sizing reference row AND both
  // worst-case rows fit, and the stack fits the height (gs_compact.h). One
  // type size, no per-resolution table: the same rule renders a readable
  // bar on a 720p panel and a bigger one on a 4K panel.
  const MaskAtlas* best = nullptr;
  int best_px = 0;
  for (int px = 1; px <= 512; ++px) {
    const MaskAtlas* a = font_.atlas(px);
    if (!a || a->advance_x <= 0) continue;
    bool fits = sizing_row_width(*a) <= avail_w &&
                worst_grid_width(*a, kMaxCards) <= avail_w;
    if (!fits) continue;
    // The stack also has to sit ABOVE the bottom inset without running off
    // the top of a short surface.
    const int block_h = kRows * a->glyph_h + (kRows - 1) * (int)(kRowGap * scale + 0.5);
    if (block_h + inset_y > screen_h) continue;
    if (a->px > best_px) { best_px = a->px; best = a; }
  }
  if (!best) {
    if (err) *err = "gs osd: compact bar does not fit at this screen size";
    return false;
  }

  atlas_ = best;
  rec_atlas_ = font_.nearest(best->px / 2);
  screen_w_ = screen_w;
  inset_x_ = inset_x;
  gap_ = item_gap(best);
  // The cell's descender sits below the baseline, so the baseline has to
  // rise by that much for the BOX -- shadow pad included -- to clear the
  // bottom inset. Rows stack by a full glyph_h plus a margin: a shorter
  // pitch would have row 0's padded box overlap row 1's, and an overlap is
  // one row's clear erasing the other's ink.
  const int row_pitch = best->glyph_h + (int)(kRowGap * scale + 0.5);
  baseline_y_[kRows - 1] = screen_h - inset_y - (best->glyph_h - best->baseline);
  for (int row = kRows - 2; row >= 0; --row)
    baseline_y_[row] = baseline_y_[row + 1] - row_pitch;
  // The corner item hangs off the TOP inset instead, so its box's top edge
  // -- shadow pad included -- sits exactly on it.
  corner_baseline_ = (int)(kInsetTop * scale + 0.5) + rec_atlas_->baseline;
  // Reserve the worst case until the first snapshot says how many cards
  // there really are. Nothing draws before then (update() reconciles the
  // count first), but bounds() is legitimately asked for in between, and it
  // must cover everything the bar can ever touch.
  place_(kMaxCards);
  laid_out_ = true;
  return true;
}

void GsCompactBar::place_(int n_cards) {
  if (!atlas_) return;
  const int pad = pad_h(atlas_);
  int w[(size_t)GsBarField::kCount];
  for (int i = 0; i < (int)GsBarField::kCount; ++i) {
    f_(kOrder[i]).active = true;
    w[i] = text_width(*atlas_for_(kOrder[i]),
                      worst_case(kOrder[i], n_cards, rec_target_).c_str());
  }

  // The corner item: right-flushed at the top inset, reserving the shadow
  // pad so its box does not overhang the inset it is flush against.
  for (int i = 0; i < (int)GsBarField::kCount; ++i) {
    if (kRow[i] != kCorner) continue;
    Field& f = f_(kOrder[i]);
    const MaskAtlas* ca = atlas_for_(kOrder[i]);
    const int cpad = pad_h(ca);
    f.pen_x = screen_w_ - inset_x_ - w[i] - cpad;
    f.baseline_y = corner_baseline_;
    f.box = DirtyRect{f.pen_x - cpad, corner_baseline_ - ca->baseline,
                      w[i] + 2 * cpad, ca->glyph_h};
    bounds_ = union_of(bounds_, f.box);
  }

  // The rows share a column grid (gs_compact.h), and the GRID is centred.
  // Each item starts at its column's left edge; its box spans only its own
  // worst case, not the column, so a narrow item's redraw stays narrow.
  int cw[kCols];
  column_widths(*atlas_, n_cards, cw);
  int total = (kCols - 1) * gap_;
  for (int k = 0; k < kCols; ++k) total += cw[k];
  // Centred, but never past the inset: with kMaxCards the grid can be wide
  // enough that centring and the inset disagree, and the inset wins.
  int col_x[kCols];
  col_x[0] = std::max((screen_w_ - total) / 2, inset_x_ + pad);
  for (int k = 1; k < kCols; ++k) col_x[k] = col_x[k - 1] + cw[k - 1] + gap_;
  for (int i = 0; i < (int)GsBarField::kCount; ++i) {
    const int row = kRow[i];
    if (row == kCorner) continue;
    Field& f = f_(kOrder[i]);
    f.pen_x = col_x[col_of(kOrder[i])];
    f.baseline_y = baseline_y_[row];
    f.box = DirtyRect{f.pen_x - pad, baseline_y_[row] - atlas_->baseline,
                      w[i] + 2 * pad, atlas_->glyph_h};
    bounds_ = union_of(bounds_, f.box);
  }
}

GsCompactBar::FieldState GsCompactBar::state_of_(const GsSnapshot& snap,
                                                 bool stale,
                                                 const GsPlayerState& ps,
                                                 GsBarField id) const {
  FieldState st;
  // The one styling rule this line has: LINK-sourced items dim while the
  // sideport is quiet and hold their last value; player-measured ones stay
  // lit because they are current by construction. Without the dim, a dead
  // sideport leaves a line of entirely plausible frozen numbers.
  const uint32_t link = stale ? tok::kTextLabel : tok::kTextPrimary;
  st.rgb = tok::kTextPrimary;

  // Every clamp below keeps the rendered string inside worst_case(id) --
  // see the note on that function. gs_snapshot.cpp rejects non-finite and
  // non-numeric JSON but bounds no magnitude, so an unclamped air_pct of
  // 1e300 would draw a ~300-character string past its own box and never be
  // erased again.
  switch (id) {
    case GsBarField::kCh:
      st.rgb = link;
      // Our key file differs from the drone's (link pairing): no RCFs go
      // out in this state, so this takes
      // priority over the hop/scan suffixes below -- the channel number
      // itself is irrelevant while the link can't be controlled. Gated on
      // !stale: a frozen last-known key_mismatch from a dead sideport
      // would otherwise paint a status the player can no longer vouch for.
      if (snap.key_mismatch && !stale) {
        st.text = "KEY?";
        st.rgb = tok::kStatusCaution;
        break;
      }
      st.text = tag(gs_icon::kCh) +
                (snap.channel ? ascii_int(std::clamp(*snap.channel, 0, 999)) : "--");
      // In-flight channel hop takes priority over the boot-scan mark below --
      // both share one suffix slot (worst_case() reserves exactly "(a)"'s
      // width), and a hop mid-flight is the more actionable of the two for
      // the pilot.
      if (snap.hopped) {
        st.text += "(h)";
      } else if (snap.scan_auto) {
        // radio.scan enabled on the GS: this channel may be a pick, not the
        // configured home -- say so, in the bar's own plain-text idiom.
        st.text += "(a)";
      }
      break;
    case GsBarField::kMcs:
      st.rgb = link;
      if (snap.mcs) {
        st.text = tag(gs_icon::kMcs) + ascii_int(std::clamp(*snap.mcs, 0, 9)) + "/" +
                  (snap.bw ? ascii_int(std::clamp(*snap.bw, 20, 40)) : std::string("--"));
      } else {
        st.text = tag(gs_icon::kMcs) + "--";
      }
      break;
    case GsBarField::kAir:
      st.rgb = link;
      st.text = tag(gs_icon::kAir) +
                (snap.air_pct ? fmt_int(std::clamp(*snap.air_pct, 0.0, 100.0)) + "%" : "--");
      break;
    case GsBarField::kRssi:
      st.rgb = link;
      st.text = join_cards(gs_icon::kRssi, snap, /*snr=*/false);
      break;
    case GsBarField::kSnr:
      st.rgb = link;
      st.text = join_cards(gs_icon::kSnr, snap, /*snr=*/true);
      break;
    case GsBarField::kTemp:
      // Drone SoC temperature off the 1 Hz Telem. Link-sourced, so it dims
      // and holds while stale like its neighbours; the caution tint is
      // gated on !stale for the same reason the fps cell's is (a frozen
      // value must not keep claiming "hot"). One threshold only -- the
      // palette has no third status colour (gs_layer.h, tok).
      st.rgb = link;
      if (snap.soc_temp_c) {
        const int c = std::clamp(*snap.soc_temp_c, 0, 127);
        st.text = tag(gs_icon::kTemp) + ascii_int(c);
        if (c >= kTempCautionC && !stale) st.rgb = tok::kStatusCaution;
      } else {
        st.text = tag(gs_icon::kTemp) + "--";
      }
      break;
    case GsBarField::kBitrate:
      // Player-measured: AU bytes off the ring, not an encoder setpoint.
      st.text = tag(gs_icon::kBitrate) + fmt_one_dp(std::clamp(ps.mbps, 0.0, 999.9));
      break;
    case GsBarField::kRes:
      // The short form: decoded height plus "p" (1280x720 -> 720p). A 4:3
      // mode reads the same as its 16:9 sibling (1440x1080 -> 1080p).
      st.text = tag(gs_icon::kRes) +
                ((ps.vid_w > 0 && ps.vid_h > 0)
                     ? ascii_int(std::clamp(ps.vid_h, 0, 9999)) + "p"
                     : "--");
      break;
    case GsBarField::kFps:
      st.text = tag(gs_icon::kFps) + fmt_int(std::clamp(ps.fps, 0.0, 999.0));
      // Low-power (pre-arm) mode: the rate is low on purpose. Caution
      // tint, same text. The CELL is player-measured and so never dims --
      // but the FLAG is link-sourced, so it is gated on !stale: once the
      // sideport goes quiet, snap.low_power is the last value heard and
      // may be arbitrarily old, and tinting a live, correct fps number
      // from a frozen flag is exactly the "looks live" failure the
      // staleness rule exists to prevent. Quiet sideport = no claim.
      if (snap.low_power && !stale) st.rgb = tok::kStatusCaution;
      break;
    case GsBarField::kJit:
      st.text = tag(gs_icon::kJit) + fmt_one_dp(std::clamp(ps.jitter_ms, 0.0, 999.9));
      break;
    case GsBarField::kLat:
      // p50 then p99. No ~ marker for the relative case: this line has no room for a
      // qualifier, and lat_valid already gates the only state where the
      // number would be a fabrication.
      st.text = tag(gs_icon::kLat) +
                (ps.lat_valid
                     ? fmt_int(std::clamp((double)ps.lat_p50_e2e_ms, 0.0, 999.0)) + "/" +
                           fmt_int(std::clamp((double)ps.lat_e2e_ms, 0.0, 999.0))
                     : "--/--");
      break;
    case GsBarField::kLoss:
      st.rgb = link;
      st.text = tag(gs_icon::kLoss);
      st.text += snap.pre_loss_pct
                     ? fmt_one_dp(std::clamp(*snap.pre_loss_pct, 0.0, 100.0))
                     : "--";
      st.text += "/";
      st.text += snap.post_loss_pct
                     ? fmt_one_dp(std::clamp(*snap.post_loss_pct, 0.0, 100.0))
                     : "--";
      break;
    case GsBarField::kRec: {
      // Never dimmed -- the recorder is the player's own business and says
      // nothing about the link.
      const RecText t = rec_text(ps.rec);
      st.text = t.text;
      st.rgb = t.rgb;
      st.aux = t.aux;
      break;
    }
    case GsBarField::kCount:
      break;
  }
  return st;
}

std::string GsCompactBar::debug_field_text(const GsSnapshot& snap, bool stale,
                                           const GsPlayerState& ps,
                                           GsBarField id) const {
  return state_of_(snap, stale, ps, id).text;
}

int GsCompactBar::debug_atlas_px() const { return atlas_ ? atlas_->px : 0; }
int GsCompactBar::debug_rec_atlas_px() const { return rec_atlas_ ? rec_atlas_->px : 0; }

DirtyRect GsCompactBar::debug_field_box(GsBarField id) const {
  return f_(id).box;
}

void GsCompactBar::draw_field_(GsBarField id, const FieldState& st,
                               const Surface& s) {
  Field& f = f_(id);
  const MaskAtlas* a = atlas_for_(id);
  if (!a || !f.active) return;
  clear_region(s, f.box);
  if (st.text.empty()) return;  // cleared above; nothing more to draw

  int pen_x = f.pen_x;
  if (id == GsBarField::kRec) {
    // kRec's box is sized to rec_worst(target) (gs_layer.h), wider than most
    // of what rec_text() actually returns -- a GS-only "REC mm:ss"/"REC
    // FAULT" must land exactly where it always did, not wherever the widest
    // VTX+GS fault combo would start. Right-align within the box: the pad on
    // both sides is symmetric, so box_right - pad - text_width mirrors
    // layout()'s box_left + pad.
    const int pad = (a->glyph_w - a->advance_x) / 2;
    pen_x = f.box.x + f.box.w - pad - text_width(*a, st.text.c_str());
  }

  if (st.aux == 1) {
    // The recording dot takes kStatusRec while the rest of the item takes
    // the field colour.
    const int adv = draw_text(s, *a, pen_x, f.baseline_y, kDotFilled,
                              tok::kStatusRec);
    draw_text(s, *a, pen_x + adv, f.baseline_y,
              st.text.c_str() + std::string(kDotFilled).size(), st.rgb);
    return;
  }
  draw_text(s, *a, pen_x, f.baseline_y, st.text.c_str(), st.rgb);
}

int GsCompactBar::update(const GsSnapshot& snap, bool stale,
                         const GsPlayerState& ps, const Surface& s,
                         std::vector<DirtyRect>* out) {
  if (!laid_out_) return 0;
  int drawn = 0;

  // A changed card count changes the width of two items, so of their
  // columns, and therefore -- the grid being centred -- the x of every
  // column, on both rows. Erase everything: a column move takes the row-1
  // item under it along. A field's own next draw only ever clears its NEW box.
  const int n = std::min((int)snap.cards.size(), kMaxCards);
  if (n != n_cards_) {
    if (n_cards_ >= 0) {
      for (int i = 0; i < (int)GsBarField::kCount; ++i) {
        // The corner item is anchored, not centred: a card count change
        // cannot move it, so erasing it would cost a redraw for nothing.
        if (kRow[i] == kCorner) continue;
        Field& f = f_(kOrder[i]);
        if (!f.active) continue;
        clear_region(s, f.box);
        if (out) out->push_back(f.box);
        ++drawn;
      }
    }
    place_(n);
    for (Field& f : fields_) f.valid = false;
    n_cards_ = n;
  }

  for (int i = 0; i < (int)GsBarField::kCount; ++i) {
    const GsBarField id = kOrder[i];
    Field& f = f_(id);
    if (!f.active) continue;
    const FieldState st = state_of_(snap, stale, ps, id);
    if (f.valid && f.last == st) continue;
    f.last = st;
    f.valid = true;
    draw_field_(id, st, s);
    ++drawn;
    if (out) out->push_back(f.box);
  }
  return drawn;
}

int GsCompactBar::repaint_intersecting(const DirtyRect* rects, size_t n,
                                       const Surface& s,
                                       std::vector<DirtyRect>* out) {
  if (!laid_out_ || !rects || n == 0) return 0;
  int drawn = 0;
  for (int i = 0; i < (int)GsBarField::kCount; ++i) {
    const GsBarField id = kOrder[i];
    Field& f = f_(id);
    if (!f.active || !f.valid) continue;
    bool hit = false;
    for (size_t r = 0; r < n; ++r)
      if (intersects(f.box, rects[r])) { hit = true; break; }
    if (!hit) continue;
    draw_field_(id, f.last, s);
    ++drawn;
    if (out) out->push_back(f.box);
  }
  return drawn;
}

void GsCompactBar::invalidate() {
  for (Field& f : fields_) f.valid = false;
}

}  // namespace maburplay
