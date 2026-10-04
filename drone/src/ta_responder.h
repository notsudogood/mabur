#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "mabur/rc_proto.h"

namespace mabur {

// Turnaround bench responder (feedback-repair rollout phase 2,
// docs/feedback-repair-rollout.md). The GS sends T_TA_PING; this answers each
// one with T_TA_PONG frames on the hardware queue the ping names (`lane`),
// shaped like a repair burst (n frames of a given size), as fast as the
// drone can. The GS times ping-on-air -> pong-on-air with a witness card's
// hardware RX timestamp; each pong adds what only the drone knows: how long
// it held the ping, and how full its video queues were at the send call.
//
// Threading: on_ping() is called from the RX callback and only parses,
// filters and enqueues -- it never sends, so the RX loop is never blocked on
// a bulk-OUT. A dedicated thread (start()) drains the mailbox through
// answer(). Everything the class touches outside itself comes in through
// the four callbacks, so tests drive it with a fake clock and a capture
// sink.
class TaResponder {
 public:
  struct QueueState {
    uint16_t txq_depth = 0;
    uint16_t pool_depth = 0;
    uint16_t air_backlog_100us = 0;
  };
  struct Cfg {
    uint32_t vtx_id = 0;
    // Pings accepted per second at most; the rest are dropped and counted.
    // A bound on what a misbehaving GS can make this drone transmit.
    uint32_t max_pings_per_s = 50;
  };
  using NowUsFn = std::function<uint64_t()>;
  // Wraps one pong body in radiotap + 802.11 header for the given lane.
  using BuildFn =
      std::function<std::vector<uint8_t>(uint8_t lane, const std::vector<uint8_t>& body)>;
  using SendFn = std::function<bool(const uint8_t* frame, size_t len)>;
  using StateFn = std::function<QueueState()>;

  TaResponder(Cfg cfg, NowUsFn now_us, BuildFn build, SendFn send, StateFn state)
      : cfg_(cfg),
        now_us_(std::move(now_us)),
        build_(std::move(build)),
        send_(std::move(send)),
        state_(std::move(state)) {}

  ~TaResponder() { stop(); }
  TaResponder(const TaResponder&) = delete;
  TaResponder& operator=(const TaResponder&) = delete;

  // RX thread. `rx_us` is the callback's own clock reading, taken as early
  // as possible. Returns true iff the ping was queued for an answer.
  bool on_ping(const uint8_t* body, size_t len, uint64_t rx_us) {
    const auto p = rc::parse_ta_ping(body, len);
    if (!p || p->vtx_id != cfg_.vtx_id) return false;
    std::lock_guard<std::mutex> l(m_);
    const uint64_t min_gap =
        cfg_.max_pings_per_s ? 1000000ull / cfg_.max_pings_per_s : 0;
    if (have_last_ && rx_us - last_accept_us_ < min_gap) {
      ++rate_dropped_;
      return false;
    }
    if (q_.size() >= kMailbox) {
      ++busy_dropped_;
      return false;
    }
    have_last_ = true;
    last_accept_us_ = rx_us;
    q_.push_back({*p, rx_us});
    cv_.notify_one();
    return true;
  }

  // Answers one queued ping, waiting up to timeout_ms for it. Returns true
  // iff a ping was answered (all of its frames handed to send).
  bool pump(int timeout_ms) {
    Pending p;
    {
      std::unique_lock<std::mutex> l(m_);
      if (q_.empty())
        cv_.wait_for(l, std::chrono::milliseconds(timeout_ms),
                     [&] { return !q_.empty() || stopping_; });
      if (q_.empty()) return false;
      p = q_.front();
      q_.pop_front();
    }
    answer(p);
    return true;
  }

  void start() {
    stopping_ = false;
    thread_ = std::thread([this] {
      while (true) {
        {
          std::lock_guard<std::mutex> l(m_);
          if (stopping_) return;
        }
        pump(100);
      }
    });
  }

  void stop() {
    {
      std::lock_guard<std::mutex> l(m_);
      stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

  uint64_t answered() const { std::lock_guard<std::mutex> l(m_); return answered_; }
  uint64_t frames_sent() const { std::lock_guard<std::mutex> l(m_); return frames_sent_; }
  uint64_t send_failed() const { std::lock_guard<std::mutex> l(m_); return send_failed_; }
  uint64_t rate_dropped() const { std::lock_guard<std::mutex> l(m_); return rate_dropped_; }
  uint64_t busy_dropped() const { std::lock_guard<std::mutex> l(m_); return busy_dropped_; }

 private:
  static constexpr size_t kMailbox = 4;

  struct Pending {
    rc::TaPing ping;
    uint64_t rx_us = 0;
  };

  void answer(const Pending& p) {
    uint64_t sent = 0, failed = 0;
    for (uint8_t i = 0; i < p.ping.n_frames; ++i) {
      rc::TaPong pong;
      pong.vtx_id = cfg_.vtx_id;
      pong.seq = p.ping.seq;
      pong.lane = p.ping.lane;
      pong.idx = i;
      pong.n_frames = p.ping.n_frames;
      pong.frame_bytes = p.ping.frame_bytes;
      const QueueState q = state_();
      pong.txq_depth = q.txq_depth;
      pong.pool_depth = q.pool_depth;
      pong.air_backlog_100us = q.air_backlog_100us;
      // Stamped last, right before the send call: the hold the GS should
      // subtract is everything up to the hand-off to the radio.
      const uint64_t now = now_us_();
      const uint64_t hold = now >= p.rx_us ? now - p.rx_us : 0;
      pong.hold_us = hold > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(hold);
      const std::vector<uint8_t> frame = build_(p.ping.lane, rc::pack_ta_pong(pong));
      if (!frame.empty() && send_(frame.data(), frame.size()))
        ++sent;
      else
        ++failed;
    }
    std::lock_guard<std::mutex> l(m_);
    ++answered_;
    frames_sent_ += sent;
    send_failed_ += failed;
  }

  const Cfg cfg_;
  NowUsFn now_us_;
  BuildFn build_;
  SendFn send_;
  StateFn state_;

  mutable std::mutex m_;
  std::condition_variable cv_;
  std::deque<Pending> q_;
  bool stopping_ = false;
  bool have_last_ = false;
  uint64_t last_accept_us_ = 0;
  uint64_t answered_ = 0, frames_sent_ = 0, send_failed_ = 0;
  uint64_t rate_dropped_ = 0, busy_dropped_ = 0;
  std::thread thread_;
};

}  // namespace mabur
