// DvrDemux (common/include/mabur/dvr_demux.h): reads DvrMux's recordings
// back for the GS Playback tab (gs18).
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

MTEST_MAIN
