#ifndef MABUR_PLAYER_PHASE_LOCK_H_
#define MABUR_PLAYER_PHASE_LOCK_H_

#include <cstdint>
#include <vector>

#include "mabur/rc_proto.h"

namespace maburplay {

// Phase lock: steers the drone's sensor rate so each frame's capture-time
// slot sits a fixed margin before its vblank release (docs/phase-lock.md).
//
// Input: every frame's margin from FrameRegulator::take_margin. Each 1 s
// window gives the median error against the target (wrapped into +-P/2)
// and, from a least-squares line through the errors, the phase slope. The
// slope against a rate offset is physics -- an error of D mfps at ~60 fps
// moves the phase D/60000 s per s -- so the window's slope names the offset
// that would match the panel: n = o - slope / G. `centre` is an EMA of n
// (frequency), and the offset sent is centre - Kp * error clamped to a
// slew limit (phase). The sensor only takes whole lines, so near lock the
// correction toggles between neighbouring line counts.
//
// Output: offset_mfps(), the signed offset on the drone's trim request to
// send in every RCF. Pure logic: no I/O, no clock.
class PhaseLock {
 public:
  enum class State : uint8_t { Waiting, Acquiring, Locked, OutOfRange };

  static constexpr int32_t kRangeMfps = mabur::rc::kSnrOffRangeMfps;
  static constexpr double kCorrAcqMfps = 60.0;    // ~3 lines: slew while acquiring
  static constexpr double kCorrLockMfps = 20.0;   // ~1 line: hold while locked
  static constexpr double kKpMfpsPerMs = 30.0;    // ~2 s phase time constant
  static constexpr double kCentreAlpha = 0.3;
  static constexpr double kSlopeMsPerSecPerMfps = 1000.0 / 60000.0;
  static constexpr double kLockInMs = 1.0;
  static constexpr int kLockInWindows = 3;
  static constexpr double kLockOutMs = 3.0;
  static constexpr int kLockOutWindows = 2;
  static constexpr double kCoherentUs = 4000.0;   // window p90 - p10 limit
  static constexpr double kSlopeResidUs = 1000.0; // RMS residual limit for a slope
  // Slope fit skips samples inside this much of the window's start: the RCF
  // cadence plus maburgs's feedback_ms (50 in the bundle) before the new
  // offset reaches the drone, plus a frame or two before the sensor turns it
  // into a new rate. A window with the step still inside it is caught by the
  // residual guard in update_centre() and gives no slope.
  static constexpr uint64_t kSlopeSkipUs = 150'000;
  static constexpr int kMinSamples = 10;
  static constexpr uint64_t kWindowUs = 1'000'000;
  static constexpr uint64_t kQuietUs = 2'000'000;
  static constexpr uint64_t kAcquireLimitUs = 60'000'000;
  static constexpr uint64_t kOorRetryUs = 60'000'000;

  explicit PhaseLock(double target_margin_ms) : target_us_(target_margin_ms * 1000.0) {}

  // One frame's margin (µs) against a panel period (µs), at mono time now_us.
  void on_margin(double margin_us, double period_us, uint64_t now_us);
  // The pts timeline restarted (drone reboot): forget the centre, send 0.
  void on_discontinuity(uint64_t now_us);
  // Closes the window when due and runs the state machine. True when the
  // state or the offset changed.
  bool tick(uint64_t now_us);

  State state() const { return state_; }
  bool locked() const { return state_ == State::Locked; }
  int32_t offset_mfps() const { return offset_; }
  double centre_mfps() const { return centre_; }
  double error_ms() const { return err_ms_; }

 private:
  struct Sample {
    uint64_t t_us;
    double e_us;
  };
  void close_window(uint64_t now_us);
  void update_centre();
  void enter(State s, uint64_t now_us);

  const double target_us_;
  State state_ = State::Waiting;
  std::vector<Sample> samples_;
  bool window_open_ = false;
  uint64_t win_start_us_ = 0;
  int32_t window_offset_ = 0;   // the offset in force during the open window
  uint64_t last_window_us_ = 0;
  uint64_t acq_since_us_ = 0;
  uint64_t oor_since_us_ = 0;
  double period_us_ = 16'667.0;
  double centre_ = 0.0;
  int32_t offset_ = 0;
  double err_ms_ = 0.0;
  int in_count_ = 0;
  int out_count_ = 0;
};

const char* phase_lock_state_name(PhaseLock::State s);

}  // namespace maburplay

#endif  // MABUR_PLAYER_PHASE_LOCK_H_
