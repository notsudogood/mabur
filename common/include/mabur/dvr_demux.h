#ifndef MABUR_DVR_DEMUX_H_
#define MABUR_DVR_DEMUX_H_

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace mabur {

// Reads a DvrMux recording (dvr_mux.h: fragmented MP4, one hvc1 track,
// timescale 1'000'000) back as Annex-B access units, for the GS menu's
// Playback tab (gs18). It knows DvrMux's layout and nothing wider: the
// init segment's hvcC carries the parameter sets; each fragment is a moof
// (tfdt v1, one trun with duration + size + flags per sample, the data
// offset from the moof's start) followed by its mdat.
//
// A recording cut by a power loss ends in a partial fragment: reading stops
// at the last whole sample, never an error. Not a general MP4 reader: a
// file another muxer wrote (other boxes, other trun flags) is refused at
// open() or ends early.
class DvrDemux {
 public:
  struct Sample {
    // Start-code NAL units (00 00 00 01). A key sample, and the first one
    // read, carries the hvcC's VPS/SPS/PPS in front of its own NALs, so a
    // decoder can start (or restart) there.
    std::vector<uint8_t> annexb;
    uint64_t pts_us = 0;   // the file's own timeline (tfdt + durations)
    uint32_t dur_us = 0;
    bool key = false;
  };

  DvrDemux() = default;
  ~DvrDemux();
  DvrDemux(const DvrDemux&) = delete;
  DvrDemux& operator=(const DvrDemux&) = delete;

  // false: unreadable, or no moov/hvcC where DvrMux puts them.
  bool open(const std::string& path);
  void close();
  // The next sample in decode order; false at the end of the file.
  bool next(Sample* s);

  int width() const { return width_; }
  int height() const { return height_; }
  // The whole file's duration (every fragment's sample durations) and the
  // first sample's pts, measured at open() without reading any sample.
  uint64_t duration_us() const { return duration_us_; }
  uint64_t start_pts_us() const { return start_pts_us_; }
  const std::vector<uint8_t>& param_sets() const { return param_sets_; }

  // duration_us() of a file without keeping it open; nullopt when open()
  // would fail.
  static std::optional<uint64_t> probe_duration_us(const std::string& path);

 private:
  struct Entry {
    uint64_t offset = 0;   // absolute file offset of the sample's bytes
    uint32_t size = 0;
    uint32_t dur_us = 0;
    bool key = false;
  };
  // Reads the fragment whose moof starts at `pos` into frag_; false when
  // there is none (end of file, a truncated box, anything else at `pos`).
  bool load_fragment_(uint64_t pos);

  std::FILE* f_ = nullptr;
  uint64_t file_size_ = 0;
  uint64_t first_moof_ = 0;
  uint64_t next_box_ = 0;          // where the next fragment's moof should start
  std::vector<Entry> frag_;
  size_t frag_i_ = 0;
  uint64_t frag_pts_ = 0;          // the next sample's pts
  bool first_ = true;
  int width_ = 0, height_ = 0;
  uint64_t duration_us_ = 0;
  uint64_t start_pts_us_ = 0;
  std::vector<uint8_t> param_sets_;
};

}  // namespace mabur

#endif  // MABUR_DVR_DEMUX_H_
