#pragma once
// Builds the three calibration sweep plans. Pure: no state, no I/O.
#include <array>
#include <cstdint>

#include "cal_analysis.h"
#include "mabur/rc_proto.h"

namespace maburgs {

// The coarse grid is [kCoarseLo, kCoarseHi] step kCoarseStep: -41, -37,
// ..., 59, 63. kCoarseLo is -41 and not the rounder -40 SO THAT THE LAST
// CELL LANDS EXACTLY ON kRailRel (63), the top of the diff field. "Never
// dipped" (kCalNoDip, gs/src/cal_analysis.cpp) then means no wall anywhere
// the drone could be set to; such a row keeps its pre-calibration power
// (2026-10-17; before that it parked at the rail). Move kCoarseLo and
// kCoarseHi together or the top of the range goes unmeasured again.
constexpr int kCoarseLo = -41;
constexpr int kCoarseHi = 63;
constexpr uint8_t kCoarseStep = 4;
static_assert((kCoarseHi - kCoarseLo) % kCoarseStep == 0,
              "the coarse grid must land on kCoarseHi exactly");
static_assert(kCoarseHi == mabur::rc::kRailRel,
              "no-dip must mean no wall up to the top of the field");
constexpr int kFineHalfWidth = 8;
// 50, not 20: the 99% pass bar (CalThresholds) needs every frame of a coarse
// cell, and at 20 frames one lost frame was already 95%. Coarse runs ~43 s.
constexpr uint16_t kCoarseFrames = 50;
constexpr uint16_t kFineFrames = 100;
constexpr uint16_t kVerifyFrames = 100;
constexpr uint16_t kSettleMs = 100;   // MEASURE THIS ON HARDWARE -- see docs
constexpr uint16_t kGapUs = 2000;

mabur::rc::CalCmd make_coarse_plan(uint32_t nonce);

// Only rows whose coarse pass found a real dip get refined. A kCalNoDip row
// keeps its pre-calibration power and a kCalUndetermined row has nothing to
// refine.
mabur::rc::CalCmd make_fine_plan(uint32_t nonce,
                                 const std::array<RateWall, 8>& coarse);

// One cell per rate at its parked index (wall - margin). kNoWall or a park
// index out of [-64,63] means that rate was undetermined and is skipped.
mabur::rc::CalCmd make_verify_plan(uint32_t nonce,
                                   const std::array<int, 8>& park_idx);

// How long the drone will transmit for this plan. The GS uses it to know when
// the listen window opens, so it must count cells exactly as CalSweep walks
// them.
uint32_t plan_duration_ms(const mabur::rc::CalCmd& c);

}  // namespace maburgs
