#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "edid.h"
#include "mtest.h"

using maburplay::parse_edid;
using maburplay::SinkInfo;

namespace {

// A base block: maker "ABC", product 0x1234, serial 7, the monitor name
// `name` in the second descriptor, `ext` CTA-861 blocks to follow.
std::vector<uint8_t> base(const char* name, int ext) {
  std::vector<uint8_t> e(128, 0);
  const uint8_t hdr[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
  std::memcpy(e.data(), hdr, 8);
  e[8] = 0x04;   // (1 << 10) | (2 << 5) | 3 = "ABC"
  e[9] = 0x43;
  e[10] = 0x34;
  e[11] = 0x12;
  e[12] = 7;
  if (name) {
    uint8_t* d = e.data() + 72;
    d[3] = 0xfc;
    std::memset(d + 5, ' ', 13);
    const size_t n = std::strlen(name);
    std::memcpy(d + 5, name, n);
    if (n < 13) d[5 + n] = 0x0a;
  }
  e[126] = static_cast<uint8_t>(ext);
  return e;
}

// Appends a CTA-861 extension holding the given data blocks.
void cta(std::vector<uint8_t>* e, const std::vector<std::vector<uint8_t>>& blocks) {
  std::vector<uint8_t> b(128, 0);
  b[0] = 0x02;
  b[1] = 0x03;
  size_t p = 4;
  for (const auto& blk : blocks) {
    std::memcpy(b.data() + p, blk.data(), blk.size());
    p += blk.size();
  }
  b[2] = static_cast<uint8_t>(p);
  e->insert(e->end(), b.begin(), b.end());
}

const std::vector<uint8_t> kVdb = {(2 << 5) | 3, 0x90, 4, 31};   // VIC 16 (native), 4, 31
const std::vector<uint8_t> kHdmiVsdb = {(3 << 5) | 5, 0x03, 0x0c, 0x00, 0x10, 0x00};

SinkInfo parse(const std::vector<uint8_t>& e) { return parse_edid(e.data(), e.size()); }

}  // namespace

TEST(base_block_identity) {
  const SinkInfo s = parse(base("HDZero", 0));
  REQUIRE(s.valid);
  CHECK(s.mfr == "ABC" && s.product == 0x1234 && s.serial == 7);
  CHECK(s.name == "HDZero");
  CHECK(s.id() == "ABC-1234");
  CHECK(!s.hdmi);                                    // no CTA block: DVI
  CHECK(s.y420_1080p60 == SinkInfo::Y420::kNo);
  CHECK(s.describe() == "\"HDZero\" ABC-1234, DVI");
}

TEST(a_13_character_name_and_no_name) {
  CHECK(parse(base("ABCDEFGHIJKLM", 0)).name == "ABCDEFGHIJKLM");
  const SinkInfo s = parse(base(nullptr, 0));
  CHECK(s.valid && s.name.empty());
  CHECK(s.describe() == "(no name) ABC-1234, DVI");
}

TEST(hdmi_and_420_capability_map) {
  std::vector<uint8_t> e = base("Goggles", 1);
  // Capability map bit 0 = the first SVD, VIC 16: 1080p60 also as 4:2:0.
  cta(&e, {kVdb, kHdmiVsdb, {(7 << 5) | 2, 15, 0x01}});
  const SinkInfo s = parse(e);
  CHECK(s.hdmi);
  CHECK(s.y420_1080p60 == SinkInfo::Y420::kAlso);
  CHECK(s.describe() == "\"Goggles\" ABC-1234, HDMI, 1080p60 also as YCbCr 4:2:0");
}

TEST(capability_map_bit_for_another_svd_does_not_count) {
  std::vector<uint8_t> e = base("Goggles", 1);
  cta(&e, {kVdb, kHdmiVsdb, {(7 << 5) | 2, 15, 0x02}});   // bit 1 = VIC 4 only
  CHECK(parse(e).y420_1080p60 == SinkInfo::Y420::kNo);
}

TEST(capability_map_without_bitmap_means_every_svd) {
  std::vector<uint8_t> e = base("Goggles", 1);
  cta(&e, {kVdb, kHdmiVsdb, {(7 << 5) | 1, 15}});
  CHECK(parse(e).y420_1080p60 == SinkInfo::Y420::kAlso);
}

TEST(svd_on_a_later_bitmap_byte) {
  std::vector<uint8_t> e = base("Goggles", 1);
  // VIC 16 is the tenth SVD: bitmap byte 1, bit 1.
  const std::vector<uint8_t> vdb = {(2 << 5) | 10, 1, 2, 3, 4, 5, 6, 7, 17, 18, 16};
  cta(&e, {vdb, {(7 << 5) | 3, 15, 0x00, 0x02}});
  CHECK(parse(e).y420_1080p60 == SinkInfo::Y420::kAlso);
}

TEST(y420_video_data_block_means_only) {
  std::vector<uint8_t> e = base("Goggles", 1);
  cta(&e, {{(2 << 5) | 1, 4}, kHdmiVsdb, {(7 << 5) | 2, 14, 16}});
  CHECK(parse(e).y420_1080p60 == SinkInfo::Y420::kOnly);
}

TEST(bad_or_short_input) {
  std::vector<uint8_t> e = base("X", 0);
  e[0] = 0x01;
  const SinkInfo s = parse(e);
  CHECK(!s.valid && s.describe() == "no EDID" && !s.matches("X"));
  CHECK(!parse_edid(nullptr, 0).valid);
  CHECK(!parse(std::vector<uint8_t>(64, 0)).valid);
  // An extension announced but cut off: the base block still parses.
  std::vector<uint8_t> t = base("Cut", 1);
  t.resize(128 + 64, 0x5a);
  const SinkInfo c = parse(t);
  CHECK(c.valid && c.name == "Cut" && !c.hdmi);
  // A data block whose length runs past the collection stops the walk.
  std::vector<uint8_t> o = base("Over", 1);
  cta(&o, {kHdmiVsdb});
  o[128 + 2] = 6;   // the collection ends inside the vendor block
  CHECK(!parse(o).hdmi);
}

TEST(matches_name_or_id_ignoring_case) {
  const SinkInfo s = parse(base("HDZero", 0));
  CHECK(s.matches("HDZero") && s.matches("hdzero") && s.matches("  HDZERO "));
  CHECK(s.matches("abc-1234") && s.matches("ABC-1234"));
  CHECK(!s.matches("HDZ") && !s.matches("") && !s.matches("ABC"));
  const SinkInfo n = parse(base(nullptr, 0));
  CHECK(!n.matches("") && n.matches("ABC-1234"));
}

MTEST_MAIN
