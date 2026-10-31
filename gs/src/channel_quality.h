#pragma once
// gs18 channel bars: one rating per member of the channel set from the
// menu's Scan (ChannelScout::scan_report). 0 = clean (green), 1 = some
// traffic (yellow), 2 = busy (red), -1 = not measured: the link's own
// channel while linked (its pair carries the drone's video), or no scan yet.
//
// A member's rating is its worst measured half: the member itself and, when
// the scan covered it (link width 40+), the other half of its 40 MHz pair --
// the span the link flies most. 80 MHz blocks are not rated as a whole.
//
// NHM busy % (airtime above radio.scan.busy_dbm) decides when the dwell has
// one: on the bench it was the honest signal where the event score was
// fooled by a co-located jammer's leakage (docs/findings/2026-10-04-
// channel-set-bench.md: 32.8 % on the jammed pair, 0-12 % elsewhere).
// Otherwise the event score (ChannelRanker::busy per 250 ms dwell: 4-14 on
// that bench's quiet halves, 444-1286 next to the jammer). Both sets of
// thresholds are provisional: one bench, no field calibration.
#include <cstdint>
#include <vector>

#include "channel_scout.h"
#include "mabur/ht40.h"

namespace maburgs {

inline constexpr double kBarYellowPct = 5.0, kBarRedPct = 20.0;
inline constexpr uint32_t kBarYellowScore = 30, kBarRedScore = 150;

inline int scan_quality(const ScanCell& c) {
  if (!c.measured) return -1;
  if (c.busy_valid) return c.busy_pct >= kBarRedPct ? 2 : c.busy_pct >= kBarYellowPct ? 1 : 0;
  return c.busy >= kBarRedScore ? 2 : c.busy >= kBarYellowScore ? 1 : 0;
}

struct MemberQuality {
  uint8_t ch = 0;
  int q = -1;
  bool busy_valid = false;
  double busy_pct = 0;   // the worst measured half's NHM busy %
};

inline std::vector<MemberQuality> member_quality(const std::vector<uint8_t>& members,
                                                 const ScanReport& r) {
  std::vector<MemberQuality> out;
  auto cell = [&](uint8_t ch) -> const ScanCell* {
    for (const ScanCell& c : r.cells)
      if (c.ch == ch) return &c;
    return nullptr;
  };
  for (uint8_t m : members) {
    MemberQuality mq;
    mq.ch = m;
    const uint8_t other = mabur::ht40_pair_other(m);
    for (const ScanCell* c : {cell(m), other ? cell(other) : nullptr}) {
      if (!c || !c->measured) continue;
      const int q = scan_quality(*c);
      if (q > mq.q) mq.q = q;
      if (c->busy_valid && (!mq.busy_valid || c->busy_pct > mq.busy_pct)) {
        mq.busy_valid = true;
        mq.busy_pct = c->busy_pct;
      }
    }
    out.push_back(mq);
  }
  return out;
}

}  // namespace maburgs
