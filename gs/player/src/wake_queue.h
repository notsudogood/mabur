#pragma once

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unistd.h>
#include <utility>
#include <sys/eventfd.h>
#include <vector>

#include "pi_mutex.h"
#include "video_backend.h"

namespace maburplay {

struct HandedFrame {
  DmaFrame f;
  uint64_t t_decoded_us = 0;
};

template <typename T>
class WakeQueue {
 public:
  WakeQueue() {
    fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd_ < 0) {
      std::abort();
    }
  }

  ~WakeQueue() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }

  WakeQueue(const WakeQueue&) = delete;
  WakeQueue& operator=(const WakeQueue&) = delete;

  // Append item under lock, then write 1 to the fd (still under lock).
  // This ensures the fd is readable whenever the queue has items.
  void push(T t) {
    std::lock_guard<PiMutex> lock(mu_);
    queue_.push_back(std::move(t));
    uint64_t val = 1;
    ssize_t ret = write(fd_, &val, sizeof(val));
    (void)ret;  // Ignore; non-blocking write failure is harmless
  }

  // Append all queued items to *out, clear the queue, and clear the eventfd
  // (all under the same lock used by push). Returns the count of items drained.
  // Holding the lock while reading the fd ensures a concurrent push cannot
  // add items after we clear the queue, leaving it non-empty with an unreadable fd.
  size_t drain(std::vector<T>* out) {
    std::lock_guard<PiMutex> lock(mu_);
    size_t count = queue_.size();
    for (T& item : queue_) {
      out->push_back(std::move(item));
    }
    queue_.clear();

    // Clear the eventfd by reading from it (under lock).
    uint64_t val = 0;
    ssize_t ret = read(fd_, &val, sizeof(val));
    (void)ret;  // Ignore EAGAIN or other errors

    return count;
  }

  // Return the eventfd. Readable iff the queue has unconsumed items.
  int fd() const {
    return fd_;
  }

  // Return the current queue size.
  size_t size() const {
    std::lock_guard<PiMutex> lock(mu_);
    return queue_.size();
  }

 private:
  int fd_ = -1;
  mutable PiMutex mu_;
  std::vector<T> queue_;
};

}  // namespace maburplay
