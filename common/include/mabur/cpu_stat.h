#pragma once
#include <cstdint>
#include <optional>
#include <vector>

namespace mabur {

// One CpuStatSampler::sample() result: busy percent since the previous call,
// for the whole chip and per core.
struct CpuSample {
  // The aggregate "cpu" line: all cores together, 0..100.
  std::optional<double> total_pct;
  // One entry per "cpuN" line, indexed by N (a gap N means that core is
  // offline: /proc/stat lists online cores only). Empty optional = no
  // baseline yet for that core, or it went offline/online in between.
  std::vector<std::optional<double>> core_pct;
};

// CPU busy share between consecutive sample() calls, from /proc/stat:
// busy = total - idle - iowait (irq, softirq and steal count as busy; guest
// time is already folded into user/nice by the kernel). Every value is empty
// on the first call (no baseline yet), and the whole sample is empty when
// the file cannot be read or has no aggregate line, in which case the
// baselines are dropped so the next good pair starts fresh. Not loadavg: on
// the SigmaStar drone image loadavg counts the SDK's parked D-state workers
// and reads a flat ~13 idle or pegged (docs/findings/2026-08-31-dq-spike.md).
// The whole-chip figure hides one saturated core (the drone's cpu0 carries
// the FEC worker and USB senders), hence the per-core list. One instance per
// reader thread; not thread-safe by design.
class CpuStatSampler {
 public:
  CpuSample sample(const char* path = "/proc/stat");

 private:
  struct Acc {
    bool have = false;
    uint64_t busy = 0, total = 0;
  };
  Acc all_;
  std::vector<Acc> cores_;
};

}  // namespace mabur
