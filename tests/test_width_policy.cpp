#include "mtest.h"
#include "width_policy.h"
using namespace maburgs;

// radio.width = 80: cards live at 40 (the primary's own pair) and widen
// only for the 80 MHz rungs. 20 and 40 are their own home.
TEST(home_width_is_40_at_80_and_the_width_itself_below) {
  CHECK(home_width(80) == 40);
  CHECK(home_width(40) == 40);
  CHECK(home_width(20) == 20);
}

// radio.width 80, narrow_20 off: floor = home = 40. Every card at the
// flown rung's width, the probe card at the probed rung's; home outside a
// session.
TEST(need_without_narrow_20_matches_the_80_mhz_policy) {
  CHECK((width_need(true, 80, 0, 40, 40, 80) == WidthNeed{80, 80}));
  CHECK((width_need(true, 80, 80, 40, 40, 80) == WidthNeed{80, 80}));
  CHECK((width_need(true, 40, 80, 40, 40, 80) == WidthNeed{40, 80}));   // the 40 -> 80 promote probe
  CHECK((width_need(true, 40, 40, 40, 40, 80) == WidthNeed{40, 40}));
  CHECK((width_need(true, 20, 0, 40, 40, 80) == WidthNeed{40, 40}));    // 20 rungs ride the 40 home
  CHECK((width_need(false, 80, 80, 40, 40, 80) == WidthNeed{40, 40}));  // no session: home
}

// narrow_20: floor 20, so a 20 MHz rung puts every card at 20 and the
// 20 -> 40 probe widens one card.
TEST(need_with_narrow_20_drops_to_20_and_probes_40_on_one_card) {
  CHECK((width_need(true, 20, 0, 40, 20, 80) == WidthNeed{20, 20}));
  CHECK((width_need(true, 20, 40, 40, 20, 80) == WidthNeed{20, 40}));
  CHECK((width_need(true, 40, 80, 40, 20, 80) == WidthNeed{40, 80}));
  CHECK((width_need(false, 20, 0, 40, 20, 80) == WidthNeed{40, 40}));   // no session: home, not 20
  // radio.width 40: nothing ever reaches 80.
  CHECK((width_need(true, 80, 80, 40, 20, 40) == WidthNeed{40, 40}));
}

// A width outlives its last sighting by hold_ms; widening is immediate.
TEST(held_need_widens_at_once_and_narrows_after_the_hold) {
  WidthHold h;
  CHECK((held_width_need(h, WidthNeed{20, 20}, 0, 3000) == WidthNeed{20, 20}));
  CHECK((held_width_need(h, WidthNeed{80, 80}, 1000, 3000) == WidthNeed{80, 80}));
  CHECK((held_width_need(h, WidthNeed{20, 20}, 3999, 3000) == WidthNeed{80, 80}));
  CHECK((held_width_need(h, WidthNeed{20, 20}, 4000, 3000) == WidthNeed{20, 20}));
  // A probe's width holds on the probe card only.
  CHECK((held_width_need(h, WidthNeed{20, 40}, 5000, 3000) == WidthNeed{20, 40}));
  CHECK((held_width_need(h, WidthNeed{20, 20}, 7999, 3000) == WidthNeed{20, 40}));
  CHECK((held_width_need(h, WidthNeed{20, 20}, 8000, 3000) == WidthNeed{20, 20}));
  // The all-card width never sits below the probe card's floor.
  CHECK((held_width_need(h, WidthNeed{40, 40}, 9000, 3000) == WidthNeed{40, 40}));
}

TEST(probe_card_prefers_a_non_tx_card_and_sticks) {
  const std::vector<bool> both{true, true};
  CHECK(pick_probe_card(both, /*tx=*/0, /*current=*/-1) == 1);
  CHECK(pick_probe_card(both, 1, -1) == 0);
  // Sticky: a TX-selector flip does not swap which card is wide.
  CHECK(pick_probe_card(both, 1, 1) == 1);
  // One capable card is used even if it is the TX card.
  CHECK(pick_probe_card({true, false}, 0, -1) == 0);
  // The current card lost the ability (closed): pick again.
  CHECK(pick_probe_card({false, true}, 1, 0) == 1);
  CHECK(pick_probe_card({false, false}, 0, -1) == -1);
}

TEST(targets_put_every_switchable_card_at_all_and_the_probe_card_at_probe) {
  const std::vector<bool> cw{true, true, false};   // card 2: a relay, 11n only
  auto t = width_targets(WidthNeed{80, 80}, cw, -1, 40);
  CHECK(t[0] == 80 && t[1] == 80 && t[2] == 40);
  t = width_targets(WidthNeed{40, 80}, cw, 1, 40);
  CHECK(t[0] == 40 && t[1] == 80 && t[2] == 40);
  t = width_targets(WidthNeed{20, 40}, cw, 0, 40);
  CHECK(t[0] == 40 && t[1] == 20 && t[2] == 40);
  t = width_targets(WidthNeed{20, 20}, cw, -1, 40);
  CHECK(t[0] == 20 && t[1] == 20 && t[2] == 40);
}

// One switch at a time, the non-TX card first, so the TX card keeps the
// uplink through the other's retune.
TEST(next_switch_is_off_target_eligible_and_non_tx_first) {
  const std::vector<uint8_t> cur{40, 40}, want{80, 80};
  CHECK(next_width_switch(cur, want, {true, true}, /*tx=*/0) == 1);
  CHECK(next_width_switch(cur, want, {true, true}, 1) == 0);
  CHECK(next_width_switch({40, 80}, want, {true, true}, 0) == 0);   // only the TX card left
  CHECK(next_width_switch(cur, want, {true, false}, 0) == 0);       // card 1 busy in a dwell
  CHECK(next_width_switch(want, want, {true, true}, 0) == -1);      // nothing to do
}
MTEST_MAIN
