#pragma once
// 10 MHz bottom rung (docs/next-build-backlog.md item 10): below MCS0/20 the
// whole link -- drone and GS cards -- re-clocks to 10 MHz narrowband, ~3 dB
// more link budget for half the airtime capacity (MCS0/10 = 3.25 Mb/s
// nominal). It is a radio mode on both ends, not a per-frame width: a 10 MHz
// receiver hears only 10 MHz frames and a 20 MHz one none of them, so every
// switch is a coordinated move with a lead card that proves the drone made
// it before the rest of the GS follows, and a rescue on each end for a move
// that went wrong.
//
//   Off      -> Entering  rung 0 in distress for enter_ms: the RCF op turns
//                         10 MHz; the lead card (a non-TX card when there is
//                         one) switches to 10, the TX card stays put so the
//                         order keeps reaching the drone at 20.
//   Entering -> On        the lead card hears the drone at 10: every card
//                         follows.
//   Entering -> Off       nothing heard within confirm_ms: cancel, cooldown.
//   On       -> Leaving   clean for clean_ms, and the best card's SNR (read at
//                         10) at least 3 dB (the narrowband gain) +
//                         promote_margin_db above where 20 MHz failed: the RCF
//                         op returns to rung 0 (20 MHz) -- a blind promote, as
//                         nothing can probe 20 MHz from a 10 MHz radio; the
//                         lead card switches back first.
//   Leaving  -> Off       the lead card hears the drone at 20: the rest
//                         follow; cooldown.
//   Leaving  -> On        nothing heard within confirm_ms: stay at 10.
//   On       -> Off       rescue: no drone frame on any card for rescue_ms.
//
// Pure decisions for the test; ChannelCore feeds it and applies the result.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace maburgs {

struct NarrowCfg {
  bool enable = false;
  // FEC on the 10 MHz rung (same literal overheads as a ladder rung).
  double overhead_base = 1.0;
  double overhead_enh = 0.75;
  int enter_ms = 1500;              // rung-0 distress sustained this long
  double enter_post_loss_pct = 1.0; // post-FEC loss over the window that counts as distress
  int lead_ms = 300;                // lead card = TX card (one-card GS): RCFs at 20 first
  int confirm_ms = 2000;            // the lead card must hear the drone within this
  int rescue_ms = 1500;             // at 10: no drone frame on any card this long -> back to 20
  int clean_ms = 8000;              // at 10: clean this long before the blind promote
  double promote_margin_db = 3.0;   // over the 3 dB narrowband gain, vs the entry SNR
  int cooldown_ms = 20000;          // after leaving 10, no re-entry for this long
};

enum class NarrowState : uint8_t { Off, Entering, On, Leaving };

inline const char* to_string(NarrowState s) {
  switch (s) {
    case NarrowState::Off: return "off";
    case NarrowState::Entering: return "entering";
    case NarrowState::On: return "on";
    case NarrowState::Leaving: return "leaving";
  }
  return "?";
}

struct NarrowIn {
  double now_ms = 0;
  bool in_session = false;
  bool blocked = false;       // calibration or a hop in flight: start nothing
  int rung = 0;               // ladder rung (0 = MCS0/20, the bottom)
  bool distress = false;      // this window: post-FEC loss or s1 util over the bar
  bool clean = false;         // this window: no post-FEC loss, util under up_util
  double best_snr_db = std::numeric_limits<double>::quiet_NaN();
  int lead_candidate = -1;    // the card that would move first (-1 = none can switch)
  int tx_card = -1;
  // Per card: tuned width now, a width switch in flight, drone frames heard
  // (monotonic).
  std::vector<uint8_t> width;
  std::vector<bool> switching;
  std::vector<uint64_t> own;
};

struct NarrowOut {
  NarrowState state = NarrowState::Off;
  bool op_narrow = false;     // the RCF op is the 10 MHz rung
  int lead_card = -1;
  uint8_t lead_w = 0;         // lead card's target width override (0 = its normal target)
  uint8_t rest_w = 0;         // every other card's override (0 = normal)
  const char* event = nullptr;  // set on a transition, for the log line
};

class NarrowMode {
 public:
  explicit NarrowMode(NarrowCfg cfg) : cfg_(cfg) {}

  NarrowOut tick(const NarrowIn& in) {
    event_ = nullptr;
    switch (state_) {
      case NarrowState::Off: tick_off_(in); break;
      case NarrowState::Entering: tick_entering_(in); break;
      case NarrowState::On: tick_on_(in); break;
      case NarrowState::Leaving: tick_leaving_(in); break;
    }
    return out_(in);
  }

  NarrowState state() const { return state_; }
  int lead_card() const { return state_ == NarrowState::Off ? -1 : lead_; }
  bool active() const { return state_ != NarrowState::Off; }
  double entry_snr_db() const { return entry_snr_; }
  uint64_t entries() const { return entries_; }
  uint64_t cancels() const { return cancels_; }
  uint64_t rescues() const { return rescues_; }
  uint64_t promotes() const { return promotes_; }

 private:
  void go_(NarrowState s, double now_ms, const char* why) {
    state_ = s;
    since_ms_ = now_ms;
    settled_ = false;
    event_ = why;
  }

  void tick_off_(const NarrowIn& in) {
    const bool may = cfg_.enable && in.in_session && !in.blocked && in.rung == 0 &&
                     in.lead_candidate >= 0 && in.now_ms >= cooldown_until_ms_;
    if (!may || !in.distress) {
      distress_since_ms_ = -1;
      return;
    }
    if (distress_since_ms_ < 0) distress_since_ms_ = in.now_ms;
    if (in.now_ms - distress_since_ms_ < cfg_.enter_ms) return;
    distress_since_ms_ = -1;
    lead_ = in.lead_candidate;
    lead_is_tx_ = in.lead_candidate == in.tx_card;
    entry_snr_ = in.best_snr_db;
    ++entries_;
    go_(NarrowState::Entering, in.now_ms, "enter: MCS0/20 in distress, ordering 10 MHz");
  }

  static uint64_t sum_(const std::vector<uint64_t>& v) {
    uint64_t t = 0;
    for (uint64_t x : v) t += x;
    return t;
  }

  // The lead card counts as moved once its tuned width is the one this
  // phase wants (10 entering, anything else leaving) with no switch in
  // flight; its own-frame count from that moment is the baseline the
  // confirm needs to see grow -- so frames it heard before it moved never
  // count.
  bool lead_heard_(const NarrowIn& in, bool want_10) {
    const size_t k = static_cast<size_t>(lead_);
    if (lead_ < 0 || k >= in.width.size() || k >= in.own.size()) return false;
    const bool at = want_10 ? in.width[k] == 10 : (in.width[k] != 10 && in.width[k] != 0);
    const bool busy = k < in.switching.size() && in.switching[k];
    if (!at || busy) {
      settled_ = false;
      return false;
    }
    if (!settled_) {
      settled_ = true;
      own_base_ = in.own[k];
      return false;
    }
    return in.own[k] > own_base_;
  }

  void tick_entering_(const NarrowIn& in) {
    if (!in.in_session) {
      fail_(in.now_ms, NarrowState::Off, "cancel: session lost while entering");
      return;
    }
    if (lead_switch_due_(in) && lead_heard_(in, true)) {
      go_(NarrowState::On, in.now_ms, "on: lead card heard the drone at 10 MHz");
      last_heard_ms_ = in.now_ms;
      heard_own_ = sum_(in.own);
      clean_since_ms_ = -1;
      return;
    }
    if (in.now_ms - since_ms_ > cfg_.confirm_ms + lead_delay_())
      fail_(in.now_ms, NarrowState::Off, "cancel: drone not heard at 10 MHz");
  }

  void tick_on_(const NarrowIn& in) {
    const uint64_t any = sum_(in.own);
    if (any != heard_own_) {
      heard_own_ = any;
      last_heard_ms_ = in.now_ms;
    }
    if (in.now_ms - last_heard_ms_ > cfg_.rescue_ms) {
      ++rescues_;
      fail_(in.now_ms, NarrowState::Off, "rescue: no drone frame at 10 MHz, back to 20");
      return;
    }
    if (!in.clean) {
      clean_since_ms_ = -1;
      return;
    }
    if (clean_since_ms_ < 0) clean_since_ms_ = in.now_ms;
    if (in.now_ms - clean_since_ms_ < cfg_.clean_ms || in.blocked) return;
    const bool snr_known = !std::isnan(in.best_snr_db) && !std::isnan(entry_snr_);
    if (snr_known && in.best_snr_db < entry_snr_ + 3.0 + cfg_.promote_margin_db) return;
    if (in.lead_candidate >= 0) lead_ = in.lead_candidate;
    lead_is_tx_ = lead_ == in.tx_card;
    go_(NarrowState::Leaving, in.now_ms, "leave: clean at 10 MHz, ordering MCS0/20 (blind)");
  }

  void tick_leaving_(const NarrowIn& in) {
    if (lead_switch_due_(in) && lead_heard_(in, false)) {
      ++promotes_;
      cooldown_until_ms_ = in.now_ms + cfg_.cooldown_ms;
      go_(NarrowState::Off, in.now_ms, "off: lead card heard the drone at 20 MHz");
      return;
    }
    if (in.now_ms - since_ms_ > cfg_.confirm_ms + lead_delay_()) {
      go_(NarrowState::On, in.now_ms, "stay: drone not heard at 20 MHz, back to 10");
      last_heard_ms_ = in.now_ms;
      heard_own_ = sum_(in.own);
      clean_since_ms_ = -1;
    }
  }

  void fail_(double now_ms, NarrowState to, const char* why) {
    if (state_ == NarrowState::Entering) ++cancels_;
    cooldown_until_ms_ = now_ms + cfg_.cooldown_ms;
    go_(to, now_ms, why);
  }

  // A lead card that is also the TX card waits lead_ms before moving, so a
  // few RCFs carrying the order leave at the old width first.
  double lead_delay_() const { return lead_is_tx_ ? cfg_.lead_ms : 0; }
  bool lead_switch_due_(const NarrowIn& in) const {
    return in.now_ms - since_ms_ >= lead_delay_();
  }

  NarrowOut out_(const NarrowIn& in) const {
    NarrowOut o;
    o.state = state_;
    o.event = event_;
    o.lead_card = lead_;
    const bool due = lead_switch_due_(in);
    switch (state_) {
      case NarrowState::Off: o.lead_card = -1; break;
      case NarrowState::Entering:
        o.op_narrow = true;
        o.lead_w = due ? 10 : 0;
        break;
      case NarrowState::On:
        o.op_narrow = true;
        o.lead_w = 10;
        o.rest_w = 10;
        break;
      case NarrowState::Leaving:
        o.op_narrow = false;
        o.lead_w = 0;
        o.rest_w = 10;
        if (!due) o.lead_w = 10;
        break;
    }
    return o;
  }

  NarrowCfg cfg_;
  NarrowState state_ = NarrowState::Off;
  double since_ms_ = 0;
  double distress_since_ms_ = -1;
  double clean_since_ms_ = -1;
  double cooldown_until_ms_ = -1e18;
  double last_heard_ms_ = 0;
  uint64_t heard_own_ = 0;
  bool settled_ = false;
  uint64_t own_base_ = 0;
  int lead_ = -1;
  bool lead_is_tx_ = false;
  double entry_snr_ = std::numeric_limits<double>::quiet_NaN();
  const char* event_ = nullptr;
  uint64_t entries_ = 0, cancels_ = 0, rescues_ = 0, promotes_ = 0;
};

}  // namespace maburgs
