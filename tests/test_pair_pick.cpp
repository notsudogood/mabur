// Pair pick for a 40 MHz boot scan (docs/bw40.md §3): halves are ranked
// as today's 20 MHz channels; a pair scores as its WORSE half and is only
// ranked once both halves have min_rounds visits.
#include <vector>
#include "mtest.h"
#include "pair_pick.h"
#include "mabur/ht40.h"
using namespace maburgs;

static_assert(mabur::ht40_pair_other(136) == 132 && mabur::ht40_pair_other(132) == 136, "132+136");
static_assert(mabur::ht40_pair_other(144) == 140 && mabur::ht40_pair_other(40) == 36, "");
static_assert(mabur::ht40_pair_other(128) == 124 && mabur::ht40_pair_other(165) == 0, "");

TEST(half_set_is_both_halves_of_home_and_candidates_in_config_order) {
  const std::vector<uint8_t> want = {132, 136, 140, 144, 36, 40, 124, 128};
  CHECK(scan_half_set({136, 144, 40, 128}) == want);
  CHECK(scan_half_set({136, 165}).size() == 2);   // no pair: skipped (config rejects it anyway)
  CHECK(scan_half_set({136, 132}).size() == 2);   // same pair as home: deduplicated
}

TEST(pair_score_is_the_worse_half_and_home_margin_applies_to_the_pair) {
  // Home 132+136: 136 clean, 132 dirty. Candidate 140+144: both moderate.
  std::vector<RankEntry> all = {{136, 0, 3, false, 0}, {132, 500, 3, false, 0},
                                {144, 100, 3, false, 0}, {140, 120, 3, false, 0}};
  CHECK(pair_proposal(all, 136, {136, 144}, 3, 20) == 144);    // 120 + 20 <= 500
  CHECK(pair_proposal(all, 136, {136, 144}, 3, 400) == 136);   // margin not met -> home
}

TEST(pair_unranked_until_both_halves_reach_min_rounds) {
  std::vector<RankEntry> all = {{136, 0, 3, false, 0}, {132, 0, 1, false, 0},
                                {144, 50, 3, false, 0}, {140, 50, 3, false, 0}};
  // Home's pair is unranked (132 has one visit): the best RANKED candidate
  // wins, the same rule ChannelRanker::proposal applies to an unranked home.
  CHECK(pair_proposal(all, 136, {136, 144}, 3, 20) == 144);
  std::vector<RankEntry> none = {{136, 0, 3, false, 0}, {132, 0, 1, false, 0},
                                 {144, 50, 3, false, 0}, {140, 50, 1, false, 0}};
  CHECK(pair_proposal(none, 136, {136, 144}, 3, 20) == 136);   // nothing ranked -> home
  std::vector<RankEntry> half = {{136, 0, 3, false, 0}, {132, 0, 3, false, 0},
                                 {144, 0, 3, false, 0}};   // 140 never visited
  CHECK(pair_proposal(half, 136, {136, 144}, 3, 0) == 136);    // a pair on half the evidence never wins
}

TEST(pair_ties_stay_on_current_then_a_strictly_cleaner_pair_wins_at_margin_zero) {
  std::vector<RankEntry> all = {{136, 10, 3, false, 0}, {132, 10, 3, false, 0},
                                {144, 10, 3, false, 0}, {140, 10, 3, false, 0},
                                {40, 10, 3, false, 0},  {36, 10, 3, false, 0}};
  CHECK(pair_proposal(all, 136, {136, 144, 40}, 3, 0) == 136);
  all[4].worst_busy = 5; all[5].worst_busy = 5;
  CHECK(pair_proposal(all, 136, {136, 144, 40}, 3, 0) == 40);
}

TEST(any_pair_ranked_needs_both_halves_of_some_pair) {
  // The boot-pick freeze's "was anything measured" test at radio.width 40:
  // one half of home's pair at min_rounds (the one-card home half also
  // collects home-window visits) is NOT a ranked pair.
  std::vector<RankEntry> one_half = {{136, 0, 3, false, 0}, {132, 0, 1, false, 0},
                                     {144, 0, 2, false, 0}, {140, 0, 2, false, 0}};
  CHECK(!any_pair_ranked(one_half, {136, 144}, 3));
  CHECK(!any_pair_ranked({{136, 0, 3, false, 0}}, {136, 144}, 3));   // 132 never visited
  CHECK(!any_pair_ranked({}, {136, 144}, 3));
  // Both halves of a candidate pair: ranked, home's pair still is not.
  std::vector<RankEntry> cand = {{136, 0, 3, false, 0}, {132, 0, 1, false, 0},
                                 {144, 0, 3, false, 0}, {140, 0, 3, false, 0}};
  CHECK(any_pair_ranked(cand, {136, 144}, 3));
  // Both halves of home's pair, no candidate ranked.
  std::vector<RankEntry> home = {{136, 0, 3, false, 0}, {132, 0, 4, false, 0},
                                 {144, 0, 1, false, 0}};
  CHECK(any_pair_ranked(home, {136, 144}, 3));
}

static maburgs::RankEntry re(uint8_t ch, uint32_t busy, double bpct) {
  maburgs::RankEntry e; e.ch = ch; e.worst_busy = busy; e.visits = 3;
  e.busy_valid = true; e.worst_busy_pct = bpct; return e;
}
TEST(one_blocked_half_blocks_the_pair) {
  std::vector<maburgs::RankEntry> all = {re(132, 0, 0), re(136, 0, 0), re(140, 0, 90), re(144, 0, 0),
                                         re(60, 5, 0), re(64, 5, 0)};
  // 140+144 scores 0 events but its 140 half is blocked; 60+64 wins over it.
  CHECK(maburgs::pair_proposal(all, 136, {136, 144, 64}, 3, 1000, 30.0) == 136);   // home clean, margin
  all[0] = re(132, 0, 100);   // home's other half blocked
  CHECK(maburgs::pair_proposal(all, 136, {136, 144, 64}, 3, 1000, 30.0) == 64);
}

TEST(pair_ranking_is_best_first_and_skips_unranked) {
  std::vector<RankEntry> all = {{136, 10, 3, false, 0}, {132, 10, 3, false, 0},
                                {144, 50, 3, false, 0}, {140, 5, 3, false, 0},
                                {40, 1, 3, false, 0},  {36, 1, 3, false, 0},
                                {112, 0, 1, false, 0}, {108, 0, 1, false, 0}};   // 112 unranked
  const std::vector<uint8_t> want = {40, 136, 144};
  CHECK(pair_ranking(all, {40, 136, 144, 112}, 3) == want);
  CHECK(!all_pairs_ranked(all, {40, 136, 144, 112}, 3));
  CHECK(all_pairs_ranked(all, {40, 136, 144}, 3));
}
TEST(current_unranked_means_best_ranked_wins) {
  std::vector<RankEntry> all = {{144, 50, 3, false, 0}, {140, 50, 3, false, 0}};
  CHECK(pair_proposal(all, 136, {136, 144}, 3, 20) == 144);
  CHECK(pair_proposal({}, 136, {136, 144}, 3, 20) == 136);   // nothing ranked: stay
}

// ---- airtime margin (radio.scan.pick_busy_margin_pct, 2026-10-11)
// The 2026-10-11 flight's K line: ch:worst_busy:floor:busy%. The link was
// found on 144 (remembered); the event score picked 60+64 (486 vs 750,
// both 0.0 % busy) and the flight then lost 0.5-3.8 % on 64 for 100 s.
static std::vector<RankEntry> k_flight2() {
  return {{36, 580, 3, true, -90, true, 3.2},  {40, 929, 3, true, -88, true, 18.3},
          {60, 486, 3, true, -90, true, 0.0},  {64, 464, 3, true, -91, true, 0.0},
          {108, 2109, 3, true, -94, true, 0.0}, {112, 25, 3, true, -95, true, 0.0},
          {140, 0, 3, true, -92, true, 0.0},   {144, 750, 3, true, -92, true, 0.0}};
}
static const std::vector<uint8_t> kSet = {40, 64, 112, 144};
TEST(airtime_margin_keeps_a_clean_current_channel) {
  const auto all = k_flight2();
  CHECK(pair_proposal(all, 144, kSet, 3, 20, 50, 0.0) == 64);    // the event score alone moved it
  CHECK(pair_proposal(all, 144, kSet, 3, 20, 50, 5.0) == 144);   // equal airtime: stay
}
TEST(airtime_margin_still_leaves_a_busy_current_channel) {
  // Remembered 40 (home Wi-Fi, 36+40 worse half 18.3 % busy): the calmest
  // unblocked candidate is 0.0 % busy, >= 5 points calmer -> move; ties on
  // busy go to the lower event score (60+64: 486 < 750 < 2109).
  CHECK(pair_proposal(k_flight2(), 40, kSet, 3, 20, 50, 5.0) == 64);
}
TEST(airtime_margin_ignores_small_busy_differences) {
  // The later 2026-10-11 flight's K line: remembered 112 (108 2.4 %, 112
  // 1.6 %) against 140+144 at 0.0 % -- 2.4 points is under the margin.
  std::vector<RankEntry> all = {{36, 143, 3, true, -90, true, 2.0},  {40, 2764, 3, true, -86, true, 16.3},
                                {60, 388, 3, true, -90, true, 0.0},  {64, 55, 3, true, -91, true, 0.4},
                                {108, 2234, 3, true, -94, true, 2.4}, {112, 41, 3, true, -95, true, 1.6},
                                {140, 102, 3, true, -92, true, 0.0}, {144, 257, 3, true, -92, true, 0.0}};
  CHECK(pair_proposal(all, 112, kSet, 3, 20, 50, 5.0) == 112);
  CHECK(pair_proposal(all, 112, kSet, 3, 20, 50, 2.0) == 144);   // calmest: 140+144 (0.0) beats 60+64 (0.4)
}
TEST(airtime_margin_does_not_hold_a_blocked_channel) {
  auto all = k_flight2();
  all[7].worst_busy_pct = 60.0;   // 144 blocked (blocked_pct 50)
  CHECK(pair_proposal(all, 144, kSet, 3, 20, 50, 5.0) == 64);
}
TEST(airtime_margin_needs_a_reading_on_both_halves) {
  auto all = k_flight2();
  all[6].busy_valid = false;      // 140 without NHM: current has no full reading -> event score
  CHECK(pair_proposal(all, 144, kSet, 3, 20, 50, 5.0) == 64);
  auto b = k_flight2();
  for (auto& e : b) if (e.ch != 140 && e.ch != 144) e.busy_valid = false;   // no candidate has one
  CHECK(pair_proposal(b, 144, kSet, 3, 20, 50, 5.0) == 64);
}

MTEST_MAIN
