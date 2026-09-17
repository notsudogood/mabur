#include <cmath>

#include "mtest.h"
#include "rung_objective.h"

using maburgs::ObjectiveCfg;
using maburgs::RungObjective;

namespace {
// The flown ladder's HT20 long-GI rates (common/src/profile.cpp).
constexpr double kMcs0 = 6.5, kMcs1 = 13.0, kMcs2 = 19.5;
constexpr double kMcs3 = 26.0, kMcs4 = 39.0, kMcs5 = 52.0;

ObjectiveCfg acting() {
  ObjectiveCfg c;
  c.enable = true;
  c.act = true;
  return c;
}
}  // namespace

// score = rate * (1 - m*L): the airtime left for video once FEC covers a
// margin over the measured loss.
TEST(score_is_rate_scaled_by_the_fec_cost) {
  CHECK(std::abs(RungObjective::score(kMcs4, 0.0, 2.0) - 39.0) < 1e-9);
  CHECK(std::abs(RungObjective::score(kMcs4, 0.175, 2.0) - 39.0 * 0.65) < 1e-9);
  // m*L >= 1 is unflyable at any overhead -- clamped at 0, not negative.
  CHECK(RungObjective::score(kMcs4, 0.5, 2.0) == 0.0);
  CHECK(RungObjective::score(kMcs4, 0.9, 2.0) == 0.0);
}

// The arm threshold must fall out of the RATE RATIO, not a tuned constant:
// L >= (1 - rate_lo/rate_hi)/m.
TEST(arm_threshold_follows_the_rate_ratio) {
  // 1.33x step (mcs4 -> mcs3): 1 - 26/39 = 1/3, over margin 2 -> 16.7%.
  CHECK(std::abs(RungObjective::arm_loss_threshold(kMcs4, kMcs3, 2.0) - 1.0 / 6.0) < 1e-9);
  // 1.5x step (mcs3 -> mcs2): 1 - 19.5/26 = 0.25, over 2 -> 12.5%.
  CHECK(std::abs(RungObjective::arm_loss_threshold(kMcs3, kMcs2, 2.0) - 0.125) < 1e-9);
  // 2.0x step (mcs1 -> mcs0): 1 - 0.5 = 0.5, over 2 -> 25%. The coarsest
  // step on the ladder demands the most loss before a demote is arguable.
  CHECK(std::abs(RungObjective::arm_loss_threshold(kMcs1, kMcs0, 2.0) - 0.25) < 1e-9);
  // Monotone: a coarser step always needs more loss to justify taking it.
  CHECK(RungObjective::arm_loss_threshold(kMcs1, kMcs0, 2.0) >
        RungObjective::arm_loss_threshold(kMcs4, kMcs3, 2.0));
}

// Below the threshold, no measurement of the lower rung could justify going
// there -- so probing it is pure airtime cost and must not happen.
TEST(probe_not_armed_below_the_ratio_threshold) {
  RungObjective o(acting());
  CHECK(!o.want_probe(kMcs4, kMcs3, 0.05, true));
  CHECK(!o.want_probe(kMcs4, kMcs3, 0.15, true));  // just under 16.7%
  CHECK(!o.armed());
}

TEST(probe_arms_once_a_demote_is_arguable) {
  RungObjective o(acting());
  CHECK(o.want_probe(kMcs4, kMcs3, 0.20, true));
  CHECK(o.armed());
}

// A probe that flaps around the boundary never accumulates the seconds of
// samples it needs, and pays airtime in bursts for nothing.
TEST(arm_disarm_is_hysteretic) {
  ObjectiveCfg c = acting();
  c.disarm_frac = 0.75;
  RungObjective o(c);
  const double thr = RungObjective::arm_loss_threshold(kMcs4, kMcs3, 2.0);
  REQUIRE(o.want_probe(kMcs4, kMcs3, thr + 0.01, true));
  // Dipping just below the ARM threshold keeps it armed...
  CHECK(o.want_probe(kMcs4, kMcs3, thr * 0.9, true));
  // ...until it falls under disarm_frac of it.
  CHECK(!o.want_probe(kMcs4, kMcs3, thr * 0.5, true));
}

// If FEC is already failing there is nothing to weigh up: demote now rather
// than spending seconds and airtime measuring the rung below.
TEST(dirty_residual_disarms_immediately) {
  RungObjective o(acting());
  REQUIRE(o.want_probe(kMcs4, kMcs3, 0.25, true));
  CHECK(!o.want_probe(kMcs4, kMcs3, 0.25, /*residual_clean=*/false));
  CHECK(!o.armed());
}

// Without a real down-probe sample the answer is ALWAYS stay. An unmeasured
// lower rung must never be treated as a clean one.
TEST(no_down_sample_never_demotes) {
  RungObjective o(acting());
  CHECK(!o.should_demote(kMcs4, kMcs3, 0.30, 0.0, /*have_lo=*/false));
  CHECK(o.score_lo() == 0.0);
}

// The headline case: on the flown ladder a demote buys no extra loss
// tolerance, so it only pays when the lower rung is genuinely cleaner.
TEST(demotes_only_when_the_lower_rung_actually_wins) {
  RungObjective o(acting());
  // mcs4 at 20% loss scores 39*0.6 = 23.4. mcs3 would need to beat 23.4 by
  // the 10% hysteresis, i.e. > 25.7, so 26*(1-2L) > 25.7 -> L < 0.6%.
  CHECK(!o.should_demote(kMcs4, kMcs3, 0.20, 0.10, true));  // 26*0.8 = 20.8, worse
  CHECK(!o.should_demote(kMcs4, kMcs3, 0.20, 0.05, true));  // 26*0.9 = 23.4, a tie
  CHECK(o.should_demote(kMcs4, kMcs3, 0.20, 0.0, true));    // 26.0 > 25.7
}

// A 2.0x step is a much worse deal, and the objective says so without any
// per-rung tuning.
TEST(a_coarse_step_demands_a_far_cleaner_lower_rung) {
  RungObjective o(acting());
  // mcs1 at 20% scores 13*0.6 = 7.8; mcs0 tops out at 6.5 even spotless, so
  // this step can NEVER win here.
  CHECK(!o.should_demote(kMcs1, kMcs0, 0.20, 0.0, true));
  // It only becomes possible once mcs1 is bad enough: at 30%, 13*0.4 = 5.2,
  // and a clean mcs0's 6.5 clears the hysteresis.
  CHECK(o.should_demote(kMcs1, kMcs0, 0.30, 0.0, true));
}

// Stage 2: armed and scoring, but the verdict does not move the ladder.
TEST(act_false_scores_but_never_demotes) {
  ObjectiveCfg c;
  c.enable = true;
  c.act = false;
  RungObjective o(c);
  CHECK(o.want_probe(kMcs4, kMcs3, 0.30, true));   // armed: it buys the data
  CHECK(!o.should_demote(kMcs4, kMcs3, 0.30, 0.0, true));
  CHECK(o.have_scores());                          // ...and records the verdict
  CHECK(o.score_lo() > o.score_hi());
}

// Stage 1: fully inert.
TEST(disabled_arms_nothing_and_decides_nothing) {
  ObjectiveCfg c;  // enable stays false
  RungObjective o(c);
  CHECK(!o.want_probe(kMcs4, kMcs3, 0.40, true));
  CHECK(!o.armed());
  CHECK(!o.should_demote(kMcs4, kMcs3, 0.40, 0.0, true));
}

// The margin must track tier 1's: the objective has to score the overhead
// the overhead policy is actually commanding.
TEST(margin_shifts_both_the_threshold_and_the_score) {
  CHECK(RungObjective::arm_loss_threshold(kMcs4, kMcs3, 1.0) >
        RungObjective::arm_loss_threshold(kMcs4, kMcs3, 2.0));
  CHECK(RungObjective::score(kMcs5, 0.10, 1.0) >
        RungObjective::score(kMcs5, 0.10, 2.0));
}

MTEST_MAIN
