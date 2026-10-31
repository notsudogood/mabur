#pragma once
// First-boot floor calibration (gs18 + d19): the GS side.
//
// A d19 drone learns its own wide-TX IGI floor (drone/src/igi_floor_learn.h)
// and marks 40 and 80 MHz verified after 10 s of clean sending at each. It
// reports the floor, its stalls and the two marks in the Telem floor block
// (mabur::rc::kTelemExtFloor). A drone that has not verified a width the GS
// flies gets it verified here, on the ground, before it flies: while the
// drone is disarmed (or has no FC) and linked, this holds the ladder on a
// 40 MHz rung, then an 80 MHz one, until the drone reports the mark. A
// stall on the way is the drone's to catch and answer (one floor step);
// this then drops to a 20 MHz rung for unstick_ms, so a frame the radio is
// still holding cannot keep the test stuck, and tries the width again.
//
// The player shows each state on screen (maburplay, GsMenuModel): "testing
// 40 MHz", "stall caught, floor 0x24 - testing again", "finished".
//
// It never runs on: a drone that is not learning (a hand-set
// radio.wide_igi_floor, igi_floor_auto = false, an older image: no block),
// an armed or stale FC, a lost link, a channel whose span holds an 8812EU
// spike (the drone cannot learn there; it waits for another channel). It
// gives up for this GS session after phase_max_ms without a mark at one
// width, or on a stall at the top floor (0x35): the GS's latency guard then
// handles stalls in flight as before d18.
//
// Pure: main.cpp feeds the drone's view each loop and applies the forced
// rung (VrxController::set_forced_rung).
#include <cstdint>

namespace maburgs {

struct FloorCalCfg {
  bool enable = true;
  uint32_t phase_max_ms = 60000;  // one width: no mark this long = give up
  uint32_t unstick_ms = 2500;     // after a stall: a 20 MHz rung this long
  uint32_t done_show_ms = 10000;  // "finished" stays the state this long
};

// The drone's Telem floor block, as of its last fresh Telem.
struct FloorView {
  bool have = false;      // a fresh Telem carried the block
  bool learning = false;  // kFloorLearning
  bool v40 = false, v80 = false;
  bool spike = false;     // kFloorSpike: the tuned span holds a spike
  bool top = false;       // kFloorTop
  uint8_t floor = 0;
  uint8_t stalls = 0;
  uint8_t arm = 0;        // 0 no FC, 1 disarmed, 2 armed, 3 stale
};

// Ladder rungs to hold: the lowest-MCS rung at 40 and at 80 MHz (-1 = the
// ladder has none: that width is not flown, so not tested) and a 20 MHz
// rung for the unstick.
struct FloorCalRungs {
  int r40 = -1, r80 = -1, r20 = 0;
};

class FloorCal {
 public:
  enum class State : uint8_t { Off, Idle, Test, Unstick, Paused, Done, Failed };

  FloorCal(FloorCalCfg cfg, FloorCalRungs rungs) : cfg_(cfg), rungs_(rungs) {}

  // One loop. Returns the rung to hold, -1 for none (the ladder runs).
  int tick(uint64_t now_ms, bool linked, const FloorView& d) {
    if (!cfg_.enable || !d.have || !d.learning) {
      set_(State::Off, now_ms);
      return -1;
    }
    if (d.stalls < stalls_seen_) stalls_seen_ = d.stalls;   // a restarted drone
    const bool need40 = !d.v40 && rungs_.r40 >= 0;
    const bool need80 = !d.v80 && rungs_.r80 >= 0;
    if (state_ == State::Failed) return -1;
    if (!need40 && !need80) {
      if (state_ == State::Test || state_ == State::Unstick || state_ == State::Paused) {
        if (state_ != State::Paused || ran_) set_(State::Done, now_ms);
        else set_(State::Idle, now_ms);
      } else if (state_ == State::Done && now_ms - since_ < cfg_.done_show_ms) {
        // keep showing it
      } else {
        set_(State::Idle, now_ms);
      }
      floor_ = d.floor;
      return -1;
    }
    floor_ = d.floor;
    const int w = need40 ? 40 : 80;
    // Blocked: armed or no fresh arm state, no link, or a spike channel.
    const char* block = !linked ? "link" : d.arm == 2 ? "armed" : d.arm == 3 ? "arm_stale"
                        : d.spike ? "spike" : nullptr;
    if (block) {
      why_ = block;
      set_(State::Paused, now_ms);
      return -1;
    }
    why_ = "";
    if (state_ == State::Unstick) {
      if (now_ms - since_ < cfg_.unstick_ms) return rungs_.r20;
      start_test_(w, now_ms, d);
      return rung_for_(w);
    }
    if (state_ != State::Test || width_ != w) {
      start_test_(w, now_ms, d);
      return rung_for_(w);
    }
    if (d.stalls > stalls_seen_) {
      stalls_seen_ = d.stalls;
      ++steps_;
      if (d.top) {
        why_ = "top";
        set_(State::Failed, now_ms);
        return -1;
      }
      set_(State::Unstick, now_ms);
      return rungs_.r20;
    }
    if (now_ms - since_ > cfg_.phase_max_ms) {
      why_ = "timeout";
      set_(State::Failed, now_ms);
      return -1;
    }
    return rung_for_(w);
  }

  State state() const { return state_; }
  // The width under test (Test, Unstick), the drone's floor as last
  // reported, stalls caught during this calibration, why it paused or gave
  // up ("link", "armed", "arm_stale", "spike" / "timeout", "top").
  int width() const { return width_; }
  uint8_t floor() const { return floor_; }
  int steps() const { return steps_; }
  const char* why() const { return why_; }
  static const char* name(State s) {
    switch (s) {
      case State::Off: return "off";
      case State::Idle: return "idle";
      case State::Test: return "test";
      case State::Unstick: return "unstick";
      case State::Paused: return "paused";
      case State::Done: return "done";
      case State::Failed: return "failed";
    }
    return "off";
  }

 private:
  int rung_for_(int w) const { return w == 80 ? rungs_.r80 : rungs_.r40; }
  void start_test_(int w, uint64_t now_ms, const FloorView& d) {
    width_ = w;
    stalls_seen_ = d.stalls;
    ran_ = true;
    set_(State::Test, now_ms, /*force=*/true);
  }
  void set_(State s, uint64_t now_ms, bool force = false) {
    if (s == state_ && !force) return;
    state_ = s;
    since_ = now_ms;
    if (s == State::Off || s == State::Idle) {
      width_ = 0;
      ran_ = false;
      steps_ = 0;
    }
  }

  FloorCalCfg cfg_;
  FloorCalRungs rungs_;
  State state_ = State::Off;
  uint64_t since_ = 0;
  int width_ = 0;
  uint8_t stalls_seen_ = 0;
  uint8_t floor_ = 0;
  int steps_ = 0;
  bool ran_ = false;
  const char* why_ = "";
};

}  // namespace maburgs
