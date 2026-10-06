// ListenWindow: the drone side of the listen window (feedback-repair rollout
// phase 3). Pure logic -- times are synthetic µs.
#include "../drone/src/listen_window.h"
#include "mtest.h"

using mabur::ListenWindow;
namespace rc = mabur::rc;

namespace {
rc::Status status(uint16_t fid, uint8_t listen_ms) {
  rc::Status s;
  s.vtx_id = 1;
  s.fid = fid;
  s.listen_ms = listen_ms;
  return s;
}
}  // namespace

TEST(bins_split_the_arrival_offset) {
  CHECK(ListenWindow::bin(-1) == 0);
  CHECK(ListenWindow::bin(0) == 1);
  CHECK(ListenWindow::bin(999) == 1);
  CHECK(ListenWindow::bin(1000) == 2);
  CHECK(ListenWindow::bin(4999) == 5);
  CHECK(ListenWindow::bin(5000) == 6);
  CHECK(ListenWindow::bin(6999) == 6);
  CHECK(ListenWindow::bin(7000) == 7);
  CHECK(ListenWindow::bin(1000000) == 7);
}

TEST(gap_follows_statuses_and_times_out) {
  ListenWindow w;
  CHECK(w.gap_us(0) == 0);  // nothing asked yet
  w.on_status(status(rc::kStatusNoFid, 4), 1000);
  CHECK(w.gap_us(1000) == 4000);
  CHECK(w.gap_us(1000 + ListenWindow::kStatusTimeoutUs) == 4000);
  CHECK(w.gap_us(1001 + ListenWindow::kStatusTimeoutUs) == 0);  // GS stopped asking
  // listen_ms 0 switches it off at once; the drone caps whatever it is asked.
  w.on_status(status(rc::kStatusNoFid, 0), 2000);
  CHECK(w.gap_us(2000) == 0);
  w.on_status(status(rc::kStatusNoFid, rc::kStatusMaxListenMs), 3000);
  CHECK(w.gap_us(3000) == ListenWindow::kMaxGapUs);
}

TEST(status_is_timed_against_its_own_aus_gap) {
  ListenWindow w;
  w.on_au_gap(10, 100000, 104000);
  w.on_au_gap(11, 116000, 120000);  // the next AU's gap is already booked
  w.on_status(status(10, 4), 102500);  // 2.5 ms into AU 10's gap
  w.on_status(status(11, 4), 115000);  // 1 ms before AU 11's burst is modelled off
  w.on_status(status(11, 4), 123500);  // 7.5 ms in: late
  w.on_status(status(99, 4), 123600);  // no gap for that AU
  w.on_status(status(rc::kStatusNoFid, 4), 123700);
  const rc::LwStat s = w.take(1, 7, 123700);
  CHECK(s.vtx_id == 1);
  CHECK(s.seq == 7);
  CHECK(s.listen_ms == 4);
  CHECK(s.status_rx == 5);
  CHECK(s.hist[0] == 1);  // early
  CHECK(s.hist[3] == 1);  // 2-3 ms
  CHECK(s.hist[7] == 1);  // >= 7 ms
  CHECK(s.nofid == 2);
  // The period counters reset; the gap setting does not.
  const rc::LwStat t = w.take(1, 8, 123800);
  CHECK(t.status_rx == 0);
  CHECK(t.hist[3] == 0);
  CHECK(t.nofid == 0);
  CHECK(t.listen_ms == 4);
}

TEST(old_gaps_age_out_of_the_ring) {
  ListenWindow w;
  w.on_au_gap(1, 1000, 5000);
  for (int k = 0; k < ListenWindow::kRing; ++k)
    w.on_au_gap(static_cast<uint16_t>(100 + k), 10000 + k, 14000 + k);
  w.on_status(status(1, 4), 2000);
  CHECK(w.take(1, 0, 2000).nofid == 1);
}

TEST(direct_sends_wait_only_inside_a_kept_gap) {
  ListenWindow w;
  // A gap that is not being kept (until == from) never holds anything.
  w.on_au_gap(1, 10000, 10000);
  CHECK(w.quiet_until(9500, 1000) == 0);
  w.on_au_gap(2, 30000, 34000);
  CHECK(w.quiet_until(20000, 1000) == 0);      // mid-burst: goes out as today
  CHECK(w.quiet_until(29100, 1000) == 34000);  // would reach the air in the gap
  CHECK(w.quiet_until(33000, 1000) == 34000);
  CHECK(w.quiet_until(34000, 1000) == 0);      // gap over
  // The next AU's gap already booked must not hide the one running now.
  w.on_au_gap(3, 50000, 54000);
  CHECK(w.quiet_until(31000, 1000) == 34000);
}

TEST(holds_are_reported_and_saturate) {
  ListenWindow w;
  w.on_gate_hold(1500);
  w.on_gate_hold(3200);
  w.on_direct_hold();
  CHECK(w.active(0));
  rc::LwStat s = w.take(1, 0, 0);
  CHECK(s.gate_holds == 2);
  CHECK(s.gate_hold_sum_ms == 4);
  CHECK(s.gate_hold_max_ms == 3);
  CHECK(s.direct_holds == 1);
  CHECK(!w.active(0));  // no statuses, nothing counted
  for (int i = 0; i < 300; ++i) w.on_status(status(rc::kStatusNoFid, 4), 1000);
  s = w.take(1, 1, 1000);
  CHECK(s.status_rx == 300);
  CHECK(s.nofid == 255);
}
MTEST_MAIN
