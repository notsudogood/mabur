#pragma once

#include <cstdint>
#include <optional>
#include <random>
#include <vector>

#include "config.h"
#include "mabur/rc_proto.h"

namespace maburgs {

// Turnaround bench, GS side (feedback-repair rollout phase 2,
// docs/feedback-repair-rollout.md): decides when to ping the drone and on
// which lane its pong should come back.
//
// Pings leave at jittered intervals (uniform 0.5x..1.5x of 1/rate_hz), not
// in the RCF slot: the point is to sample every state the drone's queue can
// be in, including the moment an AU has just landed in it. A ping that lands
// while the drone is mid-burst is lost (half-duplex), and that loss rate is
// a result too -- it is what phase 3's listen window has to beat. Lanes are
// interleaved round robin so every lane sees the same flight conditions.
//
// Pure logic: the caller supplies the clock; the seed makes tests repeatable.
class TaPinger {
 public:
  TaPinger(const TurnaroundCfg& cfg, uint32_t vtx_id, uint64_t seed)
      : cfg_(cfg), vtx_id_(vtx_id), rng_(seed) {}

  bool enabled() const { return cfg_.rate_hz > 0.0 && !cfg_.lanes.empty(); }

  // The next ping, if one is due at now_us. The first call only schedules.
  std::optional<mabur::rc::TaPing> due(uint64_t now_us) {
    if (!enabled()) return std::nullopt;
    if (!have_next_) {
      schedule(now_us);
      return std::nullopt;
    }
    if (now_us < next_us_) return std::nullopt;
    mabur::rc::TaPing p;
    p.vtx_id = vtx_id_;
    p.seq = seq_++;
    p.lane = static_cast<uint8_t>(cfg_.lanes[lane_i_++ % cfg_.lanes.size()]);
    p.n_frames = static_cast<uint8_t>(cfg_.frames);
    p.frame_bytes = static_cast<uint16_t>(cfg_.bytes);
    schedule(now_us);
    return p;
  }

  // A pause (no session, calibration, scout) restarts the schedule, so the
  // first ping after it is a fresh draw rather than an immediate catch-up.
  void pause() { have_next_ = false; }

 private:
  void schedule(uint64_t now_us) {
    std::uniform_real_distribution<double> jitter(0.5, 1.5);
    next_us_ = now_us + static_cast<uint64_t>(jitter(rng_) * 1e6 / cfg_.rate_hz);
    have_next_ = true;
  }

  TurnaroundCfg cfg_;
  uint32_t vtx_id_;
  std::mt19937_64 rng_;
  bool have_next_ = false;
  uint64_t next_us_ = 0;
  uint16_t seq_ = 0;
  size_t lane_i_ = 0;
};

}  // namespace maburgs
