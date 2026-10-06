#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>

#include "mabur/rc_proto.h"

namespace mabur {

// Listen window, drone side (feedback-repair rollout phase 3,
// docs/feedback-repair-rollout.md).
//
// The GS sends a T_STATUS at every burst end it sees. While those keep
// arriving, the drone keeps the air quiet for `listen_ms` after each burst so
// the next status has somewhere to land: the next AU's first body (and any
// late repair body of the last one) is held until the gap ends, and so is a
// control/MSP/pong send that would fall inside it. Nothing here touches the
// radio: the hot thread records each AU's gap, the TX writer and the direct
// senders ask how long to wait, the RX thread times each status against its
// AU's gap, and the agent thread takes a T_LWSTAT once per period.
//
// A gap starts at the burst's MODELLED end on air (AirClock::free_at_us after
// the AU and its probe are booked) -- the drone cannot see the chip finish.
// Model error therefore lands in the histogram as a shifted distribution, and
// the status RX stamp includes the drone's own USB RX latency; both are read
// off T_LWSTAT, not corrected for here.
//
// Thread-safe (one mutex, a handful of calls per AU). Time is the caller's
// steady-clock µs, so tests drive it synthetically.
class ListenWindow {
 public:
  // The gap switches off this long after the last status: a GS that stopped
  // asking (feature off, A/B off phase, uplink gone) costs no video.
  static constexpr uint64_t kStatusTimeoutUs = 500000;
  // Drone-side cap on the gap whatever the GS asks for.
  static constexpr uint32_t kMaxGapUs = 10000;
  static constexpr int kRing = 32;

  // RX thread: a CRC-clean T_STATUS addressed to this drone.
  void on_status(const rc::Status& s, uint64_t rx_us) {
    std::lock_guard<std::mutex> l(m_);
    last_status_us_ = rx_us;
    have_status_ = true;
    req_gap_us_ = std::min<uint32_t>(static_cast<uint32_t>(s.listen_ms) * 1000u, kMaxGapUs);
    ++status_rx_;
    const Gap* g = s.fid == rc::kStatusNoFid ? nullptr : find(s.fid);
    if (!g) {
      ++nofid_;
      return;
    }
    const int64_t off = static_cast<int64_t>(rx_us) - static_cast<int64_t>(g->from);
    ++hist_[bin(off)];
  }

  // The gap to keep after the next burst, µs: the last status's listen_ms
  // while statuses keep arriving, else 0.
  uint32_t gap_us(uint64_t now_us) const {
    std::lock_guard<std::mutex> l(m_);
    return live(now_us) ? req_gap_us_ : 0;
  }

  // Hot thread: AU `fid`'s burst (its probe included) is modelled off air at
  // from_us; the gap it keeps runs to until_us (== from_us when none).
  void on_au_gap(uint16_t fid, uint64_t from_us, uint64_t until_us) {
    std::lock_guard<std::mutex> l(m_);
    head_ = (head_ + 1) % kRing;
    ring_[static_cast<size_t>(head_)] = Gap{fid, from_us, until_us, true};
  }

  // Direct senders: when a send at now_us would reach the air inside one of
  // the recent gaps (lead_us = that path's send-to-air latency), the time the
  // gap ends; else 0. Only gaps actually being kept (until > from) count.
  uint64_t quiet_until(uint64_t now_us, uint64_t lead_us) const {
    std::lock_guard<std::mutex> l(m_);
    uint64_t until = 0;
    for (int k = 0; k < kRecent; ++k) {
      const Gap& g = ring_[static_cast<size_t>((head_ - k + kRing) % kRing)];
      if (!g.valid || g.until <= g.from) continue;
      if (now_us < g.until && now_us + lead_us >= g.from) until = std::max(until, g.until);
    }
    return until;
  }

  // TX writer: a gated body waited held_us before going to the radio.
  void on_gate_hold(uint64_t held_us) {
    std::lock_guard<std::mutex> l(m_);
    ++gate_holds_;
    gate_hold_sum_us_ += held_us;
    gate_hold_max_us_ = std::max(gate_hold_max_us_, held_us);
  }
  void on_direct_hold() {
    std::lock_guard<std::mutex> l(m_);
    ++direct_holds_;
  }

  // Worth a report this period: statuses are (or were just) arriving, or a
  // counter moved.
  bool active(uint64_t now_us) const {
    std::lock_guard<std::mutex> l(m_);
    return live(now_us) || status_rx_ || gate_holds_ || direct_holds_;
  }

  // Agent thread, once per period: the report; the period counters reset.
  rc::LwStat take(uint32_t vtx_id, uint16_t seq, uint64_t now_us) {
    std::lock_guard<std::mutex> l(m_);
    rc::LwStat s;
    s.vtx_id = vtx_id;
    s.seq = seq;
    s.listen_ms = static_cast<uint8_t>(live(now_us) ? req_gap_us_ / 1000u : 0u);
    s.status_rx = sat16(status_rx_);
    for (int i = 0; i < rc::kLwHistBins; ++i) s.hist[i] = sat8(hist_[i]);
    s.nofid = sat8(nofid_);
    s.gate_holds = sat16(gate_holds_);
    s.gate_hold_sum_ms = sat16(gate_hold_sum_us_ / 1000u);
    s.gate_hold_max_ms = sat8(gate_hold_max_us_ / 1000u);
    s.direct_holds = sat16(direct_holds_);
    status_rx_ = nofid_ = gate_holds_ = direct_holds_ = 0;
    gate_hold_sum_us_ = gate_hold_max_us_ = 0;
    for (auto& h : hist_) h = 0;
    return s;
  }

  // Histogram bin of an arrival offset (µs from the gap's start):
  // <0 | 0-1 | 1-2 | 2-3 | 3-4 | 4-5 | 5-7 | >=7 ms.
  static int bin(int64_t off_us) {
    if (off_us < 0) return 0;
    const int64_t ms = off_us / 1000;
    if (ms < 5) return 1 + static_cast<int>(ms);
    return ms < 7 ? 6 : 7;
  }

 private:
  struct Gap {
    uint16_t fid = 0;
    uint64_t from = 0, until = 0;
    bool valid = false;
  };
  // Gaps a direct send can fall into: the newest few (the hot thread may
  // already have booked the next AU's gap while this one is still running).
  static constexpr int kRecent = 4;

  bool live(uint64_t now_us) const {
    return have_status_ && now_us >= last_status_us_ &&
           now_us - last_status_us_ <= kStatusTimeoutUs;
  }
  const Gap* find(uint16_t fid) const {
    for (int k = 0; k < kRing; ++k) {
      const Gap& g = ring_[static_cast<size_t>((head_ - k + kRing) % kRing)];
      if (g.valid && g.fid == fid) return &g;
    }
    return nullptr;
  }
  static uint16_t sat16(uint64_t v) { return v > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(v); }
  static uint8_t sat8(uint64_t v) { return v > 0xFF ? 0xFF : static_cast<uint8_t>(v); }

  mutable std::mutex m_;
  std::array<Gap, kRing> ring_{};
  int head_ = 0;
  uint32_t req_gap_us_ = 0;
  uint64_t last_status_us_ = 0;
  bool have_status_ = false;
  uint64_t status_rx_ = 0, nofid_ = 0;
  uint64_t hist_[rc::kLwHistBins] = {};
  uint64_t gate_holds_ = 0, gate_hold_sum_us_ = 0, gate_hold_max_us_ = 0;
  uint64_t direct_holds_ = 0;
};

}  // namespace mabur
