#ifndef MABUR_PLAYER_RELEASE_LOOP_H_
#define MABUR_PLAYER_RELEASE_LOOP_H_

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "frame_regulator.h"
#include "lat_tracker.h"
#include "pi_mutex.h"
#include "present_sink.h"
#include "video_backend.h"
#include "wake_queue.h"

namespace maburplay {

// The release thread. It owns the FrameRegulator's release decisions and
// every call into the PresentSink, so a frame goes to the display at its
// release time whatever housekeeping is doing. It sleeps in epoll on the
// hand-over queue, the sink's event fd, an absolute CLOCK_MONOTONIC timerfd
// armed to the next release, and a control eventfd (park/stop). It never
// calls into the decoder: displaced frames, and frames it cannot present,
// go back through `releases`, which the decode pump drains. A frame given
// to the sink belongs to the sink, which returns it through its own release
// path once it is off screen.
//
// Every wake, in this order:
//  1. sink->poll_events();
//  2. every due frame is released (they are older than anything newly
//     handed over, so they go first: an offer that presents at once
//     displaces every frame still held);
//  3. the hand-over queue is drained and each frame offered; one the
//     regulator does not hold is presented at once; displaced frames go to
//     `releases`; each offer's phase margin is kept for housekeeping;
//  4. sink->set_paced(servo locked), the chain-break detector, and the
//     OSD-only commit when no frame is held;
//  5. the timer is re-armed: to the next release when a frame is held, to
//     an idle tick otherwise (a published OSD needs a commit even when no
//     video flows, and nothing else wakes the thread for it).
// The clock is re-read after every present, so a slow commit never makes a
// later decision in the same wake stale.
//
// Without a sink (no display) a frame due for release, or presented at
// once, goes straight to `releases` after lat.on_present, and the thread
// keeps no idle tick.
//
// Park: the thread drops everything the sink holds (drop_all), returns
// every held frame and every frame still in the hand-over queue to
// `releases`, clears the regulator's output history
// (FrameRegulator::reset_output_history), disarms the timer and waits on
// the control fd. Frames handed
// over while parked go straight back to `releases` (no present until
// resume). park() returns once the thread confirms; a park that times out
// is withdrawn and leaves the thread running as before. Before start()
// (or after the thread ended) park() does the same work on the caller.
//
// Threads: start/park/resume/set_sink/stop_and_join from one controlling
// thread; take_margins/take_stats/rt_applied/heals from anywhere.
class ReleaseLoop {
 public:
  struct Cfg {
    int rt_priority = 10;  // SCHED_FIFO priority; 0 = do not try
  };
  // One offer's phase margin (FrameRegulator::take_margin), with the panel
  // period and the mono time it was taken at.
  struct MarginSample {
    double margin_us;
    double period_us;
    uint64_t t_us;
  };
  // The window since the last take_stats(). rlate: µs from a held frame's
  // scheduled release to the moment it was released (n counts those
  // releases). hand: µs from a frame's decode stamp to its offer. The
  // present-jitter EMA (|Δ present interval|, ms) runs across windows.
  struct Stats {
    uint32_t n = 0;
    uint32_t rlate_p99_us = 0, rlate_max_us = 0;
    uint32_t hand_p99_us = 0, hand_max_us = 0;
    double present_jitter_ema_ms = 0;
  };

  ReleaseLoop(FrameRegulator& reg, LatTracker& lat, WakeQueue<HandedFrame>& in,
              WakeQueue<DmaFrame>& releases, PresentSink* sink /*nullable*/, Cfg cfg);
  ~ReleaseLoop();  // stops and joins
  ReleaseLoop(const ReleaseLoop&) = delete;
  ReleaseLoop& operator=(const ReleaseLoop&) = delete;

  // Starts the thread and asks for SCHED_FIFO at cfg.rt_priority. One
  // stderr line either way: the class and priority obtained, or why the
  // request was refused (the loop then runs at normal priority).
  void start();
  // true = parked (see the class comment); false = not confirmed within
  // `limit`, the request withdrawn.
  bool park(std::chrono::milliseconds limit);
  void resume();
  // Only while parked or before start() (hotplug reacquire); refused with a
  // stderr line otherwise. nullptr = no display.
  void set_sink(PresentSink* s);
  // Works parked or not. Frames still held stay in the regulator for the
  // caller to return.
  void stop_and_join();

  // Housekeeping: appends the margins taken since the last call, oldest
  // first. Bounded: if housekeeping falls kMarginCap behind, the oldest go.
  void take_margins(std::vector<MarginSample>* out);
  // Housekeeping, 1 Hz: the window since the last call; resets it.
  Stats take_stats();
  bool rt_applied() const { return rt_applied_.load(); }
  // Chain-break heals the detector fired (each one a regulator.heal_slip()).
  uint64_t heals() const { return heals_.load(); }

  static constexpr int kMarginCap = 64;
  // Percentiles rank at most this many of a window's samples (the most
  // recent); the maxima cover every sample.
  static constexpr int kStatCap = 512;
  // Wake period while nothing is held and a sink is present.
  static constexpr uint64_t kIdleTickUs = 10'000;

 private:
  void run_();
  // One wake's work (steps 1-5 of the class comment).
  void iterate_();
  void present_(const DmaFrame& f);
  void release_displaced_(const FrameRegulator::Displaced& d);
  // Steps of a park, on whichever thread does it; caller holds mu_.
  void park_work_();
  // Blocks the thread while parked; false = stopping.
  bool park_wait_();
  void arm_(uint64_t abs_us);
  void register_sink_fd_();
  void unregister_sink_fd_();
  void chain_break_check_();
  void stat_rlate_(uint64_t us);
  void stat_hand_(uint64_t us);
  void note_present_time_(uint64_t t);

  FrameRegulator& reg_;
  LatTracker& lat_;
  WakeQueue<HandedFrame>& in_;
  WakeQueue<DmaFrame>& releases_;
  PresentSink* sink_;  // written before start() or while parked (under mu_)
  const Cfg cfg_;

  int ep_ = -1;
  int timer_fd_ = -1;
  int ctl_fd_ = -1;
  int sink_fd_reg_ = -1;  // the sink event fd registered with ep_

  // Release-thread state.
  std::vector<HandedFrame> handed_;
  uint64_t armed_us_ = 0;      // 0 = disarmed
  bool armed_idle_ = false;
  uint64_t last_present_us_ = 0;
  int64_t prev_present_iv_ = -1;
  uint64_t pend_prev_ = 0;
  uint64_t last_engage_ms_ = 0;
  uint64_t last_heal_ms_ = 0;

  // Control handshake only (start/park/resume/stop): the release thread
  // takes this outside its steady-state wake, so it stays a plain mutex
  // (no normal-priority holder on the data path to invert against) and
  // keeps std::condition_variable rather than the _any variant.
  std::thread th_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool started_ = false;      // under mu_
  bool parked_ = false;       // under mu_
  bool resume_req_ = false;   // under mu_
  std::atomic<bool> park_req_{false}, stop_{false}, finished_{false};
  std::atomic<bool> rt_applied_{false};
  std::atomic<uint64_t> heals_{0};

  // Taken by the release thread itself on every wake (margin_mu_ on a
  // fresh offer margin, stats_mu_ on every present/hand-over), contended
  // against housekeeping's take_margins()/take_stats() at 1 Hz -- on the
  // release thread's data path, unlike mu_ above, so priority-inherit too.
  PiMutex margin_mu_;
  std::array<MarginSample, kMarginCap> margins_{};
  int margin_head_ = 0;  // oldest
  int margin_n_ = 0;

  PiMutex stats_mu_;
  std::array<uint32_t, kStatCap> rlate_{};
  std::array<uint32_t, kStatCap> hand_{};
  uint32_t rlate_n_ = 0, hand_n_ = 0;  // samples this window (may exceed kStatCap)
  uint32_t rlate_max_ = 0, hand_max_ = 0;
  double jitter_ema_ms_ = 0;
};

}  // namespace maburplay

#endif  // MABUR_PLAYER_RELEASE_LOOP_H_
