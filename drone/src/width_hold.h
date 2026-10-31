#pragma once
// The drone radio's narrowing rule (radio.width_step): the radio sits at
// home_width, widens at once for a wider rung (the actuator's
// widen_for_op_), and narrows only once no rung has needed the wider width
// for width_hold_ms, so a bounce between rungs does not switch each time.
//
// Not linked (FAILSAFE or RENDEZVOUS: the drone is not hearing the GS), the
// hold is skipped with radio.narrow_unlinked: the narrowest tune is the
// drone's best receiver, and the GS's control frames are what it is missing.
// With devourer's wide-TX IGI floor (0x35, d17) every 40/80 MHz tune also
// costs the receiver its gain; flight 0009 (d17, home_width 40) lost the
// uplink ~10 dB earlier than flight 0008 and sat 35 s in failsafe with the
// video still arriving.
#include <algorithm>
#include <cstdint>

namespace mabur {

// The width to narrow to on this agent tick, or 0 for no change.
// width_mhz: tuned now. need: the current op's width need (op_width_need_:
// already within [home, max_w]). last_need_ms[0..2]: when 20 / 40 / 80 MHz
// was last needed (0 = never), the caller having stamped `need` at now_ms.
inline uint8_t width_narrow_target(uint8_t width_mhz, uint8_t need, uint8_t home, uint8_t max_w,
                                   const uint64_t last_need_ms[3], uint64_t now_ms,
                                   uint32_t hold_ms, bool linked, bool narrow_unlinked) {
  if (!linked && narrow_unlinked) return need < width_mhz ? need : 0;
  uint8_t held = home;
  for (int i = 2; i >= 0; --i)
    if (last_need_ms[i] != 0 && now_ms - last_need_ms[i] < hold_ms) {
      held = static_cast<uint8_t>(std::max<int>(held, i == 2 ? 80 : i == 1 ? 40 : 20));
      break;
    }
  held = static_cast<uint8_t>(std::min<int>(std::max<int>(held, need), max_w));
  return held < width_mhz ? held : 0;
}

}  // namespace mabur
