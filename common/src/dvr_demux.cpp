#include "mabur/dvr_demux.h"

#include <cstring>

namespace mabur {

namespace {

uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
uint64_t be64(const uint8_t* p) {
  return (static_cast<uint64_t>(be32(p)) << 32) | be32(p + 4);
}
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

bool seek(std::FILE* f, uint64_t pos) {
  return fseeko(f, static_cast<off_t>(pos), SEEK_SET) == 0;
}

// A box header at `pos`: size (32-bit, or 64-bit largesize) and type.
struct BoxHead {
  uint64_t size = 0;
  uint32_t hdr = 8;
  char type[5] = {0, 0, 0, 0, 0};
};
bool read_head(std::FILE* f, uint64_t pos, uint64_t file_size, BoxHead* h) {
  uint8_t b[16];
  if (pos + 8 > file_size || !seek(f, pos) || std::fread(b, 1, 8, f) != 8) return false;
  h->size = be32(b);
  std::memcpy(h->type, b + 4, 4);
  h->hdr = 8;
  if (h->size == 1) {
    if (std::fread(b + 8, 1, 8, f) != 8) return false;
    h->size = be64(b + 8);
    h->hdr = 16;
  } else if (h->size == 0) {
    h->size = file_size - pos;   // to the end of the file
  }
  return h->size >= h->hdr;
}

// The first `type` box inside [p, end) at any depth reachable through the
// container boxes DvrMux nests (a plain scan: our init segment holds no
// other box whose payload could spell a nested fourcc by chance).
const uint8_t* find_fourcc(const uint8_t* p, const uint8_t* end, const char* type) {
  for (const uint8_t* q = p + 4; q + 4 <= end; ++q)
    if (std::memcmp(q, type, 4) == 0) return q - 4;   // the box's size field
  return nullptr;
}

// hvcC -> Annex-B VPS/SPS/PPS: 22 header bytes, numOfArrays, then per array
// one type byte, numNalus (16), per NAL its length (16) and bytes.
std::vector<uint8_t> hvcc_param_sets(const uint8_t* p, size_t n) {
  std::vector<uint8_t> out;
  if (n < 23) return out;
  size_t i = 22;
  const unsigned arrays = p[i++];
  for (unsigned a = 0; a < arrays && i + 3 <= n; ++a) {
    ++i;   // array_completeness / NAL type
    const unsigned nalus = be16(p + i);
    i += 2;
    for (unsigned k = 0; k < nalus && i + 2 <= n; ++k) {
      const size_t len = be16(p + i);
      i += 2;
      if (i + len > n) return out;
      static const uint8_t kStart[4] = {0, 0, 0, 1};
      out.insert(out.end(), kStart, kStart + 4);
      out.insert(out.end(), p + i, p + i + len);
      i += len;
    }
  }
  return out;
}

}  // namespace

DvrDemux::~DvrDemux() { close(); }

void DvrDemux::close() {
  if (f_) std::fclose(f_);
  f_ = nullptr;
  frag_.clear();
  frag_i_ = 0;
}

bool DvrDemux::open(const std::string& path) {
  close();
  f_ = std::fopen(path.c_str(), "rb");
  if (!f_) return false;
  if (fseeko(f_, 0, SEEK_END) != 0) return false;
  file_size_ = static_cast<uint64_t>(ftello(f_));
  // Top level: ftyp, moov, then fragments.
  uint64_t pos = 0;
  bool have_moov = false;
  BoxHead h;
  while (read_head(f_, pos, file_size_, &h)) {
    if (std::strcmp(h.type, "moov") == 0) {
      if (h.size > (16u << 20) || pos + h.size > file_size_) return false;
      std::vector<uint8_t> moov(static_cast<size_t>(h.size));
      if (!seek(f_, pos) || std::fread(moov.data(), 1, moov.size(), f_) != moov.size()) return false;
      const uint8_t* b = moov.data();
      const uint8_t* e = b + moov.size();
      const uint8_t* hvc1 = find_fourcc(b + 8, e, "hvc1");
      const uint8_t* hvcc = find_fourcc(b + 8, e, "hvcC");
      if (!hvcc) return false;
      const uint32_t hs = be32(hvcc);
      if (hs < 8 || hvcc + hs > e) return false;
      param_sets_ = hvcc_param_sets(hvcc + 8, hs - 8);
      // VisualSampleEntry: 8 header + 24 bytes, then width, height.
      if (hvc1 && hvc1 + 8 + 28 <= e) {
        width_ = be16(hvc1 + 8 + 24);
        height_ = be16(hvc1 + 8 + 26);
      }
      have_moov = true;
    } else if (std::strcmp(h.type, "moof") == 0) {
      break;
    }
    pos += h.size;
  }
  if (!have_moov) return false;
  first_moof_ = pos;
  // Duration and start: every fragment's trun, without reading samples.
  duration_us_ = 0;
  have_pts_ = false;
  last_dur_us_ = 16667;
  bool first_frag = true;
  for (uint64_t p = first_moof_; load_fragment_(p);) {
    if (first_frag) start_pts_us_ = frag_pts_;
    first_frag = false;
    for (const Entry& e : frag_) duration_us_ += e.dur_us;
    frag_pts_ += [&] { uint64_t t = 0; for (const Entry& e : frag_) t += e.dur_us; return t; }();
    p = next_box_;
  }
  // Back to the first fragment for reading.
  frag_.clear();
  frag_i_ = 0;
  next_box_ = first_moof_;
  first_ = true;
  have_pts_ = false;
  last_dur_us_ = 16667;
  return true;
}

bool DvrDemux::load_fragment_(uint64_t pos) {
  frag_.clear();
  frag_i_ = 0;
  BoxHead moof;
  if (!read_head(f_, pos, file_size_, &moof) || std::strcmp(moof.type, "moof") != 0) return false;
  if (moof.size > (1u << 20) || pos + moof.size > file_size_) return false;
  std::vector<uint8_t> b(static_cast<size_t>(moof.size));
  if (!seek(f_, pos) || std::fread(b.data(), 1, b.size(), f_) != b.size()) return false;
  const uint8_t* s = b.data();
  const uint8_t* e = s + b.size();
  const uint8_t* tfdt = find_fourcc(s + 8, e, "tfdt");
  const uint8_t* trun = find_fourcc(s + 8, e, "trun");
  if (!tfdt || !trun || tfdt + 20 > e || trun + 20 > e) return false;
  const uint64_t tfdt_pts = tfdt[8] == 1 ? be64(tfdt + 12) : be32(tfdt + 12);
  // Continue from where the last fragment ended unless this one starts
  // there or a little later (a real gap in the recording); a jump back or
  // past kMaxStepUs is a restart the recording spans (see the header).
  if (!have_pts_ || (tfdt_pts >= frag_pts_ && tfdt_pts - frag_pts_ <= kMaxStepUs))
    frag_pts_ = tfdt_pts;
  have_pts_ = true;
  const uint32_t flags = be32(trun + 8) & 0xFFFFFF;
  if (flags != 0x000701) return false;   // DvrMux's: data offset, duration, size, flags
  const uint32_t count = be32(trun + 12);
  const uint32_t data_off = be32(trun + 16);
  const uint8_t* q = trun + 20;
  if (count > 100000 || q + static_cast<size_t>(count) * 12 > e) return false;
  uint64_t off = pos + data_off;
  for (uint32_t i = 0; i < count; ++i, q += 12) {
    Entry en;
    en.dur_us = be32(q);
    if (en.dur_us == 0 || en.dur_us > kMaxStepUs) en.dur_us = last_dur_us_;
    else last_dur_us_ = en.dur_us;
    en.size = be32(q + 4);
    en.key = q[8] == 0x02;   // DvrMux: sample_depends_on = 2 (IRAP)
    en.offset = off;
    off += en.size;
    // A truncated mdat (power loss): keep only the samples wholly present.
    if (off > file_size_) break;
    frag_.push_back(en);
  }
  // The mdat after the moof: the next fragment starts at its end.
  BoxHead mdat;
  if (read_head(f_, pos + moof.size, file_size_, &mdat) && std::strcmp(mdat.type, "mdat") == 0)
    next_box_ = pos + moof.size + mdat.size;
  else
    next_box_ = file_size_;
  return !frag_.empty();
}

bool DvrDemux::next(Sample* s) {
  if (!f_ || !s) return false;
  if (frag_i_ >= frag_.size()) {
    if (next_box_ >= file_size_ || !load_fragment_(next_box_)) return false;
  }
  const Entry& en = frag_[frag_i_++];
  std::vector<uint8_t> raw(en.size);
  if (!seek(f_, en.offset) || std::fread(raw.data(), 1, raw.size(), f_) != raw.size()) return false;
  s->annexb.clear();
  s->key = en.key;
  s->pts_us = frag_pts_;
  s->dur_us = en.dur_us;
  frag_pts_ += en.dur_us;
  // Length-prefixed NALs -> start codes, noting which parameter sets the
  // sample carries itself.
  std::vector<uint8_t> nals;
  bool vps = false, sps = false, pps = false;
  size_t i = 0;
  while (i + 4 <= raw.size()) {
    const uint32_t len = be32(raw.data() + i);
    i += 4;
    if (len == 0 || i + len > raw.size()) break;
    const unsigned type = (raw[i] >> 1) & 0x3F;
    vps |= type == 32;
    sps |= type == 33;
    pps |= type == 34;
    static const uint8_t kStart[4] = {0, 0, 0, 1};
    nals.insert(nals.end(), kStart, kStart + 4);
    nals.insert(nals.end(), raw.data() + i, raw.data() + i + len);
    i += len;
  }
  // The init segment's VPS/SPS/PPS go in front of the first sample, and of
  // a key sample only when it lacks its own: a key that carries them has
  // the encoder's CURRENT ones, and the hvcC's (from the recording's start)
  // ahead of them would be stale wherever the encoder re-sent changed ones.
  if (first_ || (en.key && !(vps && sps && pps))) s->annexb = param_sets_;
  first_ = false;
  s->annexb.insert(s->annexb.end(), nals.begin(), nals.end());
  return true;
}

std::optional<uint64_t> DvrDemux::probe_duration_us(const std::string& path) {
  DvrDemux d;
  if (!d.open(path)) return std::nullopt;
  return d.duration_us();
}

}  // namespace mabur
