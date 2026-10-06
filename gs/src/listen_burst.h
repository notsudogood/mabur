#pragma once
#include <cstdint>
#include <optional>

#include "config.h"
#include "mabur/rc_proto.h"

namespace maburgs {

// Listen window, GS side (feedback-repair rollout phase 3,
// docs/feedback-repair-rollout.md): when did a drone burst end?
//
// The GS sends one T_STATUS at every burst end, into the quiet gap the drone
// keeps after it (drone/src/listen_window.h). A burst ends with its probe body
// (one trails every AU while a probe is commanded), so the probe's arrival is
// the signal -- the same one RcfSlotter releases on. Each AU yields exactly
// one burst end, named by its frame id (the probe's enh_fid, which the drone
// keys its gaps by):
//   * the first probe copy for an AU, from either card (the second card's
//     copy is the same burst end and is ignored);
//   * the learned deadline (completion + tail_ub) if that AU's probe never
//     arrives;
//   * the AU's completion itself when no probe is commanded.
// The next AU's first body cancels a pending deadline: its burst has started,
// so the gap is gone and a status now would land in it.
//
// Pure logic, core thread only; time is the core loop's mono ms.
struct BurstEnd {
  mabur::rc::StatusTrig trig = mabur::rc::StatusTrig::Probe;
  uint16_t fid = mabur::rc::kStatusNoFid;
  uint64_t t_ms = 0;
};

class BurstEndTracker {
 public:
  // FrameStream begin-of-AU: the AU now on air.
  void on_au_first(uint16_t fid) {
    cur_fid_ = fid;
    have_cur_ = true;
    armed_ = false;
  }

  // FrameStream end-of-AU. probe_follows: a probe body trails this AU.
  void on_au_complete(uint64_t now_ms, bool probe_follows, int tail_ub_ms) {
    if (!have_cur_) return;
    if (!probe_follows) {
      emit(mabur::rc::StatusTrig::Completion, cur_fid_, now_ms);
      return;
    }
    if (emitted_for(cur_fid_)) return;  // its probe already arrived
    armed_ = true;
    armed_fid_ = cur_fid_;
    deadline_ms_ = now_ms + static_cast<uint64_t>(tail_ub_ms < 0 ? 0 : tail_ub_ms);
  }

  // A probe body arrived on a card. fid: its enh_fid, or nullopt when no
  // block of it parsed (then it belongs to the armed AU, else the current).
  void on_probe(uint64_t now_ms, std::optional<uint16_t> fid) {
    const uint16_t f = fid ? *fid : (armed_ ? armed_fid_ : cur_fid_);
    if (!fid && !armed_ && !have_cur_) return;
    if (emitted_for(f)) return;  // the other card's copy of the same end
    emit(mabur::rc::StatusTrig::Probe, f, now_ms);
    if (armed_ && armed_fid_ == f) armed_ = false;
  }

  // Core-loop tick.
  void poll(uint64_t now_ms) {
    if (armed_ && now_ms >= deadline_ms_) {
      armed_ = false;
      emit(mabur::rc::StatusTrig::Deadline, armed_fid_, now_ms);
    }
  }

  // The burst end since the last take, if any (a newer one replaces an
  // untaken older one: only the gap now open is worth a status).
  std::optional<BurstEnd> take() {
    auto out = pending_;
    pending_.reset();
    return out;
  }

 private:
  bool emitted_for(uint16_t fid) const { return have_emitted_ && emitted_fid_ == fid; }
  void emit(mabur::rc::StatusTrig trig, uint16_t fid, uint64_t now_ms) {
    pending_ = BurstEnd{trig, fid, now_ms};
    have_emitted_ = true;
    emitted_fid_ = fid;
  }

  uint16_t cur_fid_ = 0;
  bool have_cur_ = false;
  bool armed_ = false;
  uint16_t armed_fid_ = 0;
  uint64_t deadline_ms_ = 0;
  bool have_emitted_ = false;
  uint16_t emitted_fid_ = 0;
  std::optional<BurstEnd> pending_;
};

// Whether the listen window is on at now_ms: [listen] ms > 0, and with
// ab_s > 0 only in the even ab_s-long periods, so one flight carries both
// arms of the A/B (off in the odd ones, today's RcfSlotter behaviour).
inline bool listen_on(const ListenCfg& c, uint64_t now_ms) {
  if (c.ms <= 0) return false;
  if (c.ab_s <= 0) return true;
  return (now_ms / (static_cast<uint64_t>(c.ab_s) * 1000u)) % 2 == 0;
}

}  // namespace maburgs
