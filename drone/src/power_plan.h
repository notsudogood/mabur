#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "mabur/rc_proto.h"

namespace mabur {

// Relative-wall power plan: every wall is a signed index RELATIVE to the
// chip's own per-channel TXAGC anchor (the efuse reference devourer programs
// into 0x18e8 on each channel set). diff[r] = rel[r] - m parks rate r at
// wall-minus-margin on whatever channel the chip is on, because the chip
// adds the right anchor itself.
// m = round(margin_db * 4) converts dB to the chip's 0.25 dB index steps.
//
// anchor_idx: the reference index read back at bring-up ON THE BOOT
// CHANNEL, used only to keep reference + diff <= 127 (the vendor driver
// guarantees that in software; hardware behaviour beyond it is undefined).
// <= 0 means unknown: no cap. On this unit (anchors 39-57) the cap never
// binds -- it lands at +70..+88, above kRelMax (+63); it exists for a
// blank-efuse card (devourer fallback 75), which caps at +52 and touches
// only no-dip rows.
//
// It is therefore a BOOT-CHANNEL APPROXIMATION, and deliberately so: the
// per-channel anchor the diffs ride on is NOT this number. devourer
// re-derives it from the efuse on every channel change, driven by the
// ReApplyTxPower() call maburd makes right after each FastRetune
// (drone/src/main.cpp, RealActuator::retune_now_). A stale value here can
// only mis-size a guard that does not bind; it cannot mis-place a wall.
//
// This plan is the WHOLE of mabur's power policy: programmed once at
// bring-up (and re-programmed live by a calibration apply), global offset
// zeroed beside it, nothing moves power afterwards.
struct PowerPlan {
  int8_t cck;
  int8_t legacy;
  int8_t mcs[8];
};

// The 8822E's own per-rate shape, the one power_mode = "none" flies:
// devourer's phy_reg_pg walk programs diff = pg[rate] - pg[HT MCS7], which
// is the same on 2.4 and 5 GHz for HT MCS0-7 and 6M
// (devourer hal/phydm/rtl8822e/Hal8822e_PhyTables.c, read 2026-10-17).
// MCS0 sits 6 dB over MCS7. VHT1SS MCS0-7 carry the same shape in stock;
// note that a custom table (power_mode = "offset") leaves every VHT rate at
// the anchor, i.e. MCS7's level (devourer src/TxPower.h).
constexpr std::array<int, 8> kStockMcsDiff8822e = {24, 24, 20, 16, 12, 8, 4, 0};
constexpr int kStockLegacyDiff8822e = 24;  // 6M

// A calibration row left undetermined -- no dip up to the top of the field,
// or never reaching the pass bar -- keeps its PRE-calibration power
// (docs/next-build-backlog.md item 2). From power_mode = "none" that is the
// stock shape: wall = stock + m, so diff = wall - m lands exactly on it. From
// "offset" the config's own wall is already what flies, so the row stays
// undetermined and apply_calibration keeps it.
inline void fill_undetermined_with_stock(std::array<int, 8>& walls, int& legacy_wall,
                                         double margin_db) {
  const int m = static_cast<int>(std::lround(margin_db * 4.0));
  for (size_t r = 0; r < 8; ++r)
    if (walls[r] == rc::kWallUndetermined) walls[r] = kStockMcsDiff8822e[r] + m;
  if (legacy_wall == rc::kWallUndetermined) legacy_wall = kStockLegacyDiff8822e + m;
}

inline PowerPlan make_power_plan(const std::array<int, 8>& walls_rel,
                                  int legacy_wall_rel, double margin_db,
                                  int anchor_idx) {
  const int m = static_cast<int>(std::lround(margin_db * 4.0));
  const int hi =
      anchor_idx > 0 ? std::min(rc::kRelMax, 127 - anchor_idx) : rc::kRelMax;
  auto clamp = [&](int v) {
    return static_cast<int8_t>(std::clamp(v, rc::kRelMin, hi));
  };
  PowerPlan p{};
  for (int r = 0; r < 8; ++r)
    p.mcs[r] = clamp(walls_rel[static_cast<size_t>(r)] - m);
  p.legacy = clamp(legacy_wall_rel - m);
  p.cck = p.legacy;  // cck rides the legacy wall
  return p;
}

}  // namespace mabur
