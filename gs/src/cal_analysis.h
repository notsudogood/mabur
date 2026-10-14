#pragma once
// Per-rate wall finding for the TX-power calibration kit. Pure: no I/O, no
// state, no clock -- one row of measured cells in, one wall out.
#include <array>
#include <cstdint>
#include <vector>

#include "mabur/rc_proto.h"

namespace maburgs {

// Sentinel for "this card measured no RSSI in this cell".
constexpr int kRssiNone = -999;

// Indices are RELATIVE to the drone chip's per-channel anchor, in the 7-bit
// signed diff-field range. -1 is a legal index.
constexpr int kNoWall = -128;   // undetermined: caller leaves config untouched
static_assert(kNoWall == mabur::rc::kWallUndetermined, "one sentinel");
constexpr int kRailRel = mabur::rc::kRailRel;  // a no-dip row parks at the field's top

// One (rate, idx) measurement. `expected` comes from the plan the GS itself
// sent, never from anything the drone reports -- so a drone that dies
// mid-phase reads as loss rather than as a shrinking denominator.
struct CalCell {
  int idx = 0;
  uint16_t expected = 0;
  std::array<uint16_t, 2> received{};   // per card, CRC-clean and parseable
  uint16_t corrupt = 0;                 // CRC-bad, diagnostics only
  std::array<int, 2> rssi_dbm{};        // per-card median
  std::array<bool, 2> have_rssi{};
};

enum CalFlag : uint32_t {
  kCalNoDip = 1u << 0,          // never fell below threshold; wall = the rail
  kCalUndetermined = 1u << 1,   // never reached threshold; no wall derivable
  kCalNarrow = 1u << 2,         // usable window too thin to trust
  kCalSaturated = 1u << 3,      // RX front-end compression suspected
  kCalCardDisagree = 1u << 4,   // per-card walls differ materially
  kCalDrift = 1u << 5,          // coarse and fine disagree (set by CalSession)
  kCalNoiseCell = 1u << 6,      // a lone shallow miss was read as noise (see below)
};

struct CalThresholds {
  double deliver_pct = 90.0;    // the >=90% in "first contiguous >=90% run"
  double sat_rssi_dbm = -45.0;  // above this, suspect RX saturation
  int narrow_span = 4;          // wall - floor <= this => kCalNarrow
  int card_disagree = 2;        // per-card wall gap beyond this => flag
  // Noise cell (2026-10-10). At 20 frames a cell, a 90% bar has no noise
  // margin: one clean cell losing 4 frames (80%) ended an mcs2 row that
  // never compresses in two bench runs of four, dropping a rail-parked rate
  // onto a "wall" -- up to ~16 dB low had the cell sat at idx 40
  // (docs/calibration.md). So a cell does not end the run when ALL of:
  //   - it has at most noise_max_frames frames (the 20-frame coarse pass;
  //     the 100-frame fine pass keeps the strict bar),
  //   - it delivered at least noise_pct (a shallow miss: 15+ of 20), and
  //   - the next TWO cells are back at deliver_pct.
  // The last two guard the overdriving direction. Past a real wall the
  // bench saw deep dips (63%, 32%, 4%, 1%) and one island of good delivery
  // 5 indices wide (mcs6, idx 53-57, docs/findings/2026-07-16-txagc-calibration.md): two
  // coarse cells span 8, so the run cannot climb through that island. A
  // forgiven cell is flagged kCalNoiseCell in cal.log either way.
  double noise_pct = 75.0;
  uint16_t noise_max_frames = 50;
};

struct RateWall {
  int wall = kNoWall;       // kNoWall = undetermined; caller leaves config untouched
  int floor_idx = kNoWall;  // lowest index that reached threshold, kNoWall if none
  int best_card = -1;
  uint32_t flags = 0;
};

// `cells` must be sorted ascending by idx. Returns the wall as the END OF THE
// FIRST CONTIGUOUS run at or above `deliver_pct`, scanned upward from the
// sensitivity-floor edge -- never "the last index that happened to be good",
// which the measured comb makes meaningless. A noise cell (CalThresholds)
// does not end the run.
RateWall analyze_rate(const std::vector<CalCell>& cells,
                      const CalThresholds& th);

}  // namespace maburgs
