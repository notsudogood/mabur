#include "cal_analysis.h"

#include <algorithm>
#include <cstdlib>

namespace maburgs {
namespace {

double delivery_pct(const CalCell& c, int card) {
  if (c.expected == 0) return 0.0;
  return 100.0 * static_cast<double>(c.received[static_cast<size_t>(card)]) /
         static_cast<double>(c.expected);
}

// End of the first contiguous run at or above threshold, scanning up from the
// first cell that reaches it; a noise cell (CalThresholds) does not end it.
// floor_idx and wall are both kNoWall when the row never reaches threshold.
struct Run {
  int floor_idx = kNoWall;
  int wall = kNoWall;
  bool noise_cell = false;
};

Run first_run(const std::vector<CalCell>& cells, int card, const CalThresholds& th) {
  const auto good = [&](size_t k) {
    return delivery_pct(cells[k], card) >= th.deliver_pct;
  };
  const auto noise = [&](size_t k) {
    return k + 2 < cells.size() && cells[k].expected <= th.noise_max_frames &&
           delivery_pct(cells[k], card) >= th.noise_pct && good(k + 1) && good(k + 2);
  };
  Run r;
  size_t i = 0;
  while (i < cells.size() && !good(i)) ++i;
  if (i == cells.size()) return r;
  r.floor_idx = static_cast<int>(cells[i].idx);
  r.wall = r.floor_idx;
  for (; i < cells.size(); ++i) {
    if (good(i)) {
      r.wall = static_cast<int>(cells[i].idx);
    } else if (noise(i)) {
      r.noise_cell = true;  // the next cell is good, so the wall moves past it
    } else {
      break;
    }
  }
  // i == cells.size() here means the run never ended -- the caller detects
  // that as no-dip by comparing wall against the last measured index.
  return r;
}

}  // namespace

RateWall analyze_rate(const std::vector<CalCell>& cells,
                      const CalThresholds& th) {
  RateWall out;
  if (cells.empty()) {
    out.flags |= kCalUndetermined;
    return out;
  }

  // Best single card, never the union: diversity is protection against
  // fading, not against PA compression -- both cards receive the same
  // degraded waveform. Scoring the union would inflate delivery and push the
  // wall up, which is the overdriven direction.
  int best = 0;
  long best_total = -1;
  for (int card = 0; card < 2; ++card) {
    long total = 0;
    for (const auto& c : cells) total += c.received[static_cast<size_t>(card)];
    if (total > best_total) {
      best_total = total;
      best = card;
    }
  }
  out.best_card = best;

  // Computed here, before any wall-finding can return early: saturation is a
  // property of the RSSI this row was measured at, not of whether a wall came
  // out of it. A row that ends undetermined most needs this flag -- it is
  // often the reason there is nothing to find.
  int peak = kRssiNone;
  for (const auto& c : cells)
    if (c.have_rssi[static_cast<size_t>(best)])
      peak = std::max(peak, c.rssi_dbm[static_cast<size_t>(best)]);
  if (peak != kRssiNone && static_cast<double>(peak) > th.sat_rssi_dbm)
    out.flags |= kCalSaturated;

  const Run run = first_run(cells, best, th);
  const int floor_idx = run.floor_idx;
  const int wall = run.wall;
  out.floor_idx = floor_idx;
  if (run.noise_cell) out.flags |= kCalNoiseCell;

  if (floor_idx == kNoWall) {
    // Never reached threshold anywhere: there is no first contiguous run to
    // end, so no wall exists. Do NOT invent one.
    out.flags |= kCalUndetermined;
    return out;
  }

  const int last_idx = static_cast<int>(cells.back().idx);
  if (wall >= last_idx) {
    // Never dipped: this rate has no compression wall inside the swept
    // range, so there is nothing for delivery to find and nothing to back
    // off from. Park it at the rail instead -- kRailRel, the top of the
    // relative range every unit's diff field can express, regardless of
    // where this unit's own anchor happens to sit.
    //
    // The rail IS the top cell of the coarse sweep -- kCoarseLo is -41
    // precisely so the grid lands on kRailRel exactly (gs/src/cal_plan.h)
    // -- so the parked index is an index this very run measured at >=90%
    // delivery, not one past the end of the sweep. That is a
    // stronger guarantee than the RSSI "saturation knee" this replaced,
    // which was an inference from a curve -- and an unreproducible one: on
    // one unit it read 72, 56, 84 and 56 across four runs, and once put
    // mcs0 and mcs1, the same PA and the same modulation class, 3 dB apart.
    out.flags |= kCalNoDip;
    out.wall = kRailRel;
  } else {
    out.wall = wall;
  }

  if (out.wall - floor_idx <= th.narrow_span) out.flags |= kCalNarrow;


  // Card disagreement means an antenna or card problem, not a PA: compression
  // is a property of the transmitter and both cards should see it together.
  bool other_has_data = false;
  const int other = best == 0 ? 1 : 0;
  for (const auto& c : cells)
    if (c.received[static_cast<size_t>(other)] > 0) other_has_data = true;
  if (other_has_data) {
    const Run o = first_run(cells, other, th);
    if (o.floor_idx != kNoWall && std::abs(o.wall - wall) > th.card_disagree)
      out.flags |= kCalCardDisagree;
  }

  return out;
}

}  // namespace maburgs
