#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>

#include "s1_loss.h"
#include "mtest.h"

using namespace maburgs;

namespace {

// Helper to check if two doubles are approximately equal
bool approx_eq(double a, double b, double eps = 1e-6) {
  return std::abs(a - b) < eps;
}

}  // namespace

TEST(constant_rate_with_deficit) {
  // Feed monotonic counters at a constant rate with 3% deficit.
  // Expected = arrival rate * (1 / 0.97) to get 3% loss.
  S1LossWindow lw(500);  // 500 ms window

  double t = 0;
  uint64_t expected = 0;
  uint64_t arrived = 0;

  // Feed samples at 50 ms intervals for 1000 ms total.
  // Each sample: arrived increases by 97, expected by 100 (3% loss).
  for (int i = 0; i < 20; ++i) {
    arrived += 97;
    expected += 100;
    lw.add(expected, arrived, t);
    t += 50;
  }

  // Sample at end: should show approximately 3% loss
  auto sample = lw.sample(t - 1);
  CHECK(sample.valid);
  CHECK(approx_eq(sample.loss, 0.03, 1e-3));
}

TEST(zero_traffic_invalid) {
  S1LossWindow lw(500);

  // Add entries with zero deltas (same counters)
  lw.add(0, 0, 0);
  lw.add(0, 0, 50);
  lw.add(0, 0, 100);

  auto sample = lw.sample(100);
  CHECK(!sample.valid);
  CHECK(sample.loss == 0.0);
}

TEST(counter_reset_flushes_window) {
  S1LossWindow lw(500);

  double t = 0;
  uint64_t expected = 0;
  uint64_t arrived = 0;

  // Feed normal increasing counters
  for (int i = 0; i < 5; ++i) {
    expected += 100;
    arrived += 100;  // 0% loss
    lw.add(expected, arrived, t);
    t += 50;
  }

  auto sample_before = lw.sample(t - 1);
  CHECK(sample_before.valid);
  CHECK(sample_before.loss == 0.0);

  // Now simulate counter reset: totals go backwards
  // After reset, sample() should be invalid until we accumulate fresh deltas
  expected = 50;  // went backwards!
  arrived = 50;
  lw.add(expected, arrived, t);
  t += 50;

  // Now sample should be invalid because dExpected is zero
  auto sample_after_reset = lw.sample(t - 1);
  CHECK(!sample_after_reset.valid);

  // Add one more entry with positive delta
  expected = 150;
  arrived = 150;
  lw.add(expected, arrived, t);
  t += 50;

  // Now we have fresh deltas AFTER the reset.
  // A single add() with positive delta is valid (one-entry window).
  auto sample_after_one_add = lw.sample(t - 1);
  CHECK(sample_after_one_add.valid);
  CHECK(sample_after_one_add.loss == 0.0);

  // Add another to verify multi-entry window still works
  expected = 250;
  arrived = 250;
  lw.add(expected, arrived, t);

  auto sample_after_two_adds = lw.sample(t - 1);
  CHECK(sample_after_two_adds.valid);
  CHECK(sample_after_two_adds.loss == 0.0);
}

TEST(old_entries_age_out) {
  S1LossWindow lw(500);  // 500 ms window

  double t = 0;
  uint64_t expected = 0;
  uint64_t arrived = 0;

  // Feed samples for 1000 ms: each sample adds 100 expected, 80 arrived (20% loss)
  for (int i = 0; i < 20; ++i) {
    expected += 100;
    arrived += 80;
    lw.add(expected, arrived, t);
    t += 50;
  }

  // At t=950, sample should reflect ~20% loss from the 500 ms window
  auto sample_before_aging = lw.sample(950);
  CHECK(sample_before_aging.valid);
  CHECK(approx_eq(sample_before_aging.loss, 0.20, 1e-3));

  // Jump forward 600 ms. Old entries should age out.
  // All entries from before t=450 should be pruned when we sample at t=1550.
  // Feed a high-quality sample at new time
  expected += 100;
  arrived += 100;  // 0% loss on the new sample
  t = 1550;
  lw.add(expected, arrived, t);

  auto sample_after_aging = lw.sample(t);
  // The old 20% loss entries have aged out; the new 0% loss sample dominates.
  // Loss should be close to 0% now.
  CHECK(sample_after_aging.valid);
  CHECK(approx_eq(sample_after_aging.loss, 0.0, 1e-3));
}

TEST(arrived_greater_than_expected_clamps_to_zero) {
  S1LossWindow lw(500);

  double t = 0;

  // Unusual but possible: arrived > expected (e.g., duplicate arrivals)
  lw.add(100, 150, t);  // arrived > expected
  t += 50;
  lw.add(200, 300, t);

  auto sample = lw.sample(t);
  CHECK(sample.valid);
  // Loss should clamp to 0.0, not go negative
  CHECK(sample.loss >= 0.0);
  CHECK(approx_eq(sample.loss, 0.0, 1e-6));
}

TEST(single_add_then_sample_is_valid) {
  // After a single add() with positive delta from zero baseline,
  // sample() should be valid with correct loss.
  S1LossWindow lw(500);

  double t = 0;
  // First add: 100 expected, 97 arrived (3% loss)
  lw.add(100, 97, t);

  auto sample = lw.sample(t);
  CHECK(sample.valid);
  CHECK(approx_eq(sample.loss, 0.03, 1e-6));
}

TEST(reset_with_nondegen_loss) {
  // Critical test: after a reset followed by real loss,
  // sample() must report that loss once valid (not 0% or garbage).
  S1LossWindow lw(500);

  double t = 0;
  uint64_t expected = 0;
  uint64_t arrived = 0;

  // Build up some initial data
  for (int i = 0; i < 3; ++i) {
    expected += 100;
    arrived += 100;
    lw.add(expected, arrived, t);
    t += 50;
  }

  // Verify initial state: valid, 0% loss
  auto sample_before_reset = lw.sample(t - 1);
  CHECK(sample_before_reset.valid);
  CHECK(sample_before_reset.loss == 0.0);

  // Reset: totals go backwards
  expected = 10;
  arrived = 10;
  lw.add(expected, arrived, t);
  t += 50;

  // After reset, window is empty: invalid
  auto sample_after_reset = lw.sample(t - 1);
  CHECK(!sample_after_reset.valid);

  // Add first entry post-reset: ~50% loss (100 expected, 50 arrived)
  expected = 110;
  arrived = 60;
  lw.add(expected, arrived, t);
  t += 50;

  // Single entry post-reset: should be valid (dExpected > 0)
  auto sample_one_entry = lw.sample(t - 1);
  CHECK(sample_one_entry.valid);
  CHECK(approx_eq(sample_one_entry.loss, 0.50, 1e-3));  // ~50% loss
}

TEST(long_run_deque_pruning) {
  // After many adds spanning >> window_ms, window_.size() stays bounded.
  // Loss reflects only the recent interval.
  S1LossWindow lw(100);  // 100 ms window for faster pruning

  double t = 0;
  uint64_t expected = 0;
  uint64_t arrived = 0;

  // Add 1000 entries over 5000 ms (50 ms intervals, 100 ms window)
  // ~2 entries in window at a time. Window should stay small.
  for (int i = 0; i < 1000; ++i) {
    expected += 10;
    arrived += 9;  // 10% loss
    lw.add(expected, arrived, t);
    t += 5;  // 5 ms per update

    // Periodically check that window size stays bounded
    if (i % 100 == 0 && i > 0) {
      int size = lw.size();
      // At t >= 500, should have at most ~20 entries (100 ms window / 5 ms per entry)
      if (t >= 500) {
        CHECK(size <= 30);  // Conservative bound
      }
    }
  }

  // At the end, window should have only recent entries (100 ms window at t=5000)
  int final_size = lw.size();
  CHECK(final_size <= 30);  // ~20 entries for 100 ms window at 5 ms intervals

  // Loss should still reflect ~10%
  auto sample = lw.sample(t - 1);
  CHECK(sample.valid);
  CHECK(approx_eq(sample.loss, 0.10, 1e-3));
}

// blank_until(): the flight ctl-0160 cascade fix. One residual
// event stayed >0 in this window across a rung transition (nothing cleared
// it), and block 4 demotes on >0 every tick — so every s1 residual event
// walked the ladder to rung 0 at 50 ms/rung. The window must forget
// pre-transition accumulation AND swallow deltas booked during the settle
// period (the ~80 ms abandonment-horizon lag books old-rung loss late).
TEST(blank_clears_accumulated_loss) {
  S1LossWindow lw(500);
  lw.add(0, 0, 0.0);        // baseline
  lw.add(100, 60, 100.0);   // 40% loss event, would demote for 500 ms
  auto before = lw.sample(150.0);
  CHECK(before.valid);
  CHECK(before.loss > 0.0);

  lw.blank_until(300.0);    // rung transition at t=150

  // The debris must be gone immediately: no traffic in window.
  auto after = lw.sample(150.0);
  CHECK(!after.valid);
}

TEST(adds_during_blank_are_baseline_only) {
  S1LossWindow lw(500);
  lw.add(0, 0, 0.0);
  lw.add(100, 60, 100.0);   // loss at old rung
  lw.blank_until(300.0);    // transition at t=100, settle until t=300
  // Horizon-lag booking of old-rung loss lands during the blank: totals
  // advance with more loss. Must update the baseline only, no entry.
  lw.add(200, 120, 150.0);
  lw.add(260, 180, 250.0);
  // After the blank expires, none of that may be visible.
  auto s = lw.sample(310.0);
  CHECK(!s.valid);
}

TEST(loss_after_blank_expiry_books_normally) {
  S1LossWindow lw(500);
  lw.add(0, 0, 0.0);
  lw.add(100, 60, 100.0);
  lw.blank_until(300.0);
  lw.add(200, 120, 250.0);  // swallowed (in blank)
  // Fresh current-rung loss after expiry must still demote: 50% loss.
  lw.add(300, 170, 350.0);
  auto s = lw.sample(350.0);
  CHECK(s.valid);
  CHECK(approx_eq(s.loss, 0.50, 1e-6));
  // And clean traffic after that reads clean.
  lw.add(400, 270, 400.0);
  auto s2 = lw.sample(900.0);  // first two entries aged out of 500 ms window
  CHECK(s2.valid);
  CHECK(approx_eq(s2.loss, 0.0, 1e-6));
}

TEST(expected_in_window_counts_window_only) {
  maburgs::S1LossWindow w(500);
  w.add(0, 0, 0.0);          // baseline
  w.add(100, 90, 100.0);     // 100 expected in-window
  w.add(250, 240, 400.0);    // 150 more
  CHECK(w.expected_in_window(400.0) == 250);
  CHECK(w.expected_in_window(2000.0) == 0);   // everything aged out
}


namespace {
// The pre-2026-10-13 window, verbatim in behaviour: a full scan per sample.
// The running-total implementation must agree with it exactly.
class RefWindow {
 public:
  explicit RefWindow(int window_ms) : window_ms_(window_ms) {}
  void blank_until(double t) { w_.clear(); blank_ = t; }
  void add(uint64_t e, uint64_t a, double now) {
    bool reset = now < blank_;
    if (have_ && (e < pe_ || a < pa_)) { w_.clear(); reset = true; }
    if (reset) { pe_ = e; pa_ = a; have_ = true; return; }
    const uint64_t de = e - pe_, da = a - pa_;
    if (de > 0 || da > 0) w_.push_back({now, de, da});
    while (!w_.empty() && w_.front().t < now - window_ms_) w_.pop_front();
    pe_ = e; pa_ = a; have_ = true;
  }
  void totals(double now, uint64_t* te, uint64_t* ta) const {
    *te = *ta = 0;
    for (const auto& x : w_) {
      if (x.t < now - window_ms_ || x.t > now) continue;
      *te += x.de; *ta += x.da;
    }
  }
 private:
  struct E { double t; uint64_t de, da; };
  int window_ms_;
  std::deque<E> w_;
  uint64_t pe_ = 0, pa_ = 0;
  bool have_ = false;
  double blank_ = -1.0;
};
}  // namespace

TEST(running_totals_match_the_full_scan) {
  // Random adds (mostly monotonic time, some out of order), counter resets,
  // blanks, and samples at, before and after the latest add -- every sample
  // and expected_in_window must equal the reference full scan exactly.
  uint32_t rng = 12345;
  auto next = [&]() { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
  for (int run = 0; run < 40; ++run) {
    const int win = 100 + static_cast<int>(next() % 900);
    S1LossWindow w(win);
    RefWindow ref(win);
    double t = 0;
    uint64_t e = 0, a = 0;
    for (int i = 0; i < 3000; ++i) {
      const uint32_t r = next() % 1000;
      if (r < 3) {                       // counter reset
        e = next() % 50; a = e / 2;
      } else {
        const uint64_t de = next() % 40;
        e += de; a += de - (next() % 5 == 0 ? std::min<uint64_t>(de, next() % 4) : 0);
      }
      if (r < 5 && run % 3 == 0) t -= static_cast<double>(next() % 30);  // out of order
      else t += static_cast<double>(next() % 20) * 0.5;
      if (r >= 995) { const double b = t + static_cast<double>(next() % 50); w.blank_until(b); ref.blank_until(b); }
      w.add(e, a, t);
      ref.add(e, a, t);
      const double st = t + static_cast<double>(static_cast<int>(next() % 400) - 100);
      uint64_t te, ta;
      ref.totals(st, &te, &ta);
      const auto s = w.sample(st);
      CHECK(s.valid == (te > 0));
      if (te > 0) {
        double want = (static_cast<double>(te) - static_cast<double>(ta)) / static_cast<double>(te);
        want = std::max(0.0, std::min(1.0, want));
        CHECK(s.loss == want);
      }
      CHECK(w.expected_in_window(st) == te);
    }
  }
}

MTEST_MAIN
