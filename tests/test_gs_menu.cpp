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
  GsMenuModel m({36, 132}, true, maburplay::GsMenuSettings{true, 36, true});
  CHECK(!m.visible());
  CHECK(m.key(K::Up, 0) == GsMenuModel::Action::None && !m.open());   // closed: only Centre opens
  m.key(K::Center, 10);
  CHECK(m.open() && m.selected() == 0 && m.lines().size() == 6);
  m.key(K::Right, 20);                  // hop off
  m.key(K::Down, 30);
  m.key(K::Right, 40);                  // channel 36 -> 132
  m.key(K::Down, 50);
  m.key(K::Left, 60);                   // wide -> normal
  CHECK(!m.pending().hop && m.pending().channel == 132 && !m.pending().wide);
  CHECK(m.current().hop);               // nothing applied yet
  m.key(K::Down, 70);                   // Apply
  CHECK(m.lines()[4].find("restarts") != std::string::npos);
  CHECK(m.key(K::Center, 80) == GsMenuModel::Action::Apply);
  CHECK(!m.open() && !m.current().hop && m.current().channel == 132 && !m.current().wide);
}

TEST(menu_model_closes_on_close_and_idle_and_wraps) {
  GsMenuModel m({36}, false, maburplay::GsMenuSettings{true, 36, false});
  m.key(K::Center, 0);
  m.key(K::Up, 1);                       // wraps to Close
  CHECK(m.selected() == 4);
  m.key(K::Center, 2);
  CHECK(!m.open());
  m.key(K::Center, 3);
  m.key(K::Down, 4);
  m.key(K::Down, 5);
  m.key(K::Right, 6);                    // Bandwidth fixed at 20/40 configs
  CHECK(!m.pending().wide);
  const uint64_t v = m.version();
  m.tick(6 + GsMenuModel::kIdleMs);
  CHECK(!m.open() && m.version() != v);
  m.set_status("Applied", 100000);
  CHECK(m.visible() && m.lines().size() == 1);
  m.tick(100000 + GsMenuModel::kStatusMs);
  CHECK(!m.visible());
}
MTEST_MAIN
