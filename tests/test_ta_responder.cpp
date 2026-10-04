#include <cstdint>
#include <vector>

#include "mtest.h"
#include "ta_responder.h"

using namespace mabur;

namespace {

// Fake world: a settable clock, a capture sink, fixed queue readings, and a
// build step that prefixes the lane so the test can see which queue each
// frame was built for.
struct World {
  uint64_t now = 1000;
  std::vector<std::vector<uint8_t>> sent;
  bool fail_sends = false;
  TaResponder::QueueState q{12, 5, 37};

  TaResponder make(uint32_t vtx = 7, uint32_t max_per_s = 50) {
    TaResponder::Cfg c;
    c.vtx_id = vtx;
    c.max_pings_per_s = max_per_s;
    return TaResponder(
        c, [this] { return now; },
        [](uint8_t lane, const std::vector<uint8_t>& body) {
          std::vector<uint8_t> f{0xA0, lane};
          f.insert(f.end(), body.begin(), body.end());
          return f;
        },
        [this](const uint8_t* p, size_t n) {
          if (fail_sends) return false;
          sent.emplace_back(p, p + n);
          now += 300;  // each send call costs time, like a sync bulk-OUT
          return true;
        },
        [this] { return q; });
  }
};

std::vector<uint8_t> ping(uint32_t vtx, uint16_t seq, uint8_t lane, uint8_t n,
                          uint16_t bytes) {
  rc::TaPing p;
  p.vtx_id = vtx;
  p.seq = seq;
  p.lane = lane;
  p.n_frames = n;
  p.frame_bytes = bytes;
  return rc::pack_ta_ping(p);
}

rc::TaPong pong_of(const std::vector<uint8_t>& frame) {
  // Strip the fake 2-byte "radiotap" the World's build step added.
  auto got = rc::parse_ta_pong(frame.data() + 2, frame.size() - 2);
  REQUIRE(got.has_value());
  return *got;
}

}  // namespace

TEST(answers_with_n_frames_on_the_pinged_lane) {
  World w;
  auto r = w.make();
  auto b = ping(7, 300, 4, 3, 400);
  REQUIRE(r.on_ping(b.data(), b.size(), /*rx_us=*/900));
  w.now = 1500;  // responder wakes 600 us after the RX callback saw it
  CHECK(r.pump(0));
  REQUIRE(w.sent.size() == 3);
  for (size_t i = 0; i < 3; ++i) {
    CHECK(w.sent[i][1] == 4);  // built for lane 4 (VO)
    CHECK(w.sent[i].size() == 2 + 400);
    const auto p = pong_of(w.sent[i]);
    CHECK(p.seq == 300);
    CHECK(p.lane == 4);
    CHECK(p.idx == i);
    CHECK(p.n_frames == 3);
    CHECK(p.txq_depth == 12);
    CHECK(p.pool_depth == 5);
    CHECK(p.air_backlog_100us == 37);
  }
  // Hold is stamped at each frame's own send call: 600, then +300 per send.
  CHECK(pong_of(w.sent[0]).hold_us == 600);
  CHECK(pong_of(w.sent[1]).hold_us == 900);
  CHECK(pong_of(w.sent[2]).hold_us == 1200);
  CHECK(r.answered() == 1);
  CHECK(r.frames_sent() == 3);
}

TEST(ignores_other_vtx_and_garbage) {
  World w;
  auto r = w.make(7);
  auto other = ping(8, 1, 0, 1, 26);
  CHECK(!r.on_ping(other.data(), other.size(), 1000));
  auto corrupt = ping(7, 1, 0, 1, 26);
  corrupt[10] ^= 0xFF;
  CHECK(!r.on_ping(corrupt.data(), corrupt.size(), 1000));
  rc::Rcf rcf;
  rcf.vtx_id = 7;
  auto not_ping = rc::pack_rcf(rcf);
  CHECK(!r.on_ping(not_ping.data(), not_ping.size(), 1000));
  CHECK(!r.pump(0));
  CHECK(w.sent.empty());
}

TEST(rate_limit_bounds_what_a_gs_can_make_us_send) {
  World w;
  auto r = w.make(7, /*max_per_s=*/10);  // >= 100 ms apart
  auto b = ping(7, 1, 0, 1, 26);
  CHECK(r.on_ping(b.data(), b.size(), 1'000'000));
  CHECK(!r.on_ping(b.data(), b.size(), 1'050'000));  // 50 ms later: dropped
  CHECK(r.on_ping(b.data(), b.size(), 1'100'000));   // 100 ms: accepted
  CHECK(r.rate_dropped() == 1);
}

TEST(mailbox_is_bounded) {
  World w;
  auto r = w.make(7, /*max_per_s=*/0);  // no rate limit: only the mailbox
  auto b = ping(7, 1, 0, 1, 26);
  int queued = 0;
  for (int i = 0; i < 10; ++i)
    if (r.on_ping(b.data(), b.size(), 1000 + static_cast<uint64_t>(i))) ++queued;
  CHECK(queued == 4);
  CHECK(r.busy_dropped() == 6);
  int answered = 0;
  while (r.pump(0)) ++answered;
  CHECK(answered == 4);
}

TEST(failed_sends_are_counted_not_retried) {
  World w;
  w.fail_sends = true;
  auto r = w.make();
  auto b = ping(7, 9, 5, 2, 26);
  REQUIRE(r.on_ping(b.data(), b.size(), 1000));
  CHECK(r.pump(0));
  CHECK(r.frames_sent() == 0);
  CHECK(r.send_failed() == 2);
  CHECK(r.answered() == 1);
}

TEST(thread_answers_and_stops) {
  World w;
  auto r = w.make();
  r.start();
  auto b = ping(7, 42, 6, 1, 26);
  REQUIRE(r.on_ping(b.data(), b.size(), 1000));
  for (int i = 0; i < 200 && r.answered() == 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  r.stop();
  CHECK(r.answered() == 1);
}

MTEST_MAIN
