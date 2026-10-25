#include <mutex>
#include <thread>
#include <vector>

#include "mtest.h"
#include "pi_mutex.h"

using maburplay::PiMutex;

TEST(lock_unlock_round_trips) {
  PiMutex m;
  m.lock();
  m.unlock();
  m.lock();
  m.unlock();
}

TEST(try_lock_fails_while_held_by_another_thread) {
  PiMutex m;
  m.lock();
  std::thread t([&] { CHECK(!m.try_lock()); });
  t.join();
  m.unlock();
  CHECK(m.try_lock());
  m.unlock();
}

TEST(lock_guard_and_unique_lock_compile_and_work) {
  PiMutex m;
  {
    std::lock_guard<PiMutex> lk(m);
  }
  {
    std::unique_lock<PiMutex> lk(m);
    CHECK(lk.owns_lock());
  }
}

TEST(mutual_exclusion_under_two_threads) {
  PiMutex m;
  long counter = 0;
  auto bump = [&] {
    for (int i = 0; i < 100000; ++i) {
      std::lock_guard<PiMutex> lk(m);
      ++counter;
    }
  };
  std::thread a(bump), b(bump);
  a.join();
  b.join();
  CHECK(counter == 200000);
}

TEST(protocol_attribute_is_accepted_on_this_platform) {
  // The host (Linux/glibc) supports PTHREAD_PRIO_INHERIT; a PiMutex
  // constructed here should have it active, and the static probe should
  // agree.
  CHECK(PiMutex::pi_supported());
  PiMutex m;
  CHECK(m.pi_active());
}

MTEST_MAIN
