#include <cstdio>
#include <filesystem>
#include <string>

#include "igi_floor_learn.h"
#include "mtest.h"
using namespace mabur;

TEST(steps_up_and_stops_at_the_top) {
  CHECK(igi_floor_next(0) == 0x24);
  CHECK(igi_floor_next(0x10) == 0x24);
  CHECK(igi_floor_next(0x24) == 0x2c);
  CHECK(igi_floor_next(0x30) == 0x35);
  CHECK(igi_floor_next(0x35) == 0x35);
  CHECK(igi_floor_next(0x50) == 0x35);
}

// Drives the learner with 25 ms watcher passes from t0 to t1 at `tuned`
// MHz with rung frames of the same width; `sends` says whether a submission
// happened in the pass ending at t. Returns the first floor raised, with
// its time.
struct Run {
  std::optional<uint8_t> f;
  uint64_t at = 0;
};
template <class Sends>
static Run drive(IgiFloorLearner& l, uint64_t t0, uint64_t t1, bool wide, bool pending,
                 uint64_t& sub, Sends sends, int tuned = 40) {
  for (uint64_t t = t0; t < t1; t += 25) {
    if (sends(t)) ++sub;
    IgiPass p;
    p.now_ms = t;
    p.tuned_w = wide ? tuned : 20;
    p.rung_w = p.tuned_w;
    p.pending = pending;
    p.submitted = sub;
    const IgiEvent ev = l.tick(p);
    if (ev.raised) return {ev.raised, t};
  }
  return {};
}
static IgiSaved off() { return IgiSaved{}; }

// A full freeze at 40/80 MHz with frames waiting: one step after 250 ms
// blocked. Revert (no stall test): no floor comes back.
TEST(a_freeze_raises_the_floor_one_step) {
  IgiFloorLearner l(off());
  uint64_t sub = 0;
  CHECK(!drive(l, 0, 1000, true, true, sub, [](uint64_t) { return true; }).f);   // healthy
  const Run r = drive(l, 1000, 3000, true, true, sub, [](uint64_t) { return false; });
  CHECK(r.f && *r.f == 0x24);
  CHECK(r.at >= 1200 && r.at <= 1250);            // freeze from 975: 250 ms blocked
  CHECK(l.floor() == 0x24 && l.stalls() == 1);
}

// Sends in bursts between hangs (released ~20 times a second, as the GS's
// uplink frames might): still a stall, at about twice a freeze's time.
// A freeze starting late in a second is caught 250 ms in all the same
// (rolling window).
TEST(burst_between_hangs_is_a_stall) {
  IgiFloorLearner l(off());
  uint64_t sub = 0;
  const Run r = drive(l, 0, 3000, true, true, sub, [](uint64_t t) { return (t / 25) % 2 == 0; });
  CHECK(r.f && *r.f == 0x24);
  CHECK(r.at <= 600);
  IgiFloorLearner m(off());
  uint64_t s2 = 0;
  const Run q = drive(m, 0, 3000, true, true, s2, [](uint64_t t) { return t < 900; });
  CHECK(q.f && q.at >= 1100 && q.at <= 1150);
}

// Sends moving, nothing waiting, or not wide (20 MHz or excused): never a
// stall, however long. One blocked pass in four (a busy-ish sender) is not
// either: 250 ms per 1 s window is the line.
TEST(progress_idle_or_narrow_is_not_a_stall) {
  IgiFloorLearner l(off());
  uint64_t sub = 0;
  auto never = [](uint64_t) { return false; };
  CHECK(!drive(l, 0, 5000, true, true, sub, [](uint64_t) { return true; }).f);
  CHECK(!drive(l, 5000, 10000, true, false, sub, never).f);    // nothing waiting
  CHECK(!drive(l, 10000, 15000, false, true, sub, never).f);   // 20 MHz or excused
  CHECK(!drive(l, 15000, 25000, true, true, sub, [](uint64_t t) { return (t / 25) % 5 != 0; }).f);
  CHECK(l.floor() == 0 && l.stalls() == 0);
}

// After a step the settle second holds the next verdict; a freeze that
// outlasts it steps again, and nothing goes past 0x35.
TEST(settle_then_step_again_and_cap) {
  IgiFloorLearner l(off());
  uint64_t sub = 0;
  auto never = [](uint64_t) { return false; };
  Run r = drive(l, 0, 5000, true, true, sub, never);
  CHECK(r.f && *r.f == 0x24);
  const uint64_t t1 = r.at;
  r = drive(l, t1 + 25, t1 + 5000, true, true, sub, never);
  CHECK(r.f && *r.f == 0x2c);
  CHECK(r.at >= t1 + 1200);                      // settle 1 s, then 250 ms blocked
  r = drive(l, r.at + 25, r.at + 5000, true, true, sub, never);
  CHECK(r.f && *r.f == 0x35);
  CHECK(!drive(l, r.at + 25, r.at + 5000, true, true, sub, never).f);   // at the top
  CHECK(l.floor() == 0x35 && l.top_stalls() >= 1);
}

// A unit that saved 0x2c starts there: its first stall goes to 0x35.
TEST(starts_from_the_saved_value) {
  IgiFloorLearner l(IgiSaved{0x2c, false, false});
  uint64_t sub = 0;
  const Run r = drive(l, 0, 2000, true, true, sub, [](uint64_t) { return false; });
  CHECK(r.f && *r.f == 0x35);
}

// A descheduled watcher (one 2 s pass) does not count its lateness.
TEST(a_late_pass_counts_at_most_100_ms) {
  IgiFloorLearner l(off());
  IgiPass p;
  p.tuned_w = p.rung_w = 40;
  p.pending = true;
  p.submitted = 5;
  p.now_ms = 0;
  CHECK(!l.tick(p).raised);
  p.now_ms = 2000;  // 2 s late: counts 100 ms
  CHECK(!l.tick(p).raised);
  p.now_ms = 2025;  // 125
  CHECK(!l.tick(p).raised);
  CHECK(l.stalls() == 0);
}

// d19 first-boot calibration: 10 s of clean sending at a width, with that
// width's rung frames, marks it verified (once); 40 and 80 are separate.
// Revert (no marks): verified(40) stays false.
TEST(clean_time_verifies_each_width) {
  IgiFloorLearner l(off(), 250, 1000, 1000, 10000);
  uint64_t sub = 0;
  bool changed = false;
  for (uint64_t t = 0; t <= 10100; t += 25) {
    IgiPass p;
    p.now_ms = t;
    p.tuned_w = p.rung_w = 40;
    p.pending = true;
    p.submitted = ++sub;
    changed = l.tick(p).marks_changed || changed;
  }
  CHECK(changed && l.verified(40) && !l.verified(80));
  CHECK(l.saved().v40 && !l.saved().v80);
  // 80: rung frames at 20 in an 80 tune (the width hold) do not count.
  for (uint64_t t = 10200; t <= 25000; t += 25) {
    IgiPass p;
    p.now_ms = t;
    p.tuned_w = 80;
    p.rung_w = 20;
    p.submitted = ++sub;
    l.tick(p);
  }
  CHECK(!l.verified(80) && l.clean_ms(80) == 0);
}

// A stall at a width clears that width's mark and clean time, not the
// other's; an excused pass (a spike in the span) neither counts nor stalls.
TEST(a_stall_clears_its_own_mark) {
  IgiFloorLearner l(IgiSaved{0x24, true, true}, 250, 1000, 1000, 10000);
  uint64_t sub = 0;
  const Run r = drive(l, 0, 3000, true, true, sub, [](uint64_t) { return false; }, 80);
  CHECK(r.f && *r.f == 0x2c);
  CHECK(l.verified(40) && !l.verified(80));
  IgiFloorLearner m(off());
  for (uint64_t t = 0; t <= 20000; t += 25) {
    IgiPass p;
    p.now_ms = t;
    p.tuned_w = p.rung_w = 40;
    p.excused = true;
    p.pending = true;
    p.submitted = 0;
    CHECK(!m.tick(p).raised);
  }
  CHECK(!m.verified(40) && m.stalls() == 0);
}

TEST(save_load_and_path) {
  CHECK(igi_floor_path("/etc/mabur.toml") == "/etc/mabur.igi_floor");
  CHECK(igi_floor_path("mabur.toml") == "mabur.igi_floor");
  const std::string p =
      (std::filesystem::temp_directory_path() / "mabur_igi_floor_test").string();
  std::remove(p.c_str());
  CHECK(!igi_floor_load(p));                       // nothing saved
  CHECK(igi_floor_save(p, IgiSaved{0x2c, true, false}));
  auto v = igi_floor_load(p);
  CHECK(v && v->floor == 0x2c && v->v40 && !v->v80);
  CHECK(igi_floor_save(p, IgiSaved{0x35, true, true}));
  v = igi_floor_load(p);
  CHECK(v && v->floor == 0x35 && v->v40 && v->v80);
  auto put = [&](const char* text) {
    std::FILE* f = std::fopen(p.c_str(), "w");
    std::fputs(text, f);
    std::fclose(f);
  };
  put("0x24\n");                                   // a d18 file: the floor alone
  v = igi_floor_load(p);
  CHECK(v && v->floor == 0x24 && !v->v40 && !v->v80);
  put("garbage\n");
  CHECK(!igi_floor_load(p));                       // unreadable = nothing learned
  put("200\n");
  CHECK(!igi_floor_load(p));                       // out of range
  put("36\n");
  v = igi_floor_load(p);
  CHECK(v && v->floor == 36);                      // decimal works too
  std::remove(p.c_str());
}

MTEST_MAIN
