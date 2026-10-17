#include "mtest.h"
#include "width_policy.h"
using namespace maburgs;

// radio.width = 80: cards live at 40 (the block's lower 40) and widen only
// for the 80 MHz rungs. 20 and 40 are their own home.
TEST(home_width_is_40_at_80_and_the_width_itself_below) {
  CHECK(home_width(80) == 40);
  CHECK(home_width(40) == 40);
  CHECK(home_width(20) == 20);
}

TEST(need_is_all_on_an_80_op_probe_on_an_80_probe_none_otherwise) {
  CHECK(wide_need(true, 80, 0) == WideNeed::All);
  CHECK(wide_need(true, 80, 80) == WideNeed::All);
  CHECK(wide_need(true, 40, 80) == WideNeed::Probe);   // the 40 -> 80 promote probe
  CHECK(wide_need(true, 40, 40) == WideNeed::None);
  CHECK(wide_need(true, 20, 0) == WideNeed::None);
  CHECK(wide_need(false, 80, 80) == WideNeed::None);    // no session: nothing to receive
}

// A need outlives its last sighting by hold_ms, so a probe pausing between
// attempts or a promote/demote bounce does not retune a card each time.
TEST(held_need_lingers_for_the_hold_then_steps_down) {
  WideHold h;
  CHECK(held_need(h, WideNeed::None, 0, 3000) == WideNeed::None);
  CHECK(held_need(h, WideNeed::All, 1000, 3000) == WideNeed::All);
  CHECK(held_need(h, WideNeed::None, 3999, 3000) == WideNeed::All);
  // All counts as a probe sighting too: after All lapses, Probe holds no longer.
  CHECK(held_need(h, WideNeed::None, 4000, 3000) == WideNeed::None);
  CHECK(held_need(h, WideNeed::Probe, 5000, 3000) == WideNeed::Probe);
  CHECK(held_need(h, WideNeed::None, 7999, 3000) == WideNeed::Probe);
  CHECK(held_need(h, WideNeed::None, 8000, 3000) == WideNeed::None);
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

TEST(targets_widen_every_capable_card_for_all_and_one_for_a_probe) {
  const std::vector<bool> cw{true, true, false};   // card 2: a relay, 11n only
  auto t = width_targets(WideNeed::All, cw, -1, 40, 80);
  CHECK(t[0] == 80 && t[1] == 80 && t[2] == 40);
  t = width_targets(WideNeed::Probe, cw, 1, 40, 80);
  CHECK(t[0] == 40 && t[1] == 80 && t[2] == 40);
  t = width_targets(WideNeed::None, cw, 1, 40, 80);
  CHECK(t[0] == 40 && t[1] == 40 && t[2] == 40);
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
