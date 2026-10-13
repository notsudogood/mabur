#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace mabur {

class SwEncoder;

// One queued repair: everything SwEncoder::execute_repair_job needs to build
// the envelope later. Coordinates only — the encoder's ring keeps the
// referenced window rows readable (kSlackRows) until the job runs.
struct FecRepairJob {
  SwEncoder* eng = nullptr;
  uint32_t repair_key = 0;
  uint32_t header_seq = 0;  // h.seq (window start) captured at credit time
  int window_len = 0;
  size_t start_slot = 0;    // ring slot of the window's oldest row
};

// Shared FEC worker thread: single producer (maburd's hot thread owns every
// layer encoder), single consumer, bounded spin-then-sleep pickup.
//
// ARMv7 rules (measured on the SSC338Q, bench/fecbench/RESULTS.md — both
// are load-bearing, do not "simplify"):
//  - Spin loops read with memory_order_relaxed and one acquire LOAD once the
//    awaited change is seen. An acquire load in the loop is LDR+DMB;
//    an idle spinner's barrier storm slowed the OTHER core 10-22%.
//  - The worker sleeps (futex) after kSpinIters; a permanent spinner keeps
//    the scheduler migrating the hot thread and erases the offload gain.
class FecWorker {
 public:
  // Default idle spin, ~50-100 us @ 800 MHz A7. At 35 Mb/s repairs arrive
  // every ~70 us while the producer is mid-frame (estimate, 2026-10-13), so
  // the worker never sleeps then: most of its CPU may be the spin itself,
  // on the drone's busy core. fec.worker_spin lets the bench measure that.
  static constexpr long kSpinIters = 16384;

  // cpu >= 0 pins the worker thread to that core (Linux; no-op elsewhere).
  // queue_slots is a test surface — a tiny queue forces the caller's
  // inline-fallback path; production uses the default, which sits above
  // SwEncoder::kMaxBacklogJobs so the backlog cap (skip) always fires
  // before queue-full (inline on the hot thread) does.
  // spin_iters: idle polls before the worker sleeps (kSpinIters by default;
  // 0 = sleep as soon as the queue is empty). maburd: fec.worker_spin.
  explicit FecWorker(int cpu = -1, uint32_t queue_slots = 512, long spin_iters = kSpinIters);
  ~FecWorker();
  FecWorker(const FecWorker&) = delete;
  FecWorker& operator=(const FecWorker&) = delete;

  // Single-producer enqueue. False = queue full; the caller must build that
  // repair inline (graceful degradation, never blocks, never drops).
  bool try_enqueue(const FecRepairJob& job);

  // Approximate queue occupancy (racy snapshot, gauge-only). Exact when the
  // producer thread calls it between its own enqueues.
  uint32_t depth() const {
    return tail_.load(std::memory_order_relaxed) -
           head_.load(std::memory_order_relaxed);
  }

  // Test surface: a held worker parks before its next dequeue (polling at
  // ~100 µs) so tests can pin the producer's behaviour against an
  // arbitrarily slow worker deterministically. Never set in production;
  // the hot-loop check is one relaxed load per job.
  void set_held(bool held) { held_.store(held, std::memory_order_relaxed); }

 private:
  void loop();

  const int cpu_;
  const long spin_iters_;  // before th_: loop() reads it
  std::vector<FecRepairJob> q_;
  std::atomic<uint32_t> head_{0}, tail_{0};
  std::atomic<bool> quit_{false}, sleeping_{false}, held_{false};
  std::mutex m_;
  std::condition_variable cv_;
  std::thread th_;
};

}  // namespace mabur
