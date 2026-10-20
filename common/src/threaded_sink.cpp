#include "mabur/threaded_sink.h"

#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <utility>

namespace mabur {

ThreadedSink::ThreadedSink(std::unique_ptr<DvrSink> inner, size_t max_queued_bytes,
                           Pacing pacing)
    : inner_(std::move(inner)), max_queued_bytes_(max_queued_bytes), pacing_(pacing) {
  if (!inner_) {
    failed_ = true;  // nothing to write to: every call fails, no thread
    return;
  }
  thread_ = std::thread([this] { run(); });
}

ThreadedSink::~ThreadedSink() {
  if (thread_.joinable()) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
    }
    work_cv_.notify_one();
    thread_.join();  // the writer exits only once the queue is empty
  }
  inner_.reset();
}

bool ThreadedSink::enqueue(Item item, size_t bytes, uint64_t* seq) {
  std::unique_lock<std::mutex> lk(mu_);
  done_cv_.wait(lk, [&] {
    return failed_ || queued_bytes_ == 0 || queued_bytes_ + bytes <= max_queued_bytes_;
  });
  if (failed_) return false;
  queue_.push_back(std::move(item));
  queued_bytes_ += bytes;
  const uint64_t s = ++enqueued_;
  if (seq) *seq = s;
  lk.unlock();
  work_cv_.notify_one();
  return true;
}

bool ThreadedSink::write(const uint8_t* p, size_t n) {
  return enqueue(Item{Op::Write, std::vector<uint8_t>(p, p + n)}, n, nullptr);
}

bool ThreadedSink::flush() { return enqueue(Item{Op::Flush, {}}, 0, nullptr); }

bool ThreadedSink::sync() {
  uint64_t seq = 0;
  if (!enqueue(Item{Op::Sync, {}}, 0, &seq)) return false;
  std::unique_lock<std::mutex> lk(mu_);
  done_cv_.wait(lk, [&] { return done_ >= seq; });
  return !failed_;
}

// One queued write, as slices with a gap after each (except the last) while
// the queue is short and nobody is draining. Reads queued_bytes_ and stop_
// under the lock for each decision, so a backlog that builds mid-write or a
// destructor that starts one ends the gaps at the next slice.
bool ThreadedSink::write_sliced(const std::vector<uint8_t>& data) {
  const size_t slice = pacing_.slice_bytes == 0 ? data.size() : pacing_.slice_bytes;
  for (size_t off = 0; off < data.size(); off += slice) {
    const size_t n = std::min(slice, data.size() - off);
    if (!inner_->write(data.data() + off, n)) return false;
    if (off + n >= data.size() || pacing_.slice_gap.count() <= 0) continue;
    bool gap;
    {
      std::lock_guard<std::mutex> lk(mu_);
      gap = !stop_ && queued_bytes_ < pacing_.backlog_bytes;
    }
    if (gap) std::this_thread::sleep_for(pacing_.slice_gap);
  }
  return true;
}

void ThreadedSink::run() {
  pthread_setname_np(pthread_self(), "mbr-dvrw");
  // Linux nice is per thread: this lowers the writer alone. Best effort --
  // an unprivileged thread may always raise its own nice value.
  const id_t tid = static_cast<id_t>(::syscall(SYS_gettid));
  const int base = ::getpriority(PRIO_PROCESS, tid);
  ::setpriority(PRIO_PROCESS, tid, base + kWriterNiceStep > 19 ? 19 : base + kWriterNiceStep);

  std::unique_lock<std::mutex> lk(mu_);
  for (;;) {
    work_cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
    if (queue_.empty()) return;  // stop_ with nothing left to write
    Item item = std::move(queue_.front());
    queue_.pop_front();
    const bool skip = failed_;
    lk.unlock();
    bool ok = true;
    if (!skip) {
      switch (item.op) {
        case Op::Write: ok = write_sliced(item.data); break;
        case Op::Flush: ok = inner_->flush(); break;
        case Op::Sync: ok = inner_->sync(); break;
      }
    }
    lk.lock();
    if (!ok) failed_ = true;
    queued_bytes_ -= item.data.size();
    ++done_;
    done_cv_.notify_all();
  }
}

}  // namespace mabur
