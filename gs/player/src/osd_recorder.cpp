#include "osd_recorder.h"

#include <cstring>

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
  records_ = 0;
  sub_n_ = 0;
  last_sub_ms_ = 0;
}

void OsdRecorder::begin(uint64_t now_ms) {
  if (!armed_ || running_) return;
  running_ = true;
  t0_ms_ = now_ms;
  osd_ = std::fopen(osd_path_.c_str(), "wb");
  if (!osd_) {
    error_ = true;
  } else {
    const auto h = header(cfg_.fc);
    osd_buf_.insert(osd_buf_.end(), h.begin(), h.end());
  }
  if (cfg_.srt) {
    srt_ = std::fopen(srt_path_.c_str(), "w");
    if (!srt_) error_ = true;
  }
}

void OsdRecorder::screen(uint64_t now_ms, const mabur::MspScreen& s) {
  if (!running_ || !osd_) return;
  const uint64_t t = now_ms > t0_ms_ ? now_ms - t0_ms_ : 0;
  append_record(osd_buf_, static_cast<uint32_t>(t), s);
  ++records_;
}

void OsdRecorder::subtitle(uint64_t now_ms, const std::string& text) {
  if (!running_ || !srt_) return;
  const uint64_t t = now_ms > t0_ms_ ? now_ms - t0_ms_ : 0;
  const uint32_t t1 = static_cast<uint32_t>(t);
  if (t1 <= last_sub_ms_ && sub_n_ > 0) return;
  srt_buf_ += srt_entry(++sub_n_, last_sub_ms_, t1, text);
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
}

void OsdRecorder::stop() {
  flush();
  if (osd_) std::fclose(osd_);
  if (srt_) std::fclose(srt_);
  osd_ = nullptr;
  srt_ = nullptr;
  osd_buf_.clear();
  srt_buf_.clear();
  armed_ = false;
  running_ = false;
}

}  // namespace maburplay
