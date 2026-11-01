// DvrDemux (common/include/mabur/dvr_demux.h): reads DvrMux's recordings
// back for the GS Playback tab (gs18).
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "mabur/dvr_demux.h"
#include "mabur/dvr_mux.h"
#include "mtest.h"

using mabur::DvrDemux;
using mabur::DvrMux;

namespace {
// An hvcC with one VPS, SPS and PPS array (22 header bytes, then arrays).
std::vector<uint8_t> hvcc() {
  std::vector<uint8_t> h(22, 0x01);
  h.push_back(3);
  const uint8_t nals[3][3] = {{0x40, 0x01, 0xAA}, {0x42, 0x01, 0xBB}, {0x44, 0x01, 0xCC}};
  for (const auto& n : nals) {
    h.push_back(static_cast<uint8_t>(0x80 | (n[0] >> 1)));
    h.push_back(0);
    h.push_back(1);
    h.push_back(0);
    h.push_back(3);
    h.insert(h.end(), n, n + 3);
  }
  return h;
}
const std::vector<uint8_t> kPs = {0, 0, 0, 1, 0x40, 0x01, 0xAA, 0, 0, 0, 1, 0x42, 0x01, 0xBB,
                                  0, 0, 0, 1, 0x44, 0x01, 0xCC};
// An Annex-B AU of two NALs, tagged with its index.
std::vector<uint8_t> au(int i, bool key) {
  std::vector<uint8_t> a = {0, 0, 0, 1, static_cast<uint8_t>(key ? 0x26 : 0x02), 0x01,
                            static_cast<uint8_t>(i), 0, 0, 0, 1, 0x02, 0x01, 0x55,
                            static_cast<uint8_t>(i)};
  return a;
}
std::string tmp(const char* name) {
  return (std::filesystem::temp_directory_path() / name).string();
}
std::string write_rec(const char* name, int n) {
  const std::string p = tmp(name);
  DvrMux m;
  REQUIRE(m.open(p, hvcc(), 1920, 1080, 1000));
  for (int i = 0; i < n; ++i) {
    const auto a = au(i, i == 0);
    m.write_sample(a.data(), a.size(), 1000000u + static_cast<uint32_t>(i) * 16667u, i == 0);
  }
  m.close();
  return p;
}
}  // namespace

// Every sample comes back in order as the Annex-B AU that went in, the
// first (a key) with the hvcC's VPS/SPS/PPS in front; pts follow the
// muxer's timeline; the file's duration is the sum of its samples'.
// Revert (no demux): nothing reads the file.
TEST(round_trips_the_muxers_samples) {
  const std::string p = write_rec("mabur_demux_rt.mp4", 150);   // 2.5 s: three fragments
  DvrDemux d;
  REQUIRE(d.open(p));
  CHECK(d.width() == 1920 && d.height() == 1080);
  CHECK(d.param_sets() == kPs);
  CHECK(d.duration_us() >= 150u * 16000 && d.duration_us() <= 151u * 16667);
  DvrDemux::Sample s;
  int n = 0;
  uint64_t first_pts = 0, last_pts = 0;
  while (d.next(&s)) {
    std::vector<uint8_t> want = au(n, n == 0);
    if (n == 0) want.insert(want.begin(), kPs.begin(), kPs.end());
    CHECK(s.annexb == want);
    CHECK(s.key == (n == 0));
    if (n == 0) first_pts = s.pts_us;
    last_pts = s.pts_us;
    ++n;
  }
  CHECK(n == 150);
  CHECK(last_pts - first_pts == 149u * 16667);
  CHECK(DvrDemux::probe_duration_us(p).value_or(0) == d.duration_us());
  std::remove(p.c_str());
}

// A recording cut mid-fragment (power loss) reads to its last whole
// sample; garbage is refused at open().
TEST(truncated_file_ends_early_and_garbage_is_refused) {
  const std::string p = write_rec("mabur_demux_cut.mp4", 150);
  const auto full = std::filesystem::file_size(p);
  std::filesystem::resize_file(p, full - 7);   // into the last sample
  DvrDemux d;
  REQUIRE(d.open(p));
  DvrDemux::Sample s;
  int n = 0;
  while (d.next(&s)) ++n;
  CHECK(n == 149);
  std::remove(p.c_str());
  const std::string g = tmp("mabur_demux_garbage.mp4");
  {
    std::FILE* f = std::fopen(g.c_str(), "wb");
    std::fputs("not an mp4 at all", f);
    std::fclose(f);
  }
  DvrDemux bad;
  CHECK(!bad.open(g));
  CHECK(!DvrDemux::probe_duration_us(g));
  CHECK(!bad.open(tmp("mabur_demux_missing.mp4")));
  std::remove(g.c_str());
}


// gs19: a recording that spans a drone restart. The drone's pts goes BACK
// (a new encoder session); DvrMux before gs19 wrote that as one ~71-min
// sample (record-0008 listed as 1:10:03). Now: the muxer continues one
// frame on, and the demux also steps over any sample or tfdt jump longer
// than kMaxStepUs (an old file, or a forward jump), so the duration is the
// real one and playback never waits there. Revert (no clamp in either):
// duration ~71 min / ~4000 s and the pts jump.
TEST(restart_inside_a_recording_keeps_one_timeline) {
  const std::string p = tmp("mabur_demux_restart.mp4");
  {
    DvrMux m;
    REQUIRE(m.open(p, hvcc(), 1920, 1080, 1000));
    for (int i = 0; i < 90; ++i) {   // 1.5 s at 60 fps, then a restart near pts 0
      const auto a = au(i, i == 0);
      m.write_sample(a.data(), a.size(), 400000000u + static_cast<uint32_t>(i) * 16667u, i == 0);
    }
    for (int i = 0; i < 90; ++i) {
      const auto a = au(90 + i, i == 0);
      m.write_sample(a.data(), a.size(), 5000u + static_cast<uint32_t>(i) * 16667u, i == 0);
    }
    m.close();
  }
  DvrDemux d;
  REQUIRE(d.open(p));
  CHECK(d.duration_us() <= 181u * 16667 && d.duration_us() >= 178u * 16667);
  DvrDemux::Sample s;
  uint64_t prev = 0, worst = 0;
  int n = 0;
  while (d.next(&s)) {
    if (n > 0) worst = std::max<uint64_t>(worst, s.pts_us - prev);
    prev = s.pts_us;
    ++n;
  }
  CHECK(n == 180);
  CHECK(worst <= 20000);   // never a step longer than about a frame
  std::remove(p.c_str());

  // A forward jump far past kMaxStepUs (the same symptom as an old file's
  // 71-min sample; the muxer keeps forward gaps as real time).
  const std::string q = tmp("mabur_demux_jump.mp4");
  {
    DvrMux m;
    REQUIRE(m.open(q, hvcc(), 1920, 1080, 1000));
    for (int i = 0; i < 120; ++i) {
      const auto a = au(i, i == 0 || i == 60);
      const uint32_t t = 1000000u + static_cast<uint32_t>(i) * 16667u + (i >= 60 ? 4000000000u : 0u);
      m.write_sample(a.data(), a.size(), t, i == 0 || i == 60);
    }
    m.close();
  }
  DvrDemux j;
  REQUIRE(j.open(q));
  CHECK(j.duration_us() <= 121u * 16667);
  prev = 0;
  worst = 0;
  n = 0;
  while (j.next(&s)) {
    if (n > 0) worst = std::max<uint64_t>(worst, s.pts_us - prev);
    prev = s.pts_us;
    ++n;
  }
  CHECK(n == 120);
  CHECK(worst <= 20000);
  // A real gap shorter than kMaxStepUs stays real time (a link outage).
  const std::string g = tmp("mabur_demux_gap.mp4");
  {
    DvrMux m;
    REQUIRE(m.open(g, hvcc(), 1920, 1080, 1000));
    for (int i = 0; i < 120; ++i) {
      const auto a = au(i, i == 0 || i == 60);
      const uint32_t t = 1000000u + static_cast<uint32_t>(i) * 16667u + (i >= 60 ? 3000000u : 0u);
      m.write_sample(a.data(), a.size(), t, i == 0 || i == 60);
    }
    m.close();
  }
  DvrDemux k;
  REQUIRE(k.open(g));
  CHECK(k.duration_us() >= 3000000u + 119u * 16000);
  std::remove(q.c_str());
  std::remove(g.c_str());
}

// gs19: the hvcC's parameter sets go in front of the first sample and of a
// key sample that lacks its own -- never in front of a key that carries
// them (those are the encoder's current ones; the hvcC's could be stale).
TEST(param_sets_only_where_the_sample_has_none) {
  const std::string p = tmp("mabur_demux_ps.mp4");
  const std::vector<uint8_t> own_ps = {0, 0, 0, 1, 0x40, 0x01, 0xDD, 0, 0, 0, 1, 0x42, 0x01, 0xDE,
                                       0, 0, 0, 1, 0x44, 0x01, 0xDF};
  std::vector<uint8_t> key_with_ps = own_ps;
  const auto k1 = au(1, true);
  key_with_ps.insert(key_with_ps.end(), k1.begin(), k1.end());
  const auto k2 = au(2, true);   // a key with no parameter sets of its own
  {
    DvrMux m;
    REQUIRE(m.open(p, hvcc(), 1920, 1080, 1000));
    const auto a0 = au(0, true);
    m.write_sample(a0.data(), a0.size(), 1000000u, true);
    m.write_sample(key_with_ps.data(), key_with_ps.size(), 1016667u, true);
    m.write_sample(k2.data(), k2.size(), 1033334u, true);
    m.close();
  }
  DvrDemux d;
  REQUIRE(d.open(p));
  DvrDemux::Sample s;
  REQUIRE(d.next(&s));
  CHECK(std::vector<uint8_t>(s.annexb.begin(), s.annexb.begin() + kPs.size()) == kPs);
  REQUIRE(d.next(&s));
  CHECK(s.annexb == key_with_ps);   // its own, nothing in front
  REQUIRE(d.next(&s));
  std::vector<uint8_t> want = kPs;
  want.insert(want.end(), k2.begin(), k2.end());
  CHECK(s.annexb == want);
  std::remove(p.c_str());
}
MTEST_MAIN
