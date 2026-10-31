// OsdRecorder (gs/player/src/osd_recorder.h): Walksnail-format .osd and an
// .srt link summary next to a raw GS recording.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

#include "mtest.h"
#include "osd_recorder.h"

using maburplay::OsdRecorder;

namespace {
mabur::MspMessage draw(int row, int col, uint8_t attrs, const std::string& text) {
  mabur::MspMessage m;
  m.cmd = mabur::MSP_CMD_DISPLAYPORT;
  m.payload = {mabur::MSP_DP_DRAW_STRING, static_cast<uint8_t>(row), static_cast<uint8_t>(col), attrs};
  m.payload.insert(m.payload.end(), text.begin(), text.end());
  return m;
}
uint16_t cell_at(const std::vector<uint8_t>& rec, int x, int y) {
  const size_t o = 4 + (static_cast<size_t>(y) * OsdRecorder::kCols + static_cast<size_t>(x)) * 2;
  return static_cast<uint16_t>(rec[o] | (rec[o + 1] << 8));
}
std::vector<uint8_t> slurp(const std::string& p) {
  std::vector<uint8_t> v;
  if (std::FILE* f = std::fopen(p.c_str(), "rb")) {
    int c;
    while ((c = std::fgetc(f)) != EOF) v.push_back(static_cast<uint8_t>(c));
    std::fclose(f);
  }
  return v;
}
}  // namespace

// What walksnail-osd-tool reads: 40-byte header, firmware tag first.
TEST(header_is_40_bytes_with_the_firmware_tag) {
  auto h = OsdRecorder::header("BTFL");
  REQUIRE(h.size() == 40);
  CHECK(h[0] == 'B' && h[1] == 'T' && h[2] == 'F' && h[3] == 'L');
  for (size_t i = 4; i < h.size(); ++i) CHECK(h[i] == 0);
}

// 2124-byte record: u32 LE ms, then 53 x 20 u16 LE, row by row; the
// DisplayPort font page rides in the glyph's high byte; a smaller canvas
// (the 50x18 default) pads with blanks.
TEST(record_layout_matches_the_walksnail_frame) {
  mabur::MspScreen s;
  s.apply(draw(0, 0, 0, "A"));
  s.apply(draw(2, 49, 1, "Z"));  // page 1, last column of the 50-wide canvas
  std::vector<uint8_t> out;
  OsdRecorder::append_record(out, 0x01020304u, s);
  REQUIRE(out.size() == OsdRecorder::kRecordBytes);
  CHECK(out.size() == 2124);
  CHECK(out[0] == 0x04 && out[1] == 0x03 && out[2] == 0x02 && out[3] == 0x01);
  CHECK(cell_at(out, 0, 0) == 'A');
  CHECK(cell_at(out, 49, 2) == (0x100 | 'Z'));
  CHECK(cell_at(out, 50, 2) == 0);   // past the canvas: blank
  CHECK(cell_at(out, 0, 19) == 0);   // past the canvas rows: blank
}

TEST(srt_entries_and_sidecar_names) {
  CHECK(OsdRecorder::srt_entry(3, 1000, 2500, "MCS4 a\nb") ==
        "3\n00:00:01,000 --> 00:00:02,500\nMCS4 a b\n\n");
  CHECK(OsdRecorder::sidecar_path("/media/dvr/record-0012.mp4", ".osd") ==
        "/media/dvr/record-0012.osd");
  CHECK(OsdRecorder::sidecar_path("/media/dvr.d/record", ".srt") == "/media/dvr.d/record.srt");
}

// End to end: nothing on disk until the video's first frame (begin), the
// buffered screens land on flush, and stop seals both files.
TEST(files_appear_next_to_the_video_and_hold_every_screen) {
  char tmpl[] = "/tmp/osdrecXXXXXX";
  REQUIRE(mkdtemp(tmpl) != nullptr);
  const std::string dir = tmpl;
  const std::string video = dir + "/record-0007.mp4";
  OsdRecorder r({"INAV", true});
  r.arm(video);
  mabur::MspScreen s;
  s.apply(draw(1, 1, 0, "HI"));
  r.screen(1000, s);                       // not begun: ignored
  CHECK(r.records() == 0);
  CHECK(slurp(dir + "/record-0007.osd").empty());
  r.begin(5000);
  REQUIRE(r.running() && !r.error());
  r.screen(5040, s);
  r.screen(5080, s);
  r.subtitle(6000, OsdRecorder::walksnail_line(3, 132, 1, 38, 24.5), "MCS4/40MHz lat 38/54ms");
  r.flush();
  auto osd = slurp(dir + "/record-0007.osd");
  REQUIRE(osd.size() == 40 + 2 * 2124);
  CHECK(osd[0] == 'I' && osd[1] == 'N' && osd[2] == 'A' && osd[3] == 'V');
  std::vector<uint8_t> rec2(osd.begin() + 40 + 2124, osd.end());
  CHECK(rec2[0] == 80 && rec2[1] == 0);    // 5080 - 5000 ms
  CHECK(cell_at(rec2, 1, 1) == 'H' && cell_at(rec2, 2, 1) == 'I');
  r.screen(5120, s);
  r.stop();
  CHECK(!r.armed() && !r.running());
  CHECK(slurp(dir + "/record-0007.osd").size() == 40 + 3 * 2124);
  auto srt = slurp(dir + "/record-0007.srt");
  const std::string srt_s(srt.begin(), srt.end());
  CHECK(srt_s ==
        "1\n00:00:00,000 --> 00:00:01,000\nSignal:3 CH:132 FlightTime:1 SBat:0.0V GBat:0.0V "
        "Delay:38ms Bitrate:24.5Mbps Distance:0m\n\n");
  // gs17: the detail rides a .ass (header + one Dialogue per cue), never a
  // second .srt (walksnail-osd-tool would pick that one up instead).
  CHECK(slurp(dir + "/record-0007.link.srt").empty());
  auto ass = slurp(dir + "/record-0007.ass");
  const std::string ass_s(ass.begin(), ass.end());
  CHECK(ass_s == OsdRecorder::ass_header() +
                     "Dialogue: 0,0:00:00.00,0:00:01.00,Link,,0,0,0,,MCS4/40MHz lat 38/54ms\n");
  std::remove((dir + "/record-0007.osd").c_str());
  std::remove((dir + "/record-0007.srt").c_str());
  std::remove((dir + "/record-0007.ass").c_str());
  rmdir(dir.c_str());
}

// dvr.folders (gs18): the sidecars go in the recording's own folder, and
// begin() makes it when the video's writer thread has not yet. Revert (no
// make_recording_dir in begin): fopen fails, error() and no .osd.
TEST(sidecars_land_in_the_recordings_own_folder) {
  char tmpl[] = "/tmp/osdrecXXXXXX";
  REQUIRE(mkdtemp(tmpl) != nullptr);
  const std::string dir = tmpl;
  const std::string folder = dir + "/record-0008";
  OsdRecorder r({"BTFL", true});
  r.arm(folder + "/record-0008.mp4");
  r.begin(1000);
  REQUIRE(r.running());
  CHECK(!r.error());
  mabur::MspScreen s;
  s.apply(draw(1, 1, 0, "HI"));
  r.screen(1040, s);
  r.stop();
  CHECK(slurp(folder + "/record-0008.osd").size() == 40 + 2124);
  for (const char* ext : {".osd", ".srt", ".ass"}) std::remove((folder + "/record-0008" + ext).c_str());
  rmdir(folder.c_str());
  rmdir(dir.c_str());
}

// walksnail-osd-tool parses exactly this shape (backend/src/srt/frame.rs:
// "Signal:{u8} CH:{u8} FlightTime:{u32} SBat:{f32}V GBat:{f32}V
// Delay:{u32}ms Bitrate:{f32}Mbps Distance:{u32}m") and skips anything else.
TEST(walksnail_line_matches_the_tools_format_and_clamps) {
  CHECK(OsdRecorder::walksnail_line(4, 8, 0, 32, 25.0) ==
        "Signal:4 CH:8 FlightTime:0 SBat:0.0V GBat:0.0V Delay:32ms Bitrate:25.0Mbps Distance:0m");
  CHECK(OsdRecorder::walksnail_line(9, 300, 7, 0, -1.0) ==
        "Signal:4 CH:255 FlightTime:7 SBat:0.0V GBat:0.0V Delay:0ms Bitrate:0.0Mbps Distance:0m");
  CHECK(OsdRecorder::signal_bars(std::nullopt) == 0);
  CHECK(OsdRecorder::signal_bars(-55.0) == 4);
  CHECK(OsdRecorder::signal_bars(-60.0) == 4);
  CHECK(OsdRecorder::signal_bars(-75.0) == 2);
  CHECK(OsdRecorder::signal_bars(-90.0) == 1);
  CHECK(OsdRecorder::signal_bars(-91.0) == 0);
}

MTEST_MAIN

// gs17: the .ass detail file. Times are H:MM:SS.cc; text that would read as
// an ASS override tag or break the line is made plain; the style sits
// bottom-left above walksnail-osd-tool's SRT line. Revert (back to a
// .link.srt): files_appear_next_to_the_video_and_hold_every_screen fails first.
TEST(ass_event_times_and_escapes) {
  CHECK(OsdRecorder::ass_event(0, 1000, "CH132 MCS4/80MHz") ==
        "Dialogue: 0,0:00:00.00,0:00:01.00,Link,,0,0,0,,CH132 MCS4/80MHz\n");
  CHECK(OsdRecorder::ass_event(3723456, 3724999, "a{b}c\\d\ne") ==
        "Dialogue: 0,1:02:03.45,1:02:04.99,Link,,0,0,0,,a(b)c/d e\n");
  const std::string h = OsdRecorder::ass_header();
  CHECK(h.find("PlayResY: 1080") != std::string::npos);
  CHECK(h.find("Style: Link,Arial,30,") != std::string::npos);
  CHECK(h.find(",1,29,29,64,1\n") != std::string::npos);   // alignment 1, margins, encoding
  CHECK(h.rfind("Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n") ==
        h.size() - std::string("Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n").size());
}
