// ReleaseLoop: the release thread against a fake PresentSink that records
// every present() with its mono time, owns an eventfd as its event fd, and
// models the OSD hand-off (a pending publish that the OSD-only commit
// clears). Timing bounds are loose on the late side: ctest runs suites in
// parallel and the host thread has no real-time priority.
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "frame_regulator.h"
#include "lat_tracker.h"
#include "mtest.h"
#include "present_sink.h"
#include "release_loop.h"
#include "wake_queue.h"
#include "worker_thread.h"

using namespace std::chrono_literals;
using maburplay::DmaFrame;
using maburplay::FrameRegulator;
using maburplay::HandedFrame;
using maburplay::LatTracker;
using maburplay::PresentSink;
using maburplay::ReleaseLoop;
using maburplay::WakeQueue;

namespace {

uint64_t now_us() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000ull + static_cast<uint64_t>(ts.tv_nsec) / 1000;
}

DmaFrame frame(uint32_t pts) {
  DmaFrame f;
  f.pts_us = pts;
  f.width = 1920;
  f.height = 1080;
  f.opaque = reinterpret_cast<void*>(static_cast<uintptr_t>(pts | 1u));
  return f;
}

// Polls cond until it holds or the deadline passes.
bool wait_until(const std::function<bool()>& cond, std::chrono::milliseconds lim = 1000ms) {
  const auto end = std::chrono::steady_clock::now() + lim;
  while (!cond()) {
    if (std::chrono::steady_clock::now() >= end) return cond();
    std::this_thread::sleep_for(100us);
  }
  return true;
}

struct FakePresentSink : PresentSink {
  struct Shown {
    uint32_t pts;
    uint64_t t_us;
  };
  mutable std::mutex mu;
  std::vector<Shown> shown;
  int efd = -1;
  // present() of the first frame sleeps this long (a slow commit).
  uint64_t first_present_delay_us = 0;
  std::atomic<int> poll_calls{0}, drop_all_calls{0}, osd_calls{0}, osd_commits{0};
  std::atomic<int> osd_calls_while_holding{0};
  std::atomic<int> paced_calls{0};
  std::atomic<bool> publish_pending{false}, paced{false};
  std::atomic<uint64_t> engagements{0};
  FrameRegulator* reg = nullptr;  // to catch an OSD commit while frames are held
  uint64_t last_t_us = 0;

  // drop_all() blocks here while block_drop_all is set, until open_latch()
  // -- stands in for a wedged display (e.g. DrmPresenter's drop_all forcing
  // a synchronous plane-disable) without a real one.
  std::atomic<bool> block_drop_all{false};
  std::mutex latch_mu;
  std::condition_variable latch_cv;
  bool latch_open = false;
  void open_latch() {
    std::lock_guard<std::mutex> lk(latch_mu);
    latch_open = true;
    latch_cv.notify_all();
  }

  // broken_fd: event_fd() is the read end of a pipe whose write end is
  // closed immediately, so epoll reports EPOLLHUP on it persistently --
  // stands in for a dead display fd (a hotplug yank, a crashed KMS driver)
  // without needing a real display.
  explicit FakePresentSink(bool broken_fd = false) {
    if (broken_fd) {
      int fds[2] = {-1, -1};
      if (pipe2(fds, O_CLOEXEC) == 0) {
        close(fds[1]);
        efd = fds[0];
      }
    } else {
      efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    }
  }
  ~FakePresentSink() override { close(efd); }

  bool present(const DmaFrame& f) override {
    std::lock_guard<std::mutex> lk(mu);
    if (shown.empty() && first_present_delay_us)
      std::this_thread::sleep_for(std::chrono::microseconds(first_present_delay_us));
    last_t_us = now_us();
    shown.push_back({f.pts_us, last_t_us});
    return true;
  }
  void poll_events() override {
    uint64_t v;
    (void)!read(efd, &v, sizeof v);
    poll_calls.fetch_add(1);
  }
  int event_fd() const override { return efd; }
  void drop_all() override {
    drop_all_calls.fetch_add(1);
    if (block_drop_all.load()) {
      std::unique_lock<std::mutex> lk(latch_mu);
      latch_cv.wait(lk, [this] { return latch_open; });
    }
  }
  void set_paced(bool p) override {
    paced.store(p);
    paced_calls.fetch_add(1);
  }
  uint64_t mailbox_engagements() const override { return engagements.load(); }
  CommitInfo last_commit() const override {
    std::lock_guard<std::mutex> lk(mu);
    return {last_t_us, 1};
  }
  bool osd_publish_pending() const override { return publish_pending.load(); }
  void commit_osd_if_pending() override {
    osd_calls.fetch_add(1);
    if (reg && reg->holding()) osd_calls_while_holding.fetch_add(1);
    if (publish_pending.exchange(false)) osd_commits.fetch_add(1);
  }

  size_t presents() const {
    std::lock_guard<std::mutex> lk(mu);
    return shown.size();
  }
  std::vector<Shown> snapshot() const {
    std::lock_guard<std::mutex> lk(mu);
    return shown;
  }
  void signal() {
    const uint64_t one = 1;
    (void)!write(efd, &one, sizeof one);
  }
};

constexpr uint64_t kVb = 16'667;   // panel period µs (the estimator's 60 Hz seed)
constexpr uint64_t kLead = 3'000;  // vsync_lead_ms 3

// Warms a servo regulator: 8 exact flips ending at last_flip.
void warm(FrameRegulator& reg, uint64_t last_flip) {
  for (int i = 7; i >= 0; --i) reg.on_flip(last_flip - static_cast<uint64_t>(i) * kVb, true);
}

// Warms the grid so a frame arriving at t lands on the vblank whose release
// is t + ahead_us (ahead_us < one period); returns that release.
uint64_t warm_for_release(FrameRegulator& reg, uint64_t t, uint64_t ahead_us) {
  const uint64_t v = t + ahead_us + kLead;
  warm(reg, v - kVb);
  return v - kLead;
}

// Every queue and tracker the loop needs; each test declares its sink
// before this so ~ReleaseLoop joins the thread before the sink goes away.
struct Rig {
  LatTracker lat;
  FrameRegulator reg;
  WakeQueue<HandedFrame> in;
  WakeQueue<DmaFrame> rel;
  ReleaseLoop rl;
  Rig(PresentSink* sink, int regulate_ms, bool vsync, int sched_cap_ms = 0, int prio = 0)
      : reg(regulate_ms, vsync, static_cast<int>(kLead / 1000), 0, sched_cap_ms),
        rl(reg, lat, in, rel, sink, ReleaseLoop::Cfg{prio}) {}
  void hand(uint32_t pts, uint64_t t_decoded) { in.push(HandedFrame{frame(pts), t_decoded}); }
  std::vector<DmaFrame> released() {
    std::vector<DmaFrame> v;
    rel.drain(&v);
    return v;
  }
};

}  // namespace

TEST(a_handed_frame_is_presented_at_its_release_not_before) {
  FakePresentSink sink;
  Rig r(&sink, 12, true);
  sink.reg = &r.reg;
  r.rl.start();
  const uint64_t t = now_us();
  const uint64_t release = warm_for_release(r.reg, t, 10'000);  // handed 10 ms early
  r.hand(0, t);
  CHECK(wait_until([&] { return sink.presents() == 1; }));
  const auto s = sink.snapshot();
  REQUIRE(s.size() == 1);
  std::printf("  present at release %+lld us\n",
              static_cast<long long>(s[0].t_us) - static_cast<long long>(release));
  CHECK(s[0].t_us + 300 >= release);
  CHECK(s[0].t_us <= release + 4'000);  // generous: no RT priority under ctest -j
  CHECK(r.reg.late_count() == 0);
  CHECK(sink.paced.load());
  CHECK(r.rel.size() == 0);  // the sink owns a presented frame
  r.rl.stop_and_join();
  CHECK(sink.osd_calls_while_holding.load() == 0);
}

TEST(late_handover_presents_immediately) {
  FakePresentSink sink;
  Rig r(&sink, 12, false);
  r.rl.start();
  // Decoded 20 ms ago with a 12 ms hold: its release passed 8 ms ago.
  const uint64_t t = now_us();
  r.hand(0, t - 20'000);
  CHECK(wait_until([&] { return sink.presents() == 1; }));
  const auto s = sink.snapshot();
  REQUIRE(s.size() == 1);
  std::printf("  late hand-over presented after %lld us\n",
              static_cast<long long>(s[0].t_us - t));
  CHECK(s[0].t_us - t <= 5'000);  // at once (1 ms unloaded; slack for ctest -j)
  CHECK(r.reg.late_count() == 1);
  CHECK(!r.reg.holding());
  CHECK(r.rel.size() == 0);
}

TEST(a_pair_partner_handed_just_after_its_partner_presented_waits_a_period) {
  // A is held and presented at its release (vblank V's). B, its pair
  // partner, was decoded before that release but is handed over after A
  // went out: B's arrival maps to V, which A already has, so B must wait
  // for V + period. Presented at once it would queue behind A's pending
  // flip and A would show twice.
  FakePresentSink sink;
  Rig r(&sink, 12, true);
  r.rl.start();
  const uint64_t t = now_us();
  const uint64_t rel_a = warm_for_release(r.reg, t, 10'000);
  r.hand(0, t);
  REQUIRE(wait_until([&] { return sink.presents() == 1; }));
  r.hand(static_cast<uint32_t>(kVb), rel_a - 100);
  const uint64_t handed = now_us();
  CHECK(handed < rel_a + kVb - 2'000);  // handed well before V + period's release
  const int64_t until = static_cast<int64_t>(rel_a + kVb - 1'000) - static_cast<int64_t>(now_us());
  if (until > 0) std::this_thread::sleep_for(std::chrono::microseconds(until));
  CHECK(sink.presents() == 1);  // nothing more before V + period's release
  REQUIRE(wait_until([&] { return sink.presents() == 2; }));
  const auto s = sink.snapshot();
  REQUIRE(s.size() == 2);
  std::printf("  B presented %+lld us from V+period's release\n",
              static_cast<long long>(s[1].t_us) - static_cast<long long>(rel_a + kVb));
  CHECK(s[1].pts == kVb);
  CHECK(s[1].t_us + 300 >= rel_a + kVb);
  CHECK(r.reg.late_count() == 0);
  CHECK(r.rel.size() == 0);
}

TEST(displaced_frames_go_to_the_release_queue_not_the_sink) {
  FakePresentSink sink;
  Rig r(&sink, 12, false);  // fallback: one slot, so the second displaces the first
  r.rl.start();
  const uint64_t t = now_us();
  r.hand(0, t);
  r.hand(16'684, t + 1'000);
  CHECK(wait_until([&] { return sink.presents() == 1; }));
  std::this_thread::sleep_for(30ms);
  const auto rel = r.released();
  CHECK(rel.size() == 1);
  if (rel.size() == 1) CHECK(rel[0].pts_us == 0);
  const auto s = sink.snapshot();
  CHECK(s.size() == 1);
  if (s.size() == 1) CHECK(s[0].pts == 16'684);
}

TEST(park_returns_every_held_frame_once) {
  FakePresentSink sink;
  Rig r(&sink, 12, true);
  sink.reg = &r.reg;
  r.rl.start();
  const uint64_t t = now_us();
  warm(r.reg, t - 1'000);
  // Arrival stamps 40 ms ahead of now put all three releases on distinct
  // vblanks 40-80 ms out, far enough that the park always beats them.
  for (uint32_t i = 0; i < 3; ++i) r.hand(i * kVb, t + 40'000 + i * kVb);
  CHECK(wait_until([&] { return r.reg.held_count() == 3; }));
  CHECK(r.reg.holding());
  CHECK(r.rl.park(100ms));
  CHECK(sink.drop_all_calls.load() == 1);
  CHECK(r.rel.size() == 3);
  CHECK(!r.reg.holding());
  // A parked thread touches nothing on the sink: no late poll_events,
  // no OSD commit, no paced flip-flop, while it sits in park_wait_().
  const int poll_at_park = sink.poll_calls.load();
  const int osd_at_park = sink.osd_calls.load();
  const bool paced_at_park = sink.paced.load();
  const int paced_calls_at_park = sink.paced_calls.load();
  std::this_thread::sleep_for(100ms);
  CHECK(sink.presents() == 0);
  CHECK(r.rel.size() == 3);  // each returned once, nothing more
  CHECK(sink.poll_calls.load() == poll_at_park);
  CHECK(sink.osd_calls.load() == osd_at_park);
  CHECK(sink.paced.load() == paced_at_park);
  CHECK(sink.paced_calls.load() == paced_calls_at_park);  // not just the bool: no flip-flop either
  // A frame handed while parked goes straight back, never to the sink.
  r.hand(3 * kVb, now_us());
  CHECK(wait_until([&] { return r.rel.size() == 4; }));
  CHECK(sink.presents() == 0);
  CHECK(r.rl.park(100ms));  // already parked: confirms at once
  CHECK(sink.drop_all_calls.load() == 1);
  r.rl.stop_and_join();      // from parked
  CHECK(sink.osd_calls_while_holding.load() == 0);
}

TEST(park_resume_presents_afterwards_on_the_new_sink) {
  FakePresentSink sink, sink2;
  Rig r(&sink, 12, false);
  r.rl.start();
  r.hand(0, now_us());
  CHECK(wait_until([&] { return sink.presents() == 1; }));
  CHECK(r.rl.park(100ms));
  r.rl.set_sink(&sink2);
  r.rl.resume();
  const uint64_t t = now_us();
  r.hand(16'684, t);
  CHECK(wait_until([&] { return sink2.presents() == 1; }));
  CHECK(sink.presents() == 1);
  const auto s = sink2.snapshot();
  if (s.size() == 1) CHECK(s[0].t_us >= t + 11'000);  // held for its release, not dumped
  // The new sink's event fd drives poll_events().
  const int before = sink2.poll_calls.load();
  sink2.signal();
  CHECK(wait_until([&] { return sink2.poll_calls.load() > before; }, 200ms));
  // Park and resume again without a sink change: still presenting.
  CHECK(r.rl.park(100ms));
  r.rl.resume();
  r.hand(2 * 16'684, now_us());
  CHECK(wait_until([&] { return sink2.presents() == 2; }));
}

TEST(park_before_start_is_immediate) {
  FakePresentSink sink;
  Rig r(&sink, 12, false);
  FrameRegulator::Displaced disp;
  const uint64_t t = now_us();
  CHECK(!r.reg.offer(frame(0), t, t, &disp));
  CHECK(r.rl.park(10ms));
  CHECK(sink.drop_all_calls.load() == 1);
  CHECK(r.rel.size() == 1);
  r.rl.resume();
  r.rl.start();
  r.hand(16'684, now_us());
  CHECK(wait_until([&] { return sink.presents() == 1; }));
}

TEST(runs_without_rt_priority) {
  FakePresentSink sink;
  Rig r(&sink, 12, false, 0, /*prio=*/10);
  r.rl.start();
  rlimit rl{};
  getrlimit(RLIMIT_RTPRIO, &rl);
  std::printf("  rt_applied=%d euid=%u rlimit_rtprio=%llu\n", r.rl.rt_applied() ? 1 : 0,
              static_cast<unsigned>(geteuid()), static_cast<unsigned long long>(rl.rlim_cur));
  CHECK(!r.rl.rt_applied() || geteuid() == 0 || rl.rlim_cur >= 10);
  r.hand(0, now_us());
  CHECK(wait_until([&] { return sink.presents() == 1; }));
}

TEST(osd_only_commit_clears_a_pending_publish) {
  FakePresentSink sink;
  Rig r(&sink, 12, false);
  sink.reg = &r.reg;
  r.rl.start();
  std::this_thread::sleep_for(30ms);  // idle: nothing held, no events
  const auto t0 = std::chrono::steady_clock::now();
  sink.publish_pending.store(true);
  CHECK(wait_until([&] { return sink.osd_commits.load() == 1; }, 250ms));  // 50 ms unloaded
  std::printf("  osd-only commit after %lld us\n",
              static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now() - t0)
                                         .count()));
  CHECK(!sink.publish_pending.load());
  CHECK(sink.osd_calls_while_holding.load() == 0);
}

TEST(stats_window_reports_release_lateness) {
  FakePresentSink sink;
  // The first present() blocks a period plus 3 ms, so the second frame,
  // due one period after the first, goes out at least 3 ms late.
  sink.first_present_delay_us = kVb + 3'000;
  Rig r(&sink, 12, true);
  r.rl.start();
  const uint64_t t = now_us();
  warm_for_release(r.reg, t, 10'000);
  r.hand(0, t);
  r.hand(static_cast<uint32_t>(kVb), t);  // same natural slot: chained to the next vblank
  CHECK(wait_until([&] { return sink.presents() == 2; }));
  const auto st = r.rl.take_stats();
  std::printf("  n=%u rlate p99=%u max=%u hand p99=%u max=%u jitter=%.2fms\n", st.n,
              st.rlate_p99_us, st.rlate_max_us, st.hand_p99_us, st.hand_max_us,
              st.present_jitter_ema_ms);
  CHECK(st.n == 2);
  CHECK(st.rlate_max_us >= 1'900);
  CHECK(st.rlate_p99_us == st.rlate_max_us);  // two samples: p99 is the larger
  CHECK(st.hand_max_us < 50'000);
  const auto st2 = r.rl.take_stats();  // the window resets
  CHECK(st2.n == 0);
  CHECK(st2.rlate_max_us == 0);
  CHECK(st2.hand_max_us == 0);
}

TEST(margins_reach_housekeeping_once_per_offer) {
  FakePresentSink sink;
  Rig r(&sink, 12, true, /*sched_cap_ms=*/5);
  r.rl.start();
  const uint64_t t = now_us();
  warm(r.reg, t - 1'000);
  for (uint32_t i = 0; i < 3; ++i) r.hand(2 + i * kVb, t + i * 500);
  std::vector<ReleaseLoop::MarginSample> m;
  CHECK(wait_until([&] {
    r.rl.take_margins(&m);
    return m.size() >= 3;
  }));
  std::this_thread::sleep_for(50ms);
  r.rl.take_margins(&m);
  CHECK(m.size() == 3);
  for (const auto& s : m) {
    CHECK(s.period_us > 16'000 && s.period_us < 17'000);
    CHECK(s.t_us >= t);
  }
  std::vector<ReleaseLoop::MarginSample> again;
  r.rl.take_margins(&again);
  CHECK(again.empty());  // taken means gone
}

TEST(no_sink_due_frames_go_to_the_release_queue) {
  Rig r(nullptr, 12, false);
  r.rl.start();
  const uint64_t t = now_us();
  r.hand(0, t);
  CHECK(wait_until([&] { return r.rel.size() == 1; }));
  const uint64_t got = now_us();
  CHECK(got >= t + 12'000 - 300);  // released at its time, not at the hand-over
  const auto rel = r.released();
  CHECK(rel.size() == 1);
  if (rel.size() == 1) CHECK(rel[0].pts_us == 0);
  CHECK(r.rl.take_stats().n == 1);
}

TEST(two_mailbox_engagements_within_100ms_heal_once) {
  FakePresentSink sink;
  Rig r(&sink, 12, false);
  r.rl.start();
  std::this_thread::sleep_for(20ms);
  sink.engagements.store(1);
  sink.signal();
  CHECK(wait_until([&] { return sink.poll_calls.load() > 0; }));
  std::this_thread::sleep_for(20ms);
  sink.engagements.store(2);
  sink.signal();
  CHECK(wait_until([&] { return r.rl.heals() == 1; }, 500ms));
  // A third engagement right after needs a fresh pair (and 150 ms) first.
  sink.engagements.store(3);
  sink.signal();
  std::this_thread::sleep_for(30ms);
  CHECK(r.rl.heals() == 1);
}

TEST(dead_sink_fd_falls_back_to_the_idle_tick_without_spinning) {
  FakePresentSink sink(/*broken_fd=*/true);
  Rig r(&sink, 12, false);
  r.rl.start();
  // The broken fd's HUP is visible to epoll immediately; give the loop a
  // moment to see it, DEL it, and settle onto the idle tick.
  std::this_thread::sleep_for(50ms);
  const int before = sink.poll_calls.load();
  std::this_thread::sleep_for(100ms);
  const int after = sink.poll_calls.load();
  // A spinning thread would run this hundreds of thousands of times in
  // 100 ms; the idle tick is 10 ms, so a healthy loop wakes roughly 10
  // times here. Generous bound for a loaded ctest -j run.
  std::printf("  poll_events calls in 100ms after the dead fd: %d\n", after - before);
  CHECK(after - before < 50);
  // Still presents afterwards: the dead fd cost it an epoll registration,
  // not the ability to show frames.
  r.hand(0, now_us());
  CHECK(wait_until([&] { return sink.presents() == 1; }));
}

TEST(park_times_out_while_drop_all_blocks_then_a_later_park_succeeds) {
  FakePresentSink sink;
  Rig r(&sink, 12, false);
  r.rl.start();
  sink.block_drop_all.store(true);
  const auto t0 = std::chrono::steady_clock::now();
  // drop_all() is stuck on the latch: park(100ms) must still return false
  // at its limit rather than block for as long as drop_all() does.
  CHECK(!r.rl.park(100ms));
  const auto dt = std::chrono::steady_clock::now() - t0;
  std::printf("  park(100ms) returned false after %lldms\n",
              static_cast<long long>(
                  std::chrono::duration_cast<std::chrono::milliseconds>(dt).count()));
  CHECK(dt < 300ms);  // bounded near the limit, not hung on the blocked drop_all
  CHECK(sink.drop_all_calls.load() == 1);  // the call happened; it just hasn't returned yet
  // State stays consistent while the thread is still inside the blocked
  // drop_all(): nothing crashes, and once the latch opens the loop picks
  // back up as a normal running loop (never entered park_wait_, since this
  // park() already withdrew its request).
  const int polls_before = sink.poll_calls.load();
  sink.open_latch();
  // Let the still-running park_work_() finish its own cleanup (including
  // draining the hand-over queue back to the decoder, since as far as it
  // knows it is still parking) before hand()ing a frame of our own --
  // otherwise this frame could race into that same drain and be returned
  // unpresented instead of exercising the resumed loop. A poll_events()
  // call only happens from iterate_(), which park_work_() never reaches
  // until after it returns, so this is a safe fence.
  CHECK(wait_until([&] { return sink.poll_calls.load() > polls_before; }));
  r.hand(0, now_us());
  CHECK(wait_until([&] { return sink.presents() == 1; }));
  // A later park(), with drop_all() no longer blocking, succeeds normally.
  CHECK(r.rl.park(200ms));
  CHECK(sink.drop_all_calls.load() == 2);
}

TEST(worker_threads_start_with_the_housekeeping_signals_blocked) {
  // start_worker_thread() (the release thread, the decode pump and the feed
  // thread all start through it): the new thread's mask is the caller's
  // plus SIGINT/SIGTERM/SIGUSR1, nothing else, and the caller's own mask
  // comes back unchanged.
  sigset_t before;
  pthread_sigmask(SIG_SETMASK, nullptr, &before);
  sigset_t inside;
  sigemptyset(&inside);
  std::thread t = maburplay::start_worker_thread(
      [&inside] { pthread_sigmask(SIG_SETMASK, nullptr, &inside); });
  t.join();
  sigset_t after;
  pthread_sigmask(SIG_SETMASK, nullptr, &after);
  for (int sig : {SIGINT, SIGTERM, SIGUSR1}) CHECK(sigismember(&inside, sig) == 1);
  for (int sig = 1; sig < 32; ++sig) {
    if (sig == SIGINT || sig == SIGTERM || sig == SIGUSR1 || sig == SIGKILL || sig == SIGSTOP)
      continue;
    CHECK(sigismember(&inside, sig) == sigismember(&before, sig));
    CHECK(sigismember(&after, sig) == sigismember(&before, sig));
  }
  for (int sig : {SIGINT, SIGTERM, SIGUSR1})
    CHECK(sigismember(&after, sig) == sigismember(&before, sig));
}

MTEST_MAIN
