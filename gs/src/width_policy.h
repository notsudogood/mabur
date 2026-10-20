#pragma once
// Per-card RX width (docs/bw80.md "GS card widths"). At radio.width = 80
// the GS cards live at the home width -- 40, the primary's own pair -- and
// widen to 80 only while it pays: every card while an 80 MHz rung is flown,
// one card while one is probed. A card tuned 80 receives the 20/40 rungs
// too, but pays ~1-1.5 dB on them (bench 2026-10-17, up to ~3 dB on one
// card), so the lower rungs keep the home width the rest of the time.
// radio.narrow_20 extends the same rule one step down: on the 20 MHz rungs
// every card sits at 20 (SNR readout 0.8-1.1 dB better than tuned 40, near
// its ceiling), and the 20 -> 40 probe widens one card first, as the 80
// probe does. Switching is one card at a time, so the other card keeps
// receiving through the switch; ChannelCore runs it off the core loop.
// These are the pure decisions, for the test.
#include <algorithm>
#include <cstdint>
#include <vector>

namespace maburgs {

// The width cards sit at when no rung asks for anything else, and outside
// a session.
inline uint8_t home_width(uint8_t radio_width) { return radio_width == 80 ? 40 : radio_width; }

// The widths the link needs: every card at `all`, the probe card at
// `probe` (>= all). Both lie in [floor, max] -- floor is the home width, or
// 20 with radio.narrow_20 -- and outside a session both are the home width.
struct WidthNeed {
  uint8_t all = 0, probe = 0;
  bool operator==(const WidthNeed& o) const { return all == o.all && probe == o.probe; }
};

inline uint8_t clamp_width(int w, uint8_t floor_w, uint8_t max_w) {
  return static_cast<uint8_t>(std::min<int>(std::max<int>(w, floor_w), max_w));
}

inline WidthNeed width_need(bool in_session, int op_bw, int probe_bw, uint8_t home,
                            uint8_t floor_w, uint8_t max_w) {
  if (!in_session) return WidthNeed{home, home};
  WidthNeed n;
  n.all = clamp_width(op_bw, floor_w, max_w);
  n.probe = std::max(n.all, probe_bw > 0 ? clamp_width(probe_bw, floor_w, max_w) : n.all);
  return n;
}

// Hysteresis: a width stays in force for hold_ms after it was last needed,
// so a probe that pauses between attempts, or a promote/demote bounce at a
// rung's edge, does not retune a card each time. Widening is immediate,
// narrowing waits out the hold.
struct WidthHold {
  double last_all_ms[3] = {-1e18, -1e18, -1e18};     // 20, 40, 80
  double last_probe_ms[3] = {-1e18, -1e18, -1e18};
};

inline int width_slot(uint8_t w) { return w >= 80 ? 2 : w >= 40 ? 1 : 0; }
inline uint8_t slot_width(int i) { return i == 2 ? 80 : i == 1 ? 40 : 20; }

inline WidthNeed held_width_need(WidthHold& h, WidthNeed now, double now_ms, double hold_ms) {
  h.last_all_ms[width_slot(now.all)] = now_ms;
  h.last_probe_ms[width_slot(now.probe)] = now_ms;
  WidthNeed out = now;
  for (int i = 2; i >= 0; --i)
    if (now_ms - h.last_all_ms[i] < hold_ms) { out.all = std::max(out.all, slot_width(i)); break; }
  for (int i = 2; i >= 0; --i)
    if (now_ms - h.last_probe_ms[i] < hold_ms) { out.probe = std::max(out.probe, slot_width(i)); break; }
  out.probe = std::max(out.probe, out.all);
  return out;
}

// The card that widens for a probe. Sticky: `current` stays while it can
// still widen, so a TX-selector flip does not swap which card is wide.
// Otherwise the first capable card that is not the TX card (retuning the TX
// card costs the uplink a beat), and the TX card only on a one-card GS.
// -1 = no card can widen.
inline int pick_probe_card(const std::vector<bool>& can_widen, int tx_card, int current) {
  const int n = static_cast<int>(can_widen.size());
  if (current >= 0 && current < n && can_widen[static_cast<size_t>(current)]) return current;
  int fallback = -1;
  for (int i = 0; i < n; ++i) {
    if (!can_widen[static_cast<size_t>(i)]) continue;
    if (i != tx_card) return i;
    fallback = i;
  }
  return fallback;
}

// Per-card target width. A card that cannot switch (a CPE relay, 11n only)
// always stays at home.
inline std::vector<uint8_t> width_targets(WidthNeed need, const std::vector<bool>& can_switch,
                                          int probe_card, uint8_t home) {
  std::vector<uint8_t> t(can_switch.size(), home);
  for (size_t i = 0; i < can_switch.size(); ++i) {
    if (!can_switch[i]) continue;
    t[i] = static_cast<int>(i) == probe_card ? need.probe : need.all;
  }
  return t;
}

// Which card to switch next, or -1. Only ready cards that are off target
// and not excluded (a dwell owns them, or this target already failed on
// them); among those, a non-TX card first, then the lowest index. The
// caller allows one switch in flight and spaces them.
inline int next_width_switch(const std::vector<uint8_t>& cur, const std::vector<uint8_t>& want,
                             const std::vector<bool>& eligible, int tx_card) {
  int tx_pick = -1;
  for (size_t i = 0; i < cur.size(); ++i) {
    if (!eligible[i] || cur[i] == want[i]) continue;
    if (static_cast<int>(i) != tx_card) return static_cast<int>(i);
    tx_pick = static_cast<int>(i);
  }
  return tx_pick;
}

}  // namespace maburgs
