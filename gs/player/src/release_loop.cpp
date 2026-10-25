#include "release_loop.h"

#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "worker_thread.h"

namespace maburplay {

namespace {

uint64_t mono_us() {
  timespec ts;
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000ull +
         static_cast<uint64_t>(ts.tv_nsec) / 1000;
}

uint32_t clamp_u32(uint64_t v) {
  return v > std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max()
                                                  : static_cast<uint32_t>(v);
}

void signal_fd(int fd) {
  const uint64_t one = 1;
  (void)!::write(fd, &one, sizeof one);
}

void clear_fd(int fd) {
  uint64_t v;
  (void)!::read(fd, &v, sizeof v);
}

// p99 of the first m entries of a (reordered in place); 0 when empty.
uint32_t p99_of(uint32_t* a, uint32_t m) {
  if (m == 0) return 0;
  const uint32_t k = static_cast<uint32_t>(std::ceil(0.99 * m)) - 1;
  std::nth_element(a, a + k, a + m);
  return a[k];
}

[[noreturn]] void die(const char* what) {
  std::fprintf(stderr, "maburplay: release thread: %s: %s\n", what, std::strerror(errno));
  std::abort();
}

}  // namespace

ReleaseLoop::ReleaseLoop(FrameRegulator& reg, LatTracker& lat, WakeQueue<HandedFrame>& in,
                         WakeQueue<DmaFrame>& releases, PresentSink* sink, Cfg cfg)
    : reg_(reg), lat_(lat), in_(in), releases_(releases), sink_(sink), cfg_(cfg) {
  ep_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (ep_ < 0) die("epoll_create1");
  timer_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (timer_fd_ < 0) die("timerfd_create");
  ctl_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (ctl_fd_ < 0) die("eventfd");
  for (int fd : {in_.fd(), timer_fd_, ctl_fd_}) {
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (::epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev) != 0) die("epoll_ctl");
  }
  register_sink_fd_();
  // Sized so the steady-state loop never allocates: a hand-over burst is
  // bounded by the decoder's frame pool.
  handed_.reserve(64);
}

ReleaseLoop::~ReleaseLoop() {
  stop_and_join();
  ::close(ctl_fd_);
  ::close(timer_fd_);
  ::close(ep_);
}

void ReleaseLoop::unregister_sink_fd_() {
  if (sink_fd_reg_ < 0) return;
  // May already be gone (a destroyed sink's fd is closed, which removes it
  // from the set on its own); the error is harmless.
  ::epoll_ctl(ep_, EPOLL_CTL_DEL, sink_fd_reg_, nullptr);
  sink_fd_reg_ = -1;
}

void ReleaseLoop::register_sink_fd_() {
  unregister_sink_fd_();
  const int fd = sink_ ? sink_->event_fd() : -1;
  if (fd < 0) return;
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = fd;
  if (::epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev) == 0) sink_fd_reg_ = fd;
  else
    std::fprintf(stderr, "maburplay: release thread: sink event fd not watched (%s)\n",
                 std::strerror(errno));
}

void ReleaseLoop::start() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (started_) return;
    started_ = true;
  }
  th_ = start_worker_thread([this] { run_(); });
  if (cfg_.rt_priority <= 0) {
    std::fprintf(stderr, "maburplay: release thread: SCHED_OTHER (rt_priority 0)\n");
    return;
  }
  sched_param sp{};
  sp.sched_priority = cfg_.rt_priority;
  const int rc = ::pthread_setschedparam(th_.native_handle(), SCHED_FIFO, &sp);
  if (rc != 0) {
    std::fprintf(stderr,
                 "maburplay: release thread: SCHED_FIFO refused (%s), running at normal "
                 "priority\n",
                 std::strerror(rc));
    return;
  }
  int policy = 0;
  sched_param got{};
  ::pthread_getschedparam(th_.native_handle(), &policy, &got);
  rt_applied_.store(policy == SCHED_FIFO);
  std::fprintf(stderr, "maburplay: release thread: %s priority %d\n",
               policy == SCHED_FIFO ? "SCHED_FIFO" : "SCHED_OTHER", got.sched_priority);
}

void ReleaseLoop::run_() {
  bool running = true;
  {
    std::unique_lock<std::mutex> lk(mu_);
    if (parked_) {  // parked before start()
      lk.unlock();
      running = park_wait_();
    }
  }
  if (running) iterate_();  // arms the first tick
  epoll_event evs[4];
  while (running && !stop_.load()) {
    const int n = ::epoll_wait(ep_, evs, 4, -1);
    if (n < 0) {
      if (errno == EINTR) continue;
      std::fprintf(stderr, "maburplay: release thread: epoll_wait: %s\n", std::strerror(errno));
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    for (int i = 0; i < n; ++i) {
      if (evs[i].data.fd == timer_fd_) {
        clear_fd(timer_fd_);
        armed_us_ = 0;  // expired
      } else if (evs[i].data.fd == ctl_fd_) {
        clear_fd(ctl_fd_);
      } else if (evs[i].data.fd == sink_fd_reg_ &&
                 (evs[i].events & (EPOLLERR | EPOLLHUP))) {
        // epoll reports an error/hangup fd as immediately ready on every
        // call, so leaving it registered would busy-spin this SCHED_FIFO
        // thread; drop it from the set and let iterate_()'s idle tick
        // carry the wake instead (it still calls sink->poll_events() every
        // time, error or not).
        std::fprintf(stderr,
                     "maburplay: release thread: display fd error, falling back to the idle "
                     "tick\n");
        unregister_sink_fd_();
      }
    }
    if (stop_.load()) break;
    if (park_req_.load()) {
      std::unique_lock<std::mutex> lk(mu_);
      // Re-checked under mu_: a park() that timed out may have withdrawn
      // it already.
      const bool still_wanted = park_req_.load() && !stop_.load();
      lk.unlock();
      if (still_wanted) {
        // park_work_() runs outside mu_: sink_->drop_all() can block for
        // as long as the sink does (a wedged display's drop_all forcing a
        // synchronous plane-disable can hang for seconds), and a
        // controlling park(limit) must still return false at its limit in
        // that case -- which needs mu_ free so cv_.wait_for's timeout path
        // can reacquire it without waiting on this thread. Safe unlocked:
        // park_work_() touches only this thread's own state and the sink,
        // and set_sink() refuses to change sink_ until parked_ is true,
        // which it still isn't here.
        park_work_();
        lk.lock();
        if (park_req_.load() && !stop_.load()) {
          parked_ = true;
          park_req_.store(false);
          cv_.notify_all();
          lk.unlock();
          if (!park_wait_()) break;
        } else {
          // The caller gave up waiting while park_work_ ran (or a stop
          // arrived meanwhile): the work already happened -- frames
          // dropped, the sink fd unregistered -- and cannot be undone, so
          // pick back up as a normal running loop instead of parking:
          // re-register the sink park_work_ removed and carry on.
          lk.unlock();
          register_sink_fd_();
        }
      }
    }
    iterate_();
  }
  std::lock_guard<std::mutex> lk(mu_);
  finished_.store(true);
  cv_.notify_all();  // a park() waiting sees the exit
}

void ReleaseLoop::iterate_() {
  PresentSink* const sink = sink_;
  if (sink) sink->poll_events();

  // Due frames first: they are older than anything waiting in the
  // hand-over queue.
  uint64_t now = mono_us();
  DmaFrame due;
  for (;;) {
    const uint64_t sched = reg_.next_release_us();
    if (sched == 0 || sched > now) break;
    if (!reg_.release_due(now, &due)) break;
    stat_rlate_(now - sched);
    present_(due);
    now = mono_us();
  }

  in_.drain(&handed_);
  for (const HandedFrame& h : handed_) {
    now = mono_us();
    stat_hand_(now > h.t_decoded_us ? now - h.t_decoded_us : 0);
    FrameRegulator::Displaced disp;
    const bool at_once = reg_.offer(h.f, h.t_decoded_us, now, &disp);
    double margin_us = 0.0;
    if (reg_.take_margin(&margin_us)) {
      const MarginSample m{margin_us, reg_.panel_period_us(), now};
      std::lock_guard<PiMutex> lk(margin_mu_);
      if (margin_n_ == kMarginCap) {
        margins_[margin_head_] = m;
        margin_head_ = (margin_head_ + 1) % kMarginCap;
      } else {
        margins_[(margin_head_ + margin_n_) % kMarginCap] = m;
        ++margin_n_;
      }
    }
    release_displaced_(disp);
    if (at_once) present_(h.f);
  }
  handed_.clear();

  const bool holding = reg_.holding();
  if (sink) {
    // Paced mode follows the servo: a frame that misses its latch is
    // dropped instead of resubmitted a period late; the fallback path
    // keeps the parking behaviour.
    sink->set_paced(reg_.servo_locked());
    chain_break_check_();
    if (!holding) sink->commit_osd_if_pending();
  }

  const uint64_t next = reg_.next_release_us();
  if (next != 0) {
    armed_idle_ = false;
    arm_(next);
  } else if (sink) {
    now = mono_us();
    if (!(armed_idle_ && armed_us_ > now)) {
      arm_(now + kIdleTickUs);
      armed_idle_ = true;
    }
  } else {
    armed_idle_ = false;
    arm_(0);
  }
}

void ReleaseLoop::present_(const DmaFrame& f) {
  const uint64_t t = mono_us();
  note_present_time_(t);
  lat_.on_present(f.pts_us, t);  // t_release
  if (PresentSink* const sink = sink_) {
    sink->present(f);
    const auto ci = sink->last_commit();
    lat_.on_commit(f.pts_us, ci.t_commit_us, ci.bits);
  } else {
    releases_.push(f);  // no display: straight back to the decoder
  }
}

void ReleaseLoop::release_displaced_(const FrameRegulator::Displaced& d) {
  for (int i = 0; i < d.n; ++i) {
    lat_.on_drop(d.f[i].pts_us);
    releases_.push(d.f[i]);
  }
}

// Chain-break heal: a release that misses its latch can self-sustain --
// each parked resubmit lands exactly on a vblank and misses the next
// latch, so every frame after it runs one vsync late. In a chain the
// presenter's mailbox engages about every frame; aligned running produces
// rare singletons. Two engagements within 100 ms is therefore unambiguous:
// slip the pending releases one slot (a single repeated frame) so the flip
// pipeline drains. Rate-limited so a persisting chain gets one heal per
// 150 ms, enough for the slip to take effect before re-judging.
void ReleaseLoop::chain_break_check_() {
  const uint64_t p = sink_->mailbox_engagements();
  const uint64_t now = mono_us() / 1000;
  // A fresh presenter restarts the counter at 0: re-baseline.
  if (p < pend_prev_) pend_prev_ = p;
  if (p > pend_prev_) {
    if (now - last_engage_ms_ <= 100 && now - last_heal_ms_ >= 150) {
      reg_.heal_slip();
      heals_.fetch_add(1);
      last_heal_ms_ = now;
      last_engage_ms_ = 0;  // a fresh pair before re-healing
    } else {
      last_engage_ms_ = now;
    }
    pend_prev_ = p;
  }
}

void ReleaseLoop::arm_(uint64_t abs_us) {
  if (abs_us == armed_us_) return;
  itimerspec its{};
  if (abs_us != 0) {
    its.it_value.tv_sec = static_cast<time_t>(abs_us / 1'000'000);
    its.it_value.tv_nsec = static_cast<long>((abs_us % 1'000'000) * 1000);
  }
  ::timerfd_settime(timer_fd_, TFD_TIMER_ABSTIME, &its, nullptr);
  armed_us_ = abs_us;
}

void ReleaseLoop::park_work_() {
  // Unregister the sink's fd now, while sink_ is still the live sink that
  // owns it: a hotplug reacquire can destroy it (closing the fd) anywhere
  // between here and resume(), and a DEL issued later against that fd
  // number would risk landing on whatever the kernel has since handed it
  // to. register_sink_fd_() on resume then only ADDs the new sink's fd.
  unregister_sink_fd_();
  if (sink_) sink_->drop_all();
  DmaFrame f;
  while (reg_.release_due(~0ull, &f)) {
    lat_.on_drop(f.pts_us);
    releases_.push(f);
  }
  // The sink holds nothing now, so no vblank is taken by a frame the
  // regulator let go of.
  reg_.reset_output_history();
  in_.drain(&handed_);
  for (const HandedFrame& h : handed_) {
    lat_.on_drop(h.f.pts_us);
    releases_.push(h.f);
  }
  handed_.clear();
  armed_idle_ = false;
  arm_(0);
}

bool ReleaseLoop::park_wait_() {
  pollfd p[2] = {{ctl_fd_, POLLIN, 0}, {in_.fd(), POLLIN, 0}};
  for (;;) {
    const int n = ::poll(p, 2, -1);
    if (n < 0 && errno != EINTR) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (n > 0 && (p[0].revents & POLLIN)) clear_fd(ctl_fd_);
    {
      // Control first: a frame handed over after resume() belongs to the
      // running loop even when it is already queued by the time this wakes.
      std::lock_guard<std::mutex> lk(mu_);
      if (stop_.load()) return false;
      if (resume_req_) {
        resume_req_ = false;
        parked_ = false;
        // The sink may have changed while parked.
        register_sink_fd_();
        pend_prev_ = sink_ ? sink_->mailbox_engagements() : 0;
        last_engage_ms_ = 0;
        return true;
      }
    }
    if (n > 0 && (p[1].revents & POLLIN)) {
      // Nothing presents while parked: straight back to the decoder.
      in_.drain(&handed_);
      for (const HandedFrame& h : handed_) {
        lat_.on_drop(h.f.pts_us);
        releases_.push(h.f);
      }
      handed_.clear();
    }
  }
}

bool ReleaseLoop::park(std::chrono::milliseconds limit) {
  std::unique_lock<std::mutex> lk(mu_);
  if (parked_) {
    resume_req_ = false;  // a resume the thread has not acted on yet is void
    return true;
  }
  if (!started_ || finished_.load()) {
    // No thread to ask (or it has ended): the same work, here.
    park_work_();
    parked_ = true;
    return true;
  }
  park_req_.store(true);
  signal_fd(ctl_fd_);
  const bool ok = cv_.wait_for(lk, limit, [this] { return parked_ || finished_.load(); });
  if (!ok) {
    park_req_.store(false);
    return false;
  }
  if (!parked_) {  // the thread ended instead
    park_req_.store(false);
    park_work_();
    parked_ = true;
  }
  return true;
}

void ReleaseLoop::resume() {
  std::lock_guard<std::mutex> lk(mu_);
  park_req_.store(false);
  if (!parked_) return;
  if (!started_ || finished_.load()) {
    parked_ = false;
    register_sink_fd_();
    return;
  }
  resume_req_ = true;
  signal_fd(ctl_fd_);
}

void ReleaseLoop::set_sink(PresentSink* s) {
  std::lock_guard<std::mutex> lk(mu_);
  if (started_ && !finished_.load() && !parked_) {
    std::fprintf(stderr, "maburplay: release thread: set_sink refused, thread not parked\n");
    return;
  }
  sink_ = s;
  pend_prev_ = 0;
  last_engage_ms_ = 0;
  // A parked thread re-registers on resume; otherwise no thread is waiting.
  if (!started_ || finished_.load()) register_sink_fd_();
}

void ReleaseLoop::stop_and_join() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_.store(true);
    signal_fd(ctl_fd_);
  }
  if (th_.joinable()) th_.join();
}

void ReleaseLoop::take_margins(std::vector<MarginSample>* out) {
  std::lock_guard<PiMutex> lk(margin_mu_);
  for (int i = 0; i < margin_n_; ++i) out->push_back(margins_[(margin_head_ + i) % kMarginCap]);
  margin_head_ = 0;
  margin_n_ = 0;
}

void ReleaseLoop::stat_rlate_(uint64_t us) {
  const uint32_t v = clamp_u32(us);
  std::lock_guard<PiMutex> lk(stats_mu_);
  rlate_[rlate_n_ % kStatCap] = v;
  ++rlate_n_;
  if (v > rlate_max_) rlate_max_ = v;
}

void ReleaseLoop::stat_hand_(uint64_t us) {
  const uint32_t v = clamp_u32(us);
  std::lock_guard<PiMutex> lk(stats_mu_);
  hand_[hand_n_ % kStatCap] = v;
  ++hand_n_;
  if (v > hand_max_) hand_max_ = v;
}

// Present-submission jitter, |Δ interval| EMA in ms: the player-side
// smoothness number. Submission clock, not vsync, but a straddle shows up
// in it either way.
void ReleaseLoop::note_present_time_(uint64_t t) {
  std::lock_guard<PiMutex> lk(stats_mu_);
  if (last_present_us_ != 0) {
    const int64_t iv = static_cast<int64_t>(t - last_present_us_);
    if (prev_present_iv_ >= 0) {
      const int64_t dj = iv > prev_present_iv_ ? iv - prev_present_iv_ : prev_present_iv_ - iv;
      jitter_ema_ms_ += (static_cast<double>(dj) / 1000.0 - jitter_ema_ms_) / 16.0;
    }
    prev_present_iv_ = iv;
  }
  last_present_us_ = t;
}

ReleaseLoop::Stats ReleaseLoop::take_stats() {
  std::array<uint32_t, kStatCap> r, h;
  uint32_t rn, hn;
  Stats st;
  {
    std::lock_guard<PiMutex> lk(stats_mu_);
    rn = std::min<uint32_t>(rlate_n_, kStatCap);
    hn = std::min<uint32_t>(hand_n_, kStatCap);
    std::copy(rlate_.begin(), rlate_.begin() + rn, r.begin());
    std::copy(hand_.begin(), hand_.begin() + hn, h.begin());
    st.n = rlate_n_;
    st.rlate_max_us = rlate_max_;
    st.hand_max_us = hand_max_;
    st.present_jitter_ema_ms = jitter_ema_ms_;
    rlate_n_ = hand_n_ = 0;
    rlate_max_ = hand_max_ = 0;
  }
  st.rlate_p99_us = p99_of(r.data(), rn);
  st.hand_p99_us = p99_of(h.data(), hn);
  return st;
}

}  // namespace maburplay
