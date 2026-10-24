#include "mtest.h"

#include <cstdio>
#include <string>

#include "gs_menu.h"
#include "gs_menu_model.h"

using maburgs::GsMenuChoice;

TEST(menu_file_round_trips_and_skips_junk) {
  GsMenuChoice m;
  m.hop = false;
  m.channel = 140;
  m.wide = false;
  const GsMenuChoice r = maburgs::parse_gs_menu(maburgs::format_gs_menu(m));
  CHECK(r.hop && !*r.hop && r.channel && *r.channel == 140 && r.wide && !*r.wide);
  const GsMenuChoice j = maburgs::parse_gs_menu("hop = on\nchannel=999\nbandwidth=ultra\nfoo=1\nbad line\n");
  CHECK(j.hop && *j.hop);
  CHECK(!j.channel && !j.wide);
  const std::string path = "/tmp/mabur_test_gs_menu";
  CHECK(maburgs::write_gs_menu_file(path, m));
  const auto back = maburgs::read_gs_menu_file(path);
  CHECK(back && back->channel && *back->channel == 140);
  std::remove(path.c_str());
  CHECK(!maburgs::read_gs_menu_file(path));
}

TEST(apply_pins_unpins_and_drops_80_mhz_rungs) {
  maburgs::Config c;
  c.radio.channels = {36, 132};
  c.radio.width = 80;
  c.link.ladder_cfg.ladder = {{0, 1.0, 0.75, 20}, {4, 0.5, 0.25, 40}, {3, 0.5, 0.25, 80}};
  GsMenuChoice m;
  m.hop = false;
  m.channel = 132;
  m.wide = false;
  std::string err;
  maburgs::Config a = c;
  const auto lines = maburgs::apply_gs_menu(a, m, &err);
  CHECK(err.empty());
  CHECK(lines.size() == 2);
  CHECK(a.radio.pin && *a.radio.pin == 132);
  CHECK(a.radio.width == 40 && a.link.ladder_cfg.ladder.size() == 2);
  // Hop on clears a pin from the TOML.
  maburgs::Config b = c;
  b.radio.pin = 36;
  GsMenuChoice on;
  on.hop = true;
  maburgs::apply_gs_menu(b, on, &err);
  CHECK(!b.radio.pin);
  // A pin outside the set leaves the config untouched.
  maburgs::Config d = c;
  GsMenuChoice bad;
  bad.hop = false;
  bad.channel = 44;
  err.clear();
  CHECK(maburgs::apply_gs_menu(d, bad, &err).empty());
  CHECK(!err.empty() && !d.radio.pin && d.radio.width == 80);
}

using maburplay::GsMenuModel;
using K = GsMenuModel::Key;

TEST(menu_model_opens_edits_and_applies) {
  GsMenuModel m({36, 100, 116, 132}, true, maburplay::GsMenuSettings{true, 36, true});
  CHECK(!m.visible());
  CHECK(m.key(K::Up, 0) == GsMenuModel::Action::None && !m.open());   // closed: only Centre opens
  m.key(K::Center, 10);                 // opens on the tab bar, Channel tab
  CHECK(m.open() && m.tab() == GsMenuModel::Tab::Channel);
  auto v = m.view(132);
  REQUIRE(v.tabs.size() == 2 && v.cells.size() == 4 && v.buttons.size() == 2);
  CHECK(v.tabs[0].focus && v.tabs[0].active && !v.tabs[1].active);
  CHECK(v.has_toggle && v.toggle_on && v.cells[3].active && !v.cells[0].active);   // 132 live
  CHECK(v.cells[0].label == "CH 36" && v.cells[0].value == "5170-5250");   // the 80 MHz block
  CHECK(v.cells[3].value == "5650-5730");
  CHECK(v.buttons[0].dim && v.buttons[0].label == "No change");
  m.key(K::Center, 20);                 // into the tab: the Hop switch
  CHECK(m.view().toggle.focus);
  m.key(K::Down, 30);                   // the channel row, on the chosen channel (36)
  CHECK(m.view().cells[0].focus);
  m.key(K::Right, 40);
  m.key(K::Right, 41);                  // 116
  CHECK(m.view().cells[2].focus);
  m.key(K::Center, 50);                 // fly on 116: Hop off, 116 marked
  CHECK(!m.pending().hop && m.pending().channel == 116);
  v = m.view();
  CHECK(!v.toggle_on && v.cells[2].marked && !v.cells[0].marked);
  CHECK(m.current().hop);               // nothing applied yet
  m.key(K::Up, 60);
  m.key(K::Up, 61);                     // the tab bar
  m.key(K::Right, 62);                  // Settings
  CHECK(m.tab() == GsMenuModel::Tab::Settings);
  v = m.view();
  REQUIRE(v.cells.size() == 1);
  CHECK(v.cells[0].label == "Bandwidth" && v.cells[0].value == "< Wide >" && v.title == "Settings");
  m.key(K::Down, 70);
  m.key(K::Left, 71);                   // wide -> normal
  CHECK(!m.pending().wide && m.view().cells[0].value == "< Normal >");
  m.key(K::Down, 80);                   // Apply
  v = m.view();
  CHECK(v.buttons[0].focus && !v.buttons[0].dim && v.help.find("restart") != std::string::npos);
  CHECK(m.key(K::Center, 90) == GsMenuModel::Action::Apply);
  CHECK(!m.open() && !m.current().hop && m.current().channel == 116 && !m.current().wide);
}

TEST(menu_model_closes_on_close_and_idle_and_wraps) {
  GsMenuModel m({36}, false, maburplay::GsMenuSettings{true, 36, false});
  m.key(K::Center, 0);
  m.key(K::Up, 1);                       // wraps from the tabs to the buttons
  CHECK(m.view().buttons[0].focus);
  m.key(K::Right, 2);                    // Close
  CHECK(m.view().buttons[1].focus);
  CHECK(m.key(K::Center, 3) == GsMenuModel::Action::None && !m.open());
  m.key(K::Center, 4);
  m.key(K::Right, 5);                    // Settings tab
  m.key(K::Down, 6);
  m.key(K::Right, 7);                    // Bandwidth fixed at 20/40 configs
  CHECK(!m.pending().wide && m.view().cells[0].dim);
  m.key(K::Down, 8);
  CHECK(m.key(K::Center, 9) == GsMenuModel::Action::None && !m.open());   // Apply, nothing to apply
  m.key(K::Center, 10);
  const uint64_t v = m.version();
  m.tick(10 + GsMenuModel::kIdleMs);
  CHECK(!m.open() && m.version() != v);
  m.set_status("Applied", 100000);
  CHECK(m.visible() && !m.view().open && m.view().status == "Applied");
  m.tick(100000 + GsMenuModel::kStatusMs);
  CHECK(!m.visible());
}

TEST(menu_model_eight_channels_take_two_rows) {
  GsMenuModel m({36, 44, 100, 108, 116, 124, 132, 140}, false,
                maburplay::GsMenuSettings{false, 124, false});
  m.key(K::Center, 0);
  m.key(K::Down, 1);                     // Hop
  m.key(K::Down, 2);                     // first channel row
  CHECK(m.view().cells[0].focus);
  m.key(K::Down, 3);                     // second row lands on the chosen 124
  auto v = m.view();
  CHECK(v.cells[5].focus && v.cells[5].marked && v.cols == 4);
  CHECK(v.cells[0].value == "5170-5210");   // Normal: the 40 MHz pair
  m.key(K::Down, 4);
  CHECK(m.view().buttons[0].focus);
}
MTEST_MAIN
