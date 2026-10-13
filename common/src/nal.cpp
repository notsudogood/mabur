#include "mabur/nal.h"

#include <cstring>

namespace mabur {

NalInfo parse_hevc_nal(const uint8_t* nal, size_t len) {
  NalInfo n;
  if (len < 2) return n;  // malformed -> treat as base layer
  n.type = (nal[0] >> 1) & 0x3F;
  int tid = static_cast<int>(nal[1] & 0x07) - 1;
  n.tid = tid < 0 ? 0 : static_cast<uint8_t>(tid);
  n.critical = (n.type >= 16 && n.type <= 23) || (n.type >= 32 && n.type <= 34);
  return n;
}

namespace {
// Next i >= from with annexb[i..i+2] == 00 00 01 and i + 4 < len (room for
// the start code plus a two-byte NAL header), or len if none. Same
// candidates, in the same order, as testing every i -- but memchr skips
// straight to each zero byte, and a zero followed by a nonzero byte rules
// out the next position too. The byte-per-iteration walk this replaced was
// most of the drone hot thread's non-FEC work at 35 Mb/s (two full-frame
// passes per AU; host dry-run profile 2026-10-13).
size_t next_start_code(const uint8_t* b, size_t len, size_t from) {
  if (len < 5) return len;
  const size_t last = len - 5;  // highest i with i + 4 < len
  size_t i = from;
  while (i <= last) {
    const void* z = std::memchr(b + i, 0, last - i + 1);
    if (!z) return len;
    i = static_cast<size_t>(static_cast<const uint8_t*>(z) - b);
    if (b[i + 1] != 0x00) { i += 2; continue; }
    if (b[i + 2] == 0x01) return i;
    ++i;
  }
  return len;
}
}  // namespace

int classify_frame(const uint8_t* annexb, size_t len) {
  if (!annexb || len < 5) return 0;
  int sid = -1;
  for (size_t i = next_start_code(annexb, len, 0); i < len;
       i = next_start_code(annexb, len, i + 3)) {
    const uint8_t* nal = annexb + i + 3;
    size_t nal_max = len - (i + 3);
    NalInfo n = parse_hevc_nal(nal, nal_max);
    if (n.critical) return 0;
    // next_start_code only returns i with i + 4 < len, so nal_max >= 2 here
    // and parse_hevc_nal never sees a truncated header -- a start code with
    // <2 following bytes is simply never returned, and a frame too short to
    // contain one is rejected by the len < 5 guard above. TRAIL_N (type 0)
    // therefore always means a real enhance slice, never a
    // default-constructed NalInfo. The scan resumes at the NAL header byte
    // (i + 3), as the byte loop it replaced did.
    if (sid < 0 && n.type < 16)
      sid = n.type == 0 ? 1 : 0;
  }
  return sid < 0 ? 0 : sid;
}

bool frame_is_trail_n(const uint8_t* annexb, size_t len) {
  if (!annexb || len < 5) return false;
  for (size_t i = next_start_code(annexb, len, 0); i < len;
       i = next_start_code(annexb, len, i + 3)) {
    NalInfo n = parse_hevc_nal(annexb + i + 3, len - (i + 3));
    if (n.critical) return false;
    if (n.type < 16) return n.type == 0;
  }
  return false;
}

AuNalCount count_au_nals(const uint8_t* annexb, size_t len) {
  AuNalCount c;
  if (!annexb || len < 5) return c;
  for (size_t i = next_start_code(annexb, len, 0); i < len;
       i = next_start_code(annexb, len, i + 3)) {
    const uint8_t type = (annexb[i + 3] >> 1) & 0x3F;
    if (type < 32) ++c.vcl;
    else if (type <= 34) c.param_set = true;
  }
  return c;
}

}  // namespace mabur
