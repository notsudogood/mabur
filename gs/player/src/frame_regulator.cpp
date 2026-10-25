#include "frame_regulator.h"

#include <mutex>
#include <utility>

namespace maburplay {

bool FrameRegulator::enabled() const {
  std::lock_guard<PiMutex> lock(mu_);
  return d_us_ > 0;
}

bool FrameRegulator::holding() const {
  std::lock_guard<PiMutex> lock(mu_);
  return count_ > 0;
}

void FrameRegulator::on_flip(uint64_t flip_us, bool exact) {
  std::lock_guard<PiMutex> lock(mu_);
  est_.on_flip(flip_us, exact);
}

void FrameRegulator::set_panel_period(double period_us) {
  std::lock_guard<PiMutex> lock(mu_);
  est_ = VblankEstimator(period_us);
  last_out_v_ = 0;  // a vblank on the old grid means nothing on the new one
}

void FrameRegulator::reset_output_history() {
  std::lock_guard<PiMutex> lock(mu_);
  last_out_v_ = 0;
}

uint64_t FrameRegulator::held_count() const {
  std::lock_guard<PiMutex> lock(mu_);
  return held_count_;
}

uint64_t FrameRegulator::late_count() const {
  std::lock_guard<PiMutex> lock(mu_);
  return late_count_;
}

uint64_t FrameRegulator::replaced_count() const {
  std::lock_guard<PiMutex> lock(mu_);
  return replaced_count_;
}

uint64_t FrameRegulator::discont_count() const {
  std::lock_guard<PiMutex> lock(mu_);
  return discont_count_;
}

double FrameRegulator::hold_ema_ms() const {
  std::lock_guard<PiMutex> lock(mu_);
  return hold_ema_ms_;
}

uint64_t FrameRegulator::vsync_skips() const {
  std::lock_guard<PiMutex> lock(mu_);
  return vsync_skips_;
}

uint64_t FrameRegulator::chained_count() const {
  std::lock_guard<PiMutex> lock(mu_);
  return chained_count_;
}

uint64_t FrameRegulator::chain_run() const {
  std::lock_guard<PiMutex> lock(mu_);
  return chain_run_;
}

uint64_t FrameRegulator::chain_max() const {
  std::lock_guard<PiMutex> lock(mu_);
  return chain_max_;
}

uint64_t FrameRegulator::chains_count() const {
  std::lock_guard<PiMutex> lock(mu_);
  return chains_count_;
}

uint64_t FrameRegulator::chain_cuts() const {
  std::lock_guard<PiMutex> lock(mu_);
  return chain_cuts_;
}

uint64_t FrameRegulator::fallback_frames() const {
  std::lock_guard<PiMutex> lock(mu_);
  return fallback_frames_;
}

void FrameRegulator::note_incomplete(uint32_t pts_us) {
  std::lock_guard<PiMutex> lock(mu_);
  incomplete_[incomplete_head_] = pts_us;
  incomplete_head_ = (incomplete_head_ + 1) % kIncomplete;
}

double FrameRegulator::allowance_ms() const {
  std::lock_guard<PiMutex> lock(mu_);
  return static_cast<double>(allowance_us_) / 1000.0;
}

uint64_t FrameRegulator::late_slot_count() const {
  std::lock_guard<PiMutex> lock(mu_);
  return late_slot_;
}

bool FrameRegulator::servo_locked() const {
  std::lock_guard<PiMutex> lock(mu_);
  return servo_now_;
}

void FrameRegulator::set_phase_lock_mode(bool on) {
  std::lock_guard<PiMutex> lock(mu_);
  phase_lock_mode_ = on;
}

void FrameRegulator::set_capture_placement(bool on) {
  std::lock_guard<PiMutex> lock(mu_);
  capture_on_ = on;
}

bool FrameRegulator::take_margin(double* margin_us) {
  std::lock_guard<PiMutex> lock(mu_);
  if (!margin_fresh_) return false;
  margin_fresh_ = false;
  *margin_us = margin_us_;
  return true;
}

double FrameRegulator::panel_period_us() const {
  std::lock_guard<PiMutex> lock(mu_);
  return est_.period_us();
}

uint64_t FrameRegulator::heals() const {
  std::lock_guard<PiMutex> lock(mu_);
  return heals_;
}

uint64_t FrameRegulator::next_release_us() const {
  std::lock_guard<PiMutex> lock(mu_);
  return count_ ? held_[0].release_us : 0;
}

void FrameRegulator::displace(int idx, Displaced* out) {
  if (idx >= count_) return;
  if (out && out->n < kMaxHeld) out->f[out->n++] = held_[idx].f;
  for (int i = idx; i + 1 < count_; ++i) held_[i] = held_[i + 1];
  --count_;
  ++replaced_count_;
}

bool FrameRegulator::offer(const DmaFrame& f, uint64_t arrival_us,
                           uint64_t now_us, Displaced* out) {
  std::lock_guard<PiMutex> lock(mu_);
  out->n = 0;
  margin_fresh_ = false;
  if (d_us_ <= 0) return true;

  const auto obs = anchor_.observe(f.pts_us, arrival_us);
  if (obs.discont) {
    // Encoder clock discontinuity (drone restart): the old timebase is
    // dead. Free any held old-timebase frames, show this one now, and
    // let the NEXT frame seed a fresh floor.
    while (count_ > 0) displace(0, out);
    ++discont_count_;
    last_out_v_ = 0;
    return true;
  }

  uint64_t release = 0;
  uint64_t target_v = 0;
  servo_now_ = vsync_lock_ && est_.valid(now_us);
  bool by_capture = false;
  if (servo_now_ && sched_cap_us_ > 0) {
    CaptureSlot slot;
    if (capture_slot(obs.pts64, f.pts_us, arrival_us, &slot)) {
      margin_us_ = static_cast<double>(static_cast<int64_t>(slot.r.release_us) - slot.want);
      margin_fresh_ = true;
      if (!phase_lock_mode_ || capture_on_)
        by_capture = place_by_capture(slot, now_us, &release, &target_v, out);
    }
  }
  if (servo_now_ && !by_capture) {
    // The implausible-phase gate stays anchored to now_us via
    // next_release(), unchanged from before this split: it asks "is the
    // NEXT release from where we are now plausible", and a corrupted
    // phase (e.g. a single flip timestamp jumping into the future) fails
    // it the same way it always did. release_at_or_after(arrival_us, ...)
    // would NOT fail it here -- it's designed to find the right vblank
    // for an arrival far from phase, so it never reads as implausible,
    // which is exactly why it's only used below to pick the slot, not to
    // gate trust in the grid.
    const auto gate = est_.next_release(now_us, lead_us_);
    const uint64_t clamp =
        static_cast<uint64_t>(2.0 * est_.period_us()) + lead_us_;
    if (gate.release_us > now_us + clamp) {
      // Implausible hold (corrupted phase): don't trust the grid for
      // this frame; the estimator self-heals on the next real flips.
      servo_now_ = false;
    } else {
      // The slot itself comes from arrival_us, not now_us:
      // release_at_or_after (unlike next_release) accepts a time before
      // the last flip and returns the vblank the frame would actually
      // land on if the pipeline weren't stalled. A hand-over whose own
      // slot has already gone by (a pump stall between decode and
      // offer) reads release_us in the past; the late check below then
      // presents it at once instead of parking it on the NEXT slot,
      // which next_release(now_us, ...) would always have forced (its
      // release is >= now_us by construction) -- a guaranteed
      // one-period-late repeat for every stalled hand-over.
      const auto r = est_.release_at_or_after(arrival_us, lead_us_);
      release = r.release_us;
      target_v = r.vblank_us;
      const uint64_t per = static_cast<uint64_t>(est_.period_us());
      const uint64_t hp = per / 2;
      // ...except onto a vblank a frame that already left was aimed at:
      // that frame is committed for it (or queued behind the flip before
      // it), and a newer one presented now would queue behind it and make
      // it show twice. The first vblank after the taken one is the
      // earliest this frame can follow it. Half a period of tolerance, as
      // for a held occupant below, absorbs a grid re-estimate between the
      // two frames.
      if (last_out_v_ != 0 && per > 0 && target_v < last_out_v_ + hp) {
        target_v += (last_out_v_ + hp - target_v + per - 1) / per * per;
        release = target_v > lead_us_ ? target_v - lead_us_ : 0;
      }
      // Sequential-slot assignment (hw): base+enh of a
      // capture pair complete together (FEC generation close), so their
      // decodes land within ~1 ms while their pts sit a full frame
      // apart. Arrival-time targeting alone aims both at the same vblank
      // and freshest-wins then drops the earlier of nearly every pair
      // (bench: ~14 skips/s, 1-in-4 frames). pts order is decode order,
      // so a frame whose natural slot is occupied by a held predecessor
      // takes the NEXT slot instead. Deliberately relative to the
      // frame's OWN natural slot, never chained off the queue's furthest
      // target -- natural advances with the clock, so targets are
      // bounded at natural+period and holds at ~2 periods by
      // construction (chaining would creep unboundedly under a
      // faster-than-panel source). If the next slot is occupied too
      // (deep burst), the newest frame claims it -- freshest wins.
      const auto occupant = [&](uint64_t slot) {
        for (int i = 0; i < count_; ++i) {
          const uint64_t hv = held_[i].target_v;
          if (hv != 0 && (hv > slot ? hv - slot : slot - hv) < hp) return i;
        }
        return -1;
      };
      const int occ = occupant(target_v);
      if (occ >= 0 && chain_budget_ > 0 && chain_run_ >= chain_budget_) {
        // Budget spent: cut the chain here. The occupant is the older
        // frame; freshest wins its slot and the stream is re-aligned.
        displace(occ, out);
        ++chain_cuts_;
        chain_run_ = 0;
      } else if (occ >= 0) {
        const uint64_t seq = target_v + per;
        const int later = occupant(seq);
        if (later >= 0) {
          ++vsync_skips_;
          displace(later, out);
        }
        target_v = seq;
        release = seq - lead_us_;
        ++chained_count_;
        if (chain_run_ == 0) ++chains_count_;
        if (++chain_run_ > chain_max_) chain_max_ = chain_run_;
      } else {
        chain_run_ = 0;
      }
    }
  }
  if (!servo_now_) {
    release = anchor_.map_us(obs.pts64) + static_cast<uint64_t>(d_us_);
    if (vsync_lock_) ++fallback_frames_;
  }

  // Order invariant: offers arrive in pts order, so any held frame whose
  // release lands after this one's own final release is older yet would
  // show later -- impossible to keep in order, so it loses, freshest
  // wins, exactly like a direct vblank collision above. This is the
  // backstop for every path that can leave a newer frame with an
  // earlier release than something already held: a stalled hand-over
  // (the natural slot comes from arrival_us, which isn't monotonic with
  // decode order the way now_us would be -- a frame can arrive promptly
  // but hand over late, or vice versa), a chain that pushed an older
  // frame forward past where this frame's own slot lands, or a drop into
  // the fallback rule while a servo-targeted frame is still held for a
  // vblank that may now be slow to arrive. It can drop a frame that is
  // already due but not yet drained: shown, it would follow a newer one.
  for (int i = 0; i < count_;) {
    if (held_[i].release_us > release) displace(i, out);
    else ++i;
  }

  // Fallback mailbox semantics: an untargeted (fallback) frame always
  // displaces an untargeted held one -- the classic single-slot rule,
  // for the pairs the order invariant above leaves alone (an arrival/now
  // combination can land a new release no later than an old fallback
  // hold's, which isn't out of order, but fallback still has only one
  // slot regardless).
  if (target_v == 0) {
    for (int i = 0; i < count_;) {
      if (held_[i].target_v == 0) displace(i, out);
      else ++i;
    }
  }
  if (count_ == kMaxHeld) displace(0, out);

  if (release <= now_us) {
    // Presented now, so it shows before anything still held -- and every
    // held frame is older. Comparing releases is not enough here: a held
    // frame whose release has passed but which the caller has not yet
    // drained (it may offer before it drains) would show after this one.
    // Flush them all, so order never depends on that call order.
    while (count_ > 0) displace(0, out);
    ++late_count_;
    if (target_v > last_out_v_) last_out_v_ = target_v;
    return true;  // never stack delay on the tail
  }

  // Insert sorted by release_us (earliest at [0]).
  int pos = count_;
  while (pos > 0 && held_[pos - 1].release_us > release) {
    held_[pos] = held_[pos - 1];
    --pos;
  }
  held_[pos] = Held{f, release, target_v};
  ++count_;
  ++held_count_;
  const double hold_ms = static_cast<double>(release - now_us) / 1000.0;
  hold_ema_ms_ += (hold_ms - hold_ema_ms_) / 16.0;
  return false;
}

void FrameRegulator::heal_slip() {
  std::lock_guard<PiMutex> lock(mu_);
  // Every entry from the first servo entry onward slips, fallback entries
  // included: the queue is sorted by release, which is pts order, and
  // moving only the servo entries would let an older servo frame overtake
  // a newer fallback frame held close behind it. The fallback entries
  // ahead of the first servo entry are older and release earlier anyway.
  int first = 0;
  while (first < count_ && held_[first].target_v == 0) ++first;
  if (first == count_) return;  // empty, or only fallback frames
  const uint64_t per = static_cast<uint64_t>(est_.period_us());
  for (int i = first; i < count_; ++i) {
    if (held_[i].target_v != 0) held_[i].target_v += per;
    held_[i].release_us += per;
  }
  ++heals_;
}

// Capture-time placement: the frame's slot follows the drone's capture
// cadence, not its arrival. The window holds each frame's raw transit,
// arrival - pts64 (the two clocks' offset plus the delivery time; it
// excludes this regulator's own hold, which would otherwise feed back into
// the allowance). Raw, not lateness against the pts anchor: the anchor's
// floor snaps down on one fast frame, which would shift every later sample
// against the ones already in the window and read as spread. The window
// gives the delivery floor (p1) and the clean-link spread (p99 - p1); the
// slot is the first vblank whose release is at or after pts64 + floor +
// allowance, allowance = spread + 1 ms capped at sched_cap_us_. A frame
// later than that slot loses it and goes down the arrival path; the next
// on-time frame claims the same vblank and wins it (freshest wins), so one
// late frame costs one hitch and no chain.
bool FrameRegulator::capture_slot(uint64_t pts64, uint32_t pts32, uint64_t arrival_us,
                                  CaptureSlot* s) {
  const int64_t transit = static_cast<int64_t>(arrival_us) - static_cast<int64_t>(pts64);
  if (!was_incomplete(pts32)) win_.add(transit);
  if (win_.empty()) return false;
  const int64_t floor_us = win_.lo();
  int64_t allow = win_.hi() - floor_us + 1000;
  if (allow > sched_cap_us_) allow = sched_cap_us_;
  if (allow < 0) allow = 0;
  allowance_us_ = allow;
  s->want = static_cast<int64_t>(pts64) + floor_us + allow;
  s->r = est_.release_at_or_after(s->want > 0 ? static_cast<uint64_t>(s->want) : 0, lead_us_);
  return true;
}

bool FrameRegulator::place_by_capture(const CaptureSlot& s, uint64_t now_us,
                                      uint64_t* release, uint64_t* target_v,
                                      Displaced* out) {
  const auto& r = s.r;
  if (r.release_us < now_us) {
    ++late_slot_;
    return false;
  }
  const uint64_t per = static_cast<uint64_t>(est_.period_us());
  const uint64_t clamp = 2 * per + lead_us_ + static_cast<uint64_t>(sched_cap_us_);
  if (r.release_us > now_us + clamp) return false;  // implausible phase
  // Every held frame is older than this one (offers arrive in decode
  // order). One on this vblank loses it (freshest wins); one targeted LATER
  // -- a late frame the arrival path chained forward -- would show after
  // this newer frame, i.e. backwards, so it goes too.
  const uint64_t hp = per / 2;
  for (int i = 0; i < count_;) {
    const uint64_t hv = held_[i].target_v;
    if (hv != 0 && hv + hp > r.vblank_us)
      displace(i, out);
    else
      ++i;
  }
  chain_run_ = 0;
  *release = r.release_us;
  *target_v = r.vblank_us;
  return true;
}

bool FrameRegulator::release_due(uint64_t now_us, DmaFrame* out) {
  std::lock_guard<PiMutex> lock(mu_);
  if (count_ == 0 || now_us < held_[0].release_us) return false;
  *out = held_[0].f;
  if (held_[0].target_v > last_out_v_) last_out_v_ = held_[0].target_v;
  for (int i = 0; i + 1 < count_; ++i) held_[i] = held_[i + 1];
  --count_;
  return true;
}

}  // namespace maburplay
