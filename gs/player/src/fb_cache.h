#ifndef MABUR_PLAYER_FB_CACHE_H_
#define MABUR_PLAYER_FB_CACHE_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "pi_mutex.h"

namespace maburplay {

// Keeps a decoded frame's imported GEM handle and its KMS framebuffer alive
// across flips, so a buffer the decoder's pool hands out again costs a
// lookup instead of a fresh import.
//
// Why: importing a dma-buf (drmPrimeFDToHandle) is a dma-buf attach plus
// an IOMMU map of the whole picture -- 2.6 ms typical and up to ~7 ms on
// the GS -- and closing the handle after every flip meant paying it on
// every frame, inside the release lead (display.vsync_lead_ms). A frame
// whose import overran the lead latched a vblank late and the frame behind
// it was dropped at the presenter's mailbox: the clean-link repeat+skip
// pair. While the handle stays open the kernel's second import of the same
// dma-buf is a hash lookup that returns the existing handle, so the handle
// is the key: the same dma-buf always maps to the same handle, and a new
// dma-buf can only reuse a handle number this cache has already closed.
//
// Bounded (max_entries) with LRU eviction so a decoder that reallocates its
// pool cannot pin freed buffers forever; entries in use by a slot (on
// screen, pending, parked) are never evicted. flush() frees everything --
// call it wherever the backend's buffers are about to be reset.
//
// Thread-safe, for one thread that pre-imports frames (prime) and one that
// presents them. The cache's mutex covers only the table: the dma-buf
// import and AddFB2 -- the milliseconds this cache exists to keep off the
// deadline -- run outside it. A lookup is therefore three steps, find()
// (import + lookup), add_fb() on a miss (unlocked), insert(), and two
// consequences are handled here rather than by the callers:
//  - Two threads can miss on the same dma-buf at once. Both get the same
//    handle from the kernel and both add a framebuffer; insert() keeps the
//    first and removes the second, which no commit has seen yet.
//  - A GEM handle is not reference-counted: every import of a dma-buf
//    returns the one handle, and one close ends it for everyone. So a
//    handle is closed only when no entry holds it and no miss is in flight
//    on it (a miss leaves a placeholder entry until its insert()), and an
//    import that raced a close is redone, since the handle it returned may
//    be the one just closed.
class FbCache {
 public:
  struct Geom {
    int width = 0, height = 0, stride = 0, vstride = 0;
    uint32_t fourcc = 0;
    bool operator==(const Geom& o) const {
      return width == o.width && height == o.height && stride == o.stride &&
             vstride == o.vstride && fourcc == o.fourcc;
    }
  };
  struct Ops {
    std::function<bool(int dmabuf_fd, uint32_t* handle)> import;
    std::function<bool(uint32_t handle, const Geom& g, uint32_t* fb_id)> add_fb;
    std::function<void(uint32_t fb_id)> rm_fb;
    std::function<void(uint32_t handle)> close_handle;
  };
  // `pinned` names handles whose framebuffer a call must not free: eviction
  // skips them, and insert() will not replace a pinned entry's framebuffer.
  using Pinned = std::function<bool(uint32_t handle)>;

  FbCache(Ops ops, size_t max_entries) : ops_(std::move(ops)), max_(max_entries) {}
  ~FbCache() {
    // No other thread is left: close what in-flight misses still hold too.
    std::lock_guard<PiMutex> lk(mu_);
    for (const Entry& e : entries_) {
      if (e.fb_id) ops_.rm_fb(e.fb_id);
      ops_.close_handle(e.handle);
    }
    entries_.clear();
  }
  FbCache(const FbCache&) = delete;
  FbCache& operator=(const FbCache&) = delete;

  // Imports the dma-buf (outside the lock) and looks its handle up. True on
  // a hit: *fb_id is the cached framebuffer for this geometry. False on a
  // miss: *fb_id is 0 and *handle is the imported handle, which the caller
  // now owes an insert() -- after add_fb(), whose failure it passes on as
  // fb_id 0. A cached entry with another geometry (a resolution change on
  // a reused pool slot) has its framebuffer removed here and is a miss.
  // *handle 0 on a miss means the import failed and nothing is owed.
  bool find(int dmabuf_fd, const Geom& g, uint64_t now, uint32_t* fb_id, uint32_t* handle) {
    *fb_id = 0;
    *handle = 0;
    uint32_t h = 0;
    std::unique_lock<PiMutex> lk = import_(dmabuf_fd, &h);
    if (h == 0) return false;
    ++imports_;
    *handle = h;
    Entry* e = entry_(h);
    if (!e) {
      Entry p;
      p.handle = h;
      p.geom = g;
      p.last_use = now;
      p.in_flight = 1;
      entries_.push_back(p);
      return false;
    }
    ++hits_;
    e->last_use = now;
    e->stale = false;  // looked up after a flush: the buffer is in the new pool too
    if (e->fb_id && e->geom == g) {
      *fb_id = e->fb_id;
      return true;
    }
    if (e->fb_id) {
      // Same buffer, new geometry: drop the framebuffer, keep the handle.
      ops_.rm_fb(e->fb_id);
      e->fb_id = 0;
    }
    e->geom = g;
    ++e->in_flight;
    return false;
  }

  // AddFB2 for a handle find() missed on. Takes no lock: call it between
  // find() and insert().
  bool add_fb(uint32_t handle, const Geom& g, uint32_t* fb_id) {
    return ops_.add_fb(handle, g, fb_id);
  }

  // Completes a miss: stores `fb_id` (0 = add_fb() failed) for `handle`
  // and returns the framebuffer the caller should use, 0 for none. If the
  // other thread completed the same miss first, its framebuffer is kept
  // and returned and `fb_id` is removed. A miss that a flush() overtook is
  // dropped (0). Evicts the LRU entry first when the cache is over its
  // bound.
  uint32_t insert(uint32_t handle, const Geom& g, uint32_t fb_id, uint64_t now,
                  const Pinned& pinned) {
    std::lock_guard<PiMutex> lk(mu_);
    if (fb_id) ++adds_;
    Entry* e = entry_(handle);
    if (!e) {
      // No find() preceded this (or its placeholder is gone): a plain store.
      if (!fb_id) return 0;
      if (entries_.size() >= max_) evict_lru_(pinned);
      Entry n;
      n.handle = handle;
      n.fb_id = fb_id;
      n.geom = g;
      n.last_use = now;
      entries_.push_back(n);
      return fb_id;
    }
    if (e->in_flight > 0) --e->in_flight;
    if (e->stale) {
      // flush() ran while this miss was in flight: the buffer belongs to
      // the pool the caller just reset. Free it rather than cache it.
      if (fb_id) ops_.rm_fb(fb_id);
      if (e->in_flight == 0) remove_(handle);
      return 0;
    }
    if (e->fb_id) {
      // The other thread's insert landed first.
      if (e->geom == g) {
        if (fb_id) ops_.rm_fb(fb_id);
        e->last_use = now;
        return e->fb_id;
      }
      if (!fb_id || (pinned && pinned(handle))) {
        if (fb_id) ops_.rm_fb(fb_id);
        return 0;
      }
      ops_.rm_fb(e->fb_id);
      e->fb_id = fb_id;
      e->geom = g;
      e->last_use = now;
      return fb_id;
    }
    if (!fb_id) {
      // The last one out closes the handle; one still in flight keeps it.
      if (e->in_flight == 0) remove_(handle);
      return 0;
    }
    // Evict while this entry is still a placeholder, which eviction skips;
    // the erase can move it, so look it up again.
    if (entries_.size() > max_) {
      evict_lru_(pinned);
      e = entry_(handle);
    }
    e->fb_id = fb_id;
    e->geom = g;
    e->last_use = now;
    return fb_id;
  }

  // The frame's framebuffer, importing and adding only when needed. 0 on
  // failure (nothing cached). `now` orders the LRU; `pinned` names handles
  // a slot still references, which eviction skips. find() + add_fb() +
  // insert() in one call.
  uint32_t acquire(int dmabuf_fd, const Geom& g, uint64_t now, const Pinned& pinned,
                   uint32_t* handle_out) {
    uint32_t fb = 0, handle = 0;
    const bool hit = find(dmabuf_fd, g, now, &fb, &handle);
    if (handle_out) *handle_out = handle;
    if (hit) return fb;
    if (handle == 0) return 0;
    if (!add_fb(handle, g, &fb)) fb = 0;
    return insert(handle, g, fb, now, pinned);
  }

  // Evicts LRU entries until the cache is back within its bound, skipping
  // pinned handles and misses in flight. For a cache whose inserts cannot
  // evict (their caller cannot say what is pinned): the thread that can
  // calls this.
  void trim(const Pinned& pinned) {
    std::lock_guard<PiMutex> lk(mu_);
    while (entries_.size() > max_)
      if (!evict_lru_(pinned)) break;
  }

  // Frees every framebuffer and closes every handle, except a handle a miss
  // is still in flight on: its framebuffer goes, the handle stays open for
  // that thread's AddFB2, and its insert() then drops it.
  void flush() {
    std::lock_guard<PiMutex> lk(mu_);
    std::vector<Entry> keep;
    for (Entry& e : entries_) {
      if (e.fb_id) ops_.rm_fb(e.fb_id);
      if (e.in_flight > 0) {
        e.fb_id = 0;
        e.stale = true;
        keep.push_back(e);
        continue;
      }
      close_(e.handle);
    }
    entries_.swap(keep);
  }

  // Cached framebuffers (placeholders of misses in flight count too).
  size_t size() const {
    std::lock_guard<PiMutex> lk(mu_);
    return entries_.size();
  }
  uint64_t imports() const {
    std::lock_guard<PiMutex> lk(mu_);
    return imports_;
  }
  uint64_t hits() const {
    std::lock_guard<PiMutex> lk(mu_);
    return hits_;
  }
  uint64_t adds() const {
    std::lock_guard<PiMutex> lk(mu_);
    return adds_;
  }
  uint64_t evictions() const {
    std::lock_guard<PiMutex> lk(mu_);
    return evictions_;
  }

 private:
  struct Entry {
    uint32_t handle = 0;
    uint32_t fb_id = 0;  // 0: a placeholder, a miss in flight
    Geom geom;
    uint64_t last_use = 0;
    int in_flight = 0;   // find() misses on this handle not yet insert()ed
    bool stale = false;  // flushed while in flight: drop the late insert
  };

  // The import, outside the lock, then the lock. A handle closed between
  // the two may be the very one the import returned -- the kernel hands an
  // already-imported dma-buf its existing handle -- so then the import is
  // redone. After a few rounds of that it is redone under the lock, where
  // no close can intervene. Returns holding the lock; *h is 0 on failure.
  std::unique_lock<PiMutex> import_(int dmabuf_fd, uint32_t* h) {
    std::unique_lock<PiMutex> lk(mu_);
    for (int attempt = 0; attempt < 3; ++attempt) {
      const uint64_t seq = close_seq_;
      lk.unlock();
      uint32_t got = 0;
      const bool ok = ops_.import(dmabuf_fd, &got) && got != 0;
      lk.lock();
      if (close_seq_ == seq) {
        *h = ok ? got : 0;
        return lk;
      }
    }
    uint32_t got = 0;
    *h = ops_.import(dmabuf_fd, &got) ? got : 0;
    return lk;
  }

  Entry* entry_(uint32_t handle) {
    for (Entry& e : entries_)
      if (e.handle == handle) return &e;
    return nullptr;
  }
  void close_(uint32_t handle) {
    ops_.close_handle(handle);
    ++close_seq_;
  }
  void remove_(uint32_t handle) {
    for (size_t i = 0; i < entries_.size(); ++i) {
      if (entries_[i].handle != handle) continue;
      if (entries_[i].fb_id) ops_.rm_fb(entries_[i].fb_id);
      close_(handle);
      entries_.erase(entries_.begin() + static_cast<long>(i));
      return;
    }
  }
  bool evict_lru_(const Pinned& pinned) {
    int victim = -1;
    for (size_t i = 0; i < entries_.size(); ++i) {
      const Entry& e = entries_[i];
      if (e.in_flight > 0 || !e.fb_id) continue;
      if (pinned && pinned(e.handle)) continue;
      if (victim < 0 || e.last_use < entries_[static_cast<size_t>(victim)].last_use)
        victim = static_cast<int>(i);
    }
    if (victim < 0) return false;  // everything in use: grow by one rather than break a slot
    ++evictions_;
    remove_(entries_[static_cast<size_t>(victim)].handle);
    return true;
  }

  Ops ops_;
  size_t max_;
  mutable PiMutex mu_;
  std::vector<Entry> entries_;
  uint64_t close_seq_ = 0;  // bumped by every handle close, under mu_
  uint64_t imports_ = 0, hits_ = 0, adds_ = 0, evictions_ = 0;
};

}  // namespace maburplay

#endif  // MABUR_PLAYER_FB_CACHE_H_
