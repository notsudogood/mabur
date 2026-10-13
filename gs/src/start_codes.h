#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace maburgs {

// One Annex-B start code: 00 00 01 (len 3) or 00 00 00 01 (len 4) at pos.
struct StartCode {
  size_t pos, len;
};

// Appends every start code in b[from, n) to out, in order, non-overlapping
// (a 4-byte code is never also reported as the 3-byte code inside it).
//
// memchr jumps to each 0x00 instead of testing every byte: the slice
// assembler scans every received video byte, and the byte loop was 3.5 % of
// the GS core thread at 35 Mb/s (GS profile, bench 2026-10-13). Same codes,
// same positions as that loop (tests/test_slice_assembler.cpp checks both
// against random buffers).
inline void find_start_codes(const uint8_t* b, size_t n, size_t from,
                             std::vector<StartCode>* out) {
  size_t j = from;
  while (j + 3 <= n) {
    // Candidate code starts are j .. n-3.
    const void* z = std::memchr(b + j, 0, n - 2 - j);
    if (!z) return;
    j = static_cast<size_t>(static_cast<const uint8_t*>(z) - b);
    if (b[j + 1] != 0) {  // no code can start at j or at j + 1
      j += 2;
      continue;
    }
    if (b[j + 2] == 1) {
      out->push_back({j, 3});
      j += 3;
      continue;
    }
    if (b[j + 2] == 0 && j + 4 <= n && b[j + 3] == 1) {
      out->push_back({j, 4});
      j += 4;
      continue;
    }
    ++j;
  }
}

}  // namespace maburgs
