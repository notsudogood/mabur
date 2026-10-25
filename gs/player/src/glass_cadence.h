#ifndef MABUR_PLAYER_GLASS_CADENCE_H_
#define MABUR_PLAYER_GLASS_CADENCE_H_

#include <cmath>
#include <cstdint>

namespace maburplay {

// Puts the burned DVR's recorded frames on the display's own vblank grid
// instead of the decoder's variable cadence. The first flip fed after
// reset() is vblank index 0 and is always recorded. Each later flip adds
// step = llround((flip - last_flip) / period) to the index, step clamped
// to >= 1 so two flips inside one period (a decoder catching up) still
// advance rather than colliding on the same index; a long hold (a stall)
// jumps the index forward by however many vblanks were skipped, same as
// the display itself held that many frames. slot = index / every thins
// the grid to one sample per `every` vblanks; a flip is recorded iff its
// slot is greater than the last recorded slot, so an empty slot's sample
// is carried forward by whichever later flip lands on the next slot
// (there is always exactly one recorded flip per non-empty slot, taken at
// its first arrival). pts_us = llround(slot * every * period_us) stamps
// the recording on the ideal grid time for that slot, not the flip's own
// (jittery) timestamp.
//
// The rounding is differential -- step comes from the flip-to-flip gap
// divided by period, never from flip_us / period directly -- so a fixed
// ppm clock drift between the panel and the host clock never accumulates
// into a step error: each step still rounds to the nearest integer number
// of periods, and the index stays exactly on the grid no matter how long
// the sequence runs.
class GlassCadence {
 public:
  struct Slot {
    bool record;
    uint64_t pts_us;
  };

  // every < 1 is treated as 1 (every vblank recorded). Forgets the last
  // flip: the next feed() is treated as the first one again (index 0).
  void reset(double period_us, int every) {
    period_us_ = period_us;
    every_ = every < 1 ? 1 : every;
    index_ = 0;
    last_slot_ = -1;
    have_flip_ = false;
  }

  // One call per flip, in flip order.
  Slot feed(uint64_t flip_us) {
    if (!have_flip_) {
      have_flip_ = true;
      last_flip_us_ = flip_us;
      index_ = 0;
    } else {
      const double diff = static_cast<double>(flip_us) - static_cast<double>(last_flip_us_);
      int64_t step = llround(diff / period_us_);
      if (step < 1) step = 1;
      index_ += static_cast<uint64_t>(step);
      last_flip_us_ = flip_us;
    }
    const int64_t slot = static_cast<int64_t>(index_ / static_cast<uint64_t>(every_));
    Slot s{false, 0};
    if (slot > last_slot_) {
      last_slot_ = slot;
      s.record = true;
      s.pts_us = static_cast<uint64_t>(
          llround(static_cast<double>(slot) * every_ * period_us_));
    }
    return s;
  }

  // Vblank index of the last flip fed (0 for the first).
  uint64_t index() const { return index_; }

 private:
  uint64_t last_flip_us_ = 0;
  uint64_t index_ = 0;
  int64_t last_slot_ = -1;
  double period_us_ = 16'667.0;
  int every_ = 1;
  bool have_flip_ = false;
};

}  // namespace maburplay

#endif  // MABUR_PLAYER_GLASS_CADENCE_H_
