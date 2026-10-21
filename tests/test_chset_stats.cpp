#include <string>

#include "mabur/chset_stats.h"
#include "mtest.h"

using devourer::ChannelSetStats;

TEST(chset_stats_empty_when_no_set_ran) {
  CHECK(mabur::chset_stats_line(ChannelSetStats{}).empty());
  CHECK(mabur::chset_miss_lines(ChannelSetStats{}).empty());
}

TEST(chset_stats_batched_breakdown_adds_up) {
  ChannelSetStats s;
  s.path = ChannelSetStats::Path::Batched;
  s.total_us = 85000;
  s.lock_us = 1000;
  s.prime_us = 2000;
  s.flush_us = 30000;
  s.flushes = 12;
  s.batched_writes = 190;
  s.sync_us = 40000;
  s.sync_ops = 45;
  s.post_us = 10000;
  s.xfers = 260;
  s.dack_loops = 8;
  const std::string l = mabur::chset_stats_line(s);
  CHECK(l.find("batched 85.0 ms") == 0);
  CHECK(l.find("batches 30.0 (12, 190 writes)") != std::string::npos);
  CHECK(l.find("single 40.0 (45)") != std::string::npos);
  CHECK(l.find("other 2.0") != std::string::npos);   // 85 - 1 - 2 - 30 - 40 - 10
  CHECK(l.find("xfers 260, dack loops 8") != std::string::npos);
  CHECK(l.find("lean") == std::string::npos);
}

TEST(chset_stats_other_never_negative) {
  ChannelSetStats s;
  s.path = ChannelSetStats::Path::Plain;
  s.total_us = 1000;
  s.sync_us = 5000;
  CHECK(mabur::chset_stats_line(s).find("other 0.0") != std::string::npos);
}

TEST(chset_stats_lean_declined_and_misses) {
  ChannelSetStats s;
  s.path = ChannelSetStats::Path::Batched;
  s.total_us = 70000;
  s.lean_skip = "readback differs from the full set";
  s.verify_regs = 12;
  s.verify_bad = 6;
  s.misses[0] = {0x1aec, 0xb8, 0xb6, 0xff};
  const std::string l = mabur::chset_stats_line(s);
  CHECK(l.find("; lean declined: readback differs from the full set") != std::string::npos);
  const auto m = mabur::chset_miss_lines(s);
  CHECK(m.size() == 5);   // four listed + "2 more"
  CHECK(m[0] == "lean verify miss: reg 0x1aec want 0x000000b8 got 0x000000b6 (mask 0x000000ff)");
  CHECK(m[4] == "lean verify miss: 2 more not listed");
}

TEST(chset_stats_lean_ran) {
  ChannelSetStats s;
  s.path = ChannelSetStats::Path::Lean;
  s.total_us = 3000;
  s.lean_writes = 16;
  s.verify_regs = 12;
  const std::string l = mabur::chset_stats_line(s);
  CHECK(l.find("lean 3.0 ms") == 0);
  CHECK(l.find("; lean 16 writes, verify 12 regs, 0 bad") != std::string::npos);
  CHECK(mabur::chset_miss_lines(s).empty());
}

MTEST_MAIN
