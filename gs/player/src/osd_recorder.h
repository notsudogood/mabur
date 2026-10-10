#ifndef MABUR_PLAYER_OSD_RECORDER_H_
#define MABUR_PLAYER_OSD_RECORDER_H_

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "mabur/msp_dp.h"

namespace maburplay {

// OSD sidecars for a raw GS recording (2026-10-10): the flight controller's
// OSD as a Walksnail-format `.osd` file, and a once-a-second link summary as
// an `.srt`, both next to the video with the same name (record-0012.mp4,
// record-0012.osd, record-0012.srt). A raw recording keeps the video exactly
// as received -- no re-encode, so none of "burned" mode's decode and display
// cost (flight 2026-10-10: burned recording put a spike >= 70 ms into 58% of
// seconds, against 5% without) -- and tools made for Walksnail goggle
// recordings (walksnail-osd-tool) draw the OSD back on afterwards.
//
// .osd layout (what walksnail-osd-tool reads): a 40-byte header whose first
// four bytes name the flight-controller firmware ("BTFL", "INAV" or "ARDU";
// the rest zero), then one 2124-byte record per OSD screen: a u32 LE time in
// ms since the video's first frame, and a 53 x 20 grid of u16 LE glyph
// indices, row by row. The glyph index is the DisplayPort cell as drawn
// (font page in the high byte), so the tool's font for that firmware
// renders it. A grid of another size is cropped or padded with blanks (0).
//
// Writing: records and subtitles are buffered in memory and written only by
// flush(), which the player calls from its once-a-second idle slot -- an SD
// card write can stall for a long time and must never sit between a frame
// and its refresh. stop() flushes and closes. Nothing is written before
// begin(): time zero is the moment the video file got its first frame.
class OsdRecorder {
 public:
  static constexpr int kCols = 53;
  static constexpr int kRows = 20;
  static constexpr size_t kHeaderBytes = 40;
  static constexpr size_t kRecordBytes = 4 + kCols * kRows * 2;  // 2124

  struct Cfg {
    std::string fc = "BTFL";  // header firmware tag: BTFL, INAV or ARDU
    bool srt = true;          // also write the .srt link summary
  };

  explicit OsdRecorder(Cfg cfg) : cfg_(std::move(cfg)) {}
  ~OsdRecorder() { stop(); }
  OsdRecorder(const OsdRecorder&) = delete;
  OsdRecorder& operator=(const OsdRecorder&) = delete;

  // Arms for the video at `video_path` (".../record-0012.mp4"). Seals any
  // previous recording first.
  void arm(const std::string& video_path);
  // The video file's first frame was written at now_ms: time zero. Opens
  // the sidecar files. No-op unless armed and not yet begun.
  void begin(uint64_t now_ms);
  bool armed() const { return armed_; }
  bool running() const { return running_; }

  // One OSD screen (call on every complete DisplayPort frame).
  void screen(uint64_t now_ms, const mabur::MspScreen& s);
  // One subtitle covering the time since the previous one (call ~1 Hz).
  void subtitle(uint64_t now_ms, const std::string& text);
  // Writes what is buffered. The player's 1 Hz idle slot.
  void flush();
  // Flushes and closes; disarmed afterwards.
  void stop();

  const std::string& osd_path() const { return osd_path_; }
  const std::string& srt_path() const { return srt_path_; }
  uint64_t records() const { return records_; }
  bool error() const { return error_; }

  // Pure helpers (tests).
  static std::vector<uint8_t> header(const std::string& fc);
  static void append_record(std::vector<uint8_t>& out, uint32_t t_ms, const mabur::MspScreen& s);
  static std::string srt_entry(uint64_t n, uint32_t t0_ms, uint32_t t1_ms, const std::string& text);
  static std::string sidecar_path(const std::string& video_path, const char* ext);

 private:
  Cfg cfg_;
  bool armed_ = false;
  bool running_ = false;
  bool error_ = false;
  std::string osd_path_, srt_path_;
  std::FILE* osd_ = nullptr;
  std::FILE* srt_ = nullptr;
  uint64_t t0_ms_ = 0;
  uint32_t last_sub_ms_ = 0;
  uint64_t sub_n_ = 0;
  uint64_t records_ = 0;
  std::vector<uint8_t> osd_buf_;
  std::string srt_buf_;
};

}  // namespace maburplay

#endif  // MABUR_PLAYER_OSD_RECORDER_H_
