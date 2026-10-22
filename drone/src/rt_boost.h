#pragma once
// Scoped SCHED_FIFO boost for the threads a radio switch waits on
// (radio.switch_rt_prio).
//
// d12 bench, 2026-10-21: a width switch took 79-135 ms on the drone and
// 25-36 ms on the GS for the same 171-173 USB control transfers, and InitWrite
// on the drone (a core to itself) measured 27 us per pipelined write. In
// flight each transfer cost the drone ~0.5 ms: every completion needs two
// wake-ups on cpu0 -- the RX thread, which holds libusb's event lock and
// reaps it, then the thread waiting on it -- and cpu0 also runs the encoder
// at SCHED_FIFO 50 and the FEC worker. Raising both above the encoder for
// the switch is the experiment; if the switch does not get faster, the time
// is in the USB host, not the scheduler.
//
// Each thread's own policy and priority are read first and put back in the
// destructor, whatever they were. A thread that cannot be raised (EPERM, a
// stale handle) is left alone and named in note().
#include <pthread.h>
#include <sched.h>

#include <cerrno>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace mabur {

class RtBoost {
 public:
  // prio <= 0: does nothing (the switch is off).
  RtBoost(std::initializer_list<pthread_t> threads, int prio) : prio_(prio) {
    if (prio <= 0) return;
    for (pthread_t t : threads) {
      Saved s{t, 0, {}};
      if (pthread_getschedparam(t, &s.policy, &s.param) != 0) {
        fail_ = "getschedparam";
        continue;
      }
      sched_param p{};
      p.sched_priority = prio;
      const int rc = pthread_setschedparam(t, SCHED_FIFO, &p);
      if (rc != 0) {
        fail_ = std::strerror(rc);
        continue;
      }
      saved_.push_back(s);
    }
  }
  ~RtBoost() {
    for (const Saved& s : saved_) pthread_setschedparam(s.t, s.policy, &s.param);
  }
  RtBoost(const RtBoost&) = delete;
  RtBoost& operator=(const RtBoost&) = delete;

  size_t raised() const { return saved_.size(); }
  // For the switch's log line: "" when off, " [rt 60 x2]" when raised,
  // " [rt 60 failed: ...]" when a thread could not be.
  std::string note() const {
    if (prio_ <= 0) return {};
    std::string s = " [rt " + std::to_string(prio_) + " x" + std::to_string(saved_.size());
    if (!fail_.empty()) s += ", failed: " + fail_;
    return s + "]";
  }

 private:
  struct Saved {
    pthread_t t;
    int policy;
    sched_param param;
  };
  int prio_ = 0;
  std::vector<Saved> saved_;
  std::string fail_;
};

}  // namespace mabur
