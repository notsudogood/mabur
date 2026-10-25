#ifndef MABUR_GS_UPLINK_DELIVERY_H_
#define MABUR_GS_UPLINK_DELIVERY_H_

#include <array>
#include <cstdint>
#include <optional>

namespace maburgs {

// Uplink delivery: the share of the GS's RCFs the drone accepted, over a
// trailing ~10 s window, plus the state the OSD colours it by.
//
//   pct = Δ Telem.rcf_rx / Δ RCFs sent      (between Telems ~10 s apart)
//
// The window, not the adjacent Telem: the drone's 1 Hz rcf_rx and the GS's
// send count are sampled at different instants, so a 1 s ratio jitters by
// a whole RCF period and can read over 100 %.
//
// The denominator is RCFs only. DISC beacons/keep-alives are not RCFs and
// the drone's rcf_rx never counts them; once video stops the GS sends
// ~45 DISC/passthru frames a second, and dividing by those reads a healthy
// uplink as 30 %. Callers feed on_rcf_sent() for RCFs that actually left
// (past the scout gate), nothing else.
//
// "Dead" comes from Telem.state, not rcf_age_ms: the drone enters FAILSAFE
// after a second without an accepted RCF and rebases its feedback clock on
// entry, so rcf_age_ms drops back to ~0 while the uplink is still gone.
//
// Idle beats dead: when the GS itself stops sending RCFs (rendezvous back
// to beaconing after video loss, a key mismatch, calibration silence) the
// drone fails safe because nothing was sent, which says nothing about the
// uplink. Dead also needs kDeadGraceMs of continuous sending before the
// Telem that reports it, so the drone's last pre-session RENDEZVOUS Telem
// cannot flash red at link-up. For the same reason the window yields no
// figure once a Telem gap has evicted its base or while it holds fewer
// than kMinRcfsInWindow RCFs: a figure from before a gap, or one divided by
// two RCFs the drone has not counted yet, would read red over a healthy
// link.
//
// Single-threaded: all three calls come from the GS core thread.
enum class UplinkState : uint8_t { kNone, kWait, kOk, kLow, kDead, kIdle, kStale };

// The sideport / web JSON spelling: "none" "wait" "ok" "low" "dead" "idle"
// "stale".
const char* to_string(UplinkState s);

struct UplinkView {
  UplinkState state = UplinkState::kNone;
  std::optional<int> pct;  // 0..100; held through stale and idle
};

class UplinkDelivery {
 public:
  void on_rcf_sent(uint64_t now_ms);
  void on_telem(uint16_t tlm_seq, uint32_t rcf_rx, uint8_t drone_state, uint64_t now_ms);
  UplinkView view(uint64_t now_ms) const;

  static constexpr uint64_t kWindowMs = 10000, kMinSpanMs = 9000, kStaleMs = 3000,
                            kIdleMs = 1000, kDeadGraceMs = 2000;
  static constexpr int kLowBelowPct = 80;
  // Fewest RCFs a window must hold before it yields a figure: ~1 s of RCFs
  // at the 20 Hz feedback rate.
  static constexpr uint64_t kMinRcfsInWindow = 20;
  static constexpr uint8_t kStateRendezvous = 1, kStateFailsafe = 3;  // Telem.state

 private:
  struct Sample {
    uint64_t t_ms = 0;
    uint32_t rcf_rx = 0;
    uint64_t sent = 0;
  };
  // Telem is 1 Hz, so ~11 samples span the window; 16 leaves slack for a
  // faster Telem without ever evicting the window's base early.
  static constexpr int kCap = 16;
  std::array<Sample, kCap> ring_{};
  int head_ = 0;  // oldest
  int n_ = 0;

  uint64_t sent_ = 0;
  bool have_sent_ = false;
  uint64_t last_sent_ms_ = 0;
  uint64_t run_start_ms_ = 0;

  bool have_telem_ = false;
  uint16_t last_seq_ = 0;
  uint32_t last_rx_ = 0;
  uint8_t drone_state_ = 0;
  uint64_t last_telem_ms_ = 0;

  std::optional<int> pct_;
};

}  // namespace maburgs

#endif  // MABUR_GS_UPLINK_DELIVERY_H_
