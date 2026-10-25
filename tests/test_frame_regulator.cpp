#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#include "frame_regulator.h"
#include "mtest.h"

using maburplay::DmaFrame;
using maburplay::FrameRegulator;

namespace {

DmaFrame frame(uint32_t pts_us) {
  DmaFrame f;
  f.pts_us = pts_us;
  f.opaque = reinterpret_cast<void*>(static_cast<uintptr_t>(pts_us | 1u));
  return f;
}

constexpr uint64_t kD = 12'000;         // regulate_ms 12 in µs
constexpr uint64_t kT0 = 5'000'000;     // arbitrary mono anchor
constexpr int64_t kFrame = 16'684;      // 59.94 fps pts step

}  // namespace

TEST(disabled_passes_through) {
  FrameRegulator reg(0);
  CHECK(!reg.enabled());
  FrameRegulator::Displaced disp;
  CHECK(reg.offer(frame(0), kT0, kT0, &disp));
  CHECK(disp.n == 0);
  CHECK(!reg.holding());
}

TEST(first_frame_seeds_floor_and_holds_exactly_d) {
  FrameRegulator reg(12);
  FrameRegulator::Displaced disp;
  DmaFrame out;
  CHECK(!reg.offer(frame(0), kT0, kT0, &disp));  // held
  CHECK(disp.n == 0);
  CHECK(reg.holding());
  CHECK(!reg.release_due(kT0 + kD - 1, &out));
  CHECK(reg.release_due(kT0 + kD, &out));
  CHECK(out.opaque == frame(0).opaque);
  CHECK(!reg.holding());
  CHECK(reg.held_count() == 1);
}

TEST(steady_cadence_releases_on_pts_grid_not_arrival) {
  FrameRegulator reg(12);
  FrameRegulator::Displaced disp;
  DmaFrame out;
  CHECK(!reg.offer(frame(0), kT0, kT0, &disp));
  CHECK(reg.release_due(kT0 + kD, &out));
  // Frame 1 arrives 5 ms jittery-late relative to the floor: it must still
  // release at floor + pts + D, i.e. only 7 ms after its own arrival.
  const uint64_t a1 = kT0 + kFrame + 5'000;
  CHECK(!reg.offer(frame(kFrame), a1, a1, &disp));
  // (small slack: the drift leak may shift the floor by ~1 µs per frame)
  CHECK(!reg.release_due(kT0 + kFrame + kD - 1'000, &out));
  CHECK(reg.release_due(kT0 + kFrame + kD + 16, &out));
  CHECK(out.opaque == frame(kFrame).opaque);
}

TEST(late_frame_presents_immediately) {
  FrameRegulator reg(12);
  FrameRegulator::Displaced disp;
  DmaFrame out;
  CHECK(!reg.offer(frame(0), kT0, kT0, &disp));
  CHECK(reg.release_due(kT0 + kD, &out));
  // Arrival AFTER its own release point (floor + pts + D).
  const uint64_t a1 = kT0 + kFrame + kD + 1'000;
  CHECK(reg.offer(frame(kFrame), a1, a1, &disp));  // present now
  CHECK(disp.n == 0);
  CHECK(!reg.holding());
  CHECK(reg.late_count() == 1);
}

TEST(faster_frame_snaps_floor_down) {
  FrameRegulator reg(12);
  FrameRegulator::Displaced disp;
  DmaFrame out;
  // Seed with a SLOW first frame (its arrival becomes the provisional floor).
  CHECK(!reg.offer(frame(0), kT0, kT0, &disp));
  CHECK(reg.release_due(kT0 + kD, &out));
  // Frame 1 arrives 3 ms faster than the provisional floor predicts: floor
  // snaps down and its release is exactly its own arrival + D.
  const uint64_t a1 = kT0 + kFrame - 3'000;
  CHECK(!reg.offer(frame(kFrame), a1, a1, &disp));
  CHECK(!reg.release_due(a1 + kD - 1, &out));
  CHECK(reg.release_due(a1 + kD, &out));
}

TEST(floor_leaks_upward_to_track_drift) {
  FrameRegulator reg(12);
  FrameRegulator::Displaced disp;
  DmaFrame out;
  // Seed the floor, then feed frames arriving a constant 2 ms above it.
  // The leak (~1 µs per frame at 60 fps) must close that gap: after 3000
  // frames the floor has crept >= 2 ms, so hold time is back to ~D.
  CHECK(!reg.offer(frame(0), kT0, kT0, &disp));
  CHECK(reg.release_due(kT0 + kD, &out));
  uint64_t pts = 0, arrival = 0;
  for (int i = 1; i <= 3000; ++i) {
    pts = static_cast<uint64_t>(i) * kFrame;
    arrival = kT0 + pts + 2'000;
    if (!reg.offer(frame(static_cast<uint32_t>(pts)), arrival, arrival, &disp))
      CHECK(reg.release_due(arrival + kD, &out));  // drain
  }
  // Final frame: floor >= arrival-offset, so release is within [arr, arr+D].
  const uint64_t a = kT0 + 3001ull * kFrame + 2'000;
  const bool held = !reg.offer(frame(static_cast<uint32_t>(3001ull * kFrame)),
                               a,
                               a, &disp);
  if (held) {
    CHECK(!reg.release_due(a - 1, &out));      // never before arrival
    CHECK(reg.release_due(a + kD, &out));      // and by arrival + D
  }
  // Leak must never make a frame release before it arrives (that would be
  // a late-classification instead).
  CHECK(reg.late_count() + reg.held_count() >= 3000);
}

TEST(newer_frame_replaces_held_mailbox_style) {
  FrameRegulator reg(12);
  FrameRegulator::Displaced disp;
  DmaFrame out;
  CHECK(!reg.offer(frame(0), kT0, kT0, &disp));
  // Burst decode: frame 1 offered before frame 0's release.
  CHECK(!reg.offer(frame(kFrame), kT0 + 1'000, kT0 + 1'000, &disp));
  CHECK(disp.n == 1);
  CHECK(disp.f[0].opaque == frame(0).opaque);  // caller must free this one
  CHECK(reg.replaced_count() == 1);
  CHECK(reg.release_due(kT0 + kFrame + kD, &out));
  CHECK(out.opaque == frame(kFrame).opaque);
}

TEST(pts_wrap_is_invisible) {
  FrameRegulator reg(12);
  FrameRegulator::Displaced disp;
  DmaFrame out;
  const uint32_t p0 = 0xFFFFF000u;  // wraps on the next step
  CHECK(!reg.offer(frame(p0), kT0, kT0, &disp));
  CHECK(reg.release_due(kT0 + kD, &out));
  const uint32_t p1 = p0 + static_cast<uint32_t>(kFrame);  // wrapped
  CHECK(!reg.offer(frame(p1), kT0 + kFrame, kT0 + kFrame, &disp));
  CHECK(!reg.release_due(kT0 + kFrame + kD - 1'000, &out));
  CHECK(reg.release_due(kT0 + kFrame + kD + 16, &out));
}

TEST(pts_discontinuity_resets_and_passes_through) {
  FrameRegulator reg(12);
  FrameRegulator::Displaced disp;
  DmaFrame out;
  CHECK(!reg.offer(frame(0), kT0, kT0, &disp));
  // Drone restart: pts jumps 5 s. The frame passes through immediately and
  // the held old-timebase frame comes back for release.
  CHECK(reg.offer(frame(5'000'000), kT0 + 2'000, kT0 + 2'000, &disp));
  CHECK(disp.n == 1);
  CHECK(disp.f[0].opaque == frame(0).opaque);
  CHECK(!reg.holding());
  // Next frame re-seeds a fresh floor and is held for exactly D again.
  const uint64_t a = kT0 + 20'000;
  CHECK(!reg.offer(frame(5'000'000 + static_cast<uint32_t>(kFrame)), a, a,
                   &disp));
  CHECK(!reg.release_due(a + kD - 1, &out));
  CHECK(reg.release_due(a + kD, &out));
}

TEST(fallback_mode_displaces_like_single_slot_mailbox) {
  FrameRegulator reg(12);  // vsync_lock defaults false
  FrameRegulator::Displaced disp;
  CHECK(!reg.offer(frame(0), kT0, kT0, &disp));
  CHECK(disp.n == 0);
  // Second frame arrives while the first is held: displaced (v=0 == v=0).
  CHECK(!reg.offer(frame(kFrame), kT0 + 2'000, kT0 + 2'000, &disp));
  CHECK(disp.n == 1);
  CHECK(disp.f[0].opaque == frame(0).opaque);
  CHECK(reg.replaced_count() == 1);
}

namespace {
constexpr uint64_t kVb = 16'667;    // panel period µs
constexpr uint64_t kLead = 3'000;   // vsync_lead_ms 3

// Warm a servo regulator: 8 exact flips on the kT0 grid.
void warm(FrameRegulator& reg) {
  for (int i = 0; i < 8; ++i) reg.on_flip(kT0 + i * kVb, true);
}

// Keeps a warmed regulator's servo live: every flip on the kT0 grid up to
// t (the estimator goes stale after a few missed periods, and a stale one
// never reaches capture-time placement). *next is the next flip's index.
void flips_until(FrameRegulator& reg, uint64_t* next, uint64_t t) {
  while (kT0 + *next * kVb <= t) reg.on_flip(kT0 + (*next)++ * kVb, true);
}
}  // namespace

TEST(servo_releases_at_vblank_minus_lead) {
  FrameRegulator reg(12, /*vsync_lock=*/true, /*vsync_lead_ms=*/3);
  warm(reg);  // phase = kT0 + 7*kVb
  const uint64_t phase = kT0 + 7 * kVb;
  FrameRegulator::Displaced disp;
  DmaFrame out;
  // Decoded 9 ms before the next vblank: plenty of lead margin.
  const uint64_t t_dec = phase + kVb - 9'000;
  CHECK(!reg.offer(frame(0), t_dec, t_dec, &disp));
  CHECK(reg.servo_locked());
  const uint64_t release = phase + kVb - kLead;
  CHECK(!reg.release_due(release - 1, &out));
  CHECK(reg.release_due(release, &out));
  CHECK(reg.fallback_frames() == 0);
}

TEST(servo_inside_lead_window_targets_next_vblank) {
  FrameRegulator reg(12, true, 3);
  warm(reg);
  const uint64_t phase = kT0 + 7 * kVb;
  FrameRegulator::Displaced disp;
  DmaFrame out;
  // Only 2 ms of margin left (< lead): must NOT gamble on this vblank.
  const uint64_t t_dec = phase + kVb - 2'000;
  CHECK(!reg.offer(frame(0), t_dec, t_dec, &disp));
  CHECK(!reg.release_due(phase + kVb, &out));            // not this one
  CHECK(reg.release_due(phase + 2 * kVb - kLead, &out)); // the next
}

TEST(servo_burst_pair_takes_sequential_vblanks) {
  // base+enh of one capture pair complete together (FEC generation close), so
  // their decodes land ~1 ms apart with pts a full frame apart. Arrival-time
  // targeting alone would aim both at the same vblank and drop the earlier
  // frame (hw: ~14 skips/s, 1-in-4 frames). Sequential-slot assignment must
  // instead push the later-pts frame to the next grid slot: both display, in
  // order.
  FrameRegulator reg(12, true, 3);
  warm(reg);
  const uint64_t phase = kT0 + 7 * kVb;
  FrameRegulator::Displaced disp;
  DmaFrame out;
  const uint64_t t1 = phase + kVb - 12'000;
  CHECK(!reg.offer(frame(0), t1, t1, &disp));
  CHECK(!reg.offer(frame(kFrame), t1 + 1'000, t1 + 1'000, &disp));
  CHECK(disp.n == 0);              // NOT displaced
  CHECK(reg.vsync_skips() == 0);
  CHECK(reg.release_due(phase + kVb - kLead, &out));
  CHECK(out.opaque == frame(0).opaque);          // first pair member
  CHECK(!reg.release_due(phase + 2 * kVb - kLead - 100, &out));
  CHECK(reg.release_due(phase + 2 * kVb - kLead + 100, &out));
  CHECK(out.opaque == frame(kFrame).opaque);     // second, next vblank
}

TEST(servo_distinct_targets_hold_both) {
  FrameRegulator reg(12, true, 3);
  warm(reg);
  const uint64_t phase = kT0 + 7 * kVb;
  FrameRegulator::Displaced disp;
  DmaFrame out;
  CHECK(!reg.offer(frame(0), phase + kVb - 9'000, phase + kVb - 9'000, &disp));
  // Second frame decodes after vblank+1's lead cutoff: targets vblank+2.
  CHECK(!reg.offer(frame(kFrame), phase + kVb - 1'000, phase + kVb - 1'000, &disp));
  CHECK(disp.n == 0);
  CHECK(reg.holding());
  CHECK(reg.release_due(phase + kVb - kLead, &out));
  CHECK(out.opaque == frame(0).opaque);
  CHECK(reg.release_due(phase + 2 * kVb - kLead, &out));
  CHECK(out.opaque == frame(kFrame).opaque);
  CHECK(reg.vsync_skips() == 0);
}

TEST(servo_falls_back_when_stale_and_recovers) {
  FrameRegulator reg(12, true, 3);
  warm(reg);
  const uint64_t phase = kT0 + 7 * kVb;
  FrameRegulator::Displaced disp;
  DmaFrame out;
  // 40 periods after the last flip: stale -> fallback rule (floor + D).
  const uint64_t t_dec = phase + 40 * kVb;
  CHECK(!reg.offer(frame(0), t_dec, t_dec, &disp));
  CHECK(!reg.servo_locked());
  CHECK(reg.fallback_frames() == 1);
  // Fallback release = anchored floor + 12 ms; this frame seeds the
  // floor at t_dec, so release = t_dec + 12 ms exactly.
  CHECK(!reg.release_due(t_dec + kD - 1, &out));
  CHECK(reg.release_due(t_dec + kD, &out));
  // Flips resume: servo re-engages (phase recency revalidates).
  for (int i = 0; i < 8; ++i) reg.on_flip(t_dec + 20'000 + i * kVb, true);
  CHECK(!reg.offer(frame(2 * kFrame), t_dec + 20'000 + 8 * kVb, t_dec + 20'000 + 8 * kVb, &disp));
  CHECK(reg.servo_locked());
}

TEST(servo_safety_clamp_uses_fallback_rule) {
  FrameRegulator reg(12, true, 3);
  warm(reg);
  // Corrupt-phase stand-in: an estimator whose next release lands
  // > 2*period + lead away must NOT be trusted for this frame.
  // Constructed via a phase far in the future: feed one exact flip 10
  // periods ahead (k=10 updates phase), then offer a frame "now".
  const uint64_t phase = kT0 + 7 * kVb;
  reg.on_flip(phase + 10 * kVb, true);
  FrameRegulator::Displaced disp;
  DmaFrame out;
  const uint64_t t_dec = phase + 10 * kVb - 60'000;  // 60 ms before phase
  CHECK(!reg.offer(frame(0), t_dec, t_dec, &disp));
  CHECK(reg.fallback_frames() == 1);  // clamp routed to fallback
  CHECK(reg.release_due(t_dec + kD, &out));
}

TEST(mixed_fallback_and_servo_targets_all_hold) {
  // Mixed fallback/servo queue: start stale (fallback frame held,
  // target_v==0), re-warm to a fresh grid, then hold TWO servo frames at
  // distinct targets. At kMaxHeld=4 all three coexist (a 2-deep queue
  // would evict the fallback head) and each releases by its own rule
  // (fallback at floor+D, servo at vblank-lead). The
  // full-queue eviction path is pinned by
  // full_queue_evicts_overdue_head_on_fifth_target.
  FrameRegulator reg(12, /*vsync_lock=*/true, /*vsync_lead_ms=*/3);
  warm(reg);
  const uint64_t phase0 = kT0 + 7 * kVb;
  FrameRegulator::Displaced disp;

  // 1) Stale offer: estimator hasn't flipped in 40 periods -> fallback
  // rule, held with target_v == 0.
  const uint64_t t_dec1 = phase0 + 40 * kVb;
  CHECK(!reg.offer(frame(0), t_dec1, t_dec1, &disp));
  CHECK(!reg.servo_locked());
  CHECK(reg.fallback_frames() == 1);

  // 2) Re-warm on a fresh grid (one exact flip actually revalidates; 8
  // mirrors the other re-warm test's cadence).
  const uint64_t phase1 = t_dec1 + 20'000 + 7 * kVb;
  for (int i = 0; i < 8; ++i) reg.on_flip(t_dec1 + 20'000 + i * kVb, true);

  // 3) Servo frame targeting vblank phase1+kVb: distinct from the held
  // fallback target (0) -> both held, count_ == 2.
  const uint64_t t_dec2 = phase1 + kVb - 9'000;
  CHECK(!reg.offer(frame(kFrame), t_dec2, t_dec2, &disp));
  CHECK(reg.servo_locked());
  CHECK(disp.n == 0);
  CHECK(reg.holding());

  // 4) Second servo frame decoded inside the FIRST target's lead window
  // (only 1 ms of margin, < the 3 ms lead): rolls to the next vblank
  // (phase1+2*kVb), matching neither held target (0 or phase1+kVb). Three
  // distinct targets, all held -- no eviction, no skip.
  const uint64_t t_dec3 = phase1 + kVb - 1'000;
  CHECK(!reg.offer(frame(2 * kFrame), t_dec3, t_dec3, &disp));
  CHECK(disp.n == 0);
  CHECK(reg.replaced_count() == 0);
  CHECK(reg.vsync_skips() == 0);
  CHECK(reg.holding());

  // Releases interleave by each frame's own rule: the fallback frame's
  // floor+D release (long overdue by phase1) heads the queue, then the
  // servo frames at their vblanks.
  DmaFrame o0, o1, o2;
  CHECK(reg.release_due(phase1 + kVb - kLead, &o0));
  CHECK(o0.opaque == frame(0).opaque);
  CHECK(reg.release_due(phase1 + kVb - kLead, &o1));
  CHECK(o1.opaque == frame(kFrame).opaque);
  CHECK(reg.release_due(phase1 + 2 * kVb - kLead, &o2));
  CHECK(o2.opaque == frame(2 * kFrame).opaque);
  CHECK(!reg.holding());
}

TEST(discont_flushes_both_held_slots) {
  // Same mixed fallback/servo construction to reach count_==2, then a
  // pts jump > PtsAnchor::kResyncUs (2 s): the discont path must flush
  // BOTH held frames regardless of which path (fallback vs servo) put
  // them there.
  FrameRegulator reg(12, /*vsync_lock=*/true, /*vsync_lead_ms=*/3);
  warm(reg);
  const uint64_t phase0 = kT0 + 7 * kVb;
  FrameRegulator::Displaced disp;

  const uint64_t t_dec1 = phase0 + 40 * kVb;
  CHECK(!reg.offer(frame(0), t_dec1, t_dec1, &disp));  // fallback frame held

  const uint64_t phase1 = t_dec1 + 20'000 + 7 * kVb;
  for (int i = 0; i < 8; ++i) reg.on_flip(t_dec1 + 20'000 + i * kVb, true);

  const uint64_t t_dec2 = phase1 + kVb - 9'000;
  CHECK(!reg.offer(frame(kFrame), t_dec2, t_dec2, &disp));  // servo frame held
  CHECK(reg.holding());

  const uint64_t t_dec3 = t_dec2 + 1'000;
  const uint32_t pts_jump = kFrame + 3'000'000u;  // > kResyncUs past last pts
  CHECK(reg.offer(frame(pts_jump), t_dec3, t_dec3, &disp));  // present now
  CHECK(disp.n == 2);
  CHECK(disp.f[0].opaque == frame(0).opaque);       // oldest (fallback) first
  CHECK(disp.f[1].opaque == frame(kFrame).opaque);  // then the servo frame
  CHECK(reg.discont_count() == 1);
  CHECK(!reg.holding());
}

namespace {
// One simulated minute of decode stream vs 60.000 Hz panel flips.
// Event-driven — exact times, no tick quantization. The estimator is
// warmed (8 exact flips) before the first decode so the servo owns every
// offer; the two arms differ only in source period, i.e. beat direction.
struct BeatSim {
  uint64_t offered = 0, released = 0, drained = 0;
  uint64_t skips = 0, replaced = 0, fallback = 0, late = 0;
};

BeatSim run_beat_sim(double src_period_us, double panel_period_us = 1e6 / 60.0) {
  FrameRegulator reg(12, true, 3);
  reg.set_panel_period(panel_period_us);
  FrameRegulator::Displaced disp;
  DmaFrame out;
  const double kPanel = panel_period_us;
  BeatSim s;
  double flip_t = static_cast<double>(kT0);
  for (int i = 0; i < 8; ++i) {
    reg.on_flip(static_cast<uint64_t>(flip_t), true);
    flip_t += kPanel;
  }
  double dec_t = flip_t + 8'000.0;  // arbitrary phase into the warm grid
  uint32_t pts = 0;
  const double t_end = static_cast<double>(kT0) + 60e6;
  while (dec_t < t_end || flip_t < t_end) {
    const double t = flip_t <= dec_t ? flip_t : dec_t;
    while (reg.release_due(static_cast<uint64_t>(t), &out)) ++s.released;
    if (flip_t <= dec_t) {
      reg.on_flip(static_cast<uint64_t>(flip_t), true);
      flip_t += kPanel;
    } else {
      pts += static_cast<uint32_t>(src_period_us);
      if (reg.offer(frame(pts), static_cast<uint64_t>(dec_t), static_cast<uint64_t>(dec_t), &disp)) {
        // late passthrough: presented immediately, counted via late_count()
      }
      ++s.offered;
      dec_t += src_period_us;
    }
  }
  while (reg.release_due(~0ull, &out)) ++s.drained;
  s.skips = reg.vsync_skips();
  s.replaced = reg.replaced_count();
  s.fallback = reg.fallback_frames();
  s.late = reg.late_count();
  return s;
}
}  // namespace

TEST(beat_simulation_slow_source_never_skips) {
  // Real hardware direction: 59.939 fps sensor vs 60.000 Hz panel. Decode
  // spacing (16683.6) exceeds the panel period (16666.7), so two decodes
  // can never share a target vblank — wraps repeat a frame on the panel,
  // they never drop one. Freshest-wins must stay silent.
  const BeatSim s = run_beat_sim(1e6 / 59.939);
  CHECK(s.fallback == 0);  // servo never disengaged
  CHECK(s.skips == 0);     // no contention, deterministically
  CHECK(s.replaced == 0);  // and no evictions either
  CHECK(s.offered == s.released + s.replaced + s.late + s.drained);
}

TEST(beat_simulation_fast_source_drops_bounded_by_wraps) {
  // Inverted beat (source faster than panel): the source gains one frame
  // on the vblank grid per ~16.4 s wrap. Depending on the wrap's phase a
  // drop surfaces either as a deep-burst slot claim (vsync_skips: both
  // the natural and next slot occupied, newest takes the later one) or
  // as an oldest-out eviction once the backlog wants a third slot; both
  // increment replaced_count, the total-drops figure, bounded by the
  // wrap count (instrumented run: skips=2 replaced=2 -- claims, no
  // evictions -- so assert the total, not the mechanism).
  const BeatSim s = run_beat_sim(1e6 / 60.061);
  CHECK(s.fallback == 0);
  // Depending on wrap phase a drop surfaces either as a both-slots-
  // occupied claim (vsync_skips) or as an oldest-out eviction; both
  // increment replaced_count, which is the total-drops figure.
  CHECK(s.skips <= s.replaced);
  CHECK(s.replaced >= 1);
  CHECK(s.replaced <= 4);
  CHECK(s.offered == s.released + s.replaced + s.late + s.drained);
}

TEST(cross_rate_90fps_on_60hz_drops_one_in_three) {
  // The live bench case: 90 fps sensor on the 60 Hz GS panel. Three
  // decodes per two vblanks -- exactly one of every three frames must be
  // dropped (freshest-wins), never more, and the servo never disengages.
  const BeatSim s = run_beat_sim(1e6 / 90.0);
  CHECK(s.fallback == 0);
  CHECK(s.offered == s.released + s.replaced + s.late + s.drained);
  CHECK(static_cast<double>(s.replaced) >= 0.30 * s.offered);
  CHECK(static_cast<double>(s.replaced) <= 0.36 * s.offered);
}

TEST(matched_120fps_on_120hz_never_drops) {
  // Matched rates on a fast panel: one decode per vblank, no contention,
  // no drops -- the servo behaves exactly as 60-on-60 does today.
  const BeatSim s = run_beat_sim(1e6 / 120.0, 1e6 / 120.0);
  CHECK(s.fallback == 0);
  CHECK(s.skips == 0);
  CHECK(s.replaced == 0);
  CHECK(s.offered == s.released + s.replaced + s.late + s.drained);
}

TEST(cross_rate_120fps_on_60hz_drops_half) {
  // Double-rate source: every second frame is dropped, freshest-wins.
  const BeatSim s = run_beat_sim(1e6 / 120.0);
  CHECK(s.fallback == 0);
  CHECK(s.offered == s.released + s.replaced + s.late + s.drained);
  CHECK(static_cast<double>(s.replaced) >= 0.46 * s.offered);
  CHECK(static_cast<double>(s.replaced) <= 0.54 * s.offered);
}

MTEST_MAIN

TEST(panel_period_reseed_locks_120hz_grid) {
  // A 120 Hz panel's flips land 8333 µs apart -- under the default 60 Hz
  // seed the estimator rounds that delta to k=0 and DROPS every flip, so
  // the servo can never engage. main.cpp reseeds from the chosen DRM
  // mode's timings before the presenter produces its first flip.
  constexpr uint64_t kVb120 = 8'333;
  FrameRegulator reg(12, true, 3);
  reg.set_panel_period(8'333.3);
  for (int i = 0; i < 8; ++i) reg.on_flip(kT0 + i * kVb120, true);
  const uint64_t phase = kT0 + 7 * kVb120;
  FrameRegulator::Displaced disp;
  DmaFrame out;
  const uint64_t t_dec = phase + kVb120 - 5'000;
  CHECK(!reg.offer(frame(0), t_dec, t_dec, &disp));
  CHECK(reg.servo_locked());
  const uint64_t release = phase + kVb120 - kLead;
  CHECK(!reg.release_due(release - 1, &out));
  CHECK(reg.release_due(release, &out));
  CHECK(reg.fallback_frames() == 0);
}

TEST(without_reseed_120hz_flips_never_warm) {
  // The defect the reseed fixes, pinned so it stays visible: same flip
  // stream, no set_panel_period -- every offer takes the fallback rule.
  constexpr uint64_t kVb120 = 8'333;
  FrameRegulator reg(12, true, 3);
  for (int i = 0; i < 8; ++i) reg.on_flip(kT0 + i * kVb120, true);
  FrameRegulator::Displaced disp;
  CHECK(!reg.offer(frame(0), kT0 + 8 * kVb120, kT0 + 8 * kVb120, &disp));
  CHECK(!reg.servo_locked());
  CHECK(reg.fallback_frames() == 1);
}

TEST(stall_backlog_holds_three_distinct_targets) {
  // Post-stall scenario (main loop order: decode sink OFFERS before the
  // loop drains release_due): f1@v1 and f2@v2 are held, the loop stalls
  // past v1's release, and f3 decodes during the catch-up -- its natural
  // slot v2 is occupied, so it takes v3. A 2-deep queue can only evict
  // f1, a frame already due for display; the deeper queue holds all
  // three and f1 merely releases late.
  FrameRegulator reg(12, true, 3);
  warm(reg);
  const uint64_t phase = kT0 + 7 * kVb;
  FrameRegulator::Displaced disp;
  DmaFrame out;
  CHECK(!reg.offer(frame(0), phase + kVb - 9'000, phase + kVb - 9'000, &disp));        // @v1
  CHECK(!reg.offer(frame(kFrame), phase + kVb - 1'000, phase + kVb - 1'000, &disp));   // @v2
  CHECK(disp.n == 0);
  // Stall: v1's release (v1 - lead) passes undrained; f3 decodes at v1+1ms.
  CHECK(!reg.offer(frame(2 * kFrame), phase + kVb + 1'000, phase + kVb + 1'000, &disp));  // @v3
  CHECK(disp.n == 0);
  CHECK(reg.replaced_count() == 0);
  CHECK(reg.vsync_skips() == 0);
  // All three drain in pts order on consecutive vblanks (f1 late).
  CHECK(reg.release_due(phase + kVb + 1'001, &out));
  CHECK(out.opaque == frame(0).opaque);
  CHECK(reg.release_due(phase + 2 * kVb - kLead, &out));
  CHECK(out.opaque == frame(kFrame).opaque);
  CHECK(reg.release_due(phase + 3 * kVb - kLead, &out));
  CHECK(out.opaque == frame(2 * kFrame).opaque);
  CHECK(!reg.holding());
}

TEST(discont_flushes_three_held_frames) {
  // Same backlog as above, then a drone-restart pts jump: the discont
  // path must hand back ALL held frames, so Displaced must carry the
  // full queue depth.
  FrameRegulator reg(12, true, 3);
  warm(reg);
  const uint64_t phase = kT0 + 7 * kVb;
  FrameRegulator::Displaced disp;
  CHECK(!reg.offer(frame(0), phase + kVb - 9'000, phase + kVb - 9'000, &disp));
  CHECK(!reg.offer(frame(kFrame), phase + kVb - 1'000, phase + kVb - 1'000, &disp));
  CHECK(!reg.offer(frame(2 * kFrame), phase + kVb + 1'000, phase + kVb + 1'000, &disp));
  const uint32_t pts_jump = 2 * kFrame + 3'000'000u;  // > kResyncUs
  CHECK(reg.offer(frame(pts_jump), phase + kVb + 2'000, phase + kVb + 2'000, &disp));
  CHECK(disp.n == 3);
  CHECK(disp.f[0].opaque == frame(0).opaque);
  CHECK(disp.f[1].opaque == frame(kFrame).opaque);
  CHECK(disp.f[2].opaque == frame(2 * kFrame).opaque);
  CHECK(!reg.holding());
}

TEST(full_queue_evicts_overdue_head_on_fifth_target) {
  // The depth cap still exists -- at FOUR. Extend the stall backlog to
  // f1..f4 on v1..v4, then a fifth distinct target evicts the oldest
  // (most-overdue) head, latency-first.
  FrameRegulator reg(12, true, 3);
  warm(reg);
  const uint64_t phase = kT0 + 7 * kVb;
  FrameRegulator::Displaced disp;
  DmaFrame out;
  CHECK(!reg.offer(frame(0), phase + kVb - 9'000, phase + kVb - 9'000, &disp));            // @v1
  CHECK(!reg.offer(frame(kFrame), phase + kVb - 1'000, phase + kVb - 1'000, &disp));       // @v2
  CHECK(!reg.offer(frame(2 * kFrame), phase + kVb + 1'000, phase + kVb + 1'000, &disp));   // @v3
  CHECK(!reg.offer(frame(3 * kFrame), phase + 2 * kVb + 1'000, phase + 2 * kVb + 1'000, &disp));  // @v4
  CHECK(disp.n == 0);
  CHECK(!reg.offer(frame(4 * kFrame), phase + 3 * kVb + 1'000, phase + 3 * kVb + 1'000, &disp));  // @v5
  CHECK(disp.n == 1);
  CHECK(disp.f[0].opaque == frame(0).opaque);  // overdue head evicted
  CHECK(reg.replaced_count() == 1);
  // Survivors still release in order.
  CHECK(reg.release_due(phase + 3 * kVb + 1'001, &out));
  CHECK(out.opaque == frame(kFrame).opaque);
  CHECK(reg.release_due(phase + 3 * kVb + 1'002, &out));
  CHECK(out.opaque == frame(2 * kFrame).opaque);
  CHECK(reg.release_due(phase + 4 * kVb - kLead, &out));
  CHECK(out.opaque == frame(3 * kFrame).opaque);
  CHECK(reg.release_due(phase + 5 * kVb - kLead, &out));
  CHECK(out.opaque == frame(4 * kFrame).opaque);
  CHECK(!reg.holding());
}

TEST(heal_slip_shifts_pending_releases_one_slot) {
  // Chain-break heal: when the main loop sees the presenter's in-flight
  // mailbox engaging continuously (a one-vsync-late present chain), it
  // calls heal_slip() -- every pending servo release slips one slot so a
  // vblank goes unfilled and the flip pipeline drains.
  FrameRegulator reg(12, true, 3);
  warm(reg);
  const uint64_t phase = kT0 + 7 * kVb;
  FrameRegulator::Displaced disp;
  DmaFrame out;
  // Burst pair -> held at v and v+p.
  const uint64_t t1 = phase + kVb - 12'000;
  CHECK(!reg.offer(frame(0), t1, t1, &disp));
  CHECK(!reg.offer(frame(kFrame), t1 + 1'000, t1 + 1'000, &disp));
  reg.heal_slip();
  CHECK(reg.heals() == 1);
  // Both slipped: first now releases for v+2p's predecessor... i.e. at
  // (v+p)-lead, second at (v+2p)-lead. Nothing due at the original slot.
  CHECK(!reg.release_due(phase + kVb - kLead, &out));
  CHECK(reg.release_due(phase + 2 * kVb - kLead, &out));
  CHECK(out.opaque == frame(0).opaque);
  CHECK(reg.release_due(phase + 3 * kVb - kLead, &out));
  CHECK(out.opaque == frame(kFrame).opaque);
}

TEST(heal_slip_noop_in_fallback_or_empty) {
  FrameRegulator reg(12, true, 3);
  reg.heal_slip();               // empty queue: no-op
  CHECK(reg.heals() == 0);
  FrameRegulator::Displaced disp;
  // Fallback-held frame (estimator cold): target 0, heal must not touch.
  CHECK(!reg.offer(frame(0), kT0, kT0, &disp));
  reg.heal_slip();
  CHECK(reg.heals() == 0);
  DmaFrame out;
  CHECK(reg.release_due(kT0 + kD, &out));  // fallback release unchanged
}

TEST(next_release_us_reports_earliest_pending) {
  FrameRegulator reg(12);
  CHECK(reg.next_release_us() == 0);          // empty
  FrameRegulator::Displaced disp;
  DmaFrame out;
  CHECK(!reg.offer(frame(0), kT0, kT0, &disp));    // fallback hold at kT0 + D
  CHECK(reg.next_release_us() == kT0 + kD);
  CHECK(reg.release_due(kT0 + kD, &out));
  CHECK(reg.next_release_us() == 0);          // drained
}

TEST(servo_triple_burst_claims_the_second_slot) {
  // Three decodes 1 ms apart (a deep fec-batch burst): first takes its
  // natural slot, second the next slot, and the THIRD -- both slots
  // occupied -- claims the later slot from the middle frame
  // (freshest-wins, vsync_skips). Survivors release on consecutive
  // vblanks.
  FrameRegulator reg(12, true, 3);
  warm(reg);
  const uint64_t phase = kT0 + 7 * kVb;
  FrameRegulator::Displaced disp;
  DmaFrame out;
  const uint64_t t1 = phase + kVb - 12'000;
  CHECK(!reg.offer(frame(0), t1, t1, &disp));
  CHECK(!reg.offer(frame(kFrame), t1 + 1'000, t1 + 1'000, &disp));
  CHECK(disp.n == 0);
  CHECK(!reg.offer(frame(2 * kFrame), t1 + 2'000, t1 + 2'000, &disp));
  CHECK(disp.n == 1);
  CHECK(disp.f[0].opaque == frame(kFrame).opaque);  // middle frame dropped
  CHECK(reg.vsync_skips() == 1);
  CHECK(reg.replaced_count() == 1);
  CHECK(reg.release_due(phase + kVb - kLead, &out));
  CHECK(out.opaque == frame(0).opaque);
  CHECK(reg.release_due(phase + 2 * kVb - kLead, &out));
  CHECK(out.opaque == frame(2 * kFrame).opaque);
  CHECK(!reg.holding());
}

TEST(order_invariant_displaces_an_older_fallback_hold_behind_a_newer_servo_frame) {
  // The order invariant is unqualified -- it applies to fallback and
  // servo entries alike. An
  // older-pts fallback frame, held with a release the vblank grid
  // knows nothing about, cannot survive if a newer-pts servo frame's
  // own vblank target happens to land earlier: showing the fallback
  // frame afterward would be backwards, so it is displaced instead,
  // freshest wins, exactly like any other collision. A mixed queue that
  // heal_slip() has to keep in order is built by
  // heal_slip_still_finds_the_true_head_in_a_mixed_queue below.
  FrameRegulator reg(12, true, 3);
  FrameRegulator::Displaced disp;
  DmaFrame out;
  CHECK(!reg.offer(frame(0), kT0, kT0, &disp));  // fallback @ kT0+12ms (cold estimator)
  // Warm with flips whose next grid slot is kT0+8000.
  for (int i = 8; i >= 1; --i) reg.on_flip(kT0 + 8'000 - i * kVb, true);
  CHECK(!reg.offer(frame(kFrame), kT0 + 2'000, kT0 + 2'000, &disp));  // servo @ kT0+5000
  CHECK(disp.n == 1);
  CHECK(disp.f[0].opaque == frame(0).opaque);  // the older fallback hold, displaced
  CHECK(reg.next_release_us() == kT0 + 5'000);
  CHECK(reg.release_due(kT0 + 5'000, &out));
  CHECK(out.opaque == frame(kFrame).opaque);   // the servo frame releases normally
  CHECK(!reg.holding());
}

TEST(heal_slip_still_finds_the_true_head_in_a_mixed_queue) {
  // heal_slip() slips every entry from the first servo entry onward,
  // fallback entries included, so a mixed queue keeps its release order
  // -- which is pts order. The reachable shape: an OLDER servo frame held
  // first, then a NEWER fallback frame held with a release close behind
  // it (within one period). Slipping only the servo entry would move the
  // older frame past the newer one and release them backwards.
  //
  // Getting there needs now_us far enough from the vblank grid's phase
  // to make the implausible-phase clamp drop this frame to the
  // fallback rule (a single artificial flip, same trick as
  // servo_safety_clamp_uses_fallback_rule), NOT estimator staleness:
  // staleness needs now_us hundreds of ms past the last flip, and the
  // fallback release (arrival_us + D) would then dwarf the servo
  // frame's by the same margin, far outside the one-period window a
  // slip can close.
  FrameRegulator reg(12, true, 3);
  warm(reg);
  const uint64_t phase = kT0 + 7 * kVb;
  FrameRegulator::Displaced disp;
  DmaFrame out;

  CHECK(!reg.offer(frame(0), phase + kVb - 9'000, phase + kVb - 9'000, &disp));
  const uint64_t release1 = phase + kVb - kLead;  // f0 (servo, older pts)
  CHECK(reg.next_release_us() == release1);

  reg.on_flip(phase + 10 * kVb, true);  // artificial future jump

  const uint64_t arrival1 = phase + kVb - 5'000;  // f1's own decode stamp
  const uint64_t now1 = phase + kVb - 2'000;      // f1's hand-over
  CHECK(!reg.offer(frame(kFrame), arrival1, now1, &disp));  // f1: clamp -> fallback, held
  CHECK(disp.n == 0);
  CHECK(reg.fallback_frames() == 1);
  const uint64_t release2 = arrival1 + kD;  // f1 (fallback, newer pts) -- still later than f0
  CHECK(release2 > release1 && release2 - release1 < kVb);  // within one period
  CHECK(reg.next_release_us() == release1);

  reg.heal_slip();
  CHECK(reg.heals() == 1);
  // Both slip one period: f0 still heads, f1 still follows.
  CHECK(reg.next_release_us() == release1 + kVb);
  std::vector<void*> drained;
  while (reg.release_due(release2 + 2 * kVb, &out)) drained.push_back(out.opaque);
  CHECK(drained == (std::vector<void*>{frame(0).opaque, frame(kFrame).opaque}));
  CHECK(!reg.holding());
}

TEST(chain_counters_track_sequential_slot_assignments) {
  // A collision (two decodes claiming one vblank) chains the newer frame
  // onto the next slot; every frame after it at source cadence then finds
  // its natural slot occupied and chains too, until a gap wider than a
  // period frees a slot. chained= counts every +1-slot assignment,
  // chain= is the current run length, chain_max= its high-water mark.
  FrameRegulator reg(12, true, 3);
  warm(reg);
  const uint64_t phase = kT0 + 7 * kVb;
  FrameRegulator::Displaced disp;
  DmaFrame out;
  CHECK(reg.chained_count() == 0);
  CHECK(reg.chain_run() == 0);
  const uint64_t t1 = phase + kVb - 12'000;
  CHECK(!reg.offer(frame(0), t1, t1, &disp));                // natural slot
  CHECK(reg.chain_run() == 0);
  CHECK(!reg.offer(frame(kFrame), t1 + 1'000, t1 + 1'000, &disp));   // collision -> +1
  CHECK(reg.chained_count() == 1);
  CHECK(reg.chain_run() == 1);
  // Next frame at exact source cadence: its natural slot is held by the
  // chained predecessor, so it chains as well.
  CHECK(!reg.offer(frame(2 * kFrame), t1 + 1'000 + kFrame, t1 + 1'000 + kFrame, &disp));
  CHECK(reg.chained_count() == 2);
  CHECK(reg.chain_run() == 2);
  CHECK(reg.chain_max() == 2);
  CHECK(disp.n == 0);
  CHECK(reg.vsync_skips() == 0);
  // Drain everything, then a frame arriving with a free natural slot
  // ends the run: chain= resets, chained= and chain_max= keep the tally.
  while (reg.release_due(phase + 10 * kVb, &out)) {
  }
  CHECK(!reg.holding());
  CHECK(!reg.offer(frame(3 * kFrame), phase + 5 * kVb - 9'000, phase + 5 * kVb - 9'000, &disp));
  CHECK(reg.chain_run() == 0);
  CHECK(reg.chained_count() == 2);
  CHECK(reg.chain_max() == 2);
}

TEST(chains_counter_counts_chain_starts_not_frames) {
  FrameRegulator reg(12, true, 3);
  warm(reg);
  const uint64_t phase = kT0 + 7 * kVb;
  FrameRegulator::Displaced disp;
  DmaFrame out;
  const uint64_t t1 = phase + kVb - 12'000;
  CHECK(!reg.offer(frame(0), t1, t1, &disp));
  CHECK(!reg.offer(frame(kFrame), t1 + 1'000, t1 + 1'000, &disp));            // start
  CHECK(!reg.offer(frame(2 * kFrame), t1 + 1'000 + kFrame, t1 + 1'000 + kFrame, &disp));  // same chain
  CHECK(reg.chains_count() == 1);
  CHECK(reg.chained_count() == 2);
  while (reg.release_due(phase + 10 * kVb, &out)) {
  }
  // Fresh collision after the queue drained = a second chain.
  const uint64_t t2 = phase + 12 * kVb - 12'000;
  CHECK(!reg.offer(frame(3 * kFrame), t2, t2, &disp));
  CHECK(!reg.offer(frame(4 * kFrame), t2 + 1'000, t2 + 1'000, &disp));
  CHECK(reg.chains_count() == 2);
}

TEST(chain_budget_cuts_run_by_dropping_held_older_frame) {
  // display.chain_budget N: once a run has N chained frames, the next
  // colliding frame takes its natural slot and the held occupant is
  // displaced (freshest wins, one drop) instead of extending the chain.
  FrameRegulator reg(12, true, 3, /*chain_budget=*/2);
  warm(reg);
  const uint64_t phase = kT0 + 7 * kVb;
  FrameRegulator::Displaced disp;
  DmaFrame out;
  const uint64_t t1 = phase + kVb - 12'000;
  CHECK(!reg.offer(frame(0), t1, t1, &disp));                              // slot 1
  CHECK(!reg.offer(frame(kFrame), t1 + 1'000, t1 + 1'000, &disp));                 // chained, slot 2
  CHECK(!reg.offer(frame(2 * kFrame), t1 + 1'000 + kFrame, t1 + 1'000 + kFrame, &disp));    // chained, slot 3
  CHECK(reg.chain_run() == 2);
  CHECK(disp.n == 0);
  // Fourth frame at cadence: natural slot 3 is held by frame 2 -- budget
  // spent, so frame 2 is displaced and frame 3 takes slot 3.
  CHECK(!reg.offer(frame(3 * kFrame), t1 + 1'000 + 2 * kFrame, t1 + 1'000 + 2 * kFrame, &disp));
  CHECK(disp.n == 1);
  CHECK(disp.f[0].opaque == frame(2 * kFrame).opaque);
  CHECK(reg.chain_cuts() == 1);
  CHECK(reg.chain_run() == 0);
  CHECK(reg.chained_count() == 2);
  CHECK(reg.vsync_skips() == 0);
  // Release order: frame 0 at slot 1, frame 1 at slot 2, frame 3 at slot 3.
  CHECK(reg.release_due(phase + kVb - kLead, &out));
  CHECK(out.opaque == frame(0).opaque);
  CHECK(reg.release_due(phase + 2 * kVb - kLead, &out));
  CHECK(out.opaque == frame(kFrame).opaque);
  CHECK(reg.release_due(phase + 3 * kVb - kLead, &out));
  CHECK(out.opaque == frame(3 * kFrame).opaque);
  CHECK(!reg.holding());
}

TEST(chain_budget_zero_never_cuts) {
  FrameRegulator reg(12, true, 3, 0);
  warm(reg);
  const uint64_t phase = kT0 + 7 * kVb;
  FrameRegulator::Displaced disp;
  const uint64_t t1 = phase + kVb - 12'000;
  CHECK(!reg.offer(frame(0), t1, t1, &disp));
  for (int i = 1; i <= 3; ++i)
    CHECK(!reg.offer(frame(i * kFrame), t1 + 1'000 + (i - 1) * kFrame, t1 + 1'000 + (i - 1) * kFrame, &disp));
  CHECK(reg.chain_run() == 3);
  CHECK(reg.chain_cuts() == 0);
  CHECK(disp.n == 0);
}

// ---- capture-time scheduling (display.sched_cap_ms) ----
//
// What the operator sees is the frame on screen at each refresh, so these
// tests model the presenter: a frame released at r latches at the first
// flip >= r, the newest of several wins the flip, and the displayed index
// sequence is scored for repeats (same frame shown twice) and skips
// (indices jumped). Delivery = capture + 20 ms + jitter; capture runs at
// the source period, the panel at 60.000 Hz.
namespace {
struct ScreenSim {
  uint64_t offered = 0, repeats = 0, skips = 0, late_slot = 0, flips = 0;
  uint64_t backwards = 0;          // a latched frame older than one already shown
  std::vector<double> hitch_s;     // when each repeat/skip happened, s from start
};

ScreenSim run_screen_sim(int sched_cap_ms, double src_period_us, double seconds,
                         const std::function<double(uint64_t)>& jitter_us,
                         const std::function<void(FrameRegulator&, double)>& on_flip = nullptr,
                         bool phase_lock_mode = false) {
  FrameRegulator reg(12, true, 3, 3, sched_cap_ms);
  reg.set_phase_lock_mode(phase_lock_mode);
  const double kPanel = 1e6 / 60.0;
  reg.set_panel_period(kPanel);
  FrameRegulator::Displaced disp;
  DmaFrame out;
  ScreenSim s;
  double flip_t = static_cast<double>(kT0);
  for (int i = 0; i < 8; ++i) {
    reg.on_flip(static_cast<uint64_t>(flip_t), true);
    flip_t += kPanel;
  }
  // Frames waiting to latch: (release time, index). Newest wins a flip.
  std::vector<std::pair<double, int64_t>> pending;
  int64_t shown = -1;
  uint64_t idx = 0;
  const double cap0 = flip_t + 3'000.0;
  double prev_dec = 0.0;
  auto dec_time = [&](uint64_t i) {
    const double t = cap0 + i * src_period_us + 20'000.0 + jitter_us(i);
    prev_dec = t > prev_dec + 300.0 ? t : prev_dec + 300.0;
    return prev_dec;
  };
  double next_dec = dec_time(0);
  const double t_end = static_cast<double>(kT0) + seconds * 1e6;
  auto index_of = [](const DmaFrame& f) {
    return static_cast<int64_t>(reinterpret_cast<uintptr_t>(f.opaque) >> 1);
  };
  while (flip_t < t_end) {
    const double t = flip_t <= next_dec ? flip_t : next_dec;
    while (reg.release_due(static_cast<uint64_t>(t), &out)) pending.push_back({t, index_of(out)});
    if (flip_t <= next_dec) {
      int64_t best = -1;
      for (const auto& p : pending) if (p.first <= flip_t && p.second > best) best = p.second;
      pending.erase(std::remove_if(pending.begin(), pending.end(),
                                   [&](const auto& p) { return p.first <= flip_t; }),
                    pending.end());
      const double now_s = (flip_t - static_cast<double>(kT0)) / 1e6;
      if (best >= 0 && shown >= 0) {
        if (best > shown + 1) {
          s.skips += static_cast<uint64_t>(best - shown - 1);
          s.hitch_s.push_back(now_s);
        } else if (best < shown) {
          ++s.backwards;
        }
      } else if (best < 0 && shown >= 0) {
        ++s.repeats;  // nothing new latched: the panel shows the old frame again
        s.hitch_s.push_back(now_s);
      }
      if (best > shown) shown = best;
      ++s.flips;
      reg.on_flip(static_cast<uint64_t>(flip_t), true);
      if (on_flip) on_flip(reg, now_s);
      flip_t += kPanel;
    } else {
      DmaFrame f;
      f.pts_us = static_cast<uint32_t>(idx * src_period_us);
      f.opaque = reinterpret_cast<void*>(static_cast<uintptr_t>((idx << 1) | 1u));
      if (reg.offer(f, static_cast<uint64_t>(next_dec), static_cast<uint64_t>(next_dec), &disp))
        pending.push_back({next_dec, static_cast<int64_t>(idx)});
      ++s.offered;
      ++idx;
      next_dec = dec_time(idx);
    }
  }
  s.late_slot = reg.late_slot_count();
  return s;
}

// Clean-link delivery: 0-2 ms noise, the GOP-start frame (every 30th) 8 ms
// late, and ~1 % of frames 6 ms late -- the shape measured on the bench.
double clean_jitter(uint64_t i) {
  uint64_t h = i * 2654435761u;
  double j = static_cast<double>(h % 2000);
  if (i % 30 == 0) j += 8'000.0;
  else if ((h >> 13) % 100 == 0) j += 6'000.0;
  return j;
}
}  // namespace

TEST(capture_sched_hides_clean_link_jitter) {
  // 59.99 fps vs 60 Hz: one beat every ~100 s, so 210 s covers two full
  // sweeps of the arrival phase. Arrival-based servo turns the late tail
  // into repeat+skip pairs for half of each sweep; capture-time slots
  // absorb it, leaving only the beat's own repeats (one per wrap).
  const ScreenSim arrival = run_screen_sim(0, 1e6 / 59.99, 210, clean_jitter);
  const ScreenSim capture = run_screen_sim(16, 1e6 / 59.99, 210, clean_jitter);
  // Two beat wraps give two repeats; the allowance settling as samples
  // age out may move a slot once or twice. Everything else is gone.
  CHECK(capture.repeats + capture.skips <= 8);
  CHECK(capture.backwards == 0);
  CHECK(arrival.skips + arrival.repeats > 50);  // the judder being fixed
}

TEST(capture_sched_loss_stall_hitches_stay_local) {
  // Frame 1200 waits 40 ms for FEC repair, holding the frames behind it
  // (in-order delivery). The screen must freeze across the stall and then
  // be back on capture-time slots: every extra hitch within ~0.15 s of the
  // stall, none later, nothing shown backwards.
  auto jit = [](uint64_t i) { return clean_jitter(i) + (i == 1200 ? 40'000.0 : 0.0); };
  const ScreenSim base = run_screen_sim(16, 1e6 / 59.99, 40, clean_jitter);
  const ScreenSim loss = run_screen_sim(16, 1e6 / 59.99, 40, jit);
  const double t_stall = 1200 * (1e6 / 59.99) / 1e6 + 8 * (1.0 / 60.0);
  int extra_local = 0, extra_far = 0;
  for (double t : loss.hitch_s) {
    bool in_base = false;
    for (double b : base.hitch_s) in_base = in_base || (b > t - 0.002 && b < t + 0.002);
    if (in_base) continue;
    if (t > t_stall - 0.05 && t < t_stall + 0.15) ++extra_local;
    else ++extra_far;
  }
  CHECK(extra_local >= 1);   // the stall is visible...
  CHECK(extra_local <= 6);   // ...as a freeze plus catch-up, bounded
  CHECK(extra_far == 0);     // and nothing trails it
  CHECK(loss.backwards == 0);
}

TEST(capture_sched_sustained_shift_beyond_cap_keeps_frames_flowing) {
  // A slower rung: every frame from t=20 s on is 25 ms later (beyond the
  // cap). Until the window's p1 catches up, every frame misses its capture
  // slot and falls back to arrival-based placement -- frames keep flowing
  // one per refresh rather than being dropped.
  auto jit = [](uint64_t i) { return clean_jitter(i) + (i >= 1200 ? 25'000.0 : 0.0); };
  const ScreenSim s = run_screen_sim(16, 1e6 / 59.99, 60, jit);
  CHECK(s.late_slot > 0);         // the transition was seen as late
  CHECK(s.skips <= 3);            // but nothing was mass-dropped
  CHECK(s.repeats <= 6);
}

TEST(capture_sched_salvaged_frames_do_not_widen_allowance) {
  // Salvaged frames (loss) are excluded from the lateness window: a burst
  // of them must not push the allowance to the cap.
  FrameRegulator reg(12, true, 3, 3, 16);
  warm(reg);
  FrameRegulator::Displaced disp;
  DmaFrame out;
  uint64_t next_flip = 8;
  for (uint64_t i = 0; i < 300; ++i) {
    const uint32_t pts = static_cast<uint32_t>(i * kFrame);
    const bool salvaged = i % 50 == 0;
    if (salvaged) reg.note_incomplete(pts);
    const uint64_t t = kT0 + 8 * kVb + i * kFrame + (salvaged ? 30'000 : 0);
    flips_until(reg, &next_flip, t);
    reg.offer(frame(pts), t, t, &disp);
    while (reg.release_due(t, &out)) {}
  }
  CHECK(reg.allowance_ms() < 3.0);
}

TEST(capture_sched_one_fast_frame_does_not_widen_allowance) {
  // A single frame delivered 5 ms faster than any before it is one sample
  // of the window -- below p1, so it moves nothing. It must not shift the
  // frames after it either: lateness measured against the pts anchor
  // jumps by 5 ms for every later frame (the anchor's floor snaps down to
  // the fast one), and the window, still full of samples taken against
  // the old floor, would read that as 5 ms of spread.
  FrameRegulator reg(12, true, 3, 3, 16);
  warm(reg);
  FrameRegulator::Displaced disp;
  DmaFrame out;
  uint64_t next_flip = 8;
  for (uint64_t i = 0; i < 400; ++i) {
    const uint32_t pts = static_cast<uint32_t>(i * kFrame);
    const uint64_t noise = (i * 7919) % 1000;
    const uint64_t t = kT0 + 8 * kVb + 5'000 + i * kFrame + noise - (i == 300 ? 5'000 : 0);
    flips_until(reg, &next_flip, t);
    reg.offer(frame(pts), t, t, &disp);
    while (reg.release_due(t, &out)) {}
  }
  CHECK(reg.allowance_ms() < 3.0);
}

TEST(capture_sched_never_shows_a_frame_backwards) {
  // Rough link: bursts of 10-60 ms repair delays and a drifting late tail,
  // so the allowance keeps moving and late frames keep falling back to the
  // arrival path. However slots get assigned, the screen must never show a
  // frame older than one it already showed.
  auto rough = [](uint64_t i) {
    uint64_t h = i * 2654435761u;
    double j = static_cast<double>(h % 3000);
    if ((i / 600) % 2 == 0 && i % 30 == 0) j += 12'000.0;  // tail comes and goes
    if ((h >> 11) % 97 == 0) j += 10'000.0 + static_cast<double>((h >> 3) % 50'000);
    return j;
  };
  const ScreenSim s = run_screen_sim(16, 1e6 / 59.99, 120, rough);
  CHECK(s.late_slot > 0);  // the rough link really did push frames late
  CHECK(s.backwards == 0);
}

// ---- phase lock (display.phase_lock) ----

TEST(margin_is_release_minus_capture_want) {
  // One frame: the window holds its own transit, so floor = transit,
  // allowance = 0 + 1 ms, want = arrival + 1 ms. Arrival 10 ms before the
  // vblank at kT0 + 8 periods: want + lead (3 ms) = vblank - 6 ms, so the
  // release is that vblank - 3 ms and the margin is 6 ms.
  FrameRegulator reg(12, true, 3, 3, 16);
  reg.set_phase_lock_mode(true);
  warm(reg);
  FrameRegulator::Displaced disp;
  double m = -1.0;
  CHECK(!reg.take_margin(&m));                       // nothing offered yet
  reg.offer(frame(0), kT0 + 8 * kVb - 10'000, kT0 + 8 * kVb - 10'000, &disp);
  REQUIRE(reg.take_margin(&m));
  CHECK(m > 5'999.0 && m < 6'001.0);
  CHECK(!reg.take_margin(&m));                       // once per offer
  CHECK(reg.panel_period_us() == static_cast<double>(kVb));
}

TEST(no_margin_without_capture_scheduling) {
  FrameRegulator reg(12, true, 3, 3, 0);             // sched_cap_ms 0
  warm(reg);
  FrameRegulator::Displaced disp;
  double m = 0.0;
  reg.offer(frame(0), kT0 + 8 * kVb - 10'000, kT0 + 8 * kVb - 10'000, &disp);
  CHECK(!reg.take_margin(&m));
}

TEST(phase_lock_mode_places_by_arrival_until_switched_on) {
  // Arrival 3.5 ms before the release point of the vblank at kT0 + 8
  // periods: the arrival path holds it 0.5 ms for that vblank; capture
  // placement (want = arrival + 1 ms) needs the next one.
  const uint64_t t = kT0 + 8 * kVb - 3'000 - 500;
  const uint64_t arrival_release = kT0 + 8 * kVb - 3'000;
  const uint64_t capture_release = kT0 + 9 * kVb - 3'000;
  FrameRegulator::Displaced disp;
  double m = 0.0;

  FrameRegulator off(12, true, 3, 3, 16);            // phase lock mode, not locked
  off.set_phase_lock_mode(true);
  warm(off);
  CHECK(!off.offer(frame(0), t, t, &disp));
  CHECK(off.next_release_us() == arrival_release);
  REQUIRE(off.take_margin(&m));                      // measured in both modes
  CHECK(m > kVb - 501.0 && m < kVb - 499.0);

  FrameRegulator on(12, true, 3, 3, 16);             // phase lock mode, locked
  on.set_phase_lock_mode(true);
  on.set_capture_placement(true);
  warm(on);
  CHECK(!on.offer(frame(0), t, t, &disp));
  CHECK(on.next_release_us() == capture_release);

  FrameRegulator classic(12, true, 3, 3, 16);        // no phase lock: today's rule
  warm(classic);
  CHECK(!classic.offer(frame(0), t, t, &disp));
  CHECK(classic.next_release_us() == capture_release);
}

TEST(phase_lock_mode_switching_placement_never_shows_backwards) {
  // The lock turns capture placement on and off as it gains and loses the
  // phase. A switch may cost a hitch, but a frame the arrival path chained to
  // a later slot must never be shown after a newer one.
  const ScreenSim s = run_screen_sim(
      16, 1e6 / 59.99, 60, clean_jitter,
      [](FrameRegulator& r, double t) {
        r.set_capture_placement(static_cast<int>(t / 2.0) % 2 == 0);
      },
      /*phase_lock_mode=*/true);
  CHECK(s.offered > 3000);
  CHECK(s.backwards == 0);
}

// ---- arrival vs now ----

TEST(arrival_feeds_the_window_and_now_decides_lateness) {
  // vsync lock on, warm the estimator with 8 exact flips 16667 apart at kT0.
  FrameRegulator reg(12, true, 3, 0, 16);
  warm(reg);
  // arrival = decode stamp 2 ms before now: this offer takes a capture
  // slot (capture scheduling is on) and just establishes a sane baseline
  // -- next_release_us() no earlier than now -- for the second offer
  // below, which is the actual point.
  FrameRegulator::Displaced d;
  const bool now_ = reg.offer(frame(1000), /*arrival*/ kT0 + 10'000,
                              /*now*/ kT0 + 12'000, &d);
  CHECK(!now_);
  CHECK(reg.next_release_us() >= kT0 + 12'000);
  // The frame's natural slot comes from arrival (barely past the first
  // one, still near kT0) via release_at_or_after, landing near-term; but
  // `now` is pushed well past that slot's release (still inside the
  // estimator's staleness window, so the servo stays engaged -- this is
  // not the stale/fallback case). A hand-over this late presents at once:
  // `now`, not `arrival`, decides lateness.
  CHECK(reg.offer(frame(1000 + kFrame), kT0 + 10'000 + kFrame, kT0 + 200'000,
                 &d));
  CHECK(reg.late_count() == 1);
  CHECK(reg.servo_locked());  // still on the grid, not the stale fallback
}

TEST(servo_handover_past_its_release_presents_now_not_held_a_period) {
  // The specific pump-stall shape: arrival maps to vblank V1 (a normal,
  // reachable slot), but the hand-over to offer() doesn't happen until
  // `now` has already slid past V1's release -- into the lead window
  // before V1 itself. The natural slot comes from arrival, so V1's
  // already-past release is what the late check sees and the frame
  // presents at once; a slot taken from now (next_release(now, ...))
  // would always be >= now and park it on V2, a guaranteed
  // one-period-late repeat.
  FrameRegulator reg(12, true, 3);
  warm(reg);
  const uint64_t phase = kT0 + 7 * kVb;
  const uint64_t arrival = phase + kVb - 9'000;    // maps to V1
  const uint64_t v1_release = phase + kVb - kLead;  // V1 - lead
  const uint64_t now = v1_release + 1'000;          // past release, before V1
  FrameRegulator::Displaced disp;
  CHECK(now > v1_release && now < phase + kVb);
  CHECK(reg.offer(frame(0), arrival, now, &disp));  // presented now
  CHECK(reg.late_count() == 1);
  CHECK(!reg.holding());  // not parked on V2
}

TEST(a_vacated_slot_stays_taken_for_a_newer_frame_behind_a_chain) {
  // f0 holds V1 and f1 collides with it, chaining to V1 + period. f0 is
  // released. f2's own arrival maps to V1 again and its hand-over is past
  // V1's release -- but f0 was committed for V1, so V1 is taken. f2 moves
  // to the first vblank after it, finds f1 there and chains once more, to
  // V1 + 2 periods. Nothing is displaced, nothing presents at once, and
  // the three come out in pts order on consecutive vblanks.
  FrameRegulator reg(12, true, 3);
  warm(reg);
  const uint64_t V1 = kT0 + 8 * kVb;
  FrameRegulator::Displaced disp;
  DmaFrame out;
  CHECK(!reg.offer(frame(0), V1 - 9'000, V1 - 9'000, &disp));       // f0 -> V1
  CHECK(!reg.offer(frame(kFrame), V1 - 8'000, V1 - 8'000, &disp));  // collides, chains to V1+kVb
  CHECK(disp.n == 0);
  CHECK(reg.release_due(V1 - kLead, &out));  // f0 goes out for V1
  CHECK(out.opaque == frame(0).opaque);
  CHECK(!reg.offer(frame(2 * kFrame), V1 - 7'000, V1 - 2'000, &disp));  // f2: held
  CHECK(disp.n == 0);
  CHECK(reg.late_count() == 0);
  CHECK(reg.release_due(V1 + kVb - kLead, &out));
  CHECK(out.opaque == frame(kFrame).opaque);
  CHECK(!reg.release_due(V1 + 2 * kVb - kLead - 1, &out));
  CHECK(reg.release_due(V1 + 2 * kVb - kLead, &out));
  CHECK(out.opaque == frame(2 * kFrame).opaque);
  CHECK(!reg.holding());
}

TEST(deeper_chain_emits_monotonic_pts_with_full_accounting) {
  // A deeper version of the same shape: f1 chains past f0's slot; f0
  // drains; f2 and f3 both map back to f0's vblank with late hand-overs;
  // f4 maps to the vblank after it. f0's vblank stays taken, so f2 and f3
  // chain past f1 and f3 takes f2's slot (freshest wins); f4 finds f1 on
  // its own slot and claims the one after it from f3. Every offered frame
  // is accounted for exactly once (emitted xor displaced), the emitted ones
  // come out in increasing pts order, and f1 -- held throughout -- is shown.
  FrameRegulator reg(12, true, 3);
  warm(reg);
  const uint64_t V1 = kT0 + 8 * kVb;
  FrameRegulator::Displaced disp;
  DmaFrame out;
  std::vector<uint32_t> emitted, displaced;
  const uint32_t pts[5] = {0, static_cast<uint32_t>(kFrame),
                           static_cast<uint32_t>(2 * kFrame),
                           static_cast<uint32_t>(3 * kFrame),
                           static_cast<uint32_t>(4 * kFrame)};

  CHECK(!reg.offer(frame(pts[0]), V1 - 9'000, V1 - 9'000, &disp));  // f0 -> V1
  CHECK(!reg.offer(frame(pts[1]), V1 - 8'000, V1 - 8'000, &disp));  // f1, chains
  CHECK(reg.release_due(V1 - 3'000, &out));
  emitted.push_back(out.pts_us);  // f0

  const bool p2 = reg.offer(frame(pts[2]), V1 - 7'000, V1 - 2'000, &disp);
  for (int i = 0; i < disp.n; ++i) displaced.push_back(disp.f[i].pts_us);
  if (p2) emitted.push_back(pts[2]);

  const bool p3 = reg.offer(frame(pts[3]), V1 - 6'000, V1 - 2'000, &disp);
  for (int i = 0; i < disp.n; ++i) displaced.push_back(disp.f[i].pts_us);
  if (p3) emitted.push_back(pts[3]);

  const bool p4 = reg.offer(frame(pts[4]), V1 - 2'000, V1 - 1'000, &disp);
  for (int i = 0; i < disp.n; ++i) displaced.push_back(disp.f[i].pts_us);
  if (p4) emitted.push_back(pts[4]);

  while (reg.release_due(V1 + 10 * kVb, &out)) emitted.push_back(out.pts_us);

  // Every offered frame is accounted for exactly once: emitted xor
  // displaced, never both, never neither.
  CHECK(emitted.size() + displaced.size() == 5);
  for (uint32_t p : pts) {
    const bool in_e = std::find(emitted.begin(), emitted.end(), p) != emitted.end();
    const bool in_d = std::find(displaced.begin(), displaced.end(), p) != displaced.end();
    CHECK(in_e != in_d);
  }
  CHECK(emitted == (std::vector<uint32_t>{pts[0], pts[1], pts[4]}));
  CHECK(displaced == (std::vector<uint32_t>{pts[2], pts[3]}));
  // Whatever IS emitted comes out in increasing pts order.
  for (size_t i = 1; i < emitted.size(); ++i) CHECK(emitted[i] > emitted[i - 1]);
}

TEST(late_frame_offered_before_the_drain_displaces_every_held_frame) {
  // The caller may offer before it drains the frames already due. f0 is
  // held on V1; nothing drains it; f1's hand-over is past its own
  // release, so f1 presents at once. f0's release is earlier than f1's,
  // so a release comparison alone would keep it -- and the drain that
  // follows would then show f0 AFTER f1. Presenting at once flushes
  // every held frame instead: they are all older.
  FrameRegulator reg(12, true, 3);
  warm(reg);
  const uint64_t V1 = kT0 + 8 * kVb;
  const uint64_t V2 = V1 + kVb;
  FrameRegulator::Displaced disp;
  DmaFrame out;
  CHECK(!reg.offer(frame(0), V1 - 9'000, V1 - 9'000, &disp));  // f0 -> V1
  CHECK(reg.offer(frame(kFrame), V2 - 9'000, V2 - 1'000, &disp));  // f1: late, now
  CHECK(disp.n == 1);
  CHECK(disp.f[0].opaque == frame(0).opaque);  // f0 displaced, never shown after f1
  CHECK(reg.late_count() == 1);
  CHECK(!reg.holding());
  CHECK(!reg.release_due(V2 - 1'000, &out));
}

TEST(pair_partner_handed_after_its_partner_left_takes_the_next_vblank) {
  // A burst pair: A is decoded and offered before its release and goes out
  // on vblank V at V - lead. B, its partner one pts step later, is decoded
  // 0.1 ms before that release but offered 0.1 ms after it. B's arrival
  // maps to V too -- the vblank A has already been committed for. A is no
  // longer held, so only the regulator's memory of what it let go of can
  // see that V is taken: B waits for V + period instead of presenting at
  // once behind A's pending flip (which would make A repeat).
  FrameRegulator reg(12, true, 3);
  warm(reg);
  const uint64_t V = kT0 + 8 * kVb;
  const uint64_t relV = V - kLead;
  FrameRegulator::Displaced disp;
  DmaFrame out;
  CHECK(!reg.offer(frame(0), relV - 800, relV - 700, &disp));  // A -> V
  CHECK(reg.release_due(relV, &out));
  CHECK(out.opaque == frame(0).opaque);
  CHECK(!reg.offer(frame(kFrame), relV - 100, relV + 100, &disp));  // B: held
  CHECK(disp.n == 0);
  CHECK(reg.late_count() == 0);
  CHECK(reg.next_release_us() == relV + kVb);
  CHECK(!reg.release_due(relV + kVb - 1, &out));
  CHECK(reg.release_due(relV + kVb, &out));
  CHECK(out.opaque == frame(kFrame).opaque);
}

TEST(a_frame_presented_at_once_marks_its_vblank_taken) {
  // S's own slot V has passed by the time it is offered and nothing went
  // out on V, so S presents at once. T, decoded before V's release and
  // offered just after S, maps to V as well: S has it, so T is held for
  // V + period.
  FrameRegulator reg(12, true, 3);
  warm(reg);
  const uint64_t V = kT0 + 8 * kVb;
  const uint64_t relV = V - kLead;
  FrameRegulator::Displaced disp;
  DmaFrame out;
  CHECK(reg.offer(frame(0), relV - 900, relV + 1'000, &disp));  // S: at once
  CHECK(reg.late_count() == 1);
  CHECK(!reg.offer(frame(kFrame), relV - 50, relV + 1'100, &disp));  // T: held
  CHECK(disp.n == 0);
  CHECK(reg.late_count() == 1);
  CHECK(reg.next_release_us() == relV + kVb);
  CHECK(reg.release_due(relV + kVb, &out));
  CHECK(out.opaque == frame(kFrame).opaque);
}

TEST(forgetting_the_output_history_lets_a_late_frame_present_at_once) {
  // The history of the last frame out is forgotten when a park dropped
  // everything (reset_output_history), when the panel grid is reseeded
  // (set_panel_period) and on a pts discontinuity. The pair partner of
  // pair_partner_handed_after_its_partner_left_takes_the_next_vblank then
  // presents at once: its slot has passed and nothing holds it any more.
  for (int how = 0; how < 3; ++how) {
    FrameRegulator reg(12, true, 3);
    warm(reg);
    const uint64_t V = kT0 + 8 * kVb;
    const uint64_t relV = V - kLead;
    FrameRegulator::Displaced disp;
    DmaFrame out;
    CHECK(!reg.offer(frame(0), relV - 800, relV - 700, &disp));
    CHECK(reg.release_due(relV, &out));
    uint32_t next_pts = static_cast<uint32_t>(kFrame);
    if (how == 0) {
      reg.reset_output_history();
    } else if (how == 1) {
      reg.set_panel_period(static_cast<double>(kVb));
      warm(reg);
    } else {
      CHECK(reg.offer(frame(5'000'000), relV - 150, relV + 50, &disp));  // drone restart
      CHECK(reg.discont_count() == 1);
      next_pts = 5'000'000 + static_cast<uint32_t>(kFrame);
    }
    const uint64_t late_before = reg.late_count();
    CHECK(reg.offer(frame(next_pts), relV - 100, relV + 100, &disp));
    CHECK(reg.late_count() == late_before + 1);
    CHECK(!reg.holding());
  }
}

namespace {
// Drives a regulator through n decode/hand-over pairs at src_period_us,
// feeding on_flip at panel_period_us. Arrivals and hand-overs both move
// forward only, and arrival_us <= now_us always, matching a real pump:
// decode finishes, THEN the caller eventually offers it.
//  - stalls: 25% of hand-overs land 0-20 ms past their own arrival.
//  - drain_first_p: the probability that the caller drains the frames
//    already due before offering; otherwise it offers first and drains
//    after (the regulator's order must not depend on which).
//  - bursts: 10% of the time a decode stall holds back 2-4 frames, which
//    then arrive back-to-back (0-1.5 ms apart) at the last one's
//    nominal arrival.
//  - pairs: every odd frame is the second of a burst pair, decoded
//    0.5-1.5 ms after its partner, and half of them are handed over up to
//    2 ms after their decode -- so a partner can be offered just after the
//    first frame of its pair was released.
// Returns every frame actually emitted (via offer()'s immediate return
// or release_due, in that order) and every frame displaced, both as
// their own pts_us.
struct OrderFuzzResult {
  std::vector<uint32_t> emitted;
  std::vector<uint32_t> displaced;
};

struct OrderFuzzMode {
  bool stalls;
  double drain_first_p;
  bool bursts;
  bool pairs;
};

OrderFuzzResult run_order_fuzz(unsigned seed, int n, double src_period_us,
                               double panel_period_us, int chain_budget,
                               int sched_cap_ms, OrderFuzzMode mode) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> U(0.0, 1.0);
  FrameRegulator reg(12, true, 3, chain_budget, sched_cap_ms);
  reg.set_panel_period(panel_period_us);
  OrderFuzzResult r;
  double flip_t = static_cast<double>(kT0);
  const auto flips_to = [&](double t) {
    while (flip_t <= t) {
      reg.on_flip(static_cast<uint64_t>(flip_t), true);
      flip_t += panel_period_us;
    }
  };
  flips_to(flip_t + 7 * panel_period_us);
  DmaFrame out;
  const auto drain_to = [&](double t) {
    flips_to(t);
    while (reg.release_due(static_cast<uint64_t>(t), &out)) r.emitted.push_back(out.pts_us);
  };
  double nominal = flip_t + 1'000.0;
  double last_arr = 0.0, last_now = 0.0;
  int burst_left = 0;
  for (int i = 0; i < n; ++i) {
    const uint32_t pts = static_cast<uint32_t>(i * src_period_us);
    double arr_t = nominal;
    if (mode.bursts && burst_left == 0 && U(rng) < 0.1) {
      burst_left = 2 + static_cast<int>(rng() % 3);
      arr_t = nominal + (burst_left - 1) * src_period_us;  // held back to the last one's slot
    } else if (burst_left > 0) {
      arr_t = last_arr + U(rng) * 1'500.0;  // decoded back-to-back
    }
    if (burst_left > 0) --burst_left;
    const bool pair_second = mode.pairs && (i % 2) == 1;
    if (pair_second) arr_t = last_arr + 500.0 + U(rng) * 1'000.0;
    arr_t = std::max(arr_t, last_arr);
    double now_t = mode.stalls && U(rng) < 0.25 ? arr_t + U(rng) * 20'000.0 : arr_t;
    if (pair_second && U(rng) < 0.5) now_t = arr_t + U(rng) * 2'000.0;
    now_t = std::max(now_t, last_now);
    last_arr = arr_t;
    last_now = now_t;
    if (U(rng) < mode.drain_first_p)
      drain_to(now_t);
    else
      flips_to(now_t);
    FrameRegulator::Displaced disp;
    const bool now_ = reg.offer(frame(pts), static_cast<uint64_t>(arr_t),
                                static_cast<uint64_t>(now_t), &disp);
    for (int k = 0; k < disp.n; ++k) r.displaced.push_back(disp.f[k].pts_us);
    if (now_) r.emitted.push_back(pts);
    drain_to(now_t);
    nominal += src_period_us + U(rng) * 2'000.0;
  }
  drain_to(flip_t + 20 * panel_period_us);
  return r;
}
}  // namespace

TEST(randomised_order_fuzz_never_emits_backwards) {
  // Fixed seeds across 59.94/100 fps sources on a 60 Hz panel, capture
  // placement on and off, chain_budget bounded and unbounded, in two
  // pump shapes: drain-then-offer with stalled hand-overs, and a mixed
  // pump (offer before the drain 30% of the time) with decode bursts on
  // top, plus burst pairs whose second frame is often handed over just
  // after the first one's release. Every offered frame is accounted for
  // exactly once (emitted xor displaced) and the emitted ones never go
  // backwards in pts. Cheap enough for the host suite.
  const double kPanel = 1e6 / 60.0;
  const double rates[] = {1e6 / 59.94, 1e6 / 100.0};
  const int budgets[] = {0, 3};
  const int caps[] = {0, 16};
  const unsigned seeds[] = {1, 2, 3};
  const OrderFuzzMode modes[] = {
      {/*stalls=*/true, /*drain_first_p=*/1.0, /*bursts=*/false, /*pairs=*/false},
      {/*stalls=*/true, /*drain_first_p=*/0.7, /*bursts=*/true, /*pairs=*/false},
      {/*stalls=*/false, /*drain_first_p=*/1.0, /*bursts=*/false, /*pairs=*/true},
      {/*stalls=*/true, /*drain_first_p=*/0.7, /*bursts=*/false, /*pairs=*/true},
  };
  int backwards = 0, misaccounted = 0;
  for (const OrderFuzzMode& mode : modes) {
    for (double src : rates) {
      for (int budget : budgets) {
        for (int cap : caps) {
          for (unsigned seed : seeds) {
            const OrderFuzzResult r = run_order_fuzz(seed, 2000, src, kPanel, budget, cap, mode);
            std::vector<uint32_t> seen = r.emitted;
            seen.insert(seen.end(), r.displaced.begin(), r.displaced.end());
            std::sort(seen.begin(), seen.end());
            if (seen.size() != 2000 ||
                std::adjacent_find(seen.begin(), seen.end()) != seen.end())
              ++misaccounted;  // lost or duplicated a frame
            for (size_t i = 1; i < r.emitted.size(); ++i)
              if (r.emitted[i] <= r.emitted[i - 1]) ++backwards;
          }
        }
      }
    }
  }
  CHECK(misaccounted == 0);
  CHECK(backwards == 0);
}

TEST(capture_window_and_lateness_split_between_arrival_and_now) {
  // Pins BOTH halves of the split against the failure mode of collapsing
  // them back into one clock. Frame A's arrival is on time (it matches
  // the capture floor the window settles on) but its hand-over (`now`) is
  // 50 ms late; frame B's roles are swapped -- its arrival is 5 ms late,
  // its hand-over is on time. Passing `now` everywhere would seed the
  // transit window from frame A's late `now` and frame B's on-time `now`
  // -- the ~5 ms spread that actually happened (frame A on time, frame B
  // late) would read as ~0, and frame A would win a capture slot instead
  // of presenting at once, late (its `now`-derived "arrival" wouldn't
  // look late to place_by_capture any more). Passing `arrival` everywhere
  // would judge frame A's lateness against its own on-time arrival
  // instead of its late hand-over, so it would ALSO wrongly win a slot.
  FrameRegulator reg(12, true, 3, 3, 16);
  warm(reg);
  const uint64_t phase = kT0 + 7 * kVb;
  FrameRegulator::Displaced disp;
  double margin = 0.0;

  // Frame A: arrival on time, hand-over 50 ms late.
  const uint64_t arrival_a = phase;
  const uint64_t now_a = phase + 50'000;
  CHECK(reg.offer(frame(0), arrival_a, now_a, &disp));  // late: presented at once
  CHECK(reg.late_count() == 1);
  CHECK(!reg.holding());
  REQUIRE(reg.take_margin(&margin));  // capture_slot still ran and set one
  CHECK(margin > 12'000.0 && margin < 13'000.0);

  // Frame B: arrival 5 ms late, hand-over on time.
  const uint64_t arrival_b = arrival_a + kFrame + 5'000;
  const uint64_t now_b = arrival_a + kFrame;
  CHECK(!reg.offer(frame(kFrame), arrival_b, now_b, &disp));  // held
  // Window now holds frame A's (on-time) and frame B's (5 ms late)
  // transit: the spread reads ~5 ms if arrival fed it, not the ~0 either
  // wrong wiring would see (both frames' `now` values are mutually
  // consistent with each other, not with either frame's real transit).
  CHECK(reg.allowance_ms() > 4.0);
  CHECK(reg.allowance_ms() < 8.0);
  CHECK(reg.next_release_us() == phase + 30'334);  // the slot its OWN arrival maps to
}

TEST(getters_are_safe_under_concurrent_offers) {
  // A smoke test for the lock: no data race under TSAN, no crash otherwise.
  FrameRegulator reg(12);
  std::atomic<bool> stop{false};
  std::thread t([&] {
    while (!stop) {
      (void)reg.held_count();
      (void)reg.allowance_ms();
      (void)reg.next_release_us();
    }
  });
  for (int i = 0; i < 2000; ++i) {
    FrameRegulator::Displaced d;
    reg.offer(frame(static_cast<uint32_t>(i * kFrame)), kT0 + i * kFrame,
             kT0 + i * kFrame, &d);
    DmaFrame o;
    while (reg.release_due(kT0 + i * kFrame + 20'000, &o)) {
    }
  }
  stop = true;
  t.join();
}
