// BurstEndTracker + listen_on: the GS side of the listen window
// (feedback-repair rollout phase 3). Time is the core loop's mono ms.
#include "listen_burst.h"
#include "mtest.h"

using maburgs::BurstEndTracker;
using mabur::rc::StatusTrig;

TEST(first_probe_copy_marks_the_burst_end_once) {
  BurstEndTracker t;
  t.on_au_first(10);
  t.on_au_complete(100, true, 5);  // probe to come
  CHECK(!t.take().has_value());
  t.on_probe(101, uint16_t{10});
  auto be = t.take();
  REQUIRE(be.has_value());
  CHECK(be->trig == StatusTrig::Probe);
  CHECK(be->fid == 10);
  CHECK(be->t_ms == 101);
  t.on_probe(102, uint16_t{10});  // the second card's copy
  CHECK(!t.take().has_value());
  t.poll(200);  // the deadline it would have used is disarmed
  CHECK(!t.take().has_value());
}

TEST(probe_before_completion_still_counts_once) {
  // A late repair can complete the AU after its probe already arrived.
  BurstEndTracker t;
  t.on_au_first(11);
  t.on_probe(100, uint16_t{11});
  REQUIRE(t.take().has_value());
  t.on_au_complete(103, true, 5);  // nothing to wait for
  t.poll(200);
  CHECK(!t.take().has_value());
}

TEST(lost_probe_falls_back_to_the_deadline) {
  BurstEndTracker t;
  t.on_au_first(12);
  t.on_au_complete(100, true, 5);
  t.poll(104);
  CHECK(!t.take().has_value());
  t.poll(105);
  auto be = t.take();
  REQUIRE(be.has_value());
  CHECK(be->trig == StatusTrig::Deadline);
  CHECK(be->fid == 12);
  // A copy that shows up after the deadline fired is the same burst end.
  t.on_probe(106, uint16_t{12});
  CHECK(!t.take().has_value());
}

TEST(next_burst_cancels_a_pending_deadline) {
  BurstEndTracker t;
  t.on_au_first(13);
  t.on_au_complete(100, true, 8);
  t.on_au_first(14);  // the next burst started: the gap is gone
  t.poll(200);
  CHECK(!t.take().has_value());
}

TEST(completion_is_the_end_without_a_probe) {
  BurstEndTracker t;
  t.on_au_first(15);
  t.on_au_complete(100, false, 5);
  auto be = t.take();
  REQUIRE(be.has_value());
  CHECK(be->trig == StatusTrig::Completion);
  CHECK(be->fid == 15);
}

TEST(unparsed_probe_belongs_to_the_armed_au) {
  BurstEndTracker t;
  t.on_au_first(16);
  t.on_au_complete(100, true, 5);
  t.on_probe(101, std::nullopt);
  auto be = t.take();
  REQUIRE(be.has_value());
  CHECK(be->fid == 16);
  t.on_probe(102, uint16_t{16});  // the other card parsed it: same end
  CHECK(!t.take().has_value());
  // Nothing at all seen yet: an unparsed probe names nothing.
  BurstEndTracker fresh;
  fresh.on_probe(1, std::nullopt);
  CHECK(!fresh.take().has_value());
}

TEST(listen_on_follows_ms_and_the_ab_period) {
  maburgs::ListenCfg c;
  CHECK(!maburgs::listen_on(c, 0));
  c.ms = 4;
  CHECK(maburgs::listen_on(c, 0));
  CHECK(maburgs::listen_on(c, 999999));
  c.ab_s = 30;
  CHECK(maburgs::listen_on(c, 0));
  CHECK(maburgs::listen_on(c, 29999));
  CHECK(!maburgs::listen_on(c, 30000));
  CHECK(!maburgs::listen_on(c, 59999));
  CHECK(maburgs::listen_on(c, 60000));
}
MTEST_MAIN
