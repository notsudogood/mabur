#ifndef MABUR_PLAYER_FIT_RECT_H_
#define MABUR_PLAYER_FIT_RECT_H_

#include <cstdint>

namespace maburplay {

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
};

// The largest rectangle with the picture's aspect ratio that fits inside
// the panel, centred: black bars top and bottom for a wider picture, left
// and right for a narrower one, none for the same aspect. Every field is
// even (NV12 chroma and the display planes want even offsets and sizes).
// A picture of unknown size fills the panel.
inline Rect fit_rect(int src_w, int src_h, int dst_w, int dst_h) {
  Rect r{0, 0, dst_w, dst_h};
  if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return r;
  const int64_t wide = static_cast<int64_t>(src_w) * dst_h;
  const int64_t tall = static_cast<int64_t>(src_h) * dst_w;
  auto even = [](int64_t num, int64_t den) {  // num/den rounded to an even integer
    return static_cast<int>(2 * ((num + den) / (2 * den)));
  };
  if (wide > tall) {
    r.h = even(static_cast<int64_t>(dst_w) * src_h, src_w);
    r.y = ((dst_h - r.h) / 2) & ~1;
  } else if (wide < tall) {
    r.w = even(static_cast<int64_t>(dst_h) * src_w, src_h);
    r.x = ((dst_w - r.w) / 2) & ~1;
  }
  return r;
}

}  // namespace maburplay

#endif  // MABUR_PLAYER_FIT_RECT_H_
