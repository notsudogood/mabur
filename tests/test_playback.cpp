// The GS Playback tab's pure half (gs/player/src/playback.h).
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "mabur/dvr_mux.h"
#include "mtest.h"
#include "playback.h"

using namespace maburplay;

namespace {
void write_rec(const std::string& path, int n) {
  mabur::DvrMux m;
  std::vector<uint8_t> hvcc(22, 0x01);
  hvcc.push_back(0);   // no arrays: enough for a duration probe
  REQUIRE(m.open(path, hvcc, 1920, 1080, 1000));
  for (int i = 0; i < n; ++i) {
    const uint8_t au[] = {0, 0, 0, 1, static_cast<uint8_t>(i == 0 ? 0x26 : 0x02), 0x01, 0x00};
    m.write_sample(au, sizeof(au), static_cast<uint32_t>(i) * 16667u, i == 0);
  }
  m.close();
}
}  // namespace

// Newest (highest index) first, capped, legacy names and unreadable files
// left out, durations from the file. Revert (sorted by name ascending): the
// first entry is record-0002.
TEST(list_recordings_newest_first_readable_only) {
  const auto dir = std::filesystem::temp_directory_path() / "mabur_pb_list";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  write_rec((dir / "record-0002.mp4").string(), 60);
  write_rec((dir / "record-0010.mp4").string(), 120);
  write_rec((dir / "record-0007.mp4").string(), 30);
  write_rec((dir / "record_20171001.mp4").string(), 30);      // legacy: not listed
  {
    std::FILE* f = std::fopen((dir / "record-0011.mp4").string().c_str(), "wb");
    std::fputs("cut short", f);                               // unreadable: not offered
    std::fclose(f);
  }
  const auto all = list_recordings(dir.string());
  REQUIRE(all.size() == 3);
  CHECK(all[0].name == "record-0010" && all[1].name == "record-0007" && all[2].name == "record-0002");
  CHECK(all[0].index == 10 && all[0].duration_us >= 119u * 16667 && all[0].duration_us <= 121u * 16667);
  CHECK(list_recordings(dir.string(), 2).size() == 2);
  CHECK(list_recordings((dir / "nope").string()).empty());
  std::filesystem::remove_all(dir);
}

// The file's timeline from the start, lead_us early; a pause stops it and
// the resume shifts it by the paused time.
TEST(pacer_follows_the_timeline_and_pauses) {
  PlaybackPacer p(30000);
  p.start(1000000, 5000000);
  CHECK(p.due(1000000, 5000000));                  // the first sample at once
  CHECK(!p.due(1000000, 5100000));                 // 100 ms ahead: not yet
  CHECK(p.due(1070000, 5100000));                  // 30 ms lead
  CHECK(p.position_us(1500000) == 500000);
  p.pause(1500000);
  CHECK(!p.due(9000000, 5100000) && p.paused());
  CHECK(p.position_us(9000000) == 500000);
  p.resume(3500000);                               // paused 2 s
  CHECK(p.position_us(3500000) == 500000);
  CHECK(!p.due(3500000, 5600000) && p.due(3571000, 5600000));
}

TEST(playback_line_and_clock) {
  CHECK(clock_text(0) == "0:00" && clock_text(83000000) == "1:23" && clock_text(3723000000ull) == "1:02:03");
  CHECK(playback_line("record-0012", 83000000, 296000000, false) ==
        "Playing record-0012  1:23 / 4:56  - centre: pause, left: back to live");
  CHECK(playback_line("record-0012", 400000000, 296000000, true) ==
        "Paused record-0012  4:56 / 4:56  - centre: play, left: back to live");
}

MTEST_MAIN
