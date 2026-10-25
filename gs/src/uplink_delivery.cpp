#include "uplink_delivery.h"

#include <algorithm>
#include <cmath>

namespace maburgs {

const char* to_string(UplinkState s) {
  switch (s) {
    case UplinkState::kNone: return "none";
    case UplinkState::kWait: return "wait";
    case UplinkState::kOk: return "ok";
    case UplinkState::kLow: return "low";
    case UplinkState::kDead: return "dead";
    case UplinkState::kIdle: return "idle";
    case UplinkState::kStale: return "stale";
  }
  return "none";
}

void UplinkDelivery::on_rcf_sent(uint64_t now_ms) {
  // A gap longer than kIdleMs starts a new sending run: the drone has had
  // no chance to hear us until the run is kDeadGraceMs old.
  if (!have_sent_ || now_ms - last_sent_ms_ > kIdleMs) run_start_ms_ = now_ms;
  have_sent_ = true;
  last_sent_ms_ = now_ms;
  ++sent_;
}

void UplinkDelivery::on_telem(uint16_t tlm_seq, uint32_t rcf_rx, uint8_t drone_state,
                              uint64_t now_ms) {
  // The same Telem heard twice (two cards) is one report.
  if (have_telem_ && tlm_seq == last_seq_) return;
  // A counter going backwards is a maburd restart: the old samples are
  // another process's counts.
  if (have_telem_ &&
      (rcf_rx < last_rx_ || static_cast<uint16_t>(tlm_seq - last_seq_) > 32767)) {
    n_ = 0;
    pct_.reset();
  }
  have_telem_ = true;
  last_seq_ = tlm_seq;
  last_rx_ = rcf_rx;
  drone_state_ = drone_state;
  last_telem_ms_ = now_ms;

  if (n_ == kCap) {  // full: drop the oldest
    head_ = (head_ + 1) % kCap;
    --n_;
  }
  ring_[(head_ + n_) % kCap] = Sample{now_ms, rcf_rx, sent_};
  ++n_;
  while (n_ > 1 && ring_[head_].t_ms + kWindowMs + 500 < now_ms) {
    head_ = (head_ + 1) % kCap;
    --n_;
  }

  // A Telem gap longer than the window evicted every base: the figure from
  // before the gap describes another stretch of flight and must not keep
  // judging the state (a held 0 would read red over a recovered link).
  if (n_ == 1) {
    pct_.reset();
    return;
  }
  const Sample& base = ring_[head_];
  const Sample& cur = ring_[(head_ + n_ - 1) % kCap];
  if (cur.t_ms - base.t_ms < kMinSpanMs) return;
  // Too few RCFs across the window to judge -- typically the first Telem
  // after a long GS pause, whose handful of RCFs the drone's 1 Hz counter
  // has not caught up with yet. No figure rather than a false amber/red.
  const uint64_t d_sent = cur.sent - base.sent;
  if (d_sent < kMinRcfsInWindow) {
    pct_.reset();
    return;
  }
  const double ratio = static_cast<double>(cur.rcf_rx - base.rcf_rx) / static_cast<double>(d_sent);
  pct_ = std::clamp(static_cast<int>(std::lround(100.0 * ratio)), 0, 100);
}

UplinkView UplinkDelivery::view(uint64_t now_ms) const {
  UplinkView v;
  v.pct = pct_;
  if (!have_telem_) {
    v.state = UplinkState::kNone;
  } else if (now_ms > last_telem_ms_ && now_ms - last_telem_ms_ > kStaleMs) {
    v.state = UplinkState::kStale;
  } else if (!have_sent_ || (now_ms > last_sent_ms_ && now_ms - last_sent_ms_ > kIdleMs)) {
    v.state = UplinkState::kIdle;
  } else if (((drone_state_ == kStateRendezvous || drone_state_ == kStateFailsafe) &&
              last_telem_ms_ >= run_start_ms_ + kDeadGraceMs) ||
             (pct_ && *pct_ == 0)) {
    v.state = UplinkState::kDead;
  } else if (!pct_) {
    v.state = UplinkState::kWait;
  } else if (*pct_ < kLowBelowPct) {
    v.state = UplinkState::kLow;
  } else {
    v.state = UplinkState::kOk;
  }
  return v;
}

}  // namespace maburgs
