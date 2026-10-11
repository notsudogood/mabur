#include "async_raw_dvr.h"

#include <cstdio>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace maburplay {

AsyncRawDvr::AsyncRawDvr(size_t max_queue_bytes) : max_queue_(max_queue_bytes) {
  th_ = std::thread([this] { run_(); });
}

AsyncRawDvr::~AsyncRawDvr() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    quit_ = true;
  }
  cv_.notify_all();
  if (th_.joinable()) th_.join();
}

void AsyncRawDvr::push_(Item&& it) {
  queued_.fetch_add(it.au.size(), std::memory_order_acq_rel);
  {
    std::lock_guard<std::mutex> lk(mu_);
    q_.push_back(std::move(it));
  }
  cv_.notify_one();
}

void AsyncRawDvr::start(const std::string& path, int width, int height, int fragment_ms) {
  Item it;
  it.kind = Item::Start;
  it.path = path;
  it.width = width;
  it.height = height;
  it.fragment_ms = fragment_ms;
  it.gen = ++gen_;
  armed_ = true;
  overflow_ = false;
  pending_state_ = mabur::RawDvr::State::WaitSync;
  push_(std::move(it));
}

void AsyncRawDvr::feed(const uint8_t* au, size_t n, uint32_t pts_us, bool complete) {
  if (!armed_ || !complete || n == 0) return;
  const size_t q = queued_.load(std::memory_order_acquire);
  if (q + n > max_queue_) {
    // The card cannot keep up: end the recording (what is written stays a
    // valid file) instead of stalling the live picture or queueing forever.
    std::fprintf(stderr,
                 "maburplay: rec: the SD card fell %.1f MB behind -- recording stopped "
                 "(the file so far is kept)\n",
                 static_cast<double>(q) / (1024.0 * 1024.0));
    stop(false);
    overflow_ = true;
    return;
  }
  Item it;
  it.kind = Item::Feed;
  it.au.assign(au, au + n);
  it.pts_us = pts_us;
  push_(std::move(it));
}

void AsyncRawDvr::stop(bool wait) {
  Item it;
  it.kind = Item::Stop;
  it.gen = ++gen_;
  armed_ = false;
  pending_state_ = mabur::RawDvr::State::Off;
  const uint64_t g = it.gen;
  push_(std::move(it));
  if (!wait) return;
  std::unique_lock<std::mutex> lk(mu_);
  done_cv_.wait(lk, [&] { return done_gen_.load(std::memory_order_acquire) >= g; });
}

mabur::RawDvr::State AsyncRawDvr::state() const {
  if (overflow_) return mabur::RawDvr::State::Error;
  if (done_gen_.load(std::memory_order_acquire) < gen_) return pending_state_;
  return static_cast<mabur::RawDvr::State>(state_.load(std::memory_order_acquire));
}

mabur::RawDvr::Err AsyncRawDvr::err() const {
  if (overflow_) return mabur::RawDvr::Err::Write;
  return static_cast<mabur::RawDvr::Err>(err_.load(std::memory_order_acquire));
}

void AsyncRawDvr::publish_(uint64_t gen) {
  state_.store(static_cast<int>(dvr_.state()), std::memory_order_release);
  err_.store(static_cast<int>(dvr_.err()), std::memory_order_release);
  opened_.store(dvr_.opened(), std::memory_order_release);
  bytes_.store(dvr_.bytes(), std::memory_order_release);
  samples_.store(dvr_.samples(), std::memory_order_release);
  fragments_.store(dvr_.fragments(), std::memory_order_release);
  if (gen != 0) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      done_gen_.store(gen, std::memory_order_release);
    }
    done_cv_.notify_all();
  }
}

void AsyncRawDvr::run_() {
#ifdef __linux__
  // Below the player loop: the writer only has to keep up on average.
  (void)setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
  for (;;) {
    Item it;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [&] { return quit_ || !q_.empty(); });
      if (q_.empty()) break;   // quit_ with nothing left: drained
      it = std::move(q_.front());
      q_.pop_front();
    }
    switch (it.kind) {
      case Item::Start:
        dvr_.start(it.path, it.width, it.height, it.fragment_ms);
        break;
      case Item::Feed:
        dvr_.feed(it.au.data(), it.au.size(), it.pts_us, true);
        break;
      case Item::Stop:
        dvr_.stop();
        break;
    }
    queued_.fetch_sub(it.au.size(), std::memory_order_acq_rel);
    publish_(it.gen);
  }
  dvr_.stop();
  publish_(0);
}

}  // namespace maburplay
