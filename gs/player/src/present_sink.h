#ifndef MABUR_PLAYER_PRESENT_SINK_H_
#define MABUR_PLAYER_PRESENT_SINK_H_

#include <cstdint>

#include "video_backend.h"  // DmaFrame

namespace maburplay {

// The display as the release thread sees it: what it presents frames to,
// polls flip completions from, and commits a pending OSD through.
// DrmPresenter implements it; host tests fake it. Every call comes from
// the release thread, the one thread that owns presentation, except
// osd_publish_pending(): the housekeeping thread also reads it before it
// draws the OSD, so an implementation keeps it safe to read concurrently.
class PresentSink {
 public:
  virtual ~PresentSink() = default;

  // What the last present() did: when its atomic commit returned (mono µs;
  // 0 when the frame was parked or the commit failed) and how it was
  // shaped (the implementation's commit bits).
  struct CommitInfo {
    uint64_t t_commit_us = 0;
    uint8_t bits = 0;
  };

  // Takes ownership of the frame; false = dropped (already released).
  virtual bool present(const DmaFrame&) = 0;
  // Reaps flip completions without blocking.
  virtual void poll_events() = 0;
  // A descriptor that turns readable when poll_events() has work; -1 when
  // there is none and poll_events() runs on the caller's timer only.
  virtual int event_fd() const = 0;
  // Releases every held frame now and shows nothing until the next present().
  virtual void drop_all() = 0;
  // Paced (vsync servo locked): a frame that misses its latch is dropped
  // rather than parked and resubmitted late.
  virtual void set_paced(bool) = 0;
  virtual uint64_t mailbox_engagements() const = 0;
  virtual CommitInfo last_commit() const = 0;
  // A published OSD back buffer is waiting for a commit.
  virtual bool osd_publish_pending() const = 0;
  // The OSD-only commit, for when no video commit is in flight to carry
  // the pending OSD; a no-op when its conditions do not hold.
  virtual void commit_osd_if_pending() = 0;
};

}  // namespace maburplay

#endif  // MABUR_PLAYER_PRESENT_SINK_H_
