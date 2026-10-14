#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "au_ring.h"
#include "mabur/hevc_ps.h"

namespace maburgs {

using ByteSink = std::function<void(const uint8_t*, size_t)>;

// One AU's fragment payloads, by fragment index. A decoded fragment is moved
// in whole and its FRAG header skipped in place (`off`), so storing one costs
// no copy and no allocation: the GS core thread used to spend a map node and
// a fresh vector per fragment here (profile 2026-10-13, 35 Mb/s replay).
class ChunkTable {
 public:
  bool has(uint16_t i) const { return i < c_.size() && c_[i].present; }
  const uint8_t* data(uint16_t i) const { return c_[i].buf.data() + c_[i].off; }
  size_t size(uint16_t i) const { return c_[i].buf.size() - c_[i].off; }
  bool empty() const { return n_ == 0; }
  size_t count() const { return n_; }  // fragments present
  // Sizing hint from the fragment header's count. Capped: the table grows
  // on demand anyway, and a corrupt count must not buy a large block.
  void reserve(uint16_t count) { c_.reserve(std::min<size_t>(count, kReserveCap)); }
  // Stores `buf` (bytes [off, end) are the payload), replacing any earlier
  // copy of fragment i. `off` must not exceed buf.size().
  void put(uint16_t i, std::vector<uint8_t>&& buf, size_t off) {
    if (i >= c_.size()) c_.resize(static_cast<size_t>(i) + 1);
    Chunk& c = c_[i];
    if (!c.present) ++n_;
    c.buf = std::move(buf);
    c.off = off;
    c.present = true;
  }
  void put(uint16_t i, const uint8_t* p, size_t n) { put(i, std::vector<uint8_t>(p, p + n), 0); }
  void erase(uint16_t i) {
    if (!has(i)) return;
    c_[i] = Chunk{};
    --n_;
  }

 private:
  static constexpr size_t kReserveCap = 1024;
  struct Chunk {
    std::vector<uint8_t> buf;
    size_t off = 0;
    bool present = false;
  };
  std::vector<Chunk> c_;
  size_t n_ = 0;
};

// One access unit of a row-sliced H.265 picture, rebuilt from FrameStream's
// fragment chunks.
//
// Fragment i covers AU bytes [i*F - hdr_len, ...) where F = chunk 0's size
// (every fragment but the last is F bytes; chunk 0 also carries the
// FrameHdr). drain() streams whole NAL units of the contiguous prefix as
// they complete -- always a byte prefix of the AU. finish() emits the rest:
//  - complete AU: the remaining bytes (byte-identical output);
//  - damaged AU, salvageable: every remaining slice in order, each either
//    its original bytes (arrived complete) or a make_skip_slice() fill;
//  - damaged AU, not salvageable: the rest of the contiguous prefix,
//    exactly what FrameStream emitted before slice salvage existed.
// A slice is complete only if its start code and every byte up to the next
// start code (or the known AU end) lie in ONE run of consecutive fragments:
// a start code split by a hole is never trusted.
class SliceAssembler {
 public:
  SliceAssembler(const mabur::hevc::Sps& sps, const mabur::hevc::Pps& pps, uint8_t slice_rows,
                 uint16_t count, size_t hdr_len);

  void drain(const ChunkTable& chunks, const ByteSink& out);
  // raw_out (optional): where the passthrough remainder goes -- the rest of
  // the contiguous prefix, which may end mid-NAL. Unset = out. Everything
  // else finish() and drain() emit ends on a NAL boundary (ring v4's
  // writer contract).
  void finish(const ChunkTable& chunks, const ByteSink& out, const ByteSink& raw_out = {});

  const SliceSalvage& result() const { return result_; }
  uint8_t slices() const { return n_; }

 private:
  struct Sc { size_t pos, len; };
  void extend_prefix(const ChunkTable& chunks);
  void scan_prefix();
  SliceFallback plan(const ChunkTable& chunks, std::vector<std::vector<uint8_t>>* pieces,
                     SliceSalvage* res);

  mabur::hevc::Sps sps_;
  mabur::hevc::Pps pps_;
  uint8_t slice_rows_;
  uint16_t count_;
  size_t hdr_len_;
  uint8_t n_ = 0;
  size_t frag_ = 0;               // chunk 0's size (= every non-last fragment's)
  uint16_t prefix_chunks_ = 0;    // chunks [0, prefix_chunks_) are in buf_
  std::vector<uint8_t> buf_;      // the contiguous prefix, FrameHdr stripped
  std::vector<Sc> sc_;            // start codes found in buf_
  size_t scanned_ = 0;            // buf_ positions < scanned_ fully examined
  size_t emitted_ = 0;            // bytes of buf_ already handed out
  SliceSalvage result_;
};

}  // namespace maburgs
