#include "mabur/raw_dvr.h"

namespace mabur {

bool au_has_param_sets(const uint8_t* au, size_t n) {
  bool v = false, s = false, p = false;
  for (const NalView& nal : split_nals(au, n)) {
    v |= nal.type == 32;
    s |= nal.type == 33;
    p |= nal.type == 34;
  }
  return v && s && p;
}

void RawDvr::start(const std::string& path, int width, int height, int fragment_ms) {
  stop();
  path_ = path;
  width_ = width;
  height_ = height;
  fragment_ms_ = fragment_ms;
  opened_ = false;
  next_path_.clear();
  files_ = 0;
  err_ = Err::None;
  state_ = State::WaitSync;
}

void RawDvr::roll(const std::string& path) {
  if (state_ == State::WaitSync && !sink_) path_ = path;
  else if (state_ == State::Recording) next_path_ = path;
}

bool RawDvr::open_(const std::string& path) {
  int w = width_, h = height_;
  if ((w <= 0 || h <= 0) && !params_.sps_dimensions(&w, &h)) {
    w = 1920;
    h = 1080;
  }
  const bool ok = sink_ ? mux_.open(std::move(sink_), params_.hvcc(), w, h, fragment_ms_)
                        : mux_.open(path, params_.hvcc(), w, h, fragment_ms_);
  if (!ok || !mux_.ok()) {
    mux_.close();
    state_ = State::Error;
    err_ = Err::Open;
    return false;
  }
  path_ = path;
  opened_ = true;
  ++files_;
  state_ = State::Recording;
  return true;
}

void RawDvr::start(std::unique_ptr<DvrSink> sink, int width, int height, int fragment_ms) {
  start(std::string(), width, height, fragment_ms);
  if (!sink) {
    state_ = State::Error;
    err_ = Err::Open;
    return;
  }
  sink_ = std::move(sink);
}

void RawDvr::feed(const uint8_t* au, size_t n, uint32_t pts_us, bool complete) {
  if (state_ != State::WaitSync && state_ != State::Recording) return;
  if (!complete) return;  // DVR records complete AUs only; DvrMux cuts at the next key
  if (au_has_param_sets(au, n)) params_.feed(au, n);  // sticky across recordings, never reset
  // Sync = a real IRAP. The live encoder is GDR: VPS/SPS/PPS also ride on
  // every TRAIL_R refresh start, which references frames before it.
  // Treating those as sync opened files on a P slice and flagged P slices
  // as random-access points -- Apple's decoder (QuickTime, VLC on macOS)
  // refused the whole file (seen on a web-GS recording).
  const bool key = au_is_irap(au, n);
  if (state_ == State::WaitSync) {
    // A file must BEGIN at a sync point. params_ is sticky, so on a second
    // recording complete() is already true -- the `key` clause is what keeps
    // the file from opening on an AU whose references are not in it.
    if (!key || !params_.complete()) return;
    if (!open_(path_)) return;
  } else if (key && !next_path_.empty() && params_.complete()) {
    // roll(): seal this file and begin the next one on this sync point.
    mux_.close();
    std::string next;
    next.swap(next_path_);
    if (!open_(next)) return;
  }
  mux_.write_sample(au, n, pts_us, key);
  if (!mux_.ok()) {  // card/quota full: keep what was written, stop feeding
    mux_.close();
    state_ = State::Error;
    err_ = Err::Write;
  }
}

void RawDvr::stop() {
  if (state_ == State::Recording) mux_.close();
  next_path_.clear();
  sink_.reset();  // an unopened sink: dropped unwritten
  if (state_ != State::Off) state_ = State::Off;
}

}  // namespace mabur
