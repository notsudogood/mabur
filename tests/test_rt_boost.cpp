#include <pthread.h>
#include <sched.h>

#include "mtest.h"
#include "rt_boost.h"

using mabur::RtBoost;

namespace {
int policy_of(pthread_t t, int* prio) {
  int pol = -1;
  sched_param p{};
  pthread_getschedparam(t, &pol, &p);
  *prio = p.sched_priority;
  return pol;
}
}  // namespace

TEST(rt_boost_off_touches_nothing) {
  int p0 = 0;
  const int pol0 = policy_of(pthread_self(), &p0);
  {
    RtBoost b({pthread_self()}, 0);
    CHECK(b.raised() == 0);
    CHECK(b.note().empty());
    int p = 0;
    CHECK(policy_of(pthread_self(), &p) == pol0 && p == p0);
  }
}

// Raised for the scope and put back after; where the host refuses SCHED_FIFO
// (no CAP_SYS_NICE) the boost raises nothing, says so, and still leaves the
// thread as it was.
TEST(rt_boost_restores_the_thread_after_its_scope) {
  int p0 = 0;
  const int pol0 = policy_of(pthread_self(), &p0);
  {
    RtBoost b({pthread_self()}, 10);
    int p = 0;
    const int pol = policy_of(pthread_self(), &p);
    if (b.raised() == 1) {
      CHECK(pol == SCHED_FIFO && p == 10);
      CHECK(b.note() == " [rt 10 x1]");
    } else {
      CHECK(pol == pol0 && p == p0);
      CHECK(b.note().find("failed") != std::string::npos);
    }
  }
  int p = 0;
  CHECK(policy_of(pthread_self(), &p) == pol0 && p == p0);
}

MTEST_MAIN
