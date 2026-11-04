#include <string>

#include "mtest.h"
#include "narrow_mode.h"
using namespace maburgs;

// Two USB cards: card 0 transmits, card 1 leads every move.
namespace {
struct Rig {
  NarrowCfg cfg;
  NarrowMode nm{NarrowCfg{}};
  NarrowIn in;
  Rig() {
    cfg.enable = true;
    nm = NarrowMode(cfg);
    in.in_session = true;
    in.rung = 0;
    in.lead_candidate = 1;
    in.tx_card = 0;
    in.width = {40, 40};
    in.switching = {false, false};
    in.own = {100, 100};
    in.best_snr_db = 6.0;
  }
  NarrowOut at(double t) {
    in.now_ms = t;
    return nm.tick(in);
  }
};
}  // namespace

TEST(enters_only_after_sustained_distress_on_rung_0) {
  Rig r;
  r.in.distress = true;
  CHECK(r.at(0).state == NarrowState::Off);
  CHECK(r.at(1499).state == NarrowState::Off);
  auto o = r.at(1500);
  CHECK(o.state == NarrowState::Entering && o.op_narrow && o.event != nullptr);
  CHECK(o.lead_card == 1 && o.lead_w == 10 && o.rest_w == 0);   // TX card stays put
  // Not on rung 0, disabled, or no session: never.
  Rig r1;
  r1.in.distress = true;
  r1.in.rung = 1;
  CHECK(r1.at(0).state == NarrowState::Off && r1.at(5000).state == NarrowState::Off);
  Rig r2;
  r2.cfg.enable = false;
  r2.nm = NarrowMode(r2.cfg);
  r2.in.distress = true;
  CHECK(r2.at(0).state == NarrowState::Off && r2.at(5000).state == NarrowState::Off);
  // A gap in the distress restarts the clock.
  Rig r3;
  r3.in.distress = true;
  r3.at(0);
  r3.in.distress = false;
  r3.at(1000);
  r3.in.distress = true;
  r3.at(1100);
  CHECK(r3.at(2599).state == NarrowState::Off);
  CHECK(r3.at(2600).state == NarrowState::Entering);
}

TEST(confirm_counts_only_frames_heard_after_the_lead_card_moved) {
  Rig r;
  r.in.distress = true;
  r.at(0);
  r.at(1500);                       // Entering
  r.in.own[1] = 120;                // heard at 40, before moving: does not count
  CHECK(r.at(1600).state == NarrowState::Entering);
  r.in.switching[1] = true;         // switch in flight
  CHECK(r.at(1650).state == NarrowState::Entering);
  r.in.switching[1] = false;
  r.in.width[1] = 10;               // at 10: baseline taken
  CHECK(r.at(1700).state == NarrowState::Entering);
  r.in.own[1] = 121;                // the drone at 10
  auto o = r.at(1750);
  CHECK(o.state == NarrowState::On && o.op_narrow && o.lead_w == 10 && o.rest_w == 10);
}

TEST(entering_cancels_without_the_drone_and_cools_down) {
  Rig r;
  r.in.distress = true;
  r.at(0);
  r.at(1500);
  r.in.width[1] = 10;
  r.at(1600);
  auto o = r.at(1500 + 2001);
  CHECK(o.state == NarrowState::Off && !o.op_narrow && o.lead_w == 0);
  CHECK(r.nm.cancels() == 1);
  // Cooldown: distress again right away does not re-enter for 20 s.
  r.in.width[1] = 40;
  CHECK(r.at(4000).state == NarrowState::Off);
  CHECK(r.at(10000).state == NarrowState::Off);
  CHECK(r.at(23502).state == NarrowState::Off);   // distress clock restarts after cooldown
  CHECK(r.at(25002).state == NarrowState::Entering);
}

namespace {
void to_on(Rig& r) {
  r.in.distress = true;
  r.at(0);
  r.at(1500);
  r.in.width[1] = 10;
  r.at(1600);
  r.in.own[1] += 1;
  r.at(1700);
  r.in.distress = false;
  r.in.width[0] = 10;
}
}  // namespace

TEST(on_rescues_when_no_card_hears_the_drone) {
  Rig r;
  to_on(r);
  CHECK(r.nm.state() == NarrowState::On);
  r.in.own[0] += 5;
  CHECK(r.at(3000).state == NarrowState::On);
  CHECK(r.at(4499).state == NarrowState::On);
  auto o = r.at(4501);
  CHECK(o.state == NarrowState::Off && !o.op_narrow && o.rest_w == 0);
  CHECK(r.nm.rescues() == 1);
}

TEST(blind_promote_needs_clean_time_and_snr_margin_then_a_confirm) {
  Rig r;
  to_on(r);
  r.in.clean = true;
  r.in.best_snr_db = 10.0;   // entry 6: needs >= 6 + 3 + 3 = 12
  for (double t = 1800; t < 12000; t += 100) {
    r.in.own[0] += 1;
    CHECK(r.at(t).state == NarrowState::On);
  }
  r.in.best_snr_db = 12.5;
  r.in.own[0] += 1;
  auto o = r.at(12000);
  CHECK(o.state == NarrowState::Leaving && !o.op_narrow);
  CHECK(o.lead_w == 0 && o.rest_w == 10);   // lead back to its normal width first
  // Lead card back at 40, hears the drone at 20: done.
  r.in.width[1] = 40;
  r.at(12100);
  r.in.own[1] += 1;
  o = r.at(12200);
  CHECK(o.state == NarrowState::Off && o.lead_w == 0 && o.rest_w == 0);
  CHECK(r.nm.promotes() == 1);
}

TEST(leaving_returns_to_10_when_the_drone_stays) {
  Rig r;
  to_on(r);
  r.in.clean = true;
  r.in.best_snr_db = std::numeric_limits<double>::quiet_NaN();   // unknown: clean time alone
  double t = 1800;
  for (; r.nm.state() == NarrowState::On && t < 20000; t += 100) {
    r.in.own[0] += 1;
    r.at(t);
  }
  CHECK(r.nm.state() == NarrowState::Leaving);
  r.in.width[1] = 40;
  auto o = r.at(t + 2100);
  CHECK(o.state == NarrowState::On && o.op_narrow && o.lead_w == 10);
}

TEST(one_card_gs_leads_with_the_tx_card_after_a_delay) {
  Rig r;
  r.in.lead_candidate = 0;
  r.in.width = {40};
  r.in.switching = {false};
  r.in.own = {10};
  r.in.distress = true;
  r.at(0);
  auto o = r.at(1500);
  CHECK(o.state == NarrowState::Entering && o.lead_w == 0);   // RCFs at 40 first
  o = r.at(1800);
  CHECK(o.lead_w == 10);
}
MTEST_MAIN

// --- link.narrow10.silence (gs21): a video stop on MCS0/20 ------------------

namespace {
Rig silence_rig() {
  Rig r;
  r.cfg.silence = true;
  r.nm = NarrowMode(r.cfg);
  return r;
}
}  // namespace

TEST(silence_off_a_video_stop_never_enters) {
  // The old rule this replaces: no video is "no data", which resets the run.
  Rig r;
  for (double t = 0; t <= 5000; t += 50) {
    r.in.video_silent_ms = t;
    CHECK(r.at(t).state == NarrowState::Off);
  }
  CHECK(r.nm.entries() == 0);
}

TEST(silence_a_video_stop_enters_after_silent_ms) {
  Rig r = silence_rig();
  r.in.video_silent_ms = 0;
  CHECK(r.at(0).state == NarrowState::Off);
  r.in.video_silent_ms = 399;
  CHECK(r.at(399).state == NarrowState::Off);
  r.in.video_silent_ms = 400;
  auto o = r.at(400);
  CHECK(o.state == NarrowState::Entering && o.op_narrow);
  CHECK(o.lead_card == 1 && o.lead_w == 10 && o.rest_w == 0);   // the TX card keeps sending at 20
  CHECK(r.nm.entries() == 1 && r.nm.silent_entries() == 1);
  CHECK(std::string(o.event).find("video stopped") != std::string::npos);
}

TEST(silence_needs_rung_0_a_session_and_no_block) {
  for (int k = 0; k < 3; ++k) {
    Rig r = silence_rig();
    if (k == 0) r.in.rung = 1;
    if (k == 1) r.in.in_session = false;
    if (k == 2) r.in.blocked = true;
    for (double t = 0; t <= 3000; t += 50) {
      r.in.video_silent_ms = t;
      CHECK(r.at(t).state == NarrowState::Off);
    }
  }
}

TEST(silence_a_quiet_window_keeps_the_distress_run_going) {
  // Flickering video: partial-loss windows with short stops between them.
  // The old rule restarted the 1.5 s run at every stop; with silence on a stop of a
  // window or more counts as distress.
  Rig r = silence_rig();
  r.in.distress = true;
  r.in.video_silent_ms = 0;
  r.at(0);
  r.in.distress = false;          // an empty window...
  r.in.video_silent_ms = 300;     // ...because nothing arrived for 300 ms
  CHECK(r.at(500).state == NarrowState::Off);
  r.in.distress = true;
  r.in.video_silent_ms = 0;
  CHECK(r.at(1499).state == NarrowState::Off);
  CHECK(r.at(1500).state == NarrowState::Entering);
  CHECK(r.nm.silent_entries() == 0);
  // A short gap (under a window) with no loss still restarts it.
  Rig r2 = silence_rig();
  r2.in.distress = true;
  r2.at(0);
  r2.in.distress = false;
  r2.in.video_silent_ms = 100;
  r2.at(500);
  r2.in.distress = true;
  r2.in.video_silent_ms = 0;
  CHECK(r2.at(1500).state == NarrowState::Off);   // the run restarts here
  CHECK(r2.at(2999).state == NarrowState::Off);
  CHECK(r2.at(3000).state == NarrowState::Entering);
}

TEST(silence_entering_outlives_the_session) {
  Rig r = silence_rig();
  r.in.video_silent_ms = 400;
  r.at(400);                      // Entering
  r.in.in_session = false;        // the GS falls to beaconing 1 s after the last video
  r.in.video_silent_ms = 1100;
  CHECK(r.at(1100).state == NarrowState::Entering);
  r.in.width[1] = 10;
  CHECK(r.at(1200).state == NarrowState::Entering);   // baseline at 10
  r.in.own[1] = 101;              // the drone, heard at 10
  CHECK(r.at(1300).state == NarrowState::On);
  // Off: the old rule cancels on the session loss.
  Rig r0;
  r0.in.distress = true;
  r0.at(0);
  r0.at(1500);
  r0.in.in_session = false;
  CHECK(r0.at(1600).state == NarrowState::Off && r0.nm.cancels() == 1);
}

TEST(silence_a_cancelled_stop_entry_retries_after_retry_ms) {
  Rig r = silence_rig();
  r.in.video_silent_ms = 400;
  r.at(400);
  r.in.width[1] = 10;
  r.at(500);
  CHECK(r.at(400 + 2001).state == NarrowState::Off);   // the drone never followed
  CHECK(r.nm.cancels() == 1);
  r.in.width[1] = 40;
  r.in.in_session = true;
  r.in.video_silent_ms = 5000;
  CHECK(r.at(2401 + 4999).state == NarrowState::Off);
  CHECK(r.at(2401 + 5000).state == NarrowState::Entering);
  CHECK(r.nm.silent_entries() == 2);
}

TEST(silence_a_cancelled_distress_entry_keeps_the_long_cooldown) {
  // The lead card left 20 MHz video behind for 2 s: not every 5 s.
  Rig r = silence_rig();
  r.in.distress = true;
  r.at(0);
  r.at(1500);                     // a distress entry
  r.in.width[1] = 10;
  r.at(1600);
  CHECK(r.at(1500 + 2001).state == NarrowState::Off);
  r.in.width[1] = 40;
  r.in.video_silent_ms = 1000;    // now a full stop
  CHECK(r.at(3501 + 5000).state == NarrowState::Off);
  CHECK(r.at(3501 + 19999).state == NarrowState::Off);
  CHECK(r.at(3501 + 20000).state == NarrowState::Entering);
}

TEST(silence_a_rescue_from_10_keeps_the_long_cooldown) {
  Rig r = silence_rig();
  r.in.video_silent_ms = 400;
  r.at(400);
  r.in.width[1] = 10;
  r.at(500);
  r.in.own[1] = 101;
  r.at(600);                      // On
  REQUIRE(r.nm.state() == NarrowState::On);
  CHECK(r.at(600 + 1501).state == NarrowState::Off);   // nothing at 10 for 1.5 s
  CHECK(r.nm.rescues() == 1);
  r.in.width = {40, 40};
  r.in.video_silent_ms = 3000;
  CHECK(r.at(2101 + 5000).state == NarrowState::Off);
  CHECK(r.at(2101 + 20000).state == NarrowState::Entering);
}

// --- VideoSilence: no stop before the session's first video (gs24).

TEST(video_silence_waits_for_the_sessions_first_video) {
  VideoSilence v;
  // The drone links; its camera starts ~10 s later. No silence meanwhile.
  CHECK(v.tick(0, true, 0) < 0);
  CHECK(v.tick(5000, true, 0) < 0);
  CHECK(v.tick(10500, true, 0) < 0);
  // First video: silence counts from each arrival from now on.
  CHECK(v.tick(10600, true, 40) == 0.0);
  CHECK(v.tick(10900, true, 40) == 300.0);
  CHECK(v.tick(11000, true, 41) == 0.0);
  CHECK(v.tick(11450, true, 41) == 450.0);   // a real stop: the rule may enter
  // The session ends and a new one forms: unseen again until its video.
  CHECK(v.tick(12500, false, 41) == 1500.0);
  CHECK(v.tick(20000, true, 41) < 0);
  CHECK(v.tick(25000, true, 41) < 0);
  CHECK(v.tick(25100, true, 60) == 0.0 && v.seen());
}

TEST(video_silence_ignores_arrivals_outside_a_session) {
  VideoSilence v;
  v.tick(0, false, 0);
  CHECK(v.tick(100, false, 10) < 0);   // searching: not this session's video
  CHECK(v.tick(200, true, 12) < 0);    // the session forms; its video not yet counted
  CHECK(v.tick(300, true, 15) == 0.0);
}

// The power-up stall, end to end: the link waits for the camera with no
// video. With the silence fed from VideoSilence nothing enters.
TEST(silence_no_entry_while_the_camera_starts) {
  Rig r = silence_rig();
  VideoSilence v;
  for (double t = 0; t <= 12000; t += 50) {
    r.in.video_silent_ms = v.tick(t, true, 0);
    CHECK(r.at(t).state == NarrowState::Off);
  }
  CHECK(r.nm.entries() == 0);
}

namespace {
// A video-stop entry made at `entry_snr` dB, taken to On.
void silent_to_on(Rig& r, double entry_snr) {
  r.in.best_snr_db = entry_snr;
  r.in.video_silent_ms = 400;
  r.at(400);
  r.in.width[1] = 10;
  r.at(500);
  r.in.own[1] += 1;
  r.at(600);
  r.in.width[0] = 10;
  r.in.video_silent_ms = 0;
}
}  // namespace

TEST(silent_entry_climbs_back_on_an_absolute_snr) {
  // Entered at 38 dB (drone beside the GS). The entry-relative bar would
  // need 44 dB at 10 MHz; the video-stop bar is silent_promote_snr_db.
  Rig r = silence_rig();
  silent_to_on(r, 38.0);
  REQUIRE(r.nm.state() == NarrowState::On);
  r.in.clean = true;
  r.in.best_snr_db = 40.0;   // +2 dB at 10 MHz: short of 44
  NarrowOut o;
  for (double t = 700; t <= 8700; t += 100) {
    r.in.own[0] += 1;
    o = r.at(t);
    if (o.state != NarrowState::On) break;
  }
  CHECK(o.state == NarrowState::Leaving);
  CHECK(r.nm.silent_entries() == 1);
}

TEST(silent_entry_stays_at_10_on_a_weak_link) {
  Rig r = silence_rig();
  silent_to_on(r, 6.0);
  r.in.clean = true;
  r.in.best_snr_db = 12.0;   // under 15 dB: 20 MHz would not hold
  for (double t = 700; t <= 30000; t += 100) {
    r.in.own[0] += 1;
    CHECK(r.at(t).state == NarrowState::On);
  }
  r.in.best_snr_db = 15.5;
  r.in.own[0] += 1;
  CHECK(r.at(30100).state == NarrowState::Leaving);
}

TEST(distress_entry_keeps_the_entry_relative_bar) {
  // Unchanged for a loss-driven entry: entry + 3 + promote_margin_db.
  Rig r = silence_rig();
  to_on(r);                  // distress entry at 6 dB
  r.in.clean = true;
  r.in.best_snr_db = 11.0;   // under 6 + 3 + 3
  for (double t = 1800; t < 12000; t += 100) {
    r.in.own[0] += 1;
    CHECK(r.at(t).state == NarrowState::On);
  }
  r.in.best_snr_db = 12.5;
  r.in.own[0] += 1;
  CHECK(r.at(12000).state == NarrowState::Leaving);
}
