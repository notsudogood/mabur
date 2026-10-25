#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "frame_handoff.h"
#include "mtest.h"

TEST(ready_is_fifo_and_release_is_separate) {
  maburplay::FrameHandoff<int> h;
  h.push_ready(1);
  h.push_ready(2);
  h.push_release(9);
  int v = 0;
  CHECK(h.pop_ready(&v) && v == 1);
  CHECK(h.pop_ready(&v) && v == 2);
  CHECK(!h.pop_ready(&v));
  auto r = h.take_releases();
  CHECK(r.size() == 1 && r[0] == 9);
  CHECK(h.take_releases().empty());
}

// flush/destroy must see every frame still in either queue, exactly once.
TEST(drain_all_returns_both_queues) {
  maburplay::FrameHandoff<int> h;
  h.push_ready(1);
  h.push_release(2);
  h.push_ready(3);
  auto all = h.drain_all();
  CHECK(all.size() == 3);
  int sum = 0;
  for (int x : all) sum += x;
  CHECK(sum == 6);
  CHECK(h.drain_all().empty());
  CHECK(h.ready_size() == 0);
}

// Producer on one thread, consumer + releaser on another: every pushed
// frame is popped once and released once, none lost, none duplicated.
TEST(two_threads_conserve_frames) {
  maburplay::FrameHandoff<int> h;
  constexpr int kN = 20000;
  std::atomic<bool> done{false};
  long released_sum = 0;
  int released_n = 0;
  std::thread feed([&] {
    for (int i = 1; i <= kN; ++i) {
      for (int r : h.take_releases()) { released_sum += r; ++released_n; }
      h.push_ready(i);
    }
    done.store(true);
  });
  int popped = 0;
  for (;;) {
    int v;
    if (h.pop_ready(&v)) { ++popped; h.push_release(v); continue; }
    if (done.load() && h.ready_size() == 0) break;
  }
  feed.join();
  for (int r : h.take_releases()) { released_sum += r; ++released_n; }
  CHECK(popped == kN);
  CHECK(released_n == kN);
  CHECK(released_sum == static_cast<long>(kN) * (kN + 1) / 2);
}

// The decode-output thread sleeps inside poll() until a frame is ready: an
// empty queue holds the caller for the whole timeout and then reports no
// frame.
TEST(pop_ready_wait_times_out_empty) {
  maburplay::FrameHandoff<int> h;
  int v = -1;
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(!h.pop_ready_wait(&v, std::chrono::milliseconds(20)));
  const auto waited = std::chrono::steady_clock::now() - t0;
  CHECK(waited >= std::chrono::milliseconds(20));
  CHECK(v == -1);
}

// A push from the feed thread ends the wait at once: the frame comes out
// well before the timeout, not after it.
TEST(pop_ready_wait_wakes_on_push) {
  maburplay::FrameHandoff<int> h;
  std::thread feed([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    h.push_ready(7);
  });
  int v = 0;
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(h.pop_ready_wait(&v, std::chrono::milliseconds(500)));
  const auto waited = std::chrono::steady_clock::now() - t0;
  feed.join();
  CHECK(v == 7);
  CHECK(waited < std::chrono::milliseconds(200));
  // Already-queued frames come out without waiting.
  h.push_ready(8);
  CHECK(h.pop_ready_wait(&v, std::chrono::milliseconds(500)) && v == 8);
}

TEST(entry_gate) {
  maburplay::EntryGate g;
  CHECK(!g.open());
  CHECK(!g.admit(false));
  CHECK(!g.admit(false));
  CHECK(g.admit(true));     // opens on the entry point and admits it
  CHECK(g.admit(false));    // stays open for P pictures
  CHECK(g.gated() == 2);
  g.arm();                  // a flush
  CHECK(!g.admit(false));
  CHECK(g.admit(true));
  CHECK(g.gated() == 3);
}

// A streamed picture may start only where the gate would admit it: the gate
// is open, or this AU is itself an entry point. Asking does not change the
// gate or its count.
TEST(entry_gate_would_admit) {
  maburplay::EntryGate g;
  CHECK(!g.would_admit(false));
  CHECK(g.would_admit(true));
  CHECK(!g.open());
  CHECK(g.gated() == 0);
  CHECK(g.admit(true));
  CHECK(g.would_admit(false));
}

MTEST_MAIN
