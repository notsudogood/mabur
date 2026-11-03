#include "edid.h"

#include <cctype>
#include <cstdio>
#include <vector>

namespace maburplay {

namespace {

constexpr uint8_t kHeader[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
constexpr size_t kBlock = 128;
constexpr int kVic1080p60 = 16;

// A Short Video Descriptor's VIC (CTA-861-F 7.5.1): codes 129..192 carry
// the "native" flag in bit 7 over VICs 1..64; every other code is the VIC.
int svd_vic(uint8_t b) {
  if (b >= 129 && b <= 192) return b & 0x7f;
  if (b == 0 || b == 128 || b >= 254) return 0;
  return b;
}

std::string text13(const uint8_t* p) {
  std::string s;
  for (int i = 0; i < 13 && p[i] != 0x0a && p[i] != 0x00; ++i)
    s += (p[i] >= 0x20 && p[i] < 0x7f) ? static_cast<char>(p[i]) : '?';
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

std::string fold(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  std::string out;
  for (size_t i = a; i < b; ++i)
    out += static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
  return out;
}

}  // namespace

std::string SinkInfo::id() const {
  char b[16];
  std::snprintf(b, sizeof(b), "%s-%04X", mfr.c_str(), product);
  return b;
}

std::string SinkInfo::describe() const {
  if (!valid) return "no EDID";
  std::string s = name.empty() ? std::string("(no name)") : "\"" + name + "\"";
  s += " " + id();
  s += hdmi ? ", HDMI" : ", DVI";
  switch (y420_1080p60) {
    case Y420::kOnly: s += ", 1080p60 as YCbCr 4:2:0 only"; break;
    case Y420::kAlso: s += ", 1080p60 also as YCbCr 4:2:0"; break;
    case Y420::kNo: break;
  }
  return s;
}

bool SinkInfo::matches(const std::string& pattern) const {
  if (!valid) return false;
  const std::string p = fold(pattern);
  if (p.empty()) return false;
  return p == fold(id()) || (!name.empty() && p == fold(name));
}

SinkInfo parse_edid(const uint8_t* d, size_t len) {
  SinkInfo s;
  if (!d || len < kBlock) return s;
  for (size_t i = 0; i < 8; ++i)
    if (d[i] != kHeader[i]) return s;
  s.valid = true;

  const unsigned v = (static_cast<unsigned>(d[8]) << 8) | d[9];
  for (int shift : {10, 5, 0}) {
    const unsigned c = (v >> shift) & 0x1f;
    s.mfr += (c >= 1 && c <= 26) ? static_cast<char>('A' + c - 1) : '?';
  }
  s.product = static_cast<uint16_t>(d[10] | (d[11] << 8));
  s.serial = static_cast<uint32_t>(d[12]) | (static_cast<uint32_t>(d[13]) << 8) |
             (static_cast<uint32_t>(d[14]) << 16) | (static_cast<uint32_t>(d[15]) << 24);
  for (size_t o = 54; o + 18 <= 126; o += 18)
    if (d[o] == 0 && d[o + 1] == 0 && d[o + 3] == 0xfc) s.name = text13(d + o + 5);

  std::vector<int> svds;        // every Video Data Block's VICs, in order
  bool only420 = false;
  bool cmdb_all = false;        // a Capability Map with no bitmap: every SVD
  std::vector<uint8_t> cmdb;    // its bitmap, bit i = the i-th SVD
  const size_t blocks = d[126];
  for (size_t k = 1; k <= blocks && (k + 1) * kBlock <= len; ++k) {
    const uint8_t* b = d + k * kBlock;
    if (b[0] != 0x02) continue;               // CTA-861 extensions only
    const size_t end = b[2];                  // where the detailed timings start
    if (end < 4 || end > 127) continue;       // no data block collection
    for (size_t p = 4; p < end;) {
      const unsigned tag = b[p] >> 5, n = b[p] & 0x1f;
      if (p + 1 + n > end) break;
      const uint8_t* pl = b + p + 1;
      if (tag == 2) {
        for (unsigned i = 0; i < n; ++i) svds.push_back(svd_vic(pl[i]));
      } else if (tag == 3 && n >= 3 && pl[0] == 0x03 && pl[1] == 0x0c && pl[2] == 0x00) {
        s.hdmi = true;
      } else if (tag == 7 && n >= 1) {
        if (pl[0] == 14) {
          for (unsigned i = 1; i < n; ++i)
            if (svd_vic(pl[i]) == kVic1080p60) only420 = true;
        } else if (pl[0] == 15) {
          if (n == 1) cmdb_all = true;
          cmdb.insert(cmdb.end(), pl + 1, pl + n);
        }
      }
      p += 1 + n;
    }
  }
  if (only420) {
    s.y420_1080p60 = SinkInfo::Y420::kOnly;
  } else {
    for (size_t i = 0; i < svds.size(); ++i) {
      if (svds[i] != kVic1080p60) continue;
      const bool bit = i / 8 < cmdb.size() && ((cmdb[i / 8] >> (i % 8)) & 1u);
      if (cmdb_all || bit) s.y420_1080p60 = SinkInfo::Y420::kAlso;
    }
  }
  return s;
}

}  // namespace maburplay
