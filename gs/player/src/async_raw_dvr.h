#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mabur/raw_dvr.h"

namespace maburplay {

// maburplay's raw recorder with the file I/O off the player loop
// (2026-10-11). mabur::RawDvr is single-threaded by contract -- the web GS
// runs it inside WebAssembly -- so this wraps it rather than changing it:
// the caller's thread only copies each complete AU into a bounded queue,
// and a writer thread owns the RawDvr (sync gate, hvcC, DvrMux, the file).
//
// Why: DvrMux writes a whole fragment (fragment_ms of video, ~3 MB at
// 24 Mbps) and fflush()es it inside feed(). Inline on the player loop that
// was a once-a-second stall: on the 2026-10-11 flight, armed seconds at
// 24 Mbps had a decode-stage p99 of 41 ms with raw recording on against
// 14 ms with it off, and 91 % of seconds reached an e2e p99 of 70 ms
// against 11 % (docs/gs-recording.md).
//
// The writer never blocks the caller. If the card falls behind by more
// than max_queue_bytes (default 16 MB, ~5 s at 24 Mbps) the recording ends
// with Err::Write -- what is already written stays a valid file -- rather
// than stalling the live picture or growing without bound.
//
// Commands are processed in order. Getters read what the writer last
// published, so they can trail the caller by the queue: state() answers
// WaitSync right after start() and Off right after stop() until the writer
// catches up, so the caller's own sequence always reads consistently.
class AsyncRawDvr {
 public:
  explicit AsyncRawDvr(size_t max_queue_bytes = 16u << 20);
  ~AsyncRawDvr();   // drains the queue and seals the file
  AsyncRawDvr(const AsyncRawDvr&) = delete;
  AsyncRawDvr& operator=(const AsyncRawDvr&) = delete;

  void start(const std::string& path, int width, int height, int fragment_ms = 1000);
  // Copies the AU; incomplete AUs and AUs while not armed are dropped here,
  // before any copy.
  void feed(const uint8_t* au, size_t n, uint32_t pts_us, bool complete);
  // Asynchronous by default: the file is sealed by the writer. wait = true
  // returns once it has been (shutdown, and the --oneshot report that
  // reads the counts right after).
  void stop(bool wait = false);

  // Getters are for the caller's thread (they read its command state). A
  // start() the writer has not reached yet reads as a fresh recording --
  // not opened, 0 samples -- never as the previous file's numbers.
  mabur::RawDvr::State state() const;
  mabur::RawDvr::Err err() const;
  bool opened() const { return !start_pending_() && opened_.load(std::memory_order_acquire); }
  uint64_t bytes() const { return start_pending_() ? 0 : bytes_.load(std::memory_order_acquire); }
  uint64_t samples() const { return start_pending_() ? 0 : samples_.load(std::memory_order_acquire); }
  uint64_t fragments() const {
    return start_pending_() ? 0 : fragments_.load(std::memory_order_acquire);
  }
  // Bytes copied but not yet handed to the RawDvr (observability).
  size_t queued_bytes() const { return queued_.load(std::memory_order_acquire); }
  // True once a recording was ended because the card fell behind; cleared
  // by the next start().
  bool overflowed() const { return overflow_; }

 private:
  struct Item {
    enum Kind { Start, Feed, Stop } kind = Feed;
    std::vector<uint8_t> au;
    uint32_t pts_us = 0;
    std::string path;
    int width = 0, height = 0, fragment_ms = 1000;
    uint64_t gen = 0;   // Start/Stop: the caller's command generation
  };
  bool start_pending_() const {
    return done_gen_.load(std::memory_order_acquire) < gen_ &&
           pending_state_ == mabur::RawDvr::State::WaitSync;
  }
  void push_(Item&& it);
  void run_();
  void publish_(uint64_t gen);

  const size_t max_queue_;
  // caller side
  bool armed_ = false;
  bool overflow_ = false;
  uint64_t gen_ = 0;
  mabur::RawDvr::State pending_state_ = mabur::RawDvr::State::Off;
  // shared
  std::mutex mu_;
  std::condition_variable cv_, done_cv_;
  std::deque<Item> q_;
  bool quit_ = false;
  std::atomic<size_t> queued_{0};
  std::atomic<uint64_t> done_gen_{0};
  std::atomic<int> state_{static_cast<int>(mabur::RawDvr::State::Off)};
  std::atomic<int> err_{static_cast<int>(mabur::RawDvr::Err::None)};
  std::atomic<bool> opened_{false};
  std::atomic<uint64_t> bytes_{0}, samples_{0}, fragments_{0};
  // writer side
  mabur::RawDvr dvr_;
  std::thread th_;
};

}  // namespace maburplay
