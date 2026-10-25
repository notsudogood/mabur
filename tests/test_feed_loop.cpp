// FeedLoop: the decoder-input thread against a real ring + doorbell and a
// thread-safe fake decoder.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include "feed_rig.h"

TEST(a_slice_reaches_the_decoder_while_the_main_thread_is_busy) {
  Rig r("busy");
  r.start_and_connect();
  r.publish_whole(100, 0, true);  // complete sid-0: arms
  REQUIRE(r.fake->wait_for('W', 100, 500ms));
  std::vector<size_t> s;
  const auto au = make_au(4, &s);
  FrameHdr h;
  h.pts_us = 200;
  r.w.begin(h, 0, 4);
  r.w.append(au.data(), s[1]);  // slice 0 whole (valid ends on a NAL boundary)
  r.db.notify(0);
  // The "main thread" is busy (OSD compose, SD write): it neither pumps nor
  // services anything. The feed thread alone STARTs the picture.
  const auto t0 = std::chrono::steady_clock::now();
  REQUIRE(r.fake->wait_for('S', 200, 500ms));
  CHECK(std::chrono::steady_clock::now() - t0 < 100ms);
  r.w.append(au.data() + s[1], s[2] - s[1]);
  r.db.notify(0);
  REQUIRE(r.fake->wait_for('A', 200, 500ms));
  r.w.append(au.data() + s[2], au.size() - s[2]);
  r.w.finish(true, maburgs::AuLatMeta{});
  r.db.notify(0);
  REQUIRE(r.fake->wait_for('A', 200, 500ms, /*last=*/true));
  std::this_thread::sleep_for(10ms);
  r.service();
  bool submit_seen = false;
  for (const FeedNote& n : r.notes)
    if (n.kind == FeedNote::Kind::kSubmit && n.meta.pts_us == 200u) submit_seen = true;
  CHECK(submit_seen);
}

TEST(submit_notes_leave_through_their_own_queue) {
  Rig r("notes");
  r.start_and_connect();
  r.publish_whole(100, 0, true);  // complete sid-0: arms, submits, delivers
  REQUIRE(r.fake->wait_for('W', 100, 500ms));
  std::this_thread::sleep_for(10ms);
  std::vector<FeedNote> sub, del;
  r.fl->take_submit_notes(&sub);
  r.fl->take_notes(&del);
  CHECK(sub.size() == 1 && sub[0].kind == FeedNote::Kind::kSubmit);
  CHECK(!del.empty());
  for (auto& n : del) CHECK(n.kind == FeedNote::Kind::kDelivery);
}

TEST(flush_parks_the_feed_until_the_main_thread_resumes) {
  Rig r("flush");
  r.start_and_connect();
  r.publish_whole(100, 0, true);
  REQUIRE(r.fake->wait_for('W', 100, 500ms));
  r.publish_whole(300, 0, true, mabur::framewire::kFlagDiscont);  // flush_before
  for (int i = 0; i < 500 && !r.feed->flush_waiting(); ++i) std::this_thread::sleep_for(1ms);
  REQUIRE(r.feed->flush_waiting());
  const size_t calls = r.fake->count();
  r.publish_whole(400, 0, true);  // lands while parked: it waits too
  std::this_thread::sleep_for(30ms);
  CHECK(r.fake->count() == calls);  // nothing reached the decoder while parked
  CHECK(r.service());
  REQUIRE(r.fake->wait_for('W', 400, 500ms));
  const auto c = r.fake->snapshot();
  int f = -1, w3 = -1, w4 = -1;
  for (int i = 0; i < static_cast<int>(c.size()); ++i) {
    if (c[i].op == 'F') f = i;
    if (c[i].op == 'W' && c[i].pts == 300u) w3 = i;
    if (c[i].op == 'W' && c[i].pts == 400u) w4 = i;
  }
  CHECK(f >= 0);
  CHECK(f < w3);
  CHECK(w3 < w4);
  CHECK(r.feed->parks() == 1);
}

TEST(a_watchdog_park_breaks_a_wedged_submit_and_holds_the_feed) {
  Rig r("wd");
  r.start_and_connect();
  r.fake->block_submit = true;
  r.publish_whole(100, 0, true);  // the feed sits in submit_au (BUFFER_FULL)
  std::this_thread::sleep_for(20ms);
  const auto t0 = std::chrono::steady_clock::now();
  REQUIRE(r.feed->park(1000ms));
  CHECK(std::chrono::steady_clock::now() - t0 < 200ms);
  r.fake->block_submit = false;
  const size_t calls = r.fake->count();
  r.publish_whole(200, 0, true);
  std::this_thread::sleep_for(30ms);
  CHECK(r.fake->count() == calls);  // parked: the new record waits
  r.backend->flush();               // the watchdog's work, feed parked
  r.router->disarm();
  r.feed->resume();
  REQUIRE(r.fake->wait_for('W', 200, 500ms));
}

TEST(drain_finishes_the_streaming_picture_on_its_close) {
  Rig r("drain");
  r.start_and_connect();
  r.publish_whole(100, 0, true);
  REQUIRE(r.fake->wait_for('W', 100, 500ms));
  std::vector<size_t> s;
  const auto au = make_au(4, &s);
  FrameHdr h;
  h.pts_us = 200;
  r.w.begin(h, 0, 4);
  r.w.append(au.data(), s[2]);
  r.db.notify(0);
  REQUIRE(r.fake->wait_for('A', 200, 500ms));  // streaming: S + slice 1
  r.feed->begin_drain();
  std::this_thread::sleep_for(10ms);
  r.w.append(au.data() + s[2], au.size() - s[2]);
  r.w.finish(true, maburgs::AuLatMeta{});
  r.db.notify(0);
  for (int i = 0; i < 500 && !r.feed->finished(); ++i) {
    r.service();
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(r.feed->finished());
  r.feed->stop_and_join();
  CHECK(r.feed->drained());
  const DrainResult d = r.feed->drain_result();
  CHECK(d.kind == DrainResult::kFinished);
  CHECK(d.pts == 200u);
  CHECK(r.fake->wait_for('A', 200, 0ms, /*last=*/true));
  for (const auto& c : r.fake->snapshot()) CHECK(c.op != 'X');
}

TEST(drain_ends_the_picture_once_when_its_close_never_comes) {
  Rig r("drain2", /*drain_ms=*/30);
  r.start_and_connect();
  r.publish_whole(100, 0, true);
  REQUIRE(r.fake->wait_for('W', 100, 500ms));
  std::vector<size_t> s;
  const auto au = make_au(4, &s);
  FrameHdr h;
  h.pts_us = 200;
  r.w.begin(h, 0, 4);
  r.w.append(au.data(), s[2]);
  r.db.notify(0);
  REQUIRE(r.fake->wait_for('A', 200, 500ms));
  r.feed->begin_drain();
  for (int i = 0; i < 500 && !r.feed->finished(); ++i) std::this_thread::sleep_for(1ms);
  r.feed->stop_and_join();
  CHECK(r.feed->drain_result().kind == DrainResult::kAborted);
  CHECK(r.fake->wait_for('X', 200, 0ms));
}

TEST(periodic_runs_on_the_feed_thread) {
  Rig r("periodic");
  std::atomic<int> n{0};
  std::thread::id id;
  std::mutex m;
  r.feed->set_periodic([&] { std::lock_guard<std::mutex> lk(m); id = std::this_thread::get_id(); ++n; }, 5);
  r.start_and_connect();
  std::this_thread::sleep_for(50ms);
  CHECK(n.load() >= 2);
  std::lock_guard<std::mutex> lk(m);
  CHECK(id != std::this_thread::get_id());
}

// Design §4 rule 1 across back-to-back park gates: maburplay's loop resumes a
// flush park (service_feed) and may call the watchdog's park() right after.
// park() must not return while the resumed feed is still leaving its park --
// on its way out it submits the flush record -- and a served flush must not
// read as waiting again.
TEST(park_right_after_resume_waits_for_the_feed_to_leave_its_park) {
  Rig r("reresume");
  r.start_and_connect();
  r.publish_whole(100, 0, true);
  REQUIRE(r.fake->wait_for('W', 100, 500ms));
  int raced = 0, stale_flush = 0;
  for (uint32_t i = 0; i < 500; ++i) {
    r.publish_whole(1000 + i, 0, true, mabur::framewire::kFlagDiscont);  // flush_before
    for (int k = 0; k < 5000 && !r.feed->flush_waiting(); ++k) std::this_thread::sleep_for(100us);
    REQUIRE(r.feed->flush_waiting());
    r.feeder.on_flush();  // service_feed(), feed parked for the flush
    r.backend->flush();
    r.router->disarm();
    r.feed->resume();
    if (r.feed->flush_waiting()) ++stale_flush;
    REQUIRE(r.feed->park(2000ms));  // the watchdog, at once
    const size_t calls = r.fake->count();
    std::this_thread::sleep_for(300us);  // a feed wrongly still running lands its submit
    if (r.fake->count() != calls) ++raced;
    r.feed->resume();
  }
  CHECK(raced == 0);
  CHECK(stale_flush == 0);
}

TEST(destroying_a_parked_loop_does_not_hang) {
  // The whole Rig lives and dies on another thread so the destruction is
  // bounded and asserted: ~FeedLoop with the feed parked must stop + join.
  auto fut = std::async(std::launch::async, [] {
    Rig r("dtor");
    r.start_and_connect();
    REQUIRE(r.feed->park(1000ms));
  });  // ~Rig -> ~FeedLoop, parked
  const bool ready = fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  CHECK(ready);
  if (!ready) {
    // A hung destructor would also hang ~future (std::async's shared state
    // joins its thread), so leave without unwinding: the FAIL line above is
    // already printed. ctest's timeout stays the backstop.
    std::fflush(stdout);
    std::_Exit(1);
  }
}

MTEST_MAIN
