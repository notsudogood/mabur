#include "mabur/crc16.h"

#include <array>

namespace mabur {
namespace {
// CRC-16/CCITT-FALSE, poly 0x1021, MSB-first.
// kTable[0] is the classic one-lookup-per-byte table, built at compile time
// from the same bit-serial recurrence the reference (tests/test_crc16.cpp,
// devourer's Python) uses. kTable[k][x] is the CRC contribution of byte x
// followed by k zero bytes, so eight bytes fold in as eight independent
// lookups XORed together (slicing-by-8) instead of a chain of eight
// dependent steps: the CRC is linear, so the result is identical. 4 KiB,
// read-only. Every SBI sub-block is checked on the drone's hot thread and
// on each GS RX thread (host profile 2026-10-13: the largest per-byte cost
// on both after the FEC maths).
constexpr int kSlices = 8;
using Tables = std::array<std::array<uint16_t, 256>, kSlices>;
constexpr Tables make_tables() {
  Tables t{};
  for (int i = 0; i < 256; ++i) {
    uint16_t crc = static_cast<uint16_t>(i << 8);
    for (int b = 0; b < 8; ++b)
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                           : static_cast<uint16_t>(crc << 1);
    t[0][static_cast<size_t>(i)] = crc;
  }
  for (int k = 1; k < kSlices; ++k)
    for (int i = 0; i < 256; ++i) {
      const uint16_t p = t[static_cast<size_t>(k - 1)][static_cast<size_t>(i)];
      t[static_cast<size_t>(k)][static_cast<size_t>(i)] =
          static_cast<uint16_t>((p << 8) ^ t[0][p >> 8]);
    }
  return t;
}
constexpr Tables kTable = make_tables();
}  // namespace

uint16_t crc16_ccitt(const uint8_t* data, size_t len, uint16_t init) {
  uint16_t crc = init;
  size_t i = 0;
  for (; i + 8 <= len; i += 8) {
    const uint8_t* d = data + i;
    crc = static_cast<uint16_t>(kTable[7][(crc >> 8) ^ d[0]] ^ kTable[6][(crc & 0xFF) ^ d[1]] ^
                                kTable[5][d[2]] ^ kTable[4][d[3]] ^ kTable[3][d[4]] ^
                                kTable[2][d[5]] ^ kTable[1][d[6]] ^ kTable[0][d[7]]);
  }
  for (; i < len; ++i)
    crc = static_cast<uint16_t>((crc << 8) ^ kTable[0][((crc >> 8) ^ data[i]) & 0xFF]);
  return crc;
}

}  // namespace mabur
