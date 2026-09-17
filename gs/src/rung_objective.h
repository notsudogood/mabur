#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace maburgs {

// Tier 2 of docs/link-adaptation-v2-proposal.md §2-3: choose the operating
// rung by maximising the video it can actually sustain, rather than by
// tripping a loss threshold.
//
// THE OBJECTIVE. Take the shipped bitrate formula with an equal overhead
// pair (kbps = 1000*B*rate/(1+ov)) and hold the FEC budget at `margin` times
// the measured pre-FEC loss L -- the same margin tier 1 sizes overhead to:
//
//     budget = ov/(1+ov) = m*L   ->   1 + ov = 1/(1 - m*L)
//     kbps   = 1000 * B * rate * (1 - m*L)
//
// So the whole decision is one quantity, `rate * (1 - m*L)`, and the rung to
// be on is whichever maximises it. A rung with m*L >= 1 cannot be flown at
// any overhead. This is exact against the shipped formula for an equal pair,
// and monotone in the right directions for the unequal pair that actually
// ships (1.0/0.5) -- see the proposal's §2 caveat.
//
// WHY THIS IS NOT THE SAME AS "DEMOTE WHEN LOSSY". On the flown ladder the
// overhead pair is FLAT across rungs, so a demote cuts the rate 1.33-2.0x
// and buys exactly zero extra loss tolerance. Under the objective a demote
// is therefore only worth taking when the lower rung's own loss is low
// enough to pay for the rate it costs, which is a much higher bar than
// "the current rung is above down_util".
struct ObjectiveCfg {
  // Fully inert when false: nothing armed, nothing scored, nothing acted on.
  bool enable = false;
  // Second stage. With enable=true and act=false the down probe is armed
  // and both rungs are scored and exported, but the verdict does not move
  // the ladder -- it buys the data to judge the objective against a real
  // flight before it is allowed to decide anything. The air-clock gate
  // shipped this way (shed_ms 0) and it worked.
  bool act = false;
  // Budget headroom over measured loss. Must match OverheadCfg::margin, or
  // the objective scores an overhead tier 1 is not commanding.
  double margin = 2.0;
  // Hysteresis on the verdict: the lower rung must beat the current one by
  // this fraction before a demote is worth its IDR and its re-key.
  double demote_margin = 0.10;
  // Disarm hysteresis, as a fraction of the arm threshold. Prevents the
  // probe flapping on and off around the boundary, which would cost airtime
  // in bursts and never accumulate a usable sample.
  double disarm_frac = 0.75;
};

class RungObjective {
 public:
  explicit RungObjective(ObjectiveCfg cfg) : cfg_(cfg) {}

  // kbps-proportional score of a rung: its PHY rate scaled by the airtime
  // left once FEC covers `margin`x its loss. Clamped at 0 -- a rung whose
  // required budget exceeds 100% is not flyable, not negatively flyable.
  static double score(double rate_mbps, double loss, double margin) {
    return rate_mbps * std::max(0.0, 1.0 - margin * std::max(0.0, loss));
  }

  // The loss at which demoting from rate_hi to rate_lo becomes ARGUABLE --
  // i.e. the point where the lower rung would win even in its best case, a
  // loss of zero:
  //
  //     rate_lo * 1 >= rate_hi * (1 - m*L)   ->   L >= (1 - rate_lo/rate_hi)/m
  //
  // This is what makes the arm threshold a function of the RATE RATIO rather
  // than a tuned constant. On the flown ladder that is 16.7% at the 1.33x
  // steps and 25% at the 2.0x mcs1->mcs0 step: below it, no measurement of
  // the lower rung could justify going there, so probing it is pure cost.
  static double arm_loss_threshold(double rate_hi, double rate_lo,
                                   double margin) {
    if (rate_hi <= 0.0 || margin <= 0.0) return 1.0;
    return std::max(0.0, (1.0 - rate_lo / rate_hi) / margin);
  }

  // Should the GS command a down probe this tick?
  //
  // `residual_clean` is the caller's post-FEC verdict: if FEC is ALREADY
  // failing at the current rung there is nothing to weigh up -- demote now
  // rather than spending 2-3 s and 3-10% of airtime measuring the rung below.
  bool want_probe(double rate_hi, double rate_lo, double loss_hi,
                  bool residual_clean) {
    if (!cfg_.enable || rate_lo <= 0.0) { armed_ = false; return false; }
    if (!residual_clean) { armed_ = false; return false; }
    const double thr = arm_loss_threshold(rate_hi, rate_lo, cfg_.margin);
    // Asymmetric thresholds: arm at `thr`, disarm only once loss falls to
    // disarm_frac of it. A probe that flaps never accumulates the seconds of
    // samples it needs to say anything.
    armed_ = armed_ ? loss_hi >= thr * cfg_.disarm_frac : loss_hi >= thr;
    return armed_;
  }

  // Verdict, given a measured loss for BOTH rungs. `have_lo` is false until
  // the down probe has produced a real sample -- absent a measurement the
  // answer is always "stay", never an assumed-clean lower rung.
  //
  // Returns true iff the lower rung beats the current one by demote_margin.
  bool should_demote(double rate_hi, double rate_lo, double loss_hi,
                     double loss_lo, bool have_lo) {
    score_hi_ = score(rate_hi, loss_hi, cfg_.margin);
    score_lo_ = have_lo ? score(rate_lo, loss_lo, cfg_.margin) : 0.0;
    have_scores_ = true;
    if (!cfg_.enable || !cfg_.act || !have_lo) return false;
    return score_lo_ > score_hi_ * (1.0 + cfg_.demote_margin);
  }

  bool armed() const { return armed_; }
  // Last computed scores, in kbps-proportional units. Exported so an
  // enable=true/act=false flight records the verdict it would have reached.
  double score_hi() const { return score_hi_; }
  double score_lo() const { return score_lo_; }
  bool have_scores() const { return have_scores_; }

 private:
  ObjectiveCfg cfg_;
  bool armed_ = false;
  double score_hi_ = 0.0, score_lo_ = 0.0;
  bool have_scores_ = false;
};

}  // namespace maburgs
