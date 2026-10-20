#pragma once
// The channel set: the members both ends agree the link may live on.
// Pure, header-only, shared by the drone and GS config loaders and by
// RcAgent/ChannelPlan membership checks.
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "mabur/ht40.h"

namespace mabur {

struct ChannelSetIssue {
  std::string field, why;
};

inline bool channel_set_member(const std::vector<uint8_t>& set, uint8_t ch) {
  return ch != 0 && std::find(set.begin(), set.end(), ch) != set.end();
}

// nullopt = valid. 1-8 unique members in [1,kMaxChannel]; at 40 MHz every
// member is a standard HT40 pair primary and all share one ht40_offset
// (FastRetune keeps the offset; docs/bw40.md "Channels"). At 80 MHz every
// member is one of the four 20s of a standard 80 MHz block (vht80_ok; any of
// them may be the primary) and, as at 40, all share one ht40_offset: the
// 40 MHz rungs and the 40 MHz home width ride the primary's own pair.
inline std::optional<ChannelSetIssue> channel_set_issue(const std::vector<uint8_t>& set,
                                                        uint8_t width_mhz,
                                                        const std::string& field) {
  if (set.empty() || set.size() > 8)
    return ChannelSetIssue{field, "must list 1 to 8 channels"};
  for (size_t i = 0; i < set.size(); ++i) {
    const uint8_t ch = set[i];
    if (ch < 1 || ch > kMaxChannel)
      return ChannelSetIssue{field, "channel " + std::to_string(ch) + " must be in [1," +
                                        std::to_string(kMaxChannel) + "]"};
    for (size_t j = 0; j < i; ++j)
      if (set[j] == ch)
        return ChannelSetIssue{field, "duplicate channel " + std::to_string(ch)};
  }
  if (width_mhz == 80) {
    for (uint8_t ch : set)
      if (!vht80_ok(ch))
        return ChannelSetIssue{field, "channel " + std::to_string(ch) +
                                          " is in no 80 MHz block (36-64, 100-144, 149 up, "
                                          "every 4th channel)"};
  }
  if (width_mhz == 40 || width_mhz == 80) {
    const uint8_t off0 = ht40_offset(set[0]);
    for (uint8_t ch : set) {
      const uint8_t off = ht40_offset(ch);
      if (off == 0)
        return ChannelSetIssue{field, "channel " + std::to_string(ch) +
                                          " has no 40 MHz pair (docs/bw40.md)"};
      if (off != off0)
        return ChannelSetIssue{
            field, "channel " + std::to_string(ch) +
                       " is on the other side of the pair grid from channel " +
                       std::to_string(set[0]) +
                       "; FastRetune keeps the offset, every member must share one (docs/bw40.md)"};
    }
  }
  return std::nullopt;
}

}  // namespace mabur
