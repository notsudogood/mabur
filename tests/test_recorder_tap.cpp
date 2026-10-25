#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include "mtest.h"
#include "recorder_tap.h"
using namespace maburplay;
static DmaFrame frame(uint32_t pts) { DmaFrame f{}; f.pts_us = pts; return f; }

TEST(hook_receives_the_cadence_pts) {
  RecorderTap tap; std::vector<uint64_t> got;
  tap.set([&](const DmaFrame&, uint64_t pts) { got.push_back(pts); }, 10000.0, 2);
  tap.on_flip(frame(1), 0); tap.on_flip(frame(2), 10000); tap.on_flip(frame(3), 20000);
  CHECK(got == (std::vector<uint64_t>{0, 20000}));
  CHECK(tap.tapped() == 2);
}
TEST(no_hook_means_no_call_and_no_count) {
  RecorderTap tap; tap.on_flip(frame(1), 0); CHECK(tap.tapped() == 0);
}
TEST(clear_waits_for_a_hook_call_in_flight) {
  RecorderTap tap; std::atomic<bool> entered{false}, release{false}, cleared{false};
  tap.set([&](const DmaFrame&, uint64_t) { entered = true; while (!release) std::this_thread::yield(); }, 10000.0, 1);
  std::thread rt([&] { tap.on_flip(frame(1), 0); });
  while (!entered) std::this_thread::yield();
  std::thread hk([&] { tap.clear(); cleared = true; });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(!cleared);               // clear() is held behind the call in flight
  release = true; rt.join(); hk.join();
  CHECK(cleared);
  tap.on_flip(frame(2), 10000);  // nothing installed any more
  CHECK(tap.tapped() == 1);
}
TEST(set_resets_the_cadence) {
  RecorderTap tap; std::vector<uint64_t> got;
  tap.set([&](const DmaFrame&, uint64_t p) { got.push_back(p); }, 10000.0, 1);
  tap.on_flip(frame(1), 0); tap.on_flip(frame(2), 10000);
  tap.set([&](const DmaFrame&, uint64_t p) { got.push_back(p); }, 10000.0, 1);
  tap.on_flip(frame(3), 500000);  // first flip after set: slot 0 again
  CHECK(got == (std::vector<uint64_t>{0, 10000, 0}));
}
MTEST_MAIN
