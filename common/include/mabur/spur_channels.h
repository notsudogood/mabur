#pragma once
// RTL8812EU (8822E die) channels where the drone's radio stalls.
//
// The 8822E's synthesizer leaves strong spikes at 5760, 5800, 5840 and
// 5880 MHz. With one inside the 20 MHz the drone sends in, the radio stops
// taking frames on time (USB hand-offs 40-170 ms, queue drops, failed
// sends, latency step-downs), carrier sense on or off -- drone 2, bench
// 2026-10-24: 153, 161, 169 at 20 MHz and 173 at 40 (5880 inside the
// 173+177 pair, which phydm's spur table does not cover). 177 is predicted
// from the same spike, untested. 157 and 149 were clean at 20/40, as the
// rule predicts. A warning only: nothing here refuses a channel.
//
// Pure, header-only; the drone checks it against its own chip at bring-up,
// the GS against its config at load (it cannot see the drone's chip).
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mabur {

// Why `ch` stalls an 8812EU at this link width, or nullopt. `width_mhz` is
// the widest the link may fly (radio.width); `spur_ext` is radio.spur_ext,
// devourer's 5880 MHz notch for 177@20 and the 173+177 pair.
inline std::optional<std::string> e8822_stall_reason(uint8_t ch, int width_mhz, bool spur_ext) {
  switch (ch) {
    case 153: return std::string("the 5760 MHz spike is inside its 20 MHz (stalled on the bench)");
    case 161: return std::string("the 5800 MHz spike is inside its 20 MHz (stalled on the bench)");
    case 169: return std::string("the 5840 MHz spike is inside its 20 MHz (stalled on the bench; "
                                 "~70 % of frames failed CRC at the GS)");
    case 177: return std::string("the 5880 MHz spike is inside its 20 MHz (predicted, untested)");
    case 173:
      if (width_mhz < 40) return std::nullopt;
      return std::string(spur_ext ? "the 5880 MHz spike is inside its 40 MHz pair; radio.spur_ext "
                                    "notches it, untested"
                                  : "the 5880 MHz spike is inside its 40 MHz pair and nothing "
                                    "notches it (stalled on the bench; radio.spur_ext = false)");
    default: return std::nullopt;
  }
}

// One line per member (and the pin, when it is not a member) that stalls.
inline std::vector<std::string> e8822_stall_warnings(const std::vector<uint8_t>& set,
                                                     std::optional<uint8_t> pin, int width_mhz,
                                                     bool spur_ext) {
  std::vector<std::string> out;
  std::vector<uint8_t> chans = set;
  if (pin) {
    bool member = false;
    for (uint8_t c : set) member = member || c == *pin;
    if (!member) chans.push_back(*pin);
  }
  for (uint8_t c : chans)
    if (auto why = e8822_stall_reason(c, width_mhz, spur_ext))
      out.push_back("channel " + std::to_string(c) + ": " + *why);
  return out;
}

}  // namespace mabur
