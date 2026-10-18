#include <cstdint>
#include <vector>

#include "mtest.h"
#include "v4l2req_streamcheck/au_split.h"

namespace {
// NAL with a 3-byte start code; for VCL types `first` sets
// first_slice_segment_in_pic_flag (MSB of the first slice-header byte).
void nal(std::vector<uint8_t>& b, int type, bool first = false, int body = 4) {
  b.insert(b.end(), {0x00, 0x00, 0x01, static_cast<uint8_t>(type << 1), 0x01});
  b.push_back(first ? 0x80 : 0x00);
  for (int i = 0; i < body; ++i) b.push_back(0xAA);
}
}  // namespace

// Two AUs: [VPS SPS PPS slice(first) slice slice] [slice(first) slice].
TEST(split_aus_at_first_slice_and_parameter_sets) {
  std::vector<uint8_t> b;
  nal(b, 32); nal(b, 33); nal(b, 34);
  nal(b, 1, true); nal(b, 1); nal(b, 1);
  const size_t au2 = b.size();
  nal(b, 1, true); nal(b, 1);
  auto aus = streamcheck::split_aus(b.data(), b.size());
  REQUIRE(aus.size() == 2);
  CHECK(aus[0].begin == 0 && aus[0].end == au2);
  CHECK(aus[1].begin == au2 && aus[1].end == b.size());
}

// Parameter sets before a first slice belong to the AU they precede.
TEST(split_aus_parameter_sets_open_the_next_au) {
  std::vector<uint8_t> b;
  nal(b, 1, true); nal(b, 1);
  const size_t au2 = b.size();
  nal(b, 32); nal(b, 33); nal(b, 34); nal(b, 1, true);
  auto aus = streamcheck::split_aus(b.data(), b.size());
  REQUIRE(aus.size() == 2);
  CHECK(aus[1].begin == au2);
}

// StreamFeeder's cut: [prefix + slice0), then each later slice with any
// non-VCL NAL before it, the last piece running to the end of the AU.
TEST(stream_pieces_one_slice_each) {
  std::vector<uint8_t> b;
  nal(b, 32); nal(b, 33); nal(b, 34);
  nal(b, 1, true);
  const size_t e0 = b.size();
  nal(b, 1);
  const size_t e1 = b.size();
  nal(b, 39);  // a prefix SEI between slices travels with the next slice
  nal(b, 1);
  const size_t e2 = b.size();
  nal(b, 1);
  nal(b, 40);  // a suffix SEI rides with the last slice
  auto p = streamcheck::stream_pieces(b.data(), b.size());
  REQUIRE(p.size() == 4);
  CHECK(p[0].begin == 0 && p[0].end == e0);
  CHECK(p[1].begin == e0 && p[1].end == e1);
  CHECK(p[2].begin == e1 && p[2].end == e2);
  CHECK(p[3].begin == e2 && p[3].end == b.size());
}

TEST(stream_pieces_single_slice_is_one_piece) {
  std::vector<uint8_t> b;
  nal(b, 1, true);
  auto p = streamcheck::stream_pieces(b.data(), b.size());
  REQUIRE(p.size() == 1);
  CHECK(p[0].begin == 0 && p[0].end == b.size());
}

TEST(four_byte_start_codes) {
  std::vector<uint8_t> b = {0x00};
  nal(b, 1, true);
  b.push_back(0x00);
  const size_t au2 = b.size();
  nal(b, 1, true);
  auto aus = streamcheck::split_aus(b.data(), b.size());
  REQUIRE(aus.size() == 2);
  CHECK(aus[0].begin == 0);
  CHECK(aus[1].begin == au2 - 1);  // the leading zero of the 4-byte code opens AU 2
}

MTEST_MAIN
