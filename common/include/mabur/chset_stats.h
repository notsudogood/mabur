#pragma once
// One-line breakdown of devourer's IRtlRadio::LastChannelSetStats, appended
// to the width-switch and full-set hop log lines on both ends. A d11 width
// switch took 68-99 ms on the drone where ~5 ms was estimated; this splits
// the next one into lock wait, the priming read, write batches, single
// round trips and the re-asserts after the set, so the time sink is a
// number instead of a guess. "other" is what none of those cover: on the
// plain path the whole set, on a declined lean switch the lean attempt.
//
// Header-only: needs devourer's src/ on the include path, which every
// translation unit holding an IRtlRadio has.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "ChannelSetStats.h"

namespace mabur {

inline std::string chset_stats_line(const devourer::ChannelSetStats& s) {
  if (s.path == devourer::ChannelSetStats::Path::None) return {};
  const auto ms = [](uint32_t us) { return static_cast<double>(us) / 1000.0; };
  const uint64_t known = uint64_t{s.lock_us} + s.prime_us + s.flush_us + s.sync_us + s.post_us;
  const uint32_t other = s.total_us > known ? static_cast<uint32_t>(s.total_us - known) : 0;
  char b[320];
  std::snprintf(b, sizeof b,
                "%s %.1f ms = lock %.1f + prime %.1f + batches %.1f (%u, %u writes) + single %.1f (%u)"
                " + post %.1f + other %.1f; xfers %u, dack loops %u",
                devourer::to_string(s.path), ms(s.total_us), ms(s.lock_us), ms(s.prime_us),
                ms(s.flush_us), s.flushes, s.batched_writes, ms(s.sync_us), s.sync_ops, ms(s.post_us),
                ms(other), s.xfers, s.dack_loops);
  std::string out = b;
  if (s.lean_skip) {
    out += "; lean declined: ";
    out += s.lean_skip;
  } else if (s.path == devourer::ChannelSetStats::Path::Lean) {
    std::snprintf(b, sizeof b, "; lean %u writes, verify %u regs, %u bad", s.lean_writes, s.verify_regs,
                  s.verify_bad);
    out += b;
  }
  return out;
}

// The lean switch's readback mismatches, one line each: the registers a
// real chip does not read back as the full set left them -- what decides
// whether the lean switch can work on this radio.
inline std::vector<std::string> chset_miss_lines(const devourer::ChannelSetStats& s) {
  std::vector<std::string> out;
  const uint32_t shown = s.verify_bad < s.misses.size() ? s.verify_bad : static_cast<uint32_t>(s.misses.size());
  char b[160];
  for (uint32_t i = 0; i < shown; ++i) {
    const auto& m = s.misses[i];
    std::snprintf(b, sizeof b, "lean verify miss: reg 0x%04x want 0x%08x got 0x%08x (mask 0x%08x)",
                  static_cast<unsigned>(m.addr), static_cast<unsigned>(m.want),
                  static_cast<unsigned>(m.got), static_cast<unsigned>(m.mask));
    out.emplace_back(b);
  }
  if (s.verify_bad > shown) {
    std::snprintf(b, sizeof b, "lean verify miss: %u more not listed", s.verify_bad - shown);
    out.emplace_back(b);
  }
  return out;
}

}  // namespace mabur
