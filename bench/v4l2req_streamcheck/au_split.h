#ifndef MABUR_BENCH_STREAMCHECK_AU_SPLIT_H_
#define MABUR_BENCH_STREAMCHECK_AU_SPLIT_H_

#include <cstddef>
#include <cstdint>
#include <vector>

// Cuts an Annex-B HEVC capture (ausniff --dump-annexb: AUs back to back) into
// access units, and an access unit into the packets maburplay's StreamFeeder
// hands a streaming decoder. Pure; host-tested.
namespace streamcheck {

struct Range {
  size_t begin, end;
};

struct Nal {
  size_t begin;  // first byte of its start code (incl. a 4-byte code's leading 0)
  int type;
  bool first_slice;  // VCL only: first_slice_segment_in_pic_flag
};

inline std::vector<Nal> scan_nals(const uint8_t* p, size_t n) {
  std::vector<Nal> out;
  for (size_t i = 0; i + 3 < n; ++i) {
    if (p[i] != 0 || p[i + 1] != 0 || p[i + 2] != 1) continue;
    Nal v;
    v.begin = (i > 0 && p[i - 1] == 0) ? i - 1 : i;
    v.type = (p[i + 3] >> 1) & 0x3f;
    v.first_slice = v.type < 32 && i + 5 < n && (p[i + 5] & 0x80);
    out.push_back(v);
    i += 2;
  }
  return out;
}

inline bool is_vcl(int type) { return type < 32; }
// NALs that can only precede the first slice of an access unit.
inline bool opens_au(int type) { return (type >= 32 && type <= 35) || type == 39; }

inline std::vector<Range> split_aus(const uint8_t* p, size_t n) {
  std::vector<Range> out;
  const std::vector<Nal> nals = scan_nals(p, n);
  size_t au_begin = nals.empty() ? n : nals[0].begin;
  bool has_vcl = false;
  size_t pending = SIZE_MAX;  // begin of a run of opening NALs after a VCL
  for (const Nal& v : nals) {
    if (opens_au(v.type) && has_vcl) {
      if (pending == SIZE_MAX) pending = v.begin;
      continue;
    }
    if (is_vcl(v.type) && v.first_slice && has_vcl) {
      const size_t cut = pending != SIZE_MAX ? pending : v.begin;
      out.push_back({au_begin, cut});
      au_begin = cut;
      has_vcl = false;
    }
    pending = SIZE_MAX;
    if (is_vcl(v.type)) has_vcl = true;
  }
  if (au_begin < n) out.push_back({au_begin, n});
  return out;
}

inline std::vector<Range> stream_pieces(const uint8_t* au, size_t n) {
  const std::vector<Nal> nals = scan_nals(au, n);
  std::vector<size_t> vcl_end;  // end of each VCL NAL = begin of the next NAL
  for (size_t k = 0; k < nals.size(); ++k) {
    if (!is_vcl(nals[k].type)) continue;
    vcl_end.push_back(k + 1 < nals.size() ? nals[k + 1].begin : n);
  }
  std::vector<Range> out;
  if (vcl_end.empty()) return {{0, n}};
  size_t from = 0;
  for (size_t k = 0; k + 1 < vcl_end.size(); ++k) {
    out.push_back({from, vcl_end[k]});
    from = vcl_end[k];
  }
  out.push_back({from, n});
  return out;
}

}  // namespace streamcheck

#endif  // MABUR_BENCH_STREAMCHECK_AU_SPLIT_H_
