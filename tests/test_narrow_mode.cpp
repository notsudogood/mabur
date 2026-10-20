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
