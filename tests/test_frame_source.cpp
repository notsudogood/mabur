#include <vector>
#include "mtest.h"
#include "frame_source.h"
using namespace mabur;

namespace {
const char* kName = "/mabur_test_frame_ring";
}

TEST(reads_frame_with_meta) {
  venc_frame_ring_t* prod = venc_frame_ring_create(kName, 16, 64 * 1024);
  REQUIRE(prod != nullptr);
  FrameSource src(kName, 10);
  std::vector<uint8_t> frame(20000);
  for (size_t i = 0; i < frame.size(); ++i) frame[i] = static_cast<uint8_t>(i);
  VencFrameMeta meta{};
  meta.pts = 123456;
  meta.codec = VENC_FRAME_CODEC_H265;
  meta.flags = VENC_FRAME_FLAG_IDR;
  REQUIRE(venc_frame_ring_write(prod, &meta, frame.data(),
                                static_cast<uint32_t>(frame.size())) == 0);
  std::vector<uint8_t> buf(VENC_FRAME_META_SIZE + 64 * 1024);
  VencFrameMeta got{};
  int n = src.read(buf.data(), buf.size(), 200, &got);
  REQUIRE(n == static_cast<int>(frame.size()));
  CHECK(got.pts == 123456u);
  CHECK(got.flags == VENC_FRAME_FLAG_IDR);
  CHECK(std::vector<uint8_t>(buf.begin() + VENC_FRAME_META_SIZE,
                             buf.begin() + VENC_FRAME_META_SIZE + n) == frame);
  venc_frame_ring_destroy(prod);
}

TEST(timeout_returns_zero) {
  venc_frame_ring_t* prod = venc_frame_ring_create(kName, 16, 64 * 1024);
  REQUIRE(prod != nullptr);
  FrameSource src(kName, 10);
  std::vector<uint8_t> buf(VENC_FRAME_META_SIZE + 64 * 1024);
  VencFrameMeta got{};
  CHECK(src.read(buf.data(), buf.size(), 20, &got) == 0);
  venc_frame_ring_destroy(prod);
}

TEST(producer_restart_reattaches) {
  venc_frame_ring_t* prod = venc_frame_ring_create(kName, 16, 64 * 1024);
  REQUIRE(prod != nullptr);
  FrameSource src(kName, 10);
  std::vector<uint8_t> buf(VENC_FRAME_META_SIZE + 64 * 1024);
  VencFrameMeta meta{}, got{};
  uint8_t payload[100] = {0x42};
  venc_frame_ring_write(prod, &meta, payload, sizeof payload);
  REQUIRE(src.read(buf.data(), buf.size(), 200, &got) == 100);
  // Producer restarts: destroy (unlinks shm) + recreate = new inode.
  venc_frame_ring_destroy(prod);
  prod = venc_frame_ring_create(kName, 16, 64 * 1024);
  REQUIRE(prod != nullptr);
  // First read after restart hits the stale mapping, detects the inode
  // change and detaches; a following read reattaches and delivers.
  int n = 0;
  for (int i = 0; i < 50 && n == 0; ++i) {
    venc_frame_ring_write(prod, &meta, payload, sizeof payload);
    n = src.read(buf.data(), buf.size(), 20, &got);
  }
  CHECK(n == 100);
  CHECK(src.reattach_count() >= 1);
  venc_frame_ring_destroy(prod);
}

// venc_frame_ring_fill_t is a snapshot of the *local handle's* counters (the
// vendored header calls it "Producer-side observability"), and only the write
// path touches writes/full_drops. maburd attaches as a consumer, so those two
// fields are structurally pinned at 0 in its process no matter what the
// producer does — the shm header carries write_idx/read_idx and no counters,
// so there is no path for them to cross. Locks that in so nobody re-adds them
// to maburd's stats line expecting a live number.
TEST(consumer_fill_reports_only_consumer_side_counters) {
  venc_frame_ring_t* prod = venc_frame_ring_create(kName, 16, 64 * 1024);
  REQUIRE(prod != nullptr);
  FrameSource src(kName, 10);
  std::vector<uint8_t> buf(VENC_FRAME_META_SIZE + 64 * 1024);
  VencFrameMeta meta{}, got{};
  uint8_t payload[100] = {0x42};
  for (int i = 0; i < 3; ++i) {
    REQUIRE(venc_frame_ring_write(prod, &meta, payload, sizeof payload) == 0);
    REQUIRE(src.read(buf.data(), buf.size(), 200, &got) == 100);
  }

  venc_frame_ring_fill_t consumer{};
  REQUIRE(src.fill(&consumer));
  CHECK(consumer.reads == 3);       // the consumer's own work: real
  CHECK(consumer.writes == 0);      // producer-only: always 0 here
  CHECK(consumer.full_drops == 0);  // producer-only: always 0 here

  // The same counters are live in the producer's handle, which is the process
  // that would have to report them.
  venc_frame_ring_fill_t producer{};
  REQUIRE(venc_frame_ring_get_fill(prod, &producer) == 0);
  CHECK(producer.writes == 3);
  CHECK(producer.reads == 0);

  venc_frame_ring_destroy(prod);
}

// Upstream waybeam_venc (0.69+) writes a version-2 ring: same 192-byte
// header, health counters carved out of its pad, and in the per-frame meta
// bytes 6-7 are gdr_pos/gdr_len where this header has enc_us. maburd on
// the SSC378QE image reads that ring (the encoder is waybeam's own
// process), so the consumer attaches to v2 and must not read a GDR
// counter as an encode time. REVERT CHECK: with `!= VENC_FRAME_RING_VERSION`
// back in venc_frame_ring_attach the first read returns 0 forever.
TEST(attaches_a_version_2_ring_and_zeroes_enc_us) {
  venc_frame_ring_t* prod = venc_frame_ring_create(kName, 16, 64 * 1024);
  REQUIRE(prod != nullptr);
  prod->hdr->version = 2;  // what waybeam's producer stamps
  FrameSource src(kName, 10);
  VencFrameMeta meta{};
  meta.pts = 777;
  meta.codec = VENC_FRAME_CODEC_H265;
  meta.flags = VENC_FRAME_FLAG_IDR | VENC_FRAME_FLAG_GDR;
  meta.enc_us = 0x0304;  // = gdr_pos 4, gdr_len 3 in the v2 meta
  uint8_t payload[64] = {0x11};
  REQUIRE(venc_frame_ring_write(prod, &meta, payload, sizeof payload) == 0);
  std::vector<uint8_t> buf(VENC_FRAME_META_SIZE + 64 * 1024);
  VencFrameMeta got{};
  REQUIRE(src.read(buf.data(), buf.size(), 200, &got) == 64);
  CHECK(src.ring_version() == 2u);
  CHECK(got.pts == 777u);
  CHECK(got.flags == (VENC_FRAME_FLAG_IDR | VENC_FRAME_FLAG_GDR));
  CHECK(got.enc_us == 0);
  CHECK(buf[VENC_FRAME_META_SIZE] == 0x11);
  venc_frame_ring_destroy(prod);
}

// The in-process encoder's own ring reports its enc_us as before.
TEST(a_version_1_ring_keeps_enc_us) {
  venc_frame_ring_t* prod = venc_frame_ring_create(kName, 16, 64 * 1024);
  REQUIRE(prod != nullptr);
  FrameSource src(kName, 10);
  VencFrameMeta meta{};
  meta.enc_us = 1234;
  uint8_t payload[8] = {0};
  REQUIRE(venc_frame_ring_write(prod, &meta, payload, sizeof payload) == 0);
  std::vector<uint8_t> buf(VENC_FRAME_META_SIZE + 64 * 1024);
  VencFrameMeta got{};
  REQUIRE(src.read(buf.data(), buf.size(), 200, &got) == 8);
  CHECK(src.ring_version() == 1u);
  CHECK(got.enc_us == 1234);
  venc_frame_ring_destroy(prod);
}

// A version this consumer has never seen is refused, as before: a silent
// misread of a changed layout is the failure the version field exists for.
TEST(an_unknown_ring_version_is_refused) {
  venc_frame_ring_t* prod = venc_frame_ring_create(kName, 16, 64 * 1024);
  REQUIRE(prod != nullptr);
  prod->hdr->version = 3;
  FrameSource src(kName, 10);
  VencFrameMeta meta{};
  uint8_t payload[8] = {0};
  REQUIRE(venc_frame_ring_write(prod, &meta, payload, sizeof payload) == 0);
  std::vector<uint8_t> buf(VENC_FRAME_META_SIZE + 64 * 1024);
  CHECK(src.read(buf.data(), buf.size(), 30, nullptr) == 0);
  CHECK(!src.attached());
  venc_frame_ring_destroy(prod);
}
MTEST_MAIN
