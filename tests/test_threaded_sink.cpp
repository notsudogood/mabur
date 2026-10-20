#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "mabur/dvr_mux.h"
#include "mabur/threaded_sink.h"
#include "mtest.h"
#include "scratch.h"

using mabur::DvrMux;
using mabur::DvrSink;
using mabur::ThreadedSink;
using namespace std::chrono_literals;

namespace {

// What the gated sink saw. Shared, because ThreadedSink owns (and destroys)
// the sink itself while the test still has to read what reached it.
struct GateState {
  std::mutex mu;
  std::condition_variable cv;
  bool open = false;  // write() blocks until this is set -- a stalled card
  bool fail = false;  // write() reports a card error
  std::string bytes;
  int flushes = 0;
  int syncs = 0;
  bool destroyed = false;

  void release() {
    {
      std::lock_guard<std::mutex> lk(mu);
      open = true;
    }
    cv.notify_all();
  }
  std::string got() {
    std::lock_guard<std::mutex> lk(mu);
    return bytes;
  }
};

class GateSink final : public DvrSink {
 public:
  explicit GateSink(std::shared_ptr<GateState> s) : s_(std::move(s)) {}
  ~GateSink() override {
    std::lock_guard<std::mutex> lk(s_->mu);
    s_->destroyed = true;
  }
  bool write(const uint8_t* p, size_t n) override {
    std::unique_lock<std::mutex> lk(s_->mu);
    s_->cv.wait(lk, [this] { return s_->open; });
    if (s_->fail) return false;
    s_->bytes.append(reinterpret_cast<const char*>(p), n);
    return true;
  }
  bool flush() override {
    std::lock_guard<std::mutex> lk(s_->mu);
    ++s_->flushes;
    return true;
  }
  bool sync() override {
    std::lock_guard<std::mutex> lk(s_->mu);
    ++s_->syncs;
    return true;
  }

 private:
  std::shared_ptr<GateState> s_;
};

const uint8_t* u8(const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); }

// Whether an async call finished within `limit`. A call still blocked on the
// gate is not abandoned: each test opens the gate afterwards and get()s it.
template <class F>
bool finishes_within(std::future<F>& fut, std::chrono::milliseconds limit) {
  return fut.wait_for(limit) == std::future_status::ready;
}

}  // namespace

// The bug: the burned DVR's encoder thread wrote each finished fragment
// (~2.5 MB) to the card itself, ~22 ms on the GS's vfat card, so the next
// decoded frame was displaced. write() must hand the bytes over and return
// while the card is still busy.
TEST(threaded_sink_write_does_not_wait_for_the_card) {
  auto st = std::make_shared<GateState>();
  {
    ThreadedSink ts(std::make_unique<GateSink>(st));
    const std::string frag = "moof+mdat bytes of one fragment";
    auto fut = std::async(std::launch::async, [&] { return ts.write(u8(frag), frag.size()) && ts.flush(); });
    CHECK(finishes_within(fut, 2000ms));
    st->release();
    CHECK(fut.get());
  }
  CHECK(st->got() == "moof+mdat bytes of one fragment");
}

// Destruction is close(): everything queued reaches the inner sink, in
// order, with every flush, before the inner sink is destroyed.
TEST(threaded_sink_destructor_drains_in_order) {
  auto st = std::make_shared<GateState>();
  st->release();
  std::string expect;
  {
    ThreadedSink ts(std::make_unique<GateSink>(st));
    for (int i = 0; i < 50; ++i) {
      const std::string chunk = "chunk" + std::to_string(i) + ";";
      expect += chunk;
      CHECK(ts.write(u8(chunk), chunk.size()));
      if (i % 10 == 9) CHECK(ts.flush());
    }
  }
  CHECK(st->got() == expect);
  CHECK(st->flushes == 5);
  CHECK(st->destroyed);
}

// sync() is the durable path (DvrMux::close(durable=true)): it must not
// return before the backlog has reached the inner sink and been synced.
TEST(threaded_sink_sync_waits_for_the_backlog) {
  auto st = std::make_shared<GateState>();
  ThreadedSink ts(std::make_unique<GateSink>(st));
  const std::string a = "first fragment", b = "second fragment";
  auto wr = std::async(std::launch::async, [&] { return ts.write(u8(a), a.size()) && ts.write(u8(b), b.size()); });
  CHECK(finishes_within(wr, 2000ms));
  auto sy = std::async(std::launch::async, [&] { return ts.sync(); });
  CHECK(!finishes_within(sy, 100ms));  // the card is still stalled
  st->release();
  CHECK(wr.get());
  CHECK(sy.get());
  CHECK(st->got() == a + b);
  CHECK(st->syncs == 1);
}

// A card error surfaces on the next call (the mux stops feeding on the
// first false) and stays: nothing after a failed write reaches the card.
TEST(threaded_sink_failure_is_sticky) {
  auto st = std::make_shared<GateState>();
  st->fail = true;
  st->release();
  ThreadedSink ts(std::make_unique<GateSink>(st));
  const std::string a = "lost";
  ts.write(u8(a), a.size());
  CHECK(!ts.sync());
  CHECK(!ts.write(u8(a), a.size()));
  CHECK(!ts.flush());
  CHECK(st->got().empty());
}

// Bounded memory: a card that stalls longer than the queue holds pushes
// back on the writer instead of growing without limit.
TEST(threaded_sink_blocks_at_the_byte_cap) {
  auto st = std::make_shared<GateState>();
  ThreadedSink ts(std::make_unique<GateSink>(st), /*max_queued_bytes=*/16);
  const std::string twelve = "123456789abc";
  auto first = std::async(std::launch::async, [&] { return ts.write(u8(twelve), twelve.size()); });
  CHECK(finishes_within(first, 2000ms));
  auto second = std::async(std::launch::async, [&] { return ts.write(u8(twelve), twelve.size()); });
  CHECK(!finishes_within(second, 100ms));
  st->release();
  CHECK(first.get());
  CHECK(second.get());
  CHECK(ts.sync());
  CHECK(st->got() == twelve + twelve);
}

// End to end: a DvrMux writing through ThreadedSink produces the same file,
// byte for byte, as one writing straight to the path.
TEST(threaded_sink_mux_file_matches_direct_file) {
  ScratchFile direct("test_threaded_sink", ".mp4"), threaded("test_threaded_sink", ".mp4");
  std::vector<uint8_t> hvcc(23, 0x40);
  auto run = [&](DvrMux& mux) {
    for (int i = 0; i < 300; ++i) {
      std::vector<uint8_t> au = {0x00, 0x00, 0x01, 0x02, 0x01};
      au.resize(5 + 1000 + i, static_cast<uint8_t>(i));
      mux.write_sample(au.data(), au.size(), static_cast<uint32_t>(i * 16667), i % 60 == 0);
    }
    mux.close();
  };
  {
    DvrMux mux;
    REQUIRE(mux.open(direct.path, hvcc, 1920, 1080));
    run(mux);
  }
  {
    DvrMux mux;
    std::unique_ptr<DvrSink> file = mabur::open_file_sink(threaded.path);
    REQUIRE(file != nullptr);
    REQUIRE(mux.open(std::make_unique<ThreadedSink>(std::move(file)), hvcc, 1920, 1080));
    run(mux);
  }
  auto slurp = [](const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
  };
  const std::string d = slurp(direct.path), t = slurp(threaded.path);
  CHECK(d.size() > 300 * 1000);
  CHECK(d == t);
}

// A fragment reaches the card as a trickle, not a burst: the writer hands
// the inner sink at most pacing.slice_bytes per call, in order, so one
// fragment is many short syscalls instead of one ~22 ms stretch of kernel
// work.
TEST(threaded_sink_writes_in_slices) {
  struct ChunkSink final : DvrSink {
    std::vector<size_t>* sizes;
    std::string* bytes;
    ChunkSink(std::vector<size_t>* s, std::string* b) : sizes(s), bytes(b) {}
    bool write(const uint8_t* p, size_t n) override {
      sizes->push_back(n);
      bytes->append(reinterpret_cast<const char*>(p), n);
      return true;
    }
    bool flush() override { return true; }
  };
  std::vector<size_t> sizes;
  std::string got;
  ThreadedSink::Pacing pacing;
  pacing.slice_bytes = 300;
  pacing.slice_gap = 0ms;
  {
    ThreadedSink ts(std::make_unique<ChunkSink>(&sizes, &got), ThreadedSink::kDefaultMaxQueuedBytes,
                    pacing);
    std::string frag(1000, 'f');
    for (size_t i = 0; i < frag.size(); ++i) frag[i] = static_cast<char>('a' + i % 26);
    CHECK(ts.write(u8(frag), frag.size()));
    CHECK(ts.sync());
    CHECK(got == frag);
  }
  CHECK(sizes == (std::vector<size_t>{300, 300, 300, 100}));
}

// Between slices the writer pauses pacing.slice_gap, so the fragment's
// kernel work is spread over the second instead of landing in one burst --
// but only while the queue holds less than pacing.backlog_bytes: a backlog
// means the card is slower than the trickle and the writer must catch up.
TEST(threaded_sink_paces_slices_until_a_backlog_builds) {
  struct NullSink final : DvrSink {
    bool write(const uint8_t*, size_t) override { return true; }
    bool flush() override { return true; }
  };
  ThreadedSink::Pacing paced;
  paced.slice_bytes = 10;
  paced.slice_gap = 20ms;
  paced.backlog_bytes = 1 << 20;  // 50 queued bytes is far below it: pace
  const std::string fifty(50, 'x');
  {
    ThreadedSink ts(std::make_unique<NullSink>(), ThreadedSink::kDefaultMaxQueuedBytes, paced);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(ts.write(u8(fifty), fifty.size()));
    CHECK(ts.sync());
    const auto took = std::chrono::steady_clock::now() - t0;
    CHECK(took >= 4 * 20ms);  // five slices, four gaps
  }
  ThreadedSink::Pacing flat = paced;
  flat.backlog_bytes = 0;  // every queue is a backlog: never pause
  {
    ThreadedSink ts(std::make_unique<NullSink>(), ThreadedSink::kDefaultMaxQueuedBytes, flat);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(ts.write(u8(fifty), fifty.size()));
    CHECK(ts.sync());
    CHECK(std::chrono::steady_clock::now() - t0 < 40ms);
  }
}

// Record stop destroys the sink on the main loop: the drain must not pay
// the trickle's gaps, or every stop would freeze the picture for the length
// of the open fragment's trickle.
TEST(threaded_sink_destructor_drains_without_pacing) {
  struct NullSink final : DvrSink {
    bool write(const uint8_t*, size_t) override { return true; }
    bool flush() override { return true; }
  };
  ThreadedSink::Pacing pacing;
  pacing.slice_bytes = 10;
  pacing.slice_gap = 50ms;
  pacing.backlog_bytes = 1 << 20;
  const std::string hundred(100, 'x');  // ten slices: 450 ms if paced
  const auto t0 = std::chrono::steady_clock::now();
  {
    ThreadedSink ts(std::make_unique<NullSink>(), ThreadedSink::kDefaultMaxQueuedBytes, pacing);
    CHECK(ts.write(u8(hundred), hundred.size()));
  }
  CHECK(std::chrono::steady_clock::now() - t0 < 250ms);
}

// The writer is background work: it runs ThreadedSink::kWriterNiceStep nice
// steps below the thread that created the sink, so the scheduler gives a
// display loop that wakes during a fragment write its CPU first.
TEST(threaded_sink_writer_runs_at_lower_priority) {
  struct NiceSink final : DvrSink {
    int* seen;
    explicit NiceSink(int* s) : seen(s) {}
    bool write(const uint8_t*, size_t) override {
      *seen = ::getpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)));
      return true;
    }
    bool flush() override { return true; }
  };
  const int base = ::getpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)));
  int seen = -100;
  {
    ThreadedSink ts(std::make_unique<NiceSink>(&seen));
    const std::string a = "x";
    CHECK(ts.write(u8(a), a.size()));
    CHECK(ts.sync());
  }
  const int expect = base + ThreadedSink::kWriterNiceStep > 19 ? 19 : base + ThreadedSink::kWriterNiceStep;
  CHECK(seen == expect);
}

// A path that cannot be opened gives no sink, which DvrMux::open refuses.
TEST(threaded_sink_open_file_sink_fails_on_bad_path) {
  CHECK(mabur::open_file_sink("/nonexistent-dir/x.mp4") == nullptr);
}

MTEST_MAIN
