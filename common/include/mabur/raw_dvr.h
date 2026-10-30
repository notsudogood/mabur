#ifndef MABUR_RAW_DVR_H_
#define MABUR_RAW_DVR_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "mabur/dvr_mux.h"
#include "mabur/hevc_params.h"

namespace mabur {

// True when the AU carries VPS, SPS and PPS: the live GDR encoder's refresh
// start AND every IDR. Feeds the hvcC; NOT a sync point on its own -- a
// refresh start is a P slice. Recordings begin, and fragments are marked
// sync, only at au_is_irap() AUs.
bool au_has_param_sets(const uint8_t* au, size_t n);

// The raw DVR's recording rules, shared by maburplay's raw mode and the web
// GS: arm, wait for a sync point, write complete AUs through DvrMux, seal.
// Single-threaded.
class RawDvr {
 public:
  enum class State { Off, WaitSync, Recording, Error };
  enum class Err { None, Open, Write };

  // Arms a recording to `path` (sealing any open one first). The file is
  // created on the first IRAP once VPS/SPS/PPS are known; nothing before
  // that. IDRs are rare on the GDR link (seconds apart), so a caller that
  // can should ask the drone for one when it arms.
  // width/height <= 0: take them from the SPS (1920x1080 if it won't parse).
  void start(const std::string& path, int width, int height, int fragment_ms = 1000);

  // Same, into an already-open sink (the web GS's OPFS file), held until the
  // first sync point; dropped unwritten if stop() comes first. null = Err::Open.
  void start(std::unique_ptr<DvrSink> sink, int width, int height, int fragment_ms = 1000);

  // Every AU, in arrival order. Truncated AUs are skipped whole. Ignored
  // unless armed or recording.
  void feed(const uint8_t* au, size_t n, uint32_t pts_us, bool complete);

  // Continues the recording in a new file at `path`: the open file keeps
  // every AU up to the next IRAP, which then begins `path` -- no gap, each
  // file starting at its own sync point. Before the first file opens it
  // just retargets the pending one. Ignored when not recording. A caller
  // that can should ask the drone for an IDR with it (IRAPs come only on
  // request on the GDR link). A second roll() before the switch replaces
  // the pending path. File-path recordings only (not start(sink)).
  void roll(const std::string& path);

  // Flushes and closes an open file; Off afterwards. err() is kept so the
  // caller can report why a recording ended.
  void stop();

  State state() const { return state_; }
  Err err() const { return err_; }
  const std::string& path() const { return path_; }
  bool opened() const { return opened_; }
  uint64_t bytes() const { return opened_ ? mux_.bytes_written() : 0; }
  uint64_t samples() const { return opened_ ? mux_.samples() : 0; }
  uint64_t fragments() const { return opened_ ? mux_.fragments() : 0; }
  // Files opened since start(): 1 once the first opens, +1 per roll()
  // that switched. The caller pairs its own per-file state (the OSD
  // sidecars) with it.
  uint64_t files() const { return files_; }

 private:
  // Opens `path` (or the held sink) at the current params; Error on failure.
  bool open_(const std::string& path);

  HevcParams params_;
  DvrMux mux_;
  State state_ = State::Off;
  Err err_ = Err::None;
  std::string path_;
  std::string next_path_;          // roll(): opens at the next IRAP
  uint64_t files_ = 0;
  std::unique_ptr<DvrSink> sink_;  // start(sink) until the file opens
  bool opened_ = false;
  int width_ = 0, height_ = 0, fragment_ms_ = 1000;
};

}  // namespace mabur

#endif  // MABUR_RAW_DVR_H_
