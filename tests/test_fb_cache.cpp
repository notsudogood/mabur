#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "fb_cache.h"
#include "mtest.h"

using maburplay::FbCache;

namespace {

// A fake DRM: dma-buf fd -> handle is a fixed map (the kernel returns the
// same handle for the same dma-buf while it stays open); add_fb hands out
// increasing fb ids; rm/close are recorded.
struct Fake {
  std::map<int, uint32_t> fd_to_handle;
  uint32_t next_fb = 100;
  std::vector<uint32_t> rm, closed, added;
  bool fail_import = false, fail_add = false;
  int imports = 0;
  // Runs once, inside the next import call: the other thread's work that
  // lands while this one is in the import ioctl.
  std::function<void()> during_import;
  FbCache::Ops ops() {
    return FbCache::Ops{
        [this](int fd, uint32_t* h) {
          ++imports;
          bool ok = false;
          if (!fail_import) {
            auto it = fd_to_handle.find(fd);
            if (it != fd_to_handle.end()) {
              *h = it->second;
              ok = true;
            }
          }
          if (during_import) {
            auto hook = std::move(during_import);
            during_import = nullptr;
            hook();
          }
          return ok;
        },
        [this](uint32_t h, const FbCache::Geom&, uint32_t* fb) {
          if (fail_add) return false;
          *fb = next_fb++;
          added.push_back(h);
          return true;
        },
        [this](uint32_t fb) { rm.push_back(fb); },
        [this](uint32_t h) { closed.push_back(h); }};
  }
};

FbCache::Geom geom(int w = 1920, int h = 1080) {
  FbCache::Geom g;
  g.width = w; g.height = h; g.stride = w; g.vstride = h; g.fourcc = 0x3231564e;
  return g;
}
bool none(uint32_t) { return false; }
bool all(uint32_t) { return true; }

// What a caller outside the cache's lock does on a miss: AddFB2, then insert.
uint32_t add_and_insert(FbCache& c, uint32_t h, const FbCache::Geom& g, uint64_t now,
                        const FbCache::Pinned& pinned) {
  uint32_t fb = 0;
  if (!c.add_fb(h, g, &fb)) fb = 0;
  return c.insert(h, g, fb, now, pinned);
}

}  // namespace

TEST(a_pool_buffer_seen_again_costs_no_add_and_no_free) {
  Fake f;
  f.fd_to_handle = {{10, 7}, {11, 8}, {12, 9}, {13, 10}};
  FbCache c(f.ops(), 32);
  uint32_t h = 0;
  const uint32_t fb0 = c.acquire(10, geom(), 1, none, &h);
  CHECK(fb0 == 100);
  CHECK(h == 7);
  for (int round = 0; round < 3; ++round)
    for (int fd = 10; fd <= 13; ++fd) c.acquire(fd, geom(), static_cast<uint64_t>(10 + round), none, nullptr);
  CHECK(c.acquire(10, geom(), 50, none, nullptr) == fb0);
  CHECK(c.adds() == 4);
  CHECK(c.size() == 4);
  CHECK(f.rm.empty());
  CHECK(f.closed.empty());
}

TEST(eviction_is_lru_and_skips_pinned_handles) {
  Fake f;
  f.fd_to_handle = {{10, 7}, {11, 8}, {12, 9}};
  FbCache c(f.ops(), 2);
  const uint32_t fb7 = c.acquire(10, geom(), 1, none, nullptr);
  const uint32_t fb8 = c.acquire(11, geom(), 2, none, nullptr);
  // Handle 7 is the LRU but still on screen: 8 goes instead.
  c.acquire(12, geom(), 3, [](uint32_t h) { return h == 7; }, nullptr);
  CHECK(c.size() == 2);
  CHECK(c.evictions() == 1);
  CHECK(f.rm == std::vector<uint32_t>{fb8});
  CHECK(f.closed == std::vector<uint32_t>{8});
  // Nothing pinned: the LRU (7) goes next.
  f.fd_to_handle[11] = 8;  // the kernel hands the reopened dma-buf the same handle
  c.acquire(11, geom(), 4, none, nullptr);
  CHECK(f.rm.size() == 2 && f.rm[1] == fb7);
  CHECK(f.closed.size() == 2 && f.closed[1] == 7);
}

TEST(everything_pinned_grows_instead_of_breaking_a_slot) {
  Fake f;
  f.fd_to_handle = {{10, 7}, {11, 8}, {12, 9}};
  FbCache c(f.ops(), 2);
  c.acquire(10, geom(), 1, none, nullptr);
  c.acquire(11, geom(), 2, none, nullptr);
  c.acquire(12, geom(), 3, [](uint32_t) { return true; }, nullptr);
  CHECK(c.size() == 3);
  CHECK(c.evictions() == 0);
}

TEST(new_geometry_on_a_known_handle_replaces_the_fb_and_keeps_the_handle) {
  Fake f;
  f.fd_to_handle = {{10, 7}};
  FbCache c(f.ops(), 32);
  const uint32_t fb_a = c.acquire(10, geom(1920, 1080), 1, none, nullptr);
  const uint32_t fb_b = c.acquire(10, geom(1280, 720), 2, none, nullptr);
  CHECK(fb_b != fb_a);
  CHECK(f.rm == std::vector<uint32_t>{fb_a});
  CHECK(f.closed.empty());
  CHECK(c.size() == 1);
}

TEST(failures_cache_nothing_and_leak_nothing) {
  Fake f;
  f.fd_to_handle = {{10, 7}};
  FbCache c(f.ops(), 32);
  f.fail_import = true;
  CHECK(c.acquire(10, geom(), 1, none, nullptr) == 0);
  CHECK(c.size() == 0);
  f.fail_import = false;
  f.fail_add = true;
  CHECK(c.acquire(10, geom(), 2, none, nullptr) == 0);
  CHECK(c.size() == 0);
  CHECK(f.closed == std::vector<uint32_t>{7});  // the imported handle is not left open
}

TEST(flush_frees_every_entry) {
  Fake f;
  f.fd_to_handle = {{10, 7}, {11, 8}};
  FbCache c(f.ops(), 32);
  c.acquire(10, geom(), 1, none, nullptr);
  c.acquire(11, geom(), 2, none, nullptr);
  c.flush();
  CHECK(c.size() == 0);
  CHECK(f.rm.size() == 2);
  CHECK(f.closed.size() == 2);
  c.flush();  // idempotent
  CHECK(f.rm.size() == 2);
}

TEST(find_then_insert_equals_acquire) {
  Fake f;
  f.fd_to_handle = {{10, 7}};
  FbCache c(f.ops(), 32);
  uint32_t fb = 0, h = 0;
  // First sight: a miss that hands back the imported handle and adds nothing.
  CHECK(!c.find(10, geom(), 1, &fb, &h));
  CHECK(fb == 0);
  CHECK(h == 7);
  CHECK(f.added.empty());
  const uint32_t fb0 = add_and_insert(c, h, geom(), 1, none);
  CHECK(fb0 == 100);
  CHECK(c.size() == 1);
  CHECK(c.adds() == 1);
  // Second sight: a hit on the same framebuffer, nothing added.
  fb = 0; h = 0;
  CHECK(c.find(10, geom(), 2, &fb, &h));
  CHECK(fb == fb0);
  CHECK(h == 7);
  CHECK(c.adds() == 1);
  // New geometry: the stale fb goes exactly once, the handle stays open,
  // and the lookup is a miss the caller completes.
  fb = 0; h = 0;
  CHECK(!c.find(10, geom(1280, 720), 3, &fb, &h));
  CHECK(fb == 0);
  CHECK(h == 7);
  CHECK(f.rm == std::vector<uint32_t>{fb0});
  CHECK(f.closed.empty());
  const uint32_t fb1 = add_and_insert(c, h, geom(1280, 720), 3, none);
  CHECK(fb1 != 0 && fb1 != fb0);
  CHECK(f.rm.size() == 1);
  CHECK(f.closed.empty());
  CHECK(c.size() == 1);
  CHECK(c.find(10, geom(1280, 720), 4, &fb, &h) && fb == fb1);
}

TEST(a_prime_and_a_present_that_both_miss_keep_one_fb) {
  Fake f;
  f.fd_to_handle = {{10, 7}};
  FbCache c(f.ops(), 32);
  uint32_t fb_a = 0, fb_b = 0, ha = 0, hb = 0;
  // Both threads miss on the same dma-buf: the kernel hands both the same handle.
  CHECK(!c.find(10, geom(), 1, &fb_a, &ha));
  CHECK(!c.find(10, geom(), 1, &fb_b, &hb));
  CHECK(ha == 7 && hb == 7);
  CHECK(c.add_fb(ha, geom(), &fb_a));
  CHECK(c.add_fb(hb, geom(), &fb_b));
  CHECK(c.insert(ha, geom(), fb_a, 2, all) == fb_a);
  // The second insert keeps the first fb and frees its own, unused one.
  CHECK(c.insert(hb, geom(), fb_b, 2, none) == fb_a);
  CHECK(f.rm == std::vector<uint32_t>{fb_b});
  CHECK(f.closed.empty());
  CHECK(c.size() == 1);
  uint32_t fb = 0, h = 0;
  CHECK(c.find(10, geom(), 3, &fb, &h) && fb == fb_a);
  c.flush();
  CHECK(f.closed == std::vector<uint32_t>{7});  // one handle, closed once
}

TEST(a_failed_add_never_closes_a_handle_the_other_side_holds) {
  Fake f;
  f.fd_to_handle = {{10, 7}};
  FbCache c(f.ops(), 32);
  uint32_t fb = 0, ha = 0, hb = 0;
  CHECK(!c.find(10, geom(), 1, &fb, &ha));
  CHECK(!c.find(10, geom(), 1, &fb, &hb));
  // One side's AddFB2 fails: the other side still holds the handle.
  CHECK(c.insert(ha, geom(), 0, 2, none) == 0);
  CHECK(f.closed.empty());
  uint32_t fb_b = 0;
  CHECK(c.add_fb(hb, geom(), &fb_b));
  CHECK(c.insert(hb, geom(), fb_b, 2, none) == fb_b);
  CHECK(c.size() == 1);
  CHECK(f.closed.empty());

  // Both fail: the handle is closed exactly once, by the last one out.
  Fake g;
  g.fd_to_handle = {{10, 7}};
  FbCache d(g.ops(), 32);
  CHECK(!d.find(10, geom(), 1, &fb, &ha));
  CHECK(!d.find(10, geom(), 1, &fb, &hb));
  CHECK(d.insert(ha, geom(), 0, 2, none) == 0);
  CHECK(g.closed.empty());
  CHECK(d.insert(hb, geom(), 0, 2, none) == 0);
  CHECK(g.closed == std::vector<uint32_t>{7});
  CHECK(d.size() == 0);

  // A failure after the other side succeeded gets the other side's fb.
  Fake k;
  k.fd_to_handle = {{10, 7}};
  FbCache e(k.ops(), 32);
  CHECK(!e.find(10, geom(), 1, &fb, &ha));
  CHECK(!e.find(10, geom(), 1, &fb, &hb));
  uint32_t fb_ok = 0;
  CHECK(e.add_fb(ha, geom(), &fb_ok));
  CHECK(e.insert(ha, geom(), fb_ok, 2, none) == fb_ok);
  CHECK(e.insert(hb, geom(), 0, 2, none) == fb_ok);
  CHECK(k.closed.empty());
  CHECK(k.rm.empty());
}

TEST(a_flush_while_a_miss_is_in_flight_drops_the_late_insert) {
  Fake f;
  f.fd_to_handle = {{10, 7}, {11, 8}};
  FbCache c(f.ops(), 32);
  const uint32_t fb8 = c.acquire(11, geom(), 1, none, nullptr);
  uint32_t fb = 0, h = 0;
  CHECK(!c.find(10, geom(), 2, &fb, &h));
  // The flush frees what is cached but leaves the in-flight handle open:
  // the thread holding it is about to AddFB2 on it.
  c.flush();
  CHECK(f.rm == std::vector<uint32_t>{fb8});
  CHECK(f.closed == std::vector<uint32_t>{8});
  uint32_t late = 0;
  CHECK(c.add_fb(h, geom(), &late));
  // The late insert belongs to the flushed pool: freed, not cached.
  CHECK(c.insert(h, geom(), late, 3, all) == 0);
  CHECK(f.rm.size() == 2 && f.rm[1] == late);
  CHECK(f.closed.size() == 2 && f.closed[1] == 7);
  CHECK(c.size() == 0);

  // The same buffer looked up again after the flush (it is in the new pool
  // too) keeps the entry alive for the fresh lookup.
  CHECK(!c.find(10, geom(), 4, &fb, &h));
  c.flush();
  CHECK(!c.find(10, geom(), 5, &fb, &h));  // fresh, after the flush
  uint32_t fb_old = 0, fb_new = 0;
  CHECK(c.add_fb(h, geom(), &fb_old));
  CHECK(c.add_fb(h, geom(), &fb_new));
  CHECK(c.insert(h, geom(), fb_old, 6, all) == fb_old);
  CHECK(c.insert(h, geom(), fb_new, 6, none) == fb_old);
  CHECK(c.size() == 1);
  CHECK(f.closed.size() == 2);  // 7 stayed open throughout
}

TEST(a_handle_closed_during_the_import_is_imported_again) {
  Fake f;
  f.fd_to_handle = {{10, 7}};
  FbCache c(f.ops(), 32);
  c.acquire(10, geom(), 1, none, nullptr);
  // The import runs outside the cache's lock. While it is in the kernel
  // the other thread flushes, closing handle 7 -- the one this import just
  // returned. The kernel would hand the buffer a new handle now.
  f.during_import = [&] {
    c.flush();
    f.fd_to_handle[10] = 12;
  };
  f.fd_to_handle[10] = 7;
  uint32_t fb = 0, h = 0;
  CHECK(!c.find(10, geom(), 2, &fb, &h));
  CHECK(h == 12);
  CHECK(f.imports == 3);  // acquire, the raced import, the re-import
  CHECK(add_and_insert(c, h, geom(), 2, none) != 0);
  CHECK(f.added.back() == 12);
}

TEST(trim_evicts_down_to_the_bound_but_never_an_in_flight_or_pinned_entry) {
  Fake f;
  f.fd_to_handle = {{10, 7}, {11, 8}, {12, 9}, {13, 10}};
  FbCache c(f.ops(), 2);
  c.acquire(10, geom(), 1, none, nullptr);
  c.acquire(11, geom(), 2, none, nullptr);
  // An insert that may not evict (everything pinned) grows past the bound.
  uint32_t fb = 0, h = 0;
  CHECK(!c.find(12, geom(), 3, &fb, &h));
  CHECK(add_and_insert(c, h, geom(), 3, all) != 0);
  CHECK(c.size() == 3);
  CHECK(c.evictions() == 0);
  // A miss in flight with the oldest stamp is not a victim either.
  CHECK(!c.find(13, geom(), 0, &fb, &h));
  CHECK(h == 10);
  c.trim([](uint32_t hd) { return hd == 7; });
  CHECK(c.size() == 2);  // 7 pinned, 10 in flight
  CHECK((f.closed == std::vector<uint32_t>{8, 9}));
  CHECK(add_and_insert(c, h, geom(), 4, none) != 0);
  CHECK(c.size() == 2);
  CHECK(f.closed.size() == 2);
}

TEST(an_entry_another_miss_is_still_in_flight_on_is_not_evicted) {
  Fake f;
  f.fd_to_handle = {{10, 7}, {11, 8}};
  FbCache c(f.ops(), 1);
  uint32_t fb = 0, ha = 0, hb = 0;
  CHECK(!c.find(10, geom(), 0, &fb, &ha));
  CHECK(!c.find(10, geom(), 0, &fb, &hb));
  const uint32_t fb_a = add_and_insert(c, ha, geom(), 0, none);
  // 7 is the LRU and over the bound, but the second miss on it has already
  // added a framebuffer on the handle: closing it now would strand that.
  c.acquire(11, geom(), 5, none, nullptr);
  c.trim(none);
  CHECK(f.closed == std::vector<uint32_t>{8});  // the newer one went instead
  CHECK(add_and_insert(c, hb, geom(), 6, none) == fb_a);
  CHECK(f.closed == std::vector<uint32_t>{8});
  CHECK(c.size() == 1);
}

MTEST_MAIN
