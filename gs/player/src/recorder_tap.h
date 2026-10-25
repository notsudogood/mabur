#ifndef MABUR_PLAYER_RECORDER_TAP_H_
#define MABUR_PLAYER_RECORDER_TAP_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>

#include "glass_cadence.h"
#include "pi_mutex.h"
#include "video_backend.h"

namespace maburplay {

// The burned DVR's hook on the display's flip event: on_flip() runs the
// decoded frame through GlassCadence and, on a recorded slot, hands the
// frame and its grid pts to the installed hook. With no hook installed,
// on_flip() is a no-op -- the DVR simply isn't recording.
//
// on_flip runs on the SCHED_FIFO release thread, so the mutex guarding the
// hook is a PiMutex rather than std::mutex: a normal-priority holder
// (set/clear, both off the release thread) must not sit CFS-preempted
// while the release thread waits on it. on_flip holds the PiMutex for the
// whole hook call, so once clear() -- which takes the same mutex to drop
// the hook -- returns, no hook call is in flight and none can start: that
// is the fence the DVR relies on to tear down safely.
class RecorderTap {
 public:
  using Hook = std::function<void(const DmaFrame&, uint64_t pts_us)>;

  // Installs the hook and resets the cadence (the next on_flip() starts a
  // fresh grid at index 0, slot 0).
  void set(Hook h, double period_us, int every) {
    std::lock_guard<PiMutex> lock(mu_);
    cad_.reset(period_us, every);
    hook_ = std::move(h);
  }

  // Returns only once no hook call is in flight and none can start.
  void clear() {
    std::lock_guard<PiMutex> lock(mu_);
    hook_ = nullptr;
  }

  // Release thread, from the presenter's flip sink. No hook: nothing.
  void on_flip(const DmaFrame& f, uint64_t flip_us) {
    std::lock_guard<PiMutex> lock(mu_);
    if (!hook_) return;
    const GlassCadence::Slot s = cad_.feed(flip_us);
    if (s.record) {
      hook_(f, s.pts_us);
      ++tapped_;
    }
  }

  uint64_t tapped() const { return tapped_; }

 private:
  PiMutex mu_;
  Hook hook_;
  GlassCadence cad_;
  std::atomic<uint64_t> tapped_{0};
};

}  // namespace maburplay

#endif  // MABUR_PLAYER_RECORDER_TAP_H_
