#include <cmath>
#include <cstdint>

#include "mtest.h"
#include "overhead_policy.h"

using maburgs::OverheadCfg;
using maburgs::OverheadPolicy;

namespace {
OverheadCfg on() {
  OverheadCfg c;
  c.enable = true;
  return c;
}
}  // namespace

// The algebra tier 1 rests on: budget = ov/(1+ov) = margin*loss.
TEST(ov_for_loss_inverts_the_budget_formula) {
  // The proposal's worked example: down_util 0.35 against a base budget of
  // 0.5 demotes at L = 17.5%, and a true 2x margin there costs ov 0.538 --
  // against the 1.0 that ships. That gap is the whole argument.
  const double ov = OverheadPolicy::ov_for_loss(0.175, 2.0);
  CHECK(std::abs(ov - 0.35 / 0.65) < 1e-9);
  CHECK(std::abs(ov - 0.53846) < 1e-4);
  // Round-trips back to the budget it was asked for.
  CHECK(std::abs(ov / (1.0 + ov) - 0.35) < 1e-9);
  // Zero loss wants no repair at all (the floor is applied by feed(), not
  // here -- this is the raw algebra).
  CHECK(OverheadPolicy::ov_for_loss(0.0, 2.0) == 0.0);
}

// ov diverges as margin*loss approaches 1, so the clamp is load-bearing.
TEST(ov_for_loss_saturates_instead_of_diverging) {
  const double ov = OverheadPolicy::ov_for_loss(0.9, 2.0);  // m*L = 1.8
  CHECK(std::isfinite(ov));
  CHECK(ov > 18.0);  // ~0.95/(1-0.95); the caller clamps to max_ov
  CHECK(std::isfinite(OverheadPolicy::ov_for_loss(1.0, 2.0)));
}

// --- ov_req: the episode gauge min_ov is derived from --------------------

// Must stay byte-for-byte the same function as tools/flightreport.py's
// fec_ov_req(), which is the reference. Reproduces the 2026-09-16 bench
// figures from the fec.log commit: rung-5 base at flown ov 1.0 with
// m p50 = 11 against r ~ 41.
TEST(ov_req_reproduces_the_bench_episodes) {
  CHECK(std::abs(OverheadPolicy::ov_req(11, 41, 1.0) - 0.3869) < 1e-3);
  CHECK(std::abs(OverheadPolicy::ov_req(16, 41, 1.0) - 0.5151) < 1e-3);
  // More lost sources needs more overhead; more covering repairs needs less.
  CHECK(OverheadPolicy::ov_req(16, 41, 1.0) > OverheadPolicy::ov_req(11, 41, 1.0));
  CHECK(OverheadPolicy::ov_req(11, 82, 1.0) < OverheadPolicy::ov_req(11, 41, 1.0));
}

// No repair covered the episode at all: no overhead would have saved it.
// That is a real outcome, reported as infinity rather than treated as an
// error or silently clamped.
TEST(ov_req_is_infinite_when_no_repair_covered_the_episode) {
  CHECK(std::isinf(OverheadPolicy::ov_req(10, 0, 1.0)));
  CHECK(std::isinf(OverheadPolicy::ov_req(10, -1, 1.0)));
}

// Round-trip the derivation: at the overhead ov_req returns, the episode's
// sources and covering repairs balance -- x*(1+x) == c.
TEST(ov_req_satisfies_its_own_balance_condition) {
  const double m = 13, r = 45, ov = 0.7;
  const double x = OverheadPolicy::ov_req(m, r, ov);
  const double c = m * ov * (1.0 + ov) / r;
  CHECK(std::abs(x * (1.0 + x) - c) < 1e-9);
}

// THE REGRESSION THIS FILE EXISTS FOR. min_ov was 0.3 on a stated guess
// ("pending flight data on that variance, not a measurement"); fec.log
// measured rung-5 base episodes needing up to 0.46, so 0.3 would have
// under-protected the worst episode in the recording. The floor must stay at
// or above that measurement, and on the `step` grid so quantisation cannot
// land under it.
TEST(min_ov_floor_covers_the_measured_worst_episode) {
  OverheadCfg c;
  constexpr double kMeasuredWorstOvReq = 0.46;  // 2026-09-16 bench, rung-5 base
  CHECK(c.min_ov >= kMeasuredWorstOvReq);
  // ...and the enh layer's measured need too, with one shared floor.
  CHECK(c.min_ov >= 0.38);
  // On the quantisation grid: a floor between two steps would be rounded up
  // by feed() anyway, but keeping it ON the grid makes the commanded value
  // equal the floor exactly rather than a step above it.
  const double steps = c.min_ov / c.step;
  CHECK(std::abs(steps - std::round(steps)) < 1e-9);
  // And still a floor, not the ceiling.
  CHECK(c.min_ov < c.max_ov);
}

// The floor is what a sparse link actually flies, so pin that it binds --
// this is where the 13% rate cost is paid, and where the old 0.3 would have
// under-protected.
TEST(a_sparse_link_sits_on_the_floor_not_below_it) {
  OverheadCfg c = on();
  OverheadPolicy p(c);
  for (double L : {0.0, 0.01, 0.05, 0.10}) {
    OverheadPolicy q(c);
    const double got = q.feed(L, 1.0, 0.0);
    CHECK(got >= c.min_ov);
    CHECK(got >= 0.46);  // the measured worst episode, whatever L says
  }
  // 10% mean loss wants ov 0.25 on the mean-loss model alone -- comfortably
  // under the measured burst need, which is the whole reason for the floor.
  CHECK(OverheadPolicy::ov_for_loss(0.10, 2.0) < 0.46);
}

// Default OFF: computes and exports, commands nothing. This is the
// observe-only staging, so it has to be genuinely inert.
TEST(disabled_computes_a_target_but_commands_nothing) {
  OverheadCfg c;  // enable stays false
  OverheadPolicy p(c);
  for (int i = 0; i < 50; ++i) {
    CHECK(p.feed(0.20, 1.0, i * 100.0) == 1.0);  // commanded is unchanged
  }
  CHECK(p.target() > 0.6);  // ...but it knows what it wanted
  CHECK(p.changes() == 0);
}

TEST(enabled_moves_the_commanded_value_toward_the_target) {
  OverheadPolicy p(on());
  // L = 0.20 -> budget 0.40 -> ov 0.667 -> quantised up to 0.7.
  const double got = p.feed(0.20, 1.0, 0.0);
  CHECK(std::abs(got - 0.7) < 1e-9);
  CHECK(p.changes() == 1);
}

// Quantisation must round toward MORE protection, so it can never leave a
// layer under-protected relative to what the margin asked for.
TEST(quantisation_rounds_up_never_down) {
  OverheadCfg c = on();
  c.step = 0.1;
  c.dead_band = 0.0;
  OverheadPolicy p(c);
  p.feed(0.20, 2.0, 0.0);  // target 0.667
  CHECK(p.quantised() >= p.target());
  CHECK(std::abs(p.quantised() - 0.7) < 1e-9);
}

// A momentarily clean link must not strip protection to the config floor of
// 0.1 -- the target tracks the MEAN loss and models nothing about its
// variance, so arriving at a 9% budget before a burst loses a GOP.
TEST(floor_holds_protection_on_a_clean_link) {  // value pinned above
  OverheadCfg c = on();
  OverheadPolicy p(c);
  const double got = p.feed(0.0, 1.0, 0.0);
  CHECK(got >= c.min_ov);
  CHECK(std::abs(got - c.min_ov) < 1e-9);
  CHECK(p.target() == c.min_ov);
}

TEST(ceiling_is_the_config_range_of_the_overhead_keys) {
  OverheadCfg c = on();
  OverheadPolicy p(c);
  const double got = p.feed(0.45, 1.0, 0.0);  // m*L = 0.9 -> ov 9.0
  CHECK(got <= c.max_ov);
  CHECK(std::abs(got - 2.0) < 1e-9);
}

// Each change costs an IDR, so a target inside the dead band is not worth one.
TEST(dead_band_suppresses_a_change_not_worth_an_idr) {
  OverheadCfg c = on();
  c.dead_band = 0.15;
  OverheadPolicy p(c);
  // L = 0.30 -> budget 0.60 -> ov 1.5, quantised 1.5: 0.5 away from 1.0, so
  // it moves.
  CHECK(p.feed(0.30, 1.0, 0.0) != 1.0);
  // A target within the band of what is already commanded does not.
  OverheadPolicy q(c);
  const double near = q.feed(0.255, 1.1, 0.0);  // ov ~= 1.04 -> quantised 1.1
  CHECK(std::abs(near - 1.1) < 1e-9);
  CHECK(q.changes() == 0);
}

// The rate limit is what keeps this from being the loop that was deleted on
// 2026-09-01 for ~150 keyframes per 12-minute flight.
TEST(rate_limit_caps_downward_changes) {
  OverheadCfg c = on();
  c.min_interval_ms = 2000.0;
  c.dead_band = 0.0;
  OverheadPolicy p(c);
  double cur = 2.0;
  cur = p.feed(0.05, cur, 0.0);      // first change is free
  REQUIRE(p.changes() == 1);
  const double after_first = cur;
  for (int i = 1; i < 19; ++i) {      // 100 ms apart, inside the interval
    cur = p.feed(0.05, cur, i * 100.0);
  }
  CHECK(p.changes() == 1);            // all suppressed
  CHECK(std::abs(cur - after_first) < 1e-9);
  cur = p.feed(0.05, cur, 2100.0);    // past the interval
  CHECK(p.changes() == 1);            // ...but already at target, dead band
}

// Asymmetry: holding BACK protection the link is asking for, to save an IDR,
// is the wrong trade -- an under-protected layer sheds whole AUs.
TEST(rate_limit_never_delays_an_increase) {
  OverheadCfg c = on();
  c.min_interval_ms = 5000.0;
  c.dead_band = 0.0;
  OverheadPolicy p(c);
  double cur = p.feed(0.05, 0.6, 0.0);  // settle low
  REQUIRE(p.changes() == 1);
  // 100 ms later the link degrades badly. The increase must land now.
  const double up = p.feed(0.35, cur, 100.0);
  CHECK(up > cur);
  CHECK(p.changes() == 2);
}

// Trap 1, pinned: the input is a raw loss fraction, so feeding the same loss
// forever converges and STAYS converged. If the policy were driven by
// utilisation (whose denominator is this very actuator) it would instead
// chase its own tail.
TEST(a_steady_loss_converges_to_a_fixed_point) {
  OverheadCfg c = on();
  c.dead_band = 0.0;
  c.min_interval_ms = 0.0;
  OverheadPolicy p(c);
  double cur = 2.0;
  for (int i = 0; i < 100; ++i) cur = p.feed(0.20, cur, i * 100.0);
  CHECK(std::abs(cur - 0.7) < 1e-9);
  const uint64_t settled = p.changes();
  for (int i = 100; i < 200; ++i) cur = p.feed(0.20, cur, i * 100.0);
  CHECK(p.changes() == settled);  // no further churn, hence no further IDRs
}

MTEST_MAIN
