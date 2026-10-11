// maburplay's AsyncRawDvr (gs/player/src/async_raw_dvr.h): the raw
// recorder's file I/O on a writer thread behind a bounded queue. Same file
// as the synchronous RawDvr, consistent getters through the queue, and a
// card that falls behind ends the recording instead of stalling the caller.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "async_raw_dvr.h"
#include "mabur/raw_dvr.h"
#include "mtest.h"

using maburplay::AsyncRawDvr;
using mabur::RawDvr;

namespace {

std::string scratch(const char* name) { return std::string(MABUR_TEST_SCRATCH_DIR) + "/" + name; }

std::vector<uint8_t> slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

void nal(std::vector<uint8_t>& au, std::initializer_list<uint8_t> bytes) {
  au.insert(au.end(), {0, 0, 0, 1});
  au.insert(au.end(), bytes);
}

// Same fixtures as test_raw_dvr.cpp: a GDR refresh start (params + TRAIL_R),
// an IDR (params + IDR_W_RADL) and a plain P slice.
std::vector<uint8_t> param_au(uint8_t tag) {
  std::vector<uint8_t> au;
  nal(au, {0x40, 0x01, 0x0c, 0x01, 0xff, 0xff});
  au.insert(au.end(), {0, 0, 1});
  const uint8_t sps[] = {0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00,
                         0x03, 0x00, 0x00, 0x03, 0x00, 0x78, 0xa0, 0x02, 0x80, 0x80, 0x2d, 0x16,
                         0x59, 0x59, 0xa4, 0x93, 0x2b, 0xc0, 0x5a, 0x02, 0x00, 0x00, 0x03, 0x00,
                         0x02, 0x00, 0x00, 0x03, 0x00, 0x78, 0x10};
  au.insert(au.end(), std::begin(sps), std::end(sps));
  nal(au, {0x44, 0x01, 0xc1, 0x72});
  nal(au, {0x02, 0x01, tag, tag});
  return au;
}
std::vector<uint8_t> idr_au(uint8_t tag) {
  std::vector<uint8_t> au = param_au(tag);
  au[au.size() - 4] = 0x26;
  return au;
}
std::vector<uint8_t> p_au(uint8_t tag) {
  std::vector<uint8_t> au;
  nal(au, {0x02, 0x01, tag, tag, tag});
  return au;
}

// 3 s at 60 fps: two waits, an IDR, P slices, a refresh start each second.
std::vector<std::vector<uint8_t>> clip() {
  std::vector<std::vector<uint8_t>> v;
  v.push_back(p_au(1));
  v.push_back(param_au(2));
  v.push_back(idr_au(3));
  for (int i = 0; i < 180; ++i) v.push_back(i % 60 == 59 ? param_au(uint8_t(i)) : p_au(uint8_t(i)));
  return v;
}

}  // namespace

TEST(async_writes_the_same_file_as_the_synchronous_recorder) {
  const std::string a = scratch("async_same_sync.mp4"), b = scratch("async_same_async.mp4");
  std::remove(a.c_str());
  std::remove(b.c_str());
  RawDvr s;
  AsyncRawDvr d;
  s.start(a, 0, 0, 1000);
  d.start(b, 0, 0, 1000);
  uint32_t pts = 0;
  for (const auto& au : clip()) {
    s.feed(au.data(), au.size(), pts, true);
    d.feed(au.data(), au.size(), pts, true);
    pts += 16667;
  }
  s.stop();
  d.stop(/*wait=*/true);
  const auto fa = slurp(a), fb = slurp(b);
  CHECK(!fa.empty());
  CHECK(fa == fb);
  CHECK(d.samples() == s.samples());
  CHECK(d.fragments() == s.fragments());
  CHECK(d.state() == RawDvr::State::Off);
  CHECK(d.err() == RawDvr::Err::None);
}

TEST(async_state_reads_consistently_through_the_queue) {
  const std::string p = scratch("async_state.mp4");
  std::remove(p.c_str());
  AsyncRawDvr d;
  CHECK(d.state() == RawDvr::State::Off);
  d.start(p, 0, 0);
  CHECK(d.state() == RawDvr::State::WaitSync);   // at once, whatever the writer has reached
  CHECK(!d.opened());
  const auto w = p_au(1);
  d.feed(w.data(), w.size(), 0, true);
  const auto k = idr_au(2);
  d.feed(k.data(), k.size(), 16667, true);
  d.stop(/*wait=*/true);
  CHECK(d.state() == RawDvr::State::Off);
  CHECK(d.samples() == 1);                       // the IDR; the P before it was not a sync point
  // A new start must not read as the previous, sealed file.
  d.start(scratch("async_state2.mp4"), 0, 0);
  CHECK(d.samples() == 0);
  CHECK(!d.opened());
  CHECK(d.state() == RawDvr::State::WaitSync);
  d.stop(/*wait=*/true);
}

TEST(async_drops_unarmed_and_incomplete_aus_before_copying) {
  AsyncRawDvr d;
  const auto k = idr_au(1);
  d.feed(k.data(), k.size(), 0, true);            // not armed
  CHECK(d.queued_bytes() == 0);
  d.start(scratch("async_drop.mp4"), 0, 0);
  d.feed(k.data(), k.size(), 0, /*complete=*/false);
  CHECK(d.queued_bytes() == 0);
  d.stop(/*wait=*/true);
  CHECK(d.samples() == 0);
}

// A card that falls behind: here the queue bound is below one AU, so the
// first copy overflows. The recording ends with a write error, the caller
// is never blocked, later AUs are ignored, and the next start() clears it.
TEST(async_queue_overflow_ends_the_recording_with_a_write_error) {
  AsyncRawDvr d(/*max_queue_bytes=*/16);
  d.start(scratch("async_overflow.mp4"), 0, 0);
  const auto k = idr_au(1);
  d.feed(k.data(), k.size(), 0, true);
  CHECK(d.overflowed());
  CHECK(d.state() == RawDvr::State::Error);
  CHECK(d.err() == RawDvr::Err::Write);
  d.feed(k.data(), k.size(), 16667, true);
  CHECK(d.queued_bytes() == 0);
  d.start(scratch("async_overflow2.mp4"), 0, 0);
  CHECK(!d.overflowed());
  CHECK(d.state() == RawDvr::State::WaitSync);
  d.stop(/*wait=*/true);
}

// Destruction drains the queue and seals the file: a player that exits
// mid-recording keeps everything it was handed.
TEST(async_destructor_drains_and_seals) {
  const std::string a = scratch("async_dtor_sync.mp4"), b = scratch("async_dtor_async.mp4");
  std::remove(a.c_str());
  std::remove(b.c_str());
  RawDvr s;
  s.start(a, 0, 0, 1000);
  {
    AsyncRawDvr d;
    d.start(b, 0, 0, 1000);
    uint32_t pts = 0;
    for (const auto& au : clip()) {
      s.feed(au.data(), au.size(), pts, true);
      d.feed(au.data(), au.size(), pts, true);
      pts += 16667;
    }
  }
  s.stop();
  CHECK(!slurp(a).empty());
  CHECK(slurp(a) == slurp(b));
}

MTEST_MAIN
