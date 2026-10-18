#ifndef MABUR_PLAYER_FRAME_HANDOFF_H_
#define MABUR_PLAYER_FRAME_HANDOFF_H_

#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace maburplay {

// The thread boundary of a decoder whose API may only be driven from one
// thread (libav). The feed thread decodes and pushes finished frames to the
// ready queue; the main thread pops them for display and, once the display
// is done with one, pushes it to the release queue; the feed thread frees
// released frames before its next decode, so every decoder call -- including
// the frees, which return buffers to the decoder's pool -- stays on the feed
// thread. drain_all() is for flush/teardown, which run on the main thread
// only while the feed is parked.
template <typename T>
class FrameHandoff {
 public:
  void push_ready(T t) {
    std::lock_guard<std::mutex> lk(mu_);
    ready_.push_back(std::move(t));
  }
  bool pop_ready(T* out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (ready_.empty()) return false;
    *out = std::move(ready_.front());
    ready_.pop_front();
    return true;
  }
  void push_release(T t) {
    std::lock_guard<std::mutex> lk(mu_);
    release_.push_back(std::move(t));
  }
  std::vector<T> take_releases() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<T> out(std::make_move_iterator(release_.begin()),
                       std::make_move_iterator(release_.end()));
    release_.clear();
    return out;
  }
  std::vector<T> drain_all() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<T> out;
    out.reserve(ready_.size() + release_.size());
    for (auto& t : ready_) out.push_back(std::move(t));
    for (auto& t : release_) out.push_back(std::move(t));
    ready_.clear();
    release_.clear();
    return out;
  }
  size_t ready_size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ready_.size();
  }

 private:
  mutable std::mutex mu_;
  std::deque<T> ready_;
  std::deque<T> release_;
};

// Holds a decoder's input back until an entry point (mabur::au_is_entry_point:
// an IRAP, or the SPS-led refresh AU of an intra-refresh stream). A decoder
// has nothing to parse a picture with before the parameter sets, and every
// picture before the refresh references pictures it never decoded; libav's
// HEVC decoder then allocates placeholder references that, under a hwaccel,
// are never filled, so the hardware reads uninitialised buffers. Armed at
// init and by every flush; deliberately NOT re-armed by a decode error, since
// mid-stream loss keeps real (damaged) references that the refresh heals.
class EntryGate {
 public:
  bool admit(bool is_entry) {
    if (!open_ && is_entry) open_ = true;
    if (!open_) ++gated_;
    return open_;
  }
  // Whether admit(is_entry) would pass, without changing anything.
  bool would_admit(bool is_entry) const { return open_ || is_entry; }
  void arm() { open_ = false; }
  bool open() const { return open_; }
  uint64_t gated() const { return gated_; }

 private:
  bool open_ = false;
  uint64_t gated_ = 0;
};

}  // namespace maburplay

#endif  // MABUR_PLAYER_FRAME_HANDOFF_H_
