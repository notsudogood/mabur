// The feed-thread rig shared by the FeedLoop and DecodePump tests: a real
// AU ring + doorbell, an AuRouter and a FeedLoop feeding a thread-safe fake
// decoder (FakeBackend records every input call; it produces no frames).
#pragma once

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "au_doorbell.h"
#include "au_ring.h"
#include "au_router.h"
#include "feed_loop.h"
#include "mabur/frame_wire.h"
#include "mtest.h"
#include "ring_client.h"
#include "stream_feeder.h"
#include "video_backend.h"

using namespace std::chrono_literals;
using mabur::framewire::FrameHdr;
using maburplay::AuEvent;
using maburplay::DrainResult;
using maburplay::FeedLoop;
using maburplay::FeedNote;

namespace {

uint64_t mono_us() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

struct FakeBackend : maburplay::VideoBackend, maburplay::StreamDecoder {
  struct Call { char op; uint32_t pts; bool last; };
  mutable std::mutex mu;
  std::condition_variable cv;
  std::vector<Call> calls;
  std::atomic<bool> block_submit{false};
  const std::atomic<bool>* cancel = nullptr;
  void log(Call c) {
    std::lock_guard<std::mutex> lk(mu);
    calls.push_back(c);
    cv.notify_all();
  }
  bool init(const maburplay::BackendCfg&, FrameSink) override { return true; }
  void submit_au(const uint8_t*, size_t, uint32_t pts) override {
    // A wedged decoder: BUFFER_FULL until the feed is told to give way.
    while (block_submit.load() && !(cancel && cancel->load())) std::this_thread::sleep_for(1ms);
    log({'W', pts, false});
  }
  void flush() override { log({'F', 0, false}); }
  void release_frame(const maburplay::DmaFrame&) override {}
  bool probe() override { return true; }
  bool start(const uint8_t*, size_t, uint8_t, uint32_t pts) override { log({'S', pts, false}); return true; }
  bool append(const uint8_t*, size_t, uint32_t pts, bool last) override { log({'A', pts, last}); return true; }
  void abort(uint32_t pts) override { log({'X', pts, true}); }
  bool wait_for(char op, uint32_t pts, std::chrono::milliseconds lim, bool last = false) {
    std::unique_lock<std::mutex> lk(mu);
    return cv.wait_for(lk, lim, [&] {
      for (const Call& c : calls)
        if (c.op == op && c.pts == pts && (!last || c.last)) return true;
      return false;
    });
  }
  size_t count() const { std::lock_guard<std::mutex> lk(mu); return calls.size(); }
  std::vector<Call> snapshot() const { std::lock_guard<std::mutex> lk(mu); return calls; }
};

std::vector<uint8_t> make_au(int n, std::vector<size_t>* starts) {
  std::vector<uint8_t> au;
  for (int k = 0; k < n; ++k) {
    starts->push_back(au.size());
    const uint8_t sc[] = {0, 0, 0, 1, 0x02, 0x01};
    au.insert(au.end(), sc, sc + sizeof sc);
    au.insert(au.end(), static_cast<size_t>(20 + 7 * k), static_cast<uint8_t>(0x40 + k));
  }
  return au;
}

struct Rig {
  std::string ring_path, sock_path;
  maburgs::AuRingWriter w;
  maburgs::AuDoorbell db;
  std::unique_ptr<maburplay::VideoBackend> backend;
  FakeBackend* fake = nullptr;
  maburplay::StreamFeeder feeder{true};
  std::unique_ptr<maburplay::AuRouter> router;
  std::unique_ptr<maburplay::RingClient> ring;
  std::unique_ptr<FeedLoop> feed;
  // What the feed thread's callbacks use: set once before start() and never
  // written again. feed.reset() nulls `feed` BEFORE ~FeedLoop stops the
  // thread, so a callback reading `feed` would race it (and could see null).
  FeedLoop* fl = nullptr;
  std::vector<FeedNote> notes;  // the "main thread"'s view

  explicit Rig(const char* tag, uint32_t drain_ms = 100) {
    const std::string pid = std::to_string(getpid());
    ring_path = std::string("/tmp/test_feed_rig_") + tag + "_" + pid;
    sock_path = ring_path + ".sock";
    REQUIRE(w.open(ring_path, {4096, 16}));
    REQUIRE(db.open(sock_path, {4096, 16}));
    auto fb = std::make_unique<FakeBackend>();
    fake = fb.get();
    backend = std::move(fb);
    feeder.set_decoder(fake);
    router = std::make_unique<maburplay::AuRouter>(feeder, backend);
    ring = std::make_unique<maburplay::RingClient>(
        maburplay::RingClient::Cfg{ring_path, sock_path}, [this](AuEvent&& ev) {
          if (fl && !fl->checkpoint()) return;
          router->on_event(std::move(ev));
        });
    REQUIRE(ring->open());
    feed = std::make_unique<FeedLoop>(*ring, *router, feeder, FeedLoop::Cfg{2, drain_ms, 256});
    fl = feed.get();
    router->set_hooks({[this](FeedNote&& n) { fl->push_note(std::move(n)); },
                       [this]() { return fl->park_for_flush(); }, [] { return mono_us(); }});
    fake->cancel = &fl->cancel_flag();
  }
  ~Rig() {
    feed.reset();
    ring.reset();
    unlink(ring_path.c_str());
    unlink(sock_path.c_str());
  }
  void start_and_connect() {
    feed->start();
    for (int i = 0; i < 500 && !db.client_connected(); ++i) {
      db.poll();
      std::this_thread::sleep_for(1ms);
    }
    REQUIRE(db.client_connected());
    std::this_thread::sleep_for(10ms);  // the hello lands; the feed validates it
  }
  void publish_whole(uint32_t pts, uint8_t sid, bool complete, uint8_t flags = 0,
                     const maburgs::AuLatMeta& lat = maburgs::AuLatMeta{}) {
    std::vector<size_t> s;
    const auto au = make_au(1, &s);
    FrameHdr h;
    h.pts_us = pts;
    h.flags = flags;
    w.begin(h, sid, 1);
    w.append(au.data(), au.size());
    w.finish(complete, lat);
    db.notify(0);
  }
  // maburplay's service_feed(), minus the frame holders. Drains both queues
  // into the one "main thread" view, submit before delivery (today's order).
  bool service() {
    feed->take_submit_notes(&notes);
    feed->take_notes(&notes);
    if (!feed->flush_waiting()) return false;
    feed->take_submit_notes(&notes);
    feed->take_notes(&notes);
    feeder.on_flush();
    backend->flush();
    router->disarm();
    feed->resume();
    return true;
  }
};

}  // namespace
