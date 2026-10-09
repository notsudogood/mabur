// ListenWindow: the drone side of the listen window (feedback-repair rollout
// phase 3, revised 3b). Pure logic -- times are synthetic µs.
#include "../drone/src/listen_window.h"
#include "mtest.h"

using mabur::AuCadence;
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
  CHECK(ListenWindow::bin(1999) == 1);
  CHECK(ListenWindow::bin(2000) == 2);
  CHECK(ListenWindow::bin(4000) == 3);
  CHECK(ListenWindow::bin(7999) == 4);
  CHECK(ListenWindow::bin(8000) == 5);
  CHECK(ListenWindow::bin(9999) == 5);
  CHECK(ListenWindow::bin(10000) == 6);
  CHECK(ListenWindow::bin(14999) == 6);
  CHECK(ListenWindow::bin(15000) == 7);
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

TEST(place_puts_the_window_the_delay_after_the_burst) {
  // Plenty of room: [end + delay, end + delay + width).
  auto w = ListenWindow::place(100000, 4500, 4000, 0);
  CHECK(!w.skipped);
  CHECK(w.from == 104500);
  CHECK(w.until == 108500);
  // The next AU is due at 116 667: still fits with the margin.
  w = ListenWindow::place(100000, 4500, 4000, 116667);
  CHECK(w.until == 108500);
  // A long burst: the window is cut to end kFitMarginUs before the next AU.
  w = ListenWindow::place(110000, 4500, 4000, 116667);
  CHECK(!w.skipped);
  CHECK(w.from == 114500);
  CHECK(w.until == 116667 - ListenWindow::kFitMarginUs);
  // Cut below kMinWindowUs: skipped, nothing kept, timed against the end.
  w = ListenWindow::place(111000, 4500, 4000, 116667);
  CHECK(w.skipped);
  CHECK(w.from == 111000 && w.until == 111000);
  // Burst running past the next AU's due time: skipped.
  w = ListenWindow::place(120000, 4500, 4000, 116667);
  CHECK(w.skipped);
}

TEST(hold_until_holds_only_bodies_that_overlap_the_window) {
  const uint64_t from = 104500, until = 108500;
  CHECK(ListenWindow::hold_until(100000, 4000, from, until) == 0);       // ends before
  CHECK(ListenWindow::hold_until(100000, 4500, from, until) == 0);       // ends at its open
  CHECK(ListenWindow::hold_until(100000, 4501, from, until) == until);   // reaches in
  CHECK(ListenWindow::hold_until(106000, 300, from, until) == until);    // inside
  CHECK(ListenWindow::hold_until(108500, 300, from, until) == 0);        // after
  CHECK(ListenWindow::hold_until(106000, 300, from, from) == 0);         // no window kept
}

TEST(status_is_timed_against_its_own_aus_burst_end) {
  ListenWindow w;
  w.on_au_gap(10, 100000, 104000, 108000);
  w.on_au_gap(11, 116000, 120000, 124000);  // the next AU's window already booked
  w.on_status(status(10, 4), 105000);  // 5 ms after AU 10's burst: on air inside its window
  w.on_status(status(11, 4), 115000);  // 1 ms before AU 11's burst is modelled off
  w.on_status(status(11, 4), 133000);  // 17 ms after: late, outside
  w.on_status(status(99, 4), 133100);  // no record for that AU
  w.on_status(status(rc::kStatusNoFid, 4), 133200);
  const rc::LwStat s = w.take(1, 7, 133200);
  CHECK(s.version == 2);
  CHECK(s.vtx_id == 1);
  CHECK(s.seq == 7);
  CHECK(s.listen_ms == 4);
  CHECK(s.status_rx == 5);
  CHECK(s.hist[0] == 1);  // early
  CHECK(s.hist[3] == 1);  // 4-6 ms
  CHECK(s.hist[7] == 1);  // >= 15 ms
  CHECK(s.inside == 1);
  CHECK(s.nofid == 2);
  CHECK(s.delay_100us == ListenWindow::kPriorDelayUs / 100);  // too few to learn yet
  // The period counters reset; the width and the delay do not.
  const rc::LwStat t = w.take(1, 8, 133300);
  CHECK(t.status_rx == 0);
  CHECK(t.hist[3] == 0);
  CHECK(t.inside == 0);
  CHECK(t.listen_ms == 4);
  CHECK(t.delay_100us == ListenWindow::kPriorDelayUs / 100);
}

TEST(inside_counts_the_status_on_air_not_its_rx_stamp) {
  ListenWindow w;
  w.on_au_gap(1, 100000, 104000, 108000);
  w.on_status(status(1, 4), 104500);  // on air 103.7 ms: before the window
  w.on_status(status(1, 4), 108500);  // on air 107.7 ms: inside
  const rc::LwStat s = w.take(1, 0, 108500);
  CHECK(s.inside == 1);
  CHECK(s.hist[3] == 1);  // the histogram keeps the raw stamp: 4.5 ms
  CHECK(s.hist[5] == 1);  // 8.5 ms
}

TEST(a_skipped_window_still_times_but_never_counts_inside) {
  ListenWindow w;
  w.on_au_gap(5, 100000, 100000, 100000);  // fit rule left nothing
  w.on_fit_skip();
  w.on_status(status(5, 4), 100500);
  const rc::LwStat s = w.take(1, 0, 100500);
  CHECK(s.hist[1] == 1);
  CHECK(s.inside == 0);
  CHECK(s.fit_skips == 1);
}

TEST(delay_learns_where_statuses_land) {
  // The first flight's shape: arrivals 5-8 ms after the burst's end. With a
  // 4 ms window the delay settles where it catches the most of them.
  ListenWindow w;
  CHECK(w.delay_us() == ListenWindow::kPriorDelayUs);
  uint64_t t = 1000000;
  for (int k = 0; k < 200; ++k) {
    const uint16_t fid = static_cast<uint16_t>(k);
    w.on_au_gap(fid, t, t, t);
    // offsets cycle 5.0, 5.5, ..., 8.5 ms, then one 20 ms straggler
    const uint64_t off = (k % 9 == 8) ? 20000 : 5000 + static_cast<uint64_t>(k % 8) * 500;
    w.on_status(status(fid, 4), t + off);
    t += 16667;
  }
  // Best 4 ms window over {5.0..8.5}: starts at 5.0 and holds all eight
  // (the straggler is outside) -- on air, kRxLatencyUs before the stamp;
  // less the margin.
  CHECK(w.delay_us() == 5000 - ListenWindow::kRxLatencyUs - ListenWindow::kLearnMarginUs);
  // A later, slower GS: the delay follows within kLearnN arrivals.
  for (int k = 200; k < 200 + ListenWindow::kLearnN; ++k) {
    const uint16_t fid = static_cast<uint16_t>(k);
    w.on_au_gap(fid, t, t, t);
    w.on_status(status(fid, 4), t + 9000 + static_cast<uint64_t>(k % 4) * 500);
    t += 16667;
  }
  CHECK(w.delay_us() == 9000 - ListenWindow::kRxLatencyUs - ListenWindow::kLearnMarginUs);
}

TEST(delay_never_goes_negative_or_past_its_cap) {
  ListenWindow w;
  uint64_t t = 1000000;
  for (int k = 0; k < 40; ++k) {
    w.on_au_gap(static_cast<uint16_t>(k), t, t, t);
    w.on_status(status(static_cast<uint16_t>(k), 4), t - 2000);  // model late
    t += 16667;
  }
  CHECK(w.delay_us() == 0);
  for (int k = 40; k < 40 + ListenWindow::kLearnN; ++k) {
    w.on_au_gap(static_cast<uint16_t>(k), t, t, t);
    w.on_status(status(static_cast<uint16_t>(k), 4), t + 40000);
    t += 16667;
  }
  CHECK(w.delay_us() == ListenWindow::kMaxDelayUs);
}

TEST(old_gaps_age_out_of_the_ring) {
  ListenWindow w;
  w.on_au_gap(1, 1000, 5000, 9000);
  for (int k = 0; k < ListenWindow::kRing; ++k)
    w.on_au_gap(static_cast<uint16_t>(100 + k), 10000 + k, 14000 + k, 18000 + k);
  w.on_status(status(1, 4), 2000);
  CHECK(w.take(1, 0, 2000).nofid == 1);
}

TEST(direct_sends_wait_only_inside_a_kept_window) {
  ListenWindow w;
  // A window that is not being kept (until == from) never holds anything.
  w.on_au_gap(1, 10000, 10000, 10000);
  CHECK(w.quiet_until(9500, 1000) == 0);
  w.on_au_gap(2, 26000, 30000, 34000);
  CHECK(w.quiet_until(20000, 1000) == 0);      // mid-burst: goes out as today
  CHECK(w.quiet_until(27000, 1000) == 0);      // between the burst and its window
  CHECK(w.quiet_until(29100, 1000) == 34000);  // would reach the air in the window
  CHECK(w.quiet_until(33000, 1000) == 34000);
  CHECK(w.quiet_until(34000, 1000) == 0);      // window over
  // The next AU's window already booked must not hide the one running now.
  w.on_au_gap(3, 46000, 50000, 54000);
  CHECK(w.quiet_until(31000, 1000) == 34000);
}

TEST(holds_and_skips_are_reported_and_saturate) {
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
  w.on_fit_skip();
  CHECK(w.active(0));
  for (int i = 0; i < 300; ++i) w.on_fit_skip();
  for (int i = 0; i < 300; ++i) w.on_status(status(rc::kStatusNoFid, 4), 1000);
  s = w.take(1, 1, 1000);
  CHECK(s.status_rx == 300);
  CHECK(s.nofid == 255);
  CHECK(s.fit_skips == 255);
}

TEST(cadence_predicts_the_next_au_conservatively) {
  AuCadence c;
  CHECK(c.next_due_us() == 0);
  c.on_au_first(100000);
  CHECK(c.next_due_us() == 0);  // one AU: no interval yet
  c.on_au_first(116667);
  CHECK(c.next_due_us() == 116667 + 16667);
  for (int k = 2; k < 20; ++k) c.on_au_first(100000 + static_cast<uint64_t>(k) * 16667);
  const uint64_t last = 100000 + 19ull * 16667;
  CHECK(c.next_due_us() == last + 16667);
  // Low power (30 fps): the EWMA lags upward, the next due stays the shorter.
  c.on_au_first(last + 33333);
  CHECK(c.next_due_us() < last + 33333 + 33333);
  CHECK(c.next_due_us() >= last + 33333 + 16667);
  // Back to 60 fps: the last interval wins at once.
  c.on_au_first(last + 33333 + 16667);
  CHECK(c.next_due_us() == last + 33333 + 16667 + 16667);
  // A stall is ignored, not learned.
  const uint64_t t0 = last + 33333 + 16667;
  c.on_au_first(t0 + 500000);
  CHECK(c.next_due_us() == t0 + 500000 + 16667);
}
MTEST_MAIN
