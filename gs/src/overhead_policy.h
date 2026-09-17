#pragma once
#include <algorithm>
#include <cmath>
#include <limits>

namespace maburgs {

// Tier 1 of docs/link-adaptation-v2-proposal.md: FEC overhead as a
// controlled variable instead of a per-rung constant.
//
// WHY. The shipped ladder carries a FLAT overhead pair (1.0 base / 0.5 enh)
// on every rung, so a demote cuts the video rate 1.33-2.0x and buys exactly
// zero extra loss tolerance -- the ladder can only trade rate away, never
// trade it for protection. Worse, the pair is over-provisioned at the loss
// level where the controller gives up: down_util 0.35 against a base budget
// of 0.5 demotes at L = 17.5% while ov_base 1.0 carries repair for 50%, a
// 2.86x margin rather than the 2x the design is tuned around. Holding the
// rung at the overhead a true 2x margin actually costs delivers ~1.76x the
// video the demote gives (proposal §2.1).
//
// WHY IT IS CHEAP. mabur's FEC is a systematic sliding-window RLC, not a
// block code: SwEncoder::set_overhead() "takes effect immediately (no block
// boundary to wait for)", the decoder config-matches only symbol_size, and
// each repair carries its own window in its header. So overhead can change
// mid-stream with no coordination and no wire change -- the RCF already
// carries the pair.
//
// THE TWO TRAPS, both of which this class exists to avoid.
//
// 1. Drive from RAW pre-FEC loss, never from the controller's `u`.
//    u = pre_fec_loss / budget and budget = ov/(1+ov), so if ov becomes the
//    actuator it sits in the denominator of its own error signal: raising it
//    lowers u with no change in the link, and the loop converges on nothing.
//    feed() therefore takes a loss fraction, not a utilisation.
//
// 2. Every commanded overhead change moves the drone's derived bitrate, and
//    on Star6E every MI_VENC_SetChnAttr that changes the rate emits an IDR
//    that bypasses idr_rate_limit (measured 2026-09-01: 15 real changes ->
//    15 IDRs, 16 same-value writes -> 0), at a median 63.8 kB against a
//    21.4 kB base P frame -- ~38 ms of airtime at mcs4. The continuous
//    FEC->bitrate loop that did this was DELETED on 2026-09-01 for producing
//    ~150 keyframes per 12-minute flight. So the output is quantised to
//    `step`, gated by a dead band, and rate-limited by min_interval_ms.
//    Those three are not tuning niceties; without them this is the deleted
//    loop again.
struct OverheadCfg {
  // OFF by default. With enable=false the policy still computes and exports
  // its target (so a flight can be compared against what it WOULD have
  // done) but nothing is commanded -- the same observe-only staging the
  // air-clock gate shipped with at shed_ms 0, which worked.
  bool enable = false;
  // Budget headroom over the measured loss. 2.0 = "carry repair for twice
  // the loss we can see", which is the margin the rest of the design is
  // tuned around (probe max_util, down_util).
  double margin = 2.0;
  // Quantisation of the commanded value. Coarse on purpose: see trap 2.
  double step = 0.1;
  // Floor and ceiling. The ceiling is the config range of
  // link.ladder[].overhead_* (gs/src/config.cpp validates [0.1, 2.0]), so
  // asking for more than 2.0 is not expressible.
  //
  // The FLOOR is deliberately well above that range's 0.1. A momentarily
  // clean link drives the target toward zero, and arriving at a 9% budget
  // just before a burst is how you lose a GOP: the target above tracks the
  // MEAN loss and models nothing about its burst structure.
  //
  // 0.5, and it is a MEASUREMENT now, not the judgement call 0.3 was.
  // fec.log's per-episode gauge (2026-09-16 bench, GS deployed, ausniff
  // 60.0 fps / fid_gaps 0 twice) reports rung-5 base episodes needing
  // ov_req up to **0.46**, and rung-5 enh at flown ov 0.5 needing 0.38 --
  // see ov_req() below for what that quantity is. 0.5 is the first point on
  // the `step` 0.1 grid at or above that measured worst episode; 0.3 and 0.4
  // both sit under it and would have under-protected it.
  //
  // What this costs: the floor binds whenever the mean-loss target is below
  // it, which on a sparse link is most of the time, and kbps ~ 1/(1+ov), so
  // 0.3 -> 0.5 gives up ~13% of video rate there. That is the price of not
  // losing the worst episode in the recording, and it is the conservative
  // direction for a FLOOR: too high costs bitrate, too low costs AUs.
  //
  // TWO CAVEATS, both worth keeping in view:
  //   - Only rung 5 is measured. The same burst in TIME destroys fewer
  //     symbols at a lower MCS (longer symbols, lower bitrate), which argues
  //     rung 5 is the ladder's worst case and 0.5 covers the rest -- but
  //     that is reasoning, not data, and it cuts the other way if low rungs
  //     are flown precisely when bursts are longer. Per-rung floors are the
  //     proper fix once fec.log covers more rungs.
  //   - One floor serves BOTH layers, so enh (measured 0.38) is held at the
  //     0.5 it already flies. That costs enh nothing today and refines
  //     §2.1 of docs/link-adaptation-v2-proposal.md: base is ~2x
  //     over-provisioned (1.0 vs 0.46), enh only ~1.3x (0.5 vs 0.38), so
  //     the bitrate win tier 1 is chasing lives almost entirely on base.
  double min_ov = 0.5;
  double max_ov = 2.0;
  // Minimum wall time between commanded changes. Each one costs an IDR
  // (trap 2), so this is an airtime budget: at mcs4 an IDR is ~38 ms, so
  // 2000 ms holds the cost near 2% of airtime.
  double min_interval_ms = 2000.0;
  // Hysteresis: the target must differ from what is currently commanded by
  // more than this before a change is worth an IDR.
  double dead_band = 0.15;
};

// One layer's overhead controller. Two instances (base, enh) rather than one
// with a pair, because the two layers see different loss -- that asymmetry is
// the entire point of UEP -- and because the operator rule
// (uep-base-protection-constraint) that base >= enh is applied by the CALLER
// across the two results, not smuggled in here.
class OverheadPolicy {
 public:
  explicit OverheadPolicy(OverheadCfg cfg) : cfg_(cfg) {}

  // The unclamped, unquantised overhead a `margin`x budget on `loss` costs.
  //
  //   budget = ov/(1+ov) = margin*loss   ->   ov = mL/(1 - mL)
  //
  // Diverges as margin*loss approaches 1 (a budget of 100% needs infinite
  // repair), so the caller's clamp to max_ov is load-bearing, not cosmetic.
  static double ov_for_loss(double loss, double margin) {
    const double b = std::clamp(margin * std::max(0.0, loss), 0.0, 0.95);
    return b / (1.0 - b);
  }

  // The overhead ONE OBSERVED LOSS EPISODE would have needed, from fec.log's
  // (m, r) at the overhead `ov` it flew at. Byte-for-byte the same function
  // as tools/flightreport.py's fec_ov_req(), which is the reference and
  // whose FEC EPISODES section is where the numbers behind min_ov come from;
  // the two must not drift.
  //
  // At overhead x the same lost air carries m*(1+ov)/(1+x) sources (an
  // aggregate is a fixed number of envelopes, fewer of them repairs) against
  // r*x/ov covering repairs, and the decoder needs repairs >= sources:
  //
  //     x*(1+x) = m*ov*(1+ov)/r = c   ->   x = (sqrt(1+4c)-1)/2
  //
  // inf when no repair covered the episode at all, which is a real outcome
  // and not an error: no overhead would have saved it.
  //
  // This is here rather than only in Python because it is the quantity
  // min_ov is derived from, so the derivation should be checkable in the
  // same place as the constant -- and because driving feed() from a high
  // percentile of it, instead of from the mean loss, is the better shape
  // this policy should eventually take (proposal §8a). Nothing calls it on
  // the hot path yet.
  static double ov_req(double m, double r, double ov) {
    if (r <= 0.0) return std::numeric_limits<double>::infinity();
    const double c = m * ov * (1.0 + ov) / r;
    return (std::sqrt(1.0 + 4.0 * c) - 1.0) / 2.0;
  }

  // Feeds one window's RAW pre-FEC loss for this layer (trap 1: a loss
  // fraction, never a utilisation). `commanded` is what is on the wire now.
  // Returns the value to command, which equals `commanded` whenever the
  // dead band, the rate limit or `enable` says to leave it alone.
  double feed(double loss, double commanded, double now_ms) {
    target_ = std::clamp(ov_for_loss(loss, cfg_.margin), cfg_.min_ov,
                         cfg_.max_ov);
    // Quantise toward MORE protection: a target between two steps rounds up,
    // so quantisation error can never leave the layer under-protected
    // relative to what the margin asked for.
    const double q = std::clamp(std::ceil(target_ / cfg_.step) * cfg_.step,
                                cfg_.min_ov, cfg_.max_ov);
    quantised_ = q;
    if (!cfg_.enable) return commanded;
    if (std::abs(q - commanded) <= cfg_.dead_band) return commanded;
    if (have_changed_ && now_ms - last_change_ms_ < cfg_.min_interval_ms) {
      // Rate-limited. One exception: a target ABOVE what is commanded is the
      // link asking for protection it does not have, and holding that back
      // to save an IDR is the wrong trade -- an under-protected layer sheds
      // whole AUs, which costs far more than a keyframe.
      if (q <= commanded) return commanded;
    }
    last_change_ms_ = now_ms;
    have_changed_ = true;
    ++changes_;
    return q;
  }

  // Last computed target, before quantisation and before the gates. Exported
  // so an enable=false flight records what this WOULD have commanded.
  double target() const { return target_; }
  double quantised() const { return quantised_; }
  uint64_t changes() const { return changes_; }

 private:
  OverheadCfg cfg_;
  double target_ = 0.0, quantised_ = 0.0;
  double last_change_ms_ = 0.0;
  bool have_changed_ = false;
  uint64_t changes_ = 0;
};

}  // namespace maburgs
