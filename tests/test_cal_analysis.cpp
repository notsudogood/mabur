#include "mtest.h"
#include "cal_analysis.h"

#include <vector>

using namespace maburgs;

namespace {

// Builds one cell. rssi is the best card's median; -999 = unmeasured.
CalCell cell(int idx, int pct, int rssi = -70, int expected = 100) {
  CalCell c;
  c.idx = idx;
  c.expected = static_cast<uint16_t>(expected);
  c.received[0] = static_cast<uint16_t>(expected * pct / 100);
  c.received[1] = 0;
  c.rssi_dbm[0] = rssi;
  c.have_rssi[0] = rssi != kRssiNone;
  c.have_rssi[1] = false;
  return c;
}

}  // namespace

// mcs7 as measured on the bench: a sensitivity floor below idx 34, a clean run
// to 49, then the comb -- 2% at 56 but clean again at 57. The wall is the END OF THE
// FIRST CONTIGUOUS RUN (49). The island at 57 is not headroom.
TEST(mcs7_first_dip_not_last_good) {
  std::vector<CalCell> cells;
  for (int i = 0; i <= 33; ++i) cells.push_back(cell(i, 10));   // floor
  for (int i = 34; i <= 49; ++i) cells.push_back(cell(i, 100));  // clean run
  for (int i = 50; i <= 55; ++i) cells.push_back(cell(i, 30));  // dip
  cells.push_back(cell(56, 2));
  // An island ABOVE the threshold: the measured comb produces these (mcs6
  // reads 63% at idx 52 but 95-97% at 53-57). This is the cell that
  // discriminates -- a "last good index" implementation returns 57 here,
  // the correct first-contiguous-run implementation returns 49.
  cells.push_back(cell(57, 99));
  for (int i = 58; i <= 63; ++i) cells.push_back(cell(i, 0));

  const auto w = analyze_rate(cells, CalThresholds{});
  CHECK(w.wall == 49);
  CHECK(w.floor_idx == 34);
  CHECK((w.flags & kCalNoDip) == 0);
  CHECK((w.flags & kCalUndetermined) == 0);
}

// mcs0: never dips anywhere in the sweep, up to the top of the diff field.
// There is no wall to find, and "no dip" is not proof of no compression:
// the 2026-10-12 flight calibration parked MCS0-3 at the rail this way, ~9 dB
// over the chip's stock level, and they flew at EVM -10.5 dB against -16.5
// at stock (docs/next-build-backlog.md item 2). So the row comes out with no
// wall (kNoWall, flagged no_dip) and the drone keeps its pre-calibration
// power for it.
//
// The fixture sweeps the real coarse grid, -41..63 step 4, so "no dip" means
// no dip up to the field's top.
TEST(mcs0_no_dip_keeps_its_power) {
  std::vector<CalCell> cells;
  // Flat floor to 28, ~0.3 dB/idx ramp, then a flat ceiling -- the real
  // measured shape, which this rule deliberately no longer reads.
  for (int i = -41; i <= 63; i += 4) {
    int rssi;
    if (i <= 28) rssi = -80;
    else if (i <= 91) rssi = -80 + (i - 28) * 3 / 10;
    else rssi = -80 + (91 - 28) * 3 / 10;
    cells.push_back(cell(i, 100, rssi));
  }
  const auto w = analyze_rate(cells, CalThresholds{});
  CHECK((w.flags & kCalNoDip) != 0);
  CHECK((w.flags & kCalUndetermined) == 0);
  CHECK(w.wall == kNoWall);
}

// The coarse grid (-41..63 step 4, 50 frames a cell) with one cell set to
// `pct` at `bad_idx` -- and optionally a second one at `bad2_idx`.
namespace {
std::vector<CalCell> coarse_row(int bad_idx, int pct, int bad2_idx = 999, int pct2 = 0) {
  std::vector<CalCell> cells;
  for (int i = -41; i <= 63; i += 4)
    cells.push_back(cell(i, i == bad_idx ? pct : i == bad2_idx ? pct2 : 100, -70, 50));
  return cells;
}
}  // namespace

// The 2026-09 defect: one clean mcs2 cell losing 4 of 20 frames ended a row
// that never compresses and put it on the wall path. A lone shallow miss
// with good cells after it is noise: the row stays no_dip, and says so with
// kCalNoiseCell. Since 2026-10-17 "shallow" means at most 2 of 50 lost.
TEST(lone_shallow_coarse_miss_is_noise_not_a_wall) {
  const auto w = analyze_rate(coarse_row(-1, 96), CalThresholds{});
  CHECK(w.wall == kNoWall);
  CHECK((w.flags & kCalNoDip) != 0);
  CHECK((w.flags & kCalNoiseCell) != 0);
}

// The 2026-10-12 flight: a rate losing ~10% where compression starts read as
// "no dip" under the old 90% bar and 75% noise rule. Both now end the run.
TEST(a_ten_percent_miss_ends_the_run) {
  const auto w = analyze_rate(coarse_row(-1, 90), CalThresholds{});
  CHECK(w.wall == -5);
  CHECK((w.flags & kCalNoDip) == 0);
  CHECK((w.flags & kCalNoiseCell) == 0);
  // One frame short of the bar is not a pass either: 49 of 50 is 98%.
  CHECK(analyze_rate(coarse_row(15, 98, 19, 98), CalThresholds{}).wall == 11);
}

// Past a real wall the bench saw deep dips (63%, 32%, 4%): a deep miss ends
// the run as it always did, even with good cells after it.
TEST(deep_coarse_miss_still_ends_the_run) {
  const auto w = analyze_rate(coarse_row(-1, 60), CalThresholds{});
  CHECK(w.wall == -5);
  CHECK((w.flags & kCalNoDip) == 0);
  CHECK((w.flags & kCalNoiseCell) == 0);
}

// Two misses in a row are a dip, however shallow.
TEST(two_shallow_misses_in_a_row_end_the_run) {
  const auto w = analyze_rate(coarse_row(-1, 96, 3, 96), CalThresholds{});
  CHECK(w.wall == -5);
  CHECK((w.flags & kCalNoiseCell) == 0);
}

// The overdrive guard: a shallow miss followed by ONE good cell and then a
// collapse is a dip with an island after it (mcs6 measured 63% at idx 52,
// 95-97% at 53-57, collapsed by 64) -- the run must not climb through it.
TEST(shallow_miss_needs_two_good_cells_after_it) {
  std::vector<CalCell> cells;
  for (int i = -41; i <= 47; i += 4) cells.push_back(cell(i, 100, -70, 50));
  cells.push_back(cell(51, 96, -70, 50));   // shallow miss
  cells.push_back(cell(55, 100, -70, 50));  // island
  cells.push_back(cell(59, 0, -70, 50));
  cells.push_back(cell(63, 0, -70, 50));
  const auto w = analyze_rate(cells, CalThresholds{});
  CHECK(w.wall == 47);
  CHECK((w.flags & kCalNoiseCell) == 0);
  // ...and a miss on the second-to-last cell has only one cell after it:
  // it ends the run (under-drive by one coarse step, never overdrive).
  CHECK(analyze_rate(coarse_row(59, 96), CalThresholds{}).wall == 55);
}

// The 100-frame fine pass keeps the strict 99% bar: there, 96% is 4 frames
// lost, not noise.
TEST(fine_cells_keep_the_strict_bar) {
  std::vector<CalCell> cells;
  for (int i = 40; i <= 56; ++i) cells.push_back(cell(i, i == 48 ? 96 : 99));
  const auto w = analyze_rate(cells, CalThresholds{});
  CHECK(w.wall == 47);
  CHECK((w.flags & kCalNoiseCell) == 0);
}

TEST(row_never_reaching_threshold_is_undetermined) {
  std::vector<CalCell> cells;
  for (int i = 0; i <= 127; i += 4) cells.push_back(cell(i, 40));
  const auto w = analyze_rate(cells, CalThresholds{});
  CHECK((w.flags & kCalUndetermined) != 0);
  CHECK(w.wall == kNoWall);  // caller must leave the config entry alone
}

TEST(negative_relative_indices_are_ordinary_cells) {
  // Relative indices: a wall can sit below the anchor (mcs7 read -4 on
  // the bench at anchor 53). Nothing may treat < 0 as a sentinel.
  std::vector<CalCell> cells;
  for (int i = -40; i <= -8; i += 4) cells.push_back(cell(i, 100));
  for (int i = -4; i <= 60; i += 4) cells.push_back(cell(i, 10));
  const auto w = analyze_rate(cells, CalThresholds{});
  CHECK(w.wall == -8);
  CHECK(w.floor_idx == -40);
  CHECK((w.flags & kCalUndetermined) == 0);
}

// Diversity is not protection against PA compression: both cards see the same
// degraded waveform. Scoring the union inflates delivery and pushes the wall
// UP, which is the overdriven direction.
TEST(uses_best_single_card_not_union) {
  std::vector<CalCell> cells;
  for (int i = 40; i <= 60; ++i) {
    CalCell c;
    c.idx = static_cast<uint8_t>(i);
    c.expected = 100;
    // Card 0 is the good card and walls at 50.
    c.received[0] = static_cast<uint16_t>(i <= 50 ? 100 : 20);
    // Card 1 hears nothing below the wall and 80% above it. A summed/union
    // implementation would therefore score 20+80 = 100% past idx 50 and find
    // no dip at all -- so this fixture fails against a union scorer and
    // passes only for best-single-card. Card 1's total volume (800) stays
    // under card 0's (1300), so card 0 is still the card selected.
    c.received[1] = static_cast<uint16_t>(i <= 50 ? 0 : 80);
    c.rssi_dbm[0] = -70;
    c.rssi_dbm[1] = -85;
    c.have_rssi[0] = c.have_rssi[1] = true;
    cells.push_back(c);
  }
  const auto w = analyze_rate(cells, CalThresholds{});
  CHECK(w.wall == 50);
  CHECK(w.best_card == 0);
  CHECK((w.flags & kCalNoDip) == 0);  // where a union scorer would land
}

TEST(flags_saturation_from_peak_rssi) {
  std::vector<CalCell> cells;
  for (int i = 0; i <= 60; ++i) cells.push_back(cell(i, 100, -40));
  const auto w = analyze_rate(cells, CalThresholds{});
  CHECK((w.flags & kCalSaturated) != 0);
}

TEST(flags_narrow_window) {
  std::vector<CalCell> cells;
  for (int i = 0; i <= 43; ++i) cells.push_back(cell(i, 10));
  for (int i = 44; i <= 46; ++i) cells.push_back(cell(i, 100));  // 3-wide
  for (int i = 47; i <= 60; ++i) cells.push_back(cell(i, 5));
  const auto w = analyze_rate(cells, CalThresholds{});
  CHECK(w.wall == 46);
  CHECK((w.flags & kCalNarrow) != 0);
}

TEST(flags_card_disagreement) {
  std::vector<CalCell> cells;
  for (int i = 40; i <= 60; ++i) {
    CalCell c;
    c.idx = static_cast<uint8_t>(i);
    c.expected = 100;
    c.received[0] = static_cast<uint16_t>(i <= 55 ? 100 : 10);
    c.received[1] = static_cast<uint16_t>(i <= 45 ? 100 : 10);
    c.rssi_dbm[0] = c.rssi_dbm[1] = -70;
    c.have_rssi[0] = c.have_rssi[1] = true;
    cells.push_back(c);
  }
  const auto w = analyze_rate(cells, CalThresholds{});
  CHECK((w.flags & kCalCardDisagree) != 0);
}

TEST(empty_input_is_undetermined) {
  const auto w = analyze_rate({}, CalThresholds{});
  CHECK(w.wall == kNoWall);
  CHECK((w.flags & kCalUndetermined) != 0);
}

MTEST_MAIN
