#ifndef MABUR_PLAYER_WORKER_THREAD_H_
#define MABUR_PLAYER_WORKER_THREAD_H_

#include <pthread.h>

#include <csignal>
#include <thread>
#include <utility>

namespace maburplay {

// Starts a worker thread that never takes SIGINT, SIGTERM or SIGUSR1:
// those belong to the housekeeping thread, whose wait they must end. The
// caller blocks every signal around the thread's creation, so the new
// thread starts with all of them blocked and no signal can land on it
// before fn's first instruction; the thread then sets its mask to the
// caller's plus those three, and the caller gets its own mask back.
template <class Fn>
std::thread start_worker_thread(Fn&& fn) {
  sigset_t all, old;
  sigfillset(&all);
  pthread_sigmask(SIG_SETMASK, &all, &old);
  sigset_t want = old;
  sigaddset(&want, SIGINT);
  sigaddset(&want, SIGTERM);
  sigaddset(&want, SIGUSR1);
  std::thread t([want, f = std::forward<Fn>(fn)]() mutable {
    pthread_sigmask(SIG_SETMASK, &want, nullptr);
    f();
  });
  pthread_sigmask(SIG_SETMASK, &old, nullptr);
  return t;
}

}  // namespace maburplay

#endif  // MABUR_PLAYER_WORKER_THREAD_H_
