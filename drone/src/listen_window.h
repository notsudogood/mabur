#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>

#include "mabur/rc_proto.h"

namespace mabur {

// Listen window, drone side (feedback-repair rollout phase 3, revised as 3b;
// docs/feedback-repair-rollout.md).
//
// The GS sends a T_STATUS at every burst end it sees. While those keep
// arriving, the drone keeps the air quiet for a window after each burst so
// the next status has somewhere to land: any body whose modelled air would
// overlap the window (the next AU's bodies, a late repair of the last one,
// a probe) is held until the window ends, and so is a control/MSP/pong send
// that would reach the air inside it. Nothing here touches the radio: the
// hot thread places each AU's window and asks which bodies to hold, the TX
// writer and the direct senders wait, the RX thread times each status
// against its AU's burst, and the agent thread takes a T_LWSTAT per period.
//
// Where the window goes (3b): the first flight put it at the burst's end and
// found the statuses landing 4-7+ ms later -- the GS needs that long to see
// the burst end, send, and reach the drone's RX. So the window now starts
// `delay` after the burst's modelled end, a delay learned from the arrivals
// themselves: the start that would have caught the most of the last kLearnN
// statuses with the window's width, less a small margin (kPriorDelayUs until
// kLearnMin have arrived). The window is cut short so it ends before the
// next AU is due (AuCadence) and skipped when that leaves less than
// kMinWindowUs, so a burst plus window that does not fit in a frame period
// never pushes the next AU back and never builds a backlog.
//
// The burst's end is MODELLED (AirClock::free_at_us after the AU and its
// probe are booked) -- the drone cannot see the chip finish; model error
// lands in the arrival times and is learned along with the GS's reaction,
// which is what the window has to match. The arrival stamp is the RX
// callback, kRxLatencyUs after the status was on air, and the window must
// be quiet while it is ON AIR: the learning and the inside count take the
// stamp less kRxLatencyUs, the histogram keeps the raw stamp (comparable
// with the first flight's).
//
// Thread-safe (one mutex, a handful of calls per AU). Time is the caller's
// steady-clock µs, so tests drive it synthetically.
class ListenWindow {
 public:
  // The window switches off this long after the last status: a GS that
  // stopped asking (feature off, A/B off phase, uplink gone) costs no video.
  static constexpr uint64_t kStatusTimeoutUs = 500000;
  // Drone-side cap on the window's width whatever the GS asks for.
  static constexpr uint32_t kMaxGapUs = 10000;
  // Learned delay: cap, the starting value (the first flight's floor), how
  // many arrivals it learns from, the fewest it trusts, and the margin it
  // starts ahead of the best-placed window.
  static constexpr uint32_t kMaxDelayUs = 15000;
  static constexpr uint32_t kPriorDelayUs = 4000;
  static constexpr int kLearnN = 128;
  static constexpr int kLearnMin = 16;
  static constexpr uint32_t kLearnMarginUs = 250;
  // On air -> the drone's RX callback, estimated: the phase-2 bench put a
  // ping on air -> reply on air at 1.9 ms p50 on the Mgmt lane, of which the
  // drone's own handling was 0.12 ms and the send path ~1 ms.
  static constexpr uint64_t kRxLatencyUs = 800;
  // Fit: the window ends this far before the next AU is due, and a window
  // cut shorter than kMinWindowUs is skipped.
  static constexpr uint64_t kFitMarginUs = 500;
  static constexpr uint64_t kMinWindowUs = 1000;
  // The longest the TX writer ever holds a body (delay + width).
  static constexpr uint64_t kMaxHoldUs = kMaxDelayUs + kMaxGapUs;
  static constexpr int kRing = 32;

  struct Window {
    uint64_t from = 0, until = 0;  // until == from: nothing kept
    bool skipped = false;          // cut to nothing by the fit rule
  };

  // Where the window after a burst modelled off air at end_us goes:
  // [end + delay, end + delay + width), ending no later than kFitMarginUs
  // before next_due_us (0 = not known yet: no cut). Cut below kMinWindowUs
  // it is skipped (from == until == end).
  static Window place(uint64_t end_us, uint32_t delay_us, uint32_t width_us,
                      uint64_t next_due_us) {
    Window w;
    w.from = end_us + delay_us;
    w.until = w.from + width_us;
    if (next_due_us && w.until + kFitMarginUs > next_due_us)
      w.until = next_due_us > kFitMarginUs ? next_due_us - kFitMarginUs : 0;
    if (w.until < w.from + kMinWindowUs) {
      w.from = w.until = end_us;
      w.skipped = true;
    }
    return w;
  }

  // A body whose modelled air [start, start + cost) overlaps the window
  // [from, until) waits until the window ends: returns `until`, else 0.
  static uint64_t hold_until(uint64_t start_us, uint64_t cost_us, uint64_t from_us,
                             uint64_t until_us) {
    if (until_us <= from_us) return 0;
    if (start_us >= until_us || start_us + cost_us <= from_us) return 0;
    return until_us;
  }

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
    const int64_t off = static_cast<int64_t>(rx_us) - static_cast<int64_t>(g->end);
    ++hist_[bin(off)];
    const uint64_t air_us = rx_us > kRxLatencyUs ? rx_us - kRxLatencyUs : 0;
    if (g->until > g->from && air_us >= g->from && air_us < g->until) ++inside_;
    const int64_t air_off = off - static_cast<int64_t>(kRxLatencyUs);
    learn_[static_cast<size_t>(learn_head_)] = static_cast<int32_t>(
        std::max<int64_t>(-1000000, std::min<int64_t>(air_off, 1000000)));
    learn_head_ = (learn_head_ + 1) % kLearnN;
    if (learn_n_ < kLearnN) ++learn_n_;
    relearn();
  }

  // The window's width to keep after the next burst, µs: the last status's
  // listen_ms while statuses keep arriving, else 0.
  uint32_t gap_us(uint64_t now_us) const {
    std::lock_guard<std::mutex> l(m_);
    return live(now_us) ? req_gap_us_ : 0;
  }

  // The learned delay from the burst's modelled end to the window, µs.
  uint32_t delay_us() const {
    std::lock_guard<std::mutex> l(m_);
    return delay_us_;
  }

  // Hot thread: AU `fid`'s burst (its probe included) is modelled off air at
  // end_us, and the window it keeps is [from_us, until_us) (until == from
  // when none).
  void on_au_gap(uint16_t fid, uint64_t end_us, uint64_t from_us, uint64_t until_us) {
    std::lock_guard<std::mutex> l(m_);
    head_ = (head_ + 1) % kRing;
    ring_[static_cast<size_t>(head_)] = Gap{fid, end_us, from_us, until_us, true};
  }

  // Hot thread: a window the fit rule cut to nothing.
  void on_fit_skip() {
    std::lock_guard<std::mutex> l(m_);
    ++fit_skips_;
  }

  // Direct senders: when a send at now_us would reach the air inside one of
  // the recent windows (lead_us = that path's send-to-air latency), the time
  // the window ends; else 0. Only windows actually kept (until > from) count.
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

  // TX writer: a held body waited held_us before going to the radio.
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
    return live(now_us) || status_rx_ || gate_holds_ || direct_holds_ || fit_skips_;
  }

  // Agent thread, once per period: the report (v2 layout, rc_proto.h); the
  // period counters reset, the learned delay does not.
  rc::LwStat take(uint32_t vtx_id, uint16_t seq, uint64_t now_us) {
    std::lock_guard<std::mutex> l(m_);
    rc::LwStat s;
    s.vtx_id = vtx_id;
    s.seq = seq;
    s.version = 2;
    s.listen_ms = static_cast<uint8_t>(live(now_us) ? req_gap_us_ / 1000u : 0u);
    s.status_rx = sat16(status_rx_);
    for (int i = 0; i < rc::kLwHistBins; ++i) s.hist[i] = sat8(hist_[i]);
    s.nofid = sat8(nofid_);
    s.inside = sat8(inside_);
    s.delay_100us = sat8(delay_us_ / 100u);
    s.fit_skips = sat8(fit_skips_);
    s.gate_holds = sat16(gate_holds_);
    s.gate_hold_sum_ms = sat16(gate_hold_sum_us_ / 1000u);
    s.gate_hold_max_ms = sat8(gate_hold_max_us_ / 1000u);
    s.direct_holds = sat16(direct_holds_);
    status_rx_ = nofid_ = inside_ = fit_skips_ = gate_holds_ = direct_holds_ = 0;
    gate_hold_sum_us_ = gate_hold_max_us_ = 0;
    for (auto& h : hist_) h = 0;
    return s;
  }

  // Histogram bin of an arrival offset (µs after the burst's modelled end):
  // <0 | 0-2 | 2-4 | 4-6 | 6-8 | 8-10 | 10-15 | >=15 ms.
  static int bin(int64_t off_us) {
    if (off_us < 0) return 0;
    const int64_t ms = off_us / 1000;
    if (ms < 10) return 1 + static_cast<int>(ms / 2);
    return ms < 15 ? 6 : 7;
  }

 private:
  struct Gap {
    uint16_t fid = 0;
    uint64_t end = 0, from = 0, until = 0;
    bool valid = false;
  };
  // Windows a direct send can fall into: the newest few (the hot thread may
  // already have booked the next AU's window while this one is still open).
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
  // Under m_: the start of the req_gap_us_-wide window that holds the most
  // of the remembered arrivals, less the margin.
  void relearn() {
    if (learn_n_ < kLearnMin || req_gap_us_ == 0) return;
    std::array<int32_t, kLearnN> v{};
    std::copy(learn_.begin(), learn_.begin() + learn_n_, v.begin());
    std::sort(v.begin(), v.begin() + learn_n_);
    int best = 0, best_n = 0;
    for (int i = 0, j = 0; i < learn_n_; ++i) {
      while (j < learn_n_ && static_cast<int64_t>(v[static_cast<size_t>(j)]) -
                                     v[static_cast<size_t>(i)] < req_gap_us_)
        ++j;
      if (j - i > best_n) {
        best_n = j - i;
        best = i;
      }
    }
    const int64_t d = static_cast<int64_t>(v[static_cast<size_t>(best)]) - kLearnMarginUs;
    delay_us_ = static_cast<uint32_t>(std::max<int64_t>(0, std::min<int64_t>(d, kMaxDelayUs)));
  }
  static uint16_t sat16(uint64_t v) { return v > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(v); }
  static uint8_t sat8(uint64_t v) { return v > 0xFF ? 0xFF : static_cast<uint8_t>(v); }

  mutable std::mutex m_;
  std::array<Gap, kRing> ring_{};
  int head_ = 0;
  uint32_t req_gap_us_ = 0;
  uint32_t delay_us_ = kPriorDelayUs;
  std::array<int32_t, kLearnN> learn_{};
  int learn_head_ = 0, learn_n_ = 0;
  uint64_t last_status_us_ = 0;
  bool have_status_ = false;
  uint64_t status_rx_ = 0, nofid_ = 0, inside_ = 0, fit_skips_ = 0;
  uint64_t hist_[rc::kLwHistBins] = {};
  uint64_t gate_holds_ = 0, gate_hold_sum_us_ = 0, gate_hold_max_us_ = 0;
  uint64_t direct_holds_ = 0;
};

// The AU cadence the hot thread sees, so a window never reaches into the
// next AU (ListenWindow::place). Hot-thread only.
class AuCadence {
 public:
  // An AU's first body was pushed at t_us.
  void on_au_first(uint64_t t_us) {
    if (last_us_ && t_us > last_us_) {
      const uint64_t dt = t_us - last_us_;
      if (dt >= kMinUs && dt <= kMaxUs) {
        ewma_us_ = ewma_us_ ? (7 * ewma_us_ + dt) / 8 : dt;
        last_dt_us_ = dt;
      }
    }
    last_us_ = t_us;
  }
  // When the next AU's first body is due, 0 = not known yet. The shorter of
  // the smoothed and the last interval: a frame-rate step up (low-power exit)
  // must not let a window run into the next AU for the EWMA's settling time.
  uint64_t next_due_us() const {
    if (!last_us_ || !ewma_us_) return 0;
    return last_us_ + std::min(ewma_us_, last_dt_us_);
  }

 private:
  static constexpr uint64_t kMinUs = 2000, kMaxUs = 100000;  // stalls and dupes ignored
  uint64_t last_us_ = 0, ewma_us_ = 0, last_dt_us_ = 0;
};

}  // namespace mabur
