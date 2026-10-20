#pragma once
// 5 GHz HT40 pairing: which side of a 20 MHz primary its 40 MHz secondary
// sits on, per the standard grid (36+40, 44+48, ... 132+136, 140+144,
// 149+153, 157+161, and on past 165 the same way: 165+169, 173+177, ...).
// Returned in devourer's SelectedChannel.ChannelOffset convention: 1 = HT40+
// (primary is the lower 20), 2 = HT40- (primary is the upper 20), 0 = no
// 40 MHz pair (off the 5 GHz grid, or the pair would pass kMaxChannel). A fixed
// "always above" choice lands off-grid (136+140 centres on 138, straddling
// the real 132+136 and 140+144), where every other 40 MHz network and
// devourer's spur table (keyed on standard centres) disagree with us
// (docs/findings/2026-09-23-bw40-sweep.md).
#include <cstdint>

namespace mabur {

// The highest channel number devourer's 8822E synthesizer tunes
// (5000 + 5 x 233 = 6165 MHz, devourer/src/AdapterCaps.h). Above 177 TX
// power and the per-channel constants are copied from 177 (uncalibrated)
// and the front end is built for <= 5925 MHz (ch 185); the operator owns
// whether and where to fly there.
constexpr uint8_t kMaxChannel = 233;

// On the 5 GHz 20 MHz grid the pairs and blocks are built on: 36..144 and
// 149 upward, every 4th channel.
constexpr bool on_5g_grid(uint8_t ch) {
  if (ch >= 36 && ch <= 144) return (ch - 36) % 4 == 0;
  if (ch >= 149 && ch <= kMaxChannel) return (ch - 149) % 4 == 0;
  return false;
}

constexpr uint8_t ht40_offset(uint8_t ch) {
  if (!on_5g_grid(ch)) return 0;
  const uint8_t base = ch <= 144 ? 36 : 149;
  if (static_cast<uint8_t>(ch + 4) > kMaxChannel && ((ch - base) / 4) % 2 == 0) return 0;
  return ((ch - base) / 4) % 2 == 0 ? 1 : 2;
}

static_assert(ht40_offset(36) == 1 && ht40_offset(40) == 2, "36+40");
static_assert(ht40_offset(132) == 1 && ht40_offset(136) == 2, "132+136");
static_assert(ht40_offset(140) == 1 && ht40_offset(144) == 2, "140+144");
static_assert(ht40_offset(116) == 1 && ht40_offset(120) == 2, "116+120");
static_assert(ht40_offset(149) == 1 && ht40_offset(153) == 2, "149+153");
static_assert(ht40_offset(157) == 1 && ht40_offset(161) == 2, "157+161");
static_assert(ht40_offset(165) == 1 && ht40_offset(169) == 2, "165+169 (UNII-4)");
static_assert(ht40_offset(173) == 1 && ht40_offset(177) == 2, "173+177 (UNII-4)");
static_assert(ht40_offset(181) == 1 && ht40_offset(185) == 2, "181+185 (past UNII-4)");
static_assert(ht40_offset(229) == 1 && ht40_offset(233) == 2, "229+233, the last pair under the tune limit");
static_assert(ht40_offset(146) == 0 && ht40_offset(6) == 0 && ht40_offset(38) == 0, "off grid");

// The other 20 MHz half of ch's standard pair (136 -> 132, 144 -> 140,
// 40 -> 36); 0 when ch has no pair. The boot scout at 40 MHz dwells on
// both halves of every pair (docs/bw40.md).
constexpr uint8_t ht40_pair_other(uint8_t ch) {
  const uint8_t off = ht40_offset(ch);
  if (off == 1) return static_cast<uint8_t>(ch + 4);
  if (off == 2) return static_cast<uint8_t>(ch - 4);
  return 0;
}

static_assert(ht40_pair_other(136) == 132 && ht40_pair_other(132) == 136, "132+136");
static_assert(ht40_pair_other(149) == 153 && ht40_pair_other(165) == 169, "149+153 / 165+169");
static_assert(ht40_pair_other(233) == 229 && ht40_pair_other(146) == 0, "229+233 / off grid");

// The lowest 20 MHz channel of the standard 80 MHz block ch sits in
// (36-48, 52-64, 100-112, 116-128, 132-144, 149-161, 165-177, and on past
// 177 in blocks of 16), or 0 when ch is in none (off the grid, 2.4 GHz, or
// the block would pass kMaxChannel). Any 20 of the block may be the
// primary: devourer tunes the block centre and carries the primary's slot
// in the baseband (its RadioManagementJaguar3::vht80_block_lowest, the
// same arithmetic).
constexpr uint8_t vht80_block_base(uint8_t ch) {
  if (!on_5g_grid(ch)) return 0;
  uint8_t base = 0;
  if (ch >= 36 && ch <= 64) base = static_cast<uint8_t>(36 + 16 * ((ch - 36) / 16));
  else if (ch >= 100 && ch <= 144) base = static_cast<uint8_t>(100 + 16 * ((ch - 100) / 16));
  else if (ch >= 149) base = static_cast<uint8_t>(149 + 16 * ((ch - 149) / 16));
  if (base == 0 || base + 12 > kMaxChannel) return 0;
  return base;
}

// True when ch can be tuned at 80 MHz: it is one of the four 20s of a
// standard 80 MHz block.
constexpr bool vht80_ok(uint8_t ch) { return vht80_block_base(ch) != 0; }

static_assert(vht80_block_base(36) == 36 && vht80_block_base(48) == 36 && vht80_block_base(52) == 52, "36/52");
static_assert(vht80_block_base(132) == 132 && vht80_block_base(144) == 132 && vht80_block_base(140) == 132, "132");
static_assert(vht80_block_base(161) == 149 && vht80_block_base(165) == 165 && vht80_block_base(177) == 165, "149/165");
static_assert(vht80_block_base(68) == 0 && vht80_block_base(96) == 0 && vht80_block_base(6) == 0, "no block");
static_assert(vht80_block_base(229) == 0 && vht80_block_base(213) == 213, "229 block passes the tune limit");

// The central channel devourer tunes for ch at width_mhz -- what
// IRtlRadio::ReadTunedCentral() reads back when the radio is where the host
// thinks it is: the block centre at 80, the pair centre at 40, ch itself at
// 20 and below. 0 = ch cannot be tuned at that width.
constexpr uint8_t tuned_central(uint8_t ch, uint8_t width_mhz) {
  if (width_mhz == 80) {
    const uint8_t base = vht80_block_base(ch);
    return base ? static_cast<uint8_t>(base + 6) : 0;
  }
  if (width_mhz == 40) {
    const uint8_t off = ht40_offset(ch);
    return off == 1 ? static_cast<uint8_t>(ch + 2) : off == 2 ? static_cast<uint8_t>(ch - 2) : 0;
  }
  return ch;
}

static_assert(tuned_central(36, 80) == 42 && tuned_central(132, 80) == 138 && tuned_central(149, 80) == 155, "80");
static_assert(tuned_central(40, 80) == 42 && tuned_central(144, 80) == 138 && tuned_central(173, 80) == 171, "80, any primary");
static_assert(tuned_central(68, 80) == 0, "80, no block");
static_assert(tuned_central(36, 40) == 38 && tuned_central(40, 40) == 38 && tuned_central(165, 40) == 167, "40");
static_assert(tuned_central(36, 20) == 36 && tuned_central(165, 20) == 165, "20");

}  // namespace mabur
