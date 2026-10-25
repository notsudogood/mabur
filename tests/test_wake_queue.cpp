#include <poll.h>
#include <thread>
#include <vector>

#include "wake_queue.h"
#include "mtest.h"

namespace {

// Helper: check if an fd is readable with poll(). 0 timeout = non-blocking.
bool readable(int fd) {
  pollfd pfd = {fd, POLLIN, 0};
  int ret = poll(&pfd, 1, 0);
  return ret > 0 && (pfd.revents & POLLIN);
}

}  // namespace

TEST(push_makes_fd_readable_and_drain_clears_it) {
  maburplay::WakeQueue<int> q;
  q.push(1);
  q.push(2);
  CHECK(readable(q.fd()));
  std::vector<int> out;
  CHECK(q.drain(&out) == 2);
  CHECK(out.size() == 2 && out[0] == 1 && out[1] == 2);
  CHECK(!readable(q.fd()));
  CHECK(q.size() == 0);
}

TEST(drain_from_another_thread_sees_every_push_in_order) {
  maburplay::WakeQueue<int> q;
  std::thread t([&] {
    for (int i = 0; i < 1000; ++i) {
      q.push(i);
    }
  });
  std::vector<int> got;
  while (got.size() < 1000) {
    pollfd pfd = {q.fd(), POLLIN, 0};
    poll(&pfd, 1, 100);  // 100ms timeout to avoid busy waiting
    q.drain(&got);
  }
  t.join();
  for (int i = 0; i < 1000; ++i) {
    CHECK(got[i] == i);
  }
}

TEST(handed_frame_carries_the_decode_stamp) {
  maburplay::WakeQueue<maburplay::HandedFrame> q;
  maburplay::DmaFrame f;
  f.pts_us = 7;
  q.push({f, 123});
  std::vector<maburplay::HandedFrame> out;
  q.drain(&out);
  CHECK(out.size() == 1);
  CHECK(out[0].f.pts_us == 7);
  CHECK(out[0].t_decoded_us == 123);
}

MTEST_MAIN
