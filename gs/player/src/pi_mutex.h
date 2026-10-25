#ifndef MABUR_PLAYER_PI_MUTEX_H_
#define MABUR_PLAYER_PI_MUTEX_H_

#include <pthread.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace maburplay {

// A mutex for the release thread's data path: BasicLockable/Lockable
// (lock/unlock/try_lock), so it drops into std::lock_guard and
// std::unique_lock exactly where a std::mutex did. Every holder of these
// locks runs for microseconds, but a normal-priority holder (decode pump,
// housekeeping) that CFS preempts mid-section would otherwise make the
// SCHED_FIFO release thread wait out a whole scheduler slice --
// PTHREAD_PRIO_INHERIT has the kernel raise that holder to the waiter's
// priority for the duration, so it runs to completion and releases
// promptly instead of sitting preempted.
//
// If the platform refuses priority inheritance, falls back to a plain
// mutex rather than aborting: priority inheritance only trims the RT
// thread's worst-case wait, it is never required for correctness. The
// refusal can come from either call: pthread_mutexattr_setprotocol only
// validates that PTHREAD_PRIO_INHERIT is a known enum value, so a kernel
// without PI futexes still accepts the attribute there and refuses only
// later, inside pthread_mutex_init -- checking just the attribute call
// and ignoring pthread_mutex_init's return would leave the mutex
// uninitialised (undefined behaviour on every lock()) while reporting PI
// as active. Only a successful pthread_mutex_init, with whichever
// protocol it was given, counts.
class PiMutex {
 public:
  PiMutex() {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    const bool want_pi = pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) == 0;
    const int rc = want_pi ? pthread_mutex_init(&m_, &attr) : -1;
    pthread_mutexattr_destroy(&attr);
    if (rc == 0) {
      pi_ = true;
      return;
    }
    // Either the attribute or the init itself was refused: fall back to
    // the platform's default mutex. A mutex that cannot even be created
    // with a default attribute is unrecoverable -- every caller on the
    // release thread's data path assumes locking one always succeeds.
    const int rc2 = pthread_mutex_init(&m_, nullptr);
    if (rc2 != 0) {
      std::fprintf(stderr, "maburplay: PiMutex: pthread_mutex_init failed: %s\n",
                   std::strerror(rc2));
      std::abort();
    }
    pi_ = false;
  }
  ~PiMutex() { pthread_mutex_destroy(&m_); }
  PiMutex(const PiMutex&) = delete;
  PiMutex& operator=(const PiMutex&) = delete;

  void lock() { pthread_mutex_lock(&m_); }
  void unlock() { pthread_mutex_unlock(&m_); }
  bool try_lock() { return pthread_mutex_trylock(&m_) == 0; }

  // True when this instance's mutex was actually created with
  // PTHREAD_PRIO_INHERIT (pthread_mutex_init succeeded with it, not just
  // the attribute call). For tests: it does not affect lock/unlock/
  // try_lock behaviour, only whether priority inheritance is active
  // underneath it.
  bool pi_active() const { return pi_; }

  // True when the platform actually creates a PTHREAD_PRIO_INHERIT mutex,
  // checked by constructing and destroying a throwaway one (the
  // attribute call alone is not enough -- see the class comment).
  static bool pi_supported() {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    bool ok = pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) == 0;
    if (ok) {
      pthread_mutex_t probe;
      ok = pthread_mutex_init(&probe, &attr) == 0;
      if (ok) pthread_mutex_destroy(&probe);
    }
    pthread_mutexattr_destroy(&attr);
    return ok;
  }

 private:
  pthread_mutex_t m_;
  bool pi_ = false;
};

}  // namespace maburplay

#endif  // MABUR_PLAYER_PI_MUTEX_H_
