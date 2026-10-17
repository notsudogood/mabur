#pragma once
// Per-card RX width with 80 MHz rungs (docs/bw80.md "GS card widths").
// At radio.width = 80 the GS cards live at the home width -- 40, the 80 MHz
// block's lower 40 -- and widen to 80 only while it pays: every card while
// an 80 MHz rung is flown, one card while one is probed. A card tuned 80
// receives the 20/40 rungs too, but pays ~1-1.5 dB on them (bench
// 2026-10-17, up to ~3 dB on one card), so the lower rungs keep the home
// width the rest of the time. Switching is one card at a time, so the other
// card keeps receiving through a full retune (tens of ms); ChannelCore runs
// the switch off the core loop. These are the pure decisions, for the test.
#include <cstdint>
#include <vector>

namespace maburgs {

// The width cards sit at when no 80 MHz rung is in play.
inline uint8_t home_width(uint8_t radio_width) { return radio_width == 80 ? 40 : radio_width; }

enum class WideNeed : uint8_t { None = 0, Probe = 1, All = 2 };

// What the link needs this tick: an 80 MHz op needs every card wide; an
// 80 MHz probe (and an op below 80) needs one; outside a session, nothing.
inline WideNeed wide_need(bool in_session, int op_bw, int probe_bw) {
  if (!in_session) return WideNeed::None;
  if (op_bw == 80) return WideNeed::All;
  if (probe_bw == 80) return WideNeed::Probe;
  return WideNeed::None;
}

// Hysteresis: a need stays in force for hold_ms after it was last seen, so
// a probe that pauses between attempts, or a promote/demote bounce at the
// 80 rung's edge, does not retune a card each time.
struct WideHold {
  double last_all_ms = -1e18;
  double last_probe_ms = -1e18;
};

inline WideNeed held_need(WideHold& h, WideNeed now_need, double now_ms, double hold_ms) {
  if (now_need == WideNeed::All) h.last_all_ms = now_ms;
  if (now_need != WideNeed::None) h.last_probe_ms = now_ms;
  if (now_ms - h.last_all_ms < hold_ms) return WideNeed::All;
  if (now_ms - h.last_probe_ms < hold_ms) return WideNeed::Probe;
  return WideNeed::None;
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

// Per-card target width. A card that cannot widen (a CPE relay, 11n only)
// always stays at home.
inline std::vector<uint8_t> width_targets(WideNeed need, const std::vector<bool>& can_widen,
                                          int probe_card, uint8_t home, uint8_t wide) {
  std::vector<uint8_t> t(can_widen.size(), home);
  for (size_t i = 0; i < can_widen.size(); ++i) {
    if (!can_widen[i]) continue;
    if (need == WideNeed::All || (need == WideNeed::Probe && static_cast<int>(i) == probe_card))
      t[i] = wide;
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
