#include "osd_recorder.h"

#include <cstring>

#include "mabur/dvr_name.h"

namespace maburplay {

std::vector<uint8_t> OsdRecorder::header(const std::string& fc) {
  std::vector<uint8_t> h(kHeaderBytes, 0);
  for (size_t i = 0; i < 4 && i < fc.size(); ++i) h[i] = static_cast<uint8_t>(fc[i]);
  return h;
}

void OsdRecorder::append_record(std::vector<uint8_t>& out, uint32_t t_ms,
                                const mabur::MspScreen& s) {
  const size_t at = out.size();
  out.resize(at + kRecordBytes, 0);
  uint8_t* p = out.data() + at;
  p[0] = static_cast<uint8_t>(t_ms);
  p[1] = static_cast<uint8_t>(t_ms >> 8);
  p[2] = static_cast<uint8_t>(t_ms >> 16);
  p[3] = static_cast<uint8_t>(t_ms >> 24);
  p += 4;
  const int rows = s.rows(), cols = s.cols();
  for (int y = 0; y < kRows; ++y)
    for (int x = 0; x < kCols; ++x) {
      const uint16_t g = (y < rows && x < cols) ? s.cell(y, x) : 0;
      *p++ = static_cast<uint8_t>(g);
      *p++ = static_cast<uint8_t>(g >> 8);
    }
}

std::string OsdRecorder::srt_entry(uint64_t n, uint32_t t0_ms, uint32_t t1_ms,
                                   const std::string& text) {
  auto ts = [](uint32_t ms) {
    char b[16];
    std::snprintf(b, sizeof(b), "%02u:%02u:%02u,%03u", ms / 3600000u, (ms / 60000u) % 60u,
                  (ms / 1000u) % 60u, ms % 1000u);
    return std::string(b);
  };
  std::string line;
  for (char c : text) line.push_back(c == '\n' || c == '\r' ? ' ' : c);
  return std::to_string(n) + "\n" + ts(t0_ms) + " --> " + ts(t1_ms) + "\n" + line + "\n\n";
}

std::string OsdRecorder::ass_header() {
  // 1920x1080 script resolution: players scale it to the video, so the
  // place and size hold at 720p too. Colours are &HAABBGGRR, alpha 00 =
  // opaque. Alignment 1 = bottom-left; MarginV 64 puts the line's bottom at
  // y 1016, above walksnail-osd-tool's SRT line (drawn from y 95 % = 1026).
  return "[Script Info]\n"
         "ScriptType: v4.00+\n"
         "PlayResX: 1920\n"
         "PlayResY: 1080\n"
         "WrapStyle: 2\n"
         "ScaledBorderAndShadow: yes\n"
         "\n"
         "[V4+ Styles]\n"
         "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, "
         "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, "
         "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
         "Style: Link,Arial,30,&H00F0F0F0,&H000000FF,&H00000000,&H80000000,0,0,0,0,100,100,0,0,"
         "1,2,1,1,29,29,64,1\n"
         "\n"
         "[Events]\n"
         "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
}

std::string OsdRecorder::ass_event(uint32_t t0_ms, uint32_t t1_ms, const std::string& text) {
  // ASS times are H:MM:SS.cc (centiseconds), truncated like the .srt's ms.
  auto ts = [](uint32_t ms) {
    char b[24];
    const uint32_t cs = ms / 10u;
    std::snprintf(b, sizeof(b), "%u:%02u:%02u.%02u", cs / 360000u, (cs / 6000u) % 60u,
                  (cs / 100u) % 60u, cs % 100u);
    return std::string(b);
  };
  std::string line;
  for (char c : text) {
    if (c == '\n' || c == '\r') line.push_back(' ');
    else if (c == '{') line.push_back('(');
    else if (c == '}') line.push_back(')');
    else if (c == '\\') line.push_back('/');
    else line.push_back(c);
  }
  return "Dialogue: 0," + ts(t0_ms) + "," + ts(t1_ms) + ",Link,,0,0,0,," + line + "\n";
}

std::string OsdRecorder::walksnail_line(int signal, int channel, uint32_t flight_s,
                                        uint32_t delay_ms, double mbps) {
  char b[160];
  std::snprintf(b, sizeof(b),
                "Signal:%d CH:%d FlightTime:%u SBat:0.0V GBat:0.0V Delay:%ums "
                "Bitrate:%.1fMbps Distance:0m",
                signal < 0 ? 0 : signal > 4 ? 4 : signal,
                channel < 0 ? 0 : channel > 255 ? 255 : channel, flight_s, delay_ms,
                mbps < 0 ? 0.0 : mbps);
  return b;
}

int OsdRecorder::signal_bars(std::optional<double> rssi_dbm) {
  if (!rssi_dbm) return 0;
  const double r = *rssi_dbm;
  return r >= -60 ? 4 : r >= -70 ? 3 : r >= -80 ? 2 : r >= -90 ? 1 : 0;
}

std::string OsdRecorder::sidecar_path(const std::string& video_path, const char* ext) {
  const size_t slash = video_path.find_last_of('/');
  const size_t dot = video_path.find_last_of('.');
  const bool has_ext = dot != std::string::npos && (slash == std::string::npos || dot > slash);
  return (has_ext ? video_path.substr(0, dot) : video_path) + ext;
}

void OsdRecorder::arm(const std::string& video_path) {
  stop();
  armed_ = true;
  running_ = false;
  error_ = false;
  osd_path_ = sidecar_path(video_path, ".osd");
  srt_path_ = sidecar_path(video_path, ".srt");
  ass_path_ = sidecar_path(video_path, ".ass");
  records_ = 0;
  sub_n_ = 0;
  last_sub_ms_ = 0;
}

void OsdRecorder::begin(uint64_t now_ms) {
  if (!armed_ || running_) return;
  running_ = true;
  t0_ms_ = now_ms;
  // dvr.folders: the video's writer thread makes the folder at its first
  // frame, which can land after this; making it here too is harmless.
  mabur::make_recording_dir(osd_path_);
  osd_ = std::fopen(osd_path_.c_str(), "wb");
  if (!osd_) {
    error_ = true;
  } else {
    const auto h = header(cfg_.fc);
    osd_buf_.insert(osd_buf_.end(), h.begin(), h.end());
  }
  if (cfg_.srt) {
    srt_ = std::fopen(srt_path_.c_str(), "w");
    ass_ = std::fopen(ass_path_.c_str(), "w");
    if (!srt_ || !ass_) error_ = true;
    if (ass_) ass_buf_ += ass_header();
  }
}

void OsdRecorder::screen(uint64_t now_ms, const mabur::MspScreen& s) {
  if (!running_ || !osd_) return;
  const uint64_t t = now_ms > t0_ms_ ? now_ms - t0_ms_ : 0;
  append_record(osd_buf_, static_cast<uint32_t>(t), s);
  ++records_;
}

void OsdRecorder::subtitle(uint64_t now_ms, const std::string& walksnail,
                           const std::string& detail) {
  if (!running_ || (!srt_ && !ass_)) return;
  const uint64_t t = now_ms > t0_ms_ ? now_ms - t0_ms_ : 0;
  const uint32_t t1 = static_cast<uint32_t>(t);
  if (t1 <= last_sub_ms_ && sub_n_ > 0) return;
  ++sub_n_;
  if (srt_) srt_buf_ += srt_entry(sub_n_, last_sub_ms_, t1, walksnail);
  if (ass_) ass_buf_ += ass_event(last_sub_ms_, t1, detail);
  last_sub_ms_ = t1;
}

void OsdRecorder::flush() {
  if (osd_ && !osd_buf_.empty()) {
    if (std::fwrite(osd_buf_.data(), 1, osd_buf_.size(), osd_) != osd_buf_.size()) error_ = true;
    std::fflush(osd_);
    osd_buf_.clear();
  }
  if (srt_ && !srt_buf_.empty()) {
    if (std::fwrite(srt_buf_.data(), 1, srt_buf_.size(), srt_) != srt_buf_.size()) error_ = true;
    std::fflush(srt_);
    srt_buf_.clear();
  }
  if (ass_ && !ass_buf_.empty()) {
    if (std::fwrite(ass_buf_.data(), 1, ass_buf_.size(), ass_) != ass_buf_.size()) error_ = true;
    std::fflush(ass_);
    ass_buf_.clear();
  }
}

void OsdRecorder::stop() {
  flush();
  if (osd_) std::fclose(osd_);
  if (srt_) std::fclose(srt_);
  if (ass_) std::fclose(ass_);
  osd_ = nullptr;
  srt_ = nullptr;
  ass_ = nullptr;
  osd_buf_.clear();
  srt_buf_.clear();
  ass_buf_.clear();
  armed_ = false;
  running_ = false;
}

}  // namespace maburplay
