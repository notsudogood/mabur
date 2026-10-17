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
    // Never dipped: no compression wall inside the swept range. The row is
    // reported UNDETERMINED (kNoWall) rather than parked at the rail, so the
    // drone keeps that rate at its pre-calibration power
    // (docs/next-build-backlog.md item 2). Parking at the rail was the
    // flight 2026-10-12 failure: MCS0-3 went ~9 dB over the chip's stock
    // level, delivered >=90% on the bench where SNR hides distortion, and
    // flew at EVM -10.5 dB against -16.5 dB at stock. Delivery alone cannot
    // tell "no wall" from "a wall delivery cannot see yet".
    out.flags |= kCalNoDip;
    return out;
  }
  out.wall = wall;

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
