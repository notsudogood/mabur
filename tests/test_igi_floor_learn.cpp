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

// Drives the learner with 25 ms watcher passes from t0 to t1; `sends` says
// whether a submission happened in the pass ending at t. Returns the first
// floor raised, with its time.
struct Run {
  std::optional<uint8_t> f;
  uint64_t at = 0;
};
template <class Sends>
static Run drive(IgiFloorLearner& l, uint64_t t0, uint64_t t1, bool wide, bool pending,
                 uint64_t& sub, Sends sends) {
  for (uint64_t t = t0; t < t1; t += 25) {
    if (sends(t)) ++sub;
    const auto f = l.tick(t, wide, pending, sub);
    if (f) return {f, t};
  }
  return {};
}

// A full freeze at 40/80 MHz with frames waiting: one step after 250 ms
// blocked. Revert (no stall test): no floor comes back.
TEST(a_freeze_raises_the_floor_one_step) {
  IgiFloorLearner l(0);
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
  IgiFloorLearner l(0);
  uint64_t sub = 0;
  const Run r = drive(l, 0, 3000, true, true, sub, [](uint64_t t) { return (t / 25) % 2 == 0; });
  CHECK(r.f && *r.f == 0x24);
  CHECK(r.at <= 600);
  IgiFloorLearner m(0);
  uint64_t s2 = 0;
  const Run q = drive(m, 0, 3000, true, true, s2, [](uint64_t t) { return t < 900; });
  CHECK(q.f && q.at >= 1100 && q.at <= 1150);
}

// Sends moving, nothing waiting, or not wide (20 MHz or excused): never a
// stall, however long. One blocked pass in four (a busy-ish sender) is not
// either: 250 ms per 1 s window is the line.
TEST(progress_idle_or_narrow_is_not_a_stall) {
  IgiFloorLearner l(0);
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
  IgiFloorLearner l(0);
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
  IgiFloorLearner l(0x2c);
  uint64_t sub = 0;
  const Run r = drive(l, 0, 2000, true, true, sub, [](uint64_t) { return false; });
  CHECK(r.f && *r.f == 0x35);
}

// A descheduled watcher (one 2 s pass) does not count its lateness.
TEST(a_late_pass_counts_at_most_100_ms) {
  IgiFloorLearner l(0);
  CHECK(!l.tick(0, true, true, 5));
  CHECK(!l.tick(2000, true, true, 5));   // 2 s late: counts 100 ms
  CHECK(!l.tick(2025, true, true, 5));   // 125
  CHECK(l.stalls() == 0);
}

TEST(save_load_and_path) {
  CHECK(igi_floor_path("/etc/mabur.toml") == "/etc/mabur.igi_floor");
  CHECK(igi_floor_path("mabur.toml") == "mabur.igi_floor");
  const std::string p =
      (std::filesystem::temp_directory_path() / "mabur_igi_floor_test").string();
  std::remove(p.c_str());
  CHECK(!igi_floor_load(p));                       // nothing saved
  CHECK(igi_floor_save(p, 0x2c));
  const auto v = igi_floor_load(p);
  CHECK(v && *v == 0x2c);
  {
    std::FILE* f = std::fopen(p.c_str(), "w");
    std::fputs("garbage\n", f);
    std::fclose(f);
  }
  CHECK(!igi_floor_load(p));                       // unreadable = nothing learned
  {
    std::FILE* f = std::fopen(p.c_str(), "w");
    std::fputs("200\n", f);
    std::fclose(f);
  }
  CHECK(!igi_floor_load(p));                       // out of range
  {
    std::FILE* f = std::fopen(p.c_str(), "w");
    std::fputs("36\n", f);
    std::fclose(f);
  }
  const auto d = igi_floor_load(p);
  CHECK(d && *d == 36);                            // decimal works too
  std::remove(p.c_str());
}

MTEST_MAIN
