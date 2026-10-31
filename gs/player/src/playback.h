#ifndef MABUR_PLAYER_PLAYBACK_H_
#define MABUR_PLAYER_PLAYBACK_H_

// The GS menu's Playback tab (gs18): the recordings it lists and the pacing
// it plays them at. Pure; main.cpp parks the live feed, flushes the decoder
// and submits the file's AUs (mabur/dvr_demux.h) as pace() says, then
// brings the live video back.
//
// Only while the drone is disarmed (or reports no FC): the live picture is
// gone for as long as a recording plays, so an arm ends playback at once.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace maburplay {

struct Recording {
  std::string path;
  std::string name;          // "record-0012"
  int index = -1;            // NNNN
  uint64_t duration_us = 0;  // 0 = unreadable (not offered)
  uint64_t bytes = 0;
};

// record-NNNN.mp4 in `dir`, or in its own record-NNNN/ folder there
// (dvr.folders, gs18), newest (highest NNNN) first, at most `max`.
// The GS clock is wrong at boot, so the index, not the file time, says
// which is newest (mabur/dvr_name.h). Files DvrDemux cannot open are left
// out.
std::vector<Recording> list_recordings(const std::string& dir, size_t max = 8);

// When to hand each sample to the decoder: on the file's own timeline from
// the moment play started, `lead_us` early so the decoder always holds the
// next picture; a pause stops the clock.
class PlaybackPacer {
 public:
  explicit PlaybackPacer(uint64_t lead_us = 30000) : lead_us_(lead_us) {}
  void start(uint64_t now_us, uint64_t first_pts_us) {
    t0_us_ = now_us;
    pts0_us_ = first_pts_us;
    paused_ = false;
  }
  bool due(uint64_t now_us, uint64_t pts_us) const {
    if (paused_) return false;
    const uint64_t at = t0_us_ + (pts_us - pts0_us_);
    return now_us + lead_us_ >= at;
  }
  void pause(uint64_t now_us) {
    if (paused_) return;
    paused_ = true;
    paused_at_us_ = now_us;
  }
  void resume(uint64_t now_us) {
    if (!paused_) return;
    paused_ = false;
    t0_us_ += now_us - paused_at_us_;   // the timeline waited with us
  }
  bool paused() const { return paused_; }
  // Where the file's timeline is now, from its first sample.
  uint64_t position_us(uint64_t now_us) const {
    const uint64_t t = paused_ ? paused_at_us_ : now_us;
    return t > t0_us_ ? t - t0_us_ : 0;
  }

 private:
  uint64_t lead_us_;
  uint64_t t0_us_ = 0, pts0_us_ = 0, paused_at_us_ = 0;
  bool paused_ = false;
};

// "m:ss" (or "h:mm:ss" from an hour).
inline std::string clock_text(uint64_t us) {
  const uint64_t s = us / 1000000;
  char b[24];
  if (s >= 3600)
    std::snprintf(b, sizeof(b), "%u:%02u:%02u", static_cast<unsigned>(s / 3600),
                  static_cast<unsigned>(s / 60 % 60), static_cast<unsigned>(s % 60));
  else
    std::snprintf(b, sizeof(b), "%u:%02u", static_cast<unsigned>(s / 60),
                  static_cast<unsigned>(s % 60));
  return b;
}

// The line shown while a recording plays.
inline std::string playback_line(const std::string& name, uint64_t pos_us, uint64_t dur_us,
                                 bool paused) {
  if (pos_us > dur_us) pos_us = dur_us;
  return std::string(paused ? "Paused " : "Playing ") + name + "  " + clock_text(pos_us) + " / " +
         clock_text(dur_us) + (paused ? "  - centre: play, left: back to live"
                                      : "  - centre: pause, left: back to live");
}

}  // namespace maburplay

#endif  // MABUR_PLAYER_PLAYBACK_H_
