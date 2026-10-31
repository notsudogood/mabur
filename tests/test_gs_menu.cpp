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
  REQUIRE(v.tabs.size() == 3 && v.cells.size() == 4 && v.buttons.size() == 3);   // Apply, Scan, Close
  CHECK(v.buttons[1].label == "Scan" && v.buttons[2].label == "Close");
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
  REQUIRE(v.cells.size() == 3);
  CHECK(v.cells[0].label == "Bandwidth" && v.cells[0].value == "< Wide >" && v.title == "Settings");
  CHECK(v.cells[1].label == "Calibrate" && v.cells[2].label == "Remove cal");
  m.key(K::Down, 70);
  m.key(K::Center, 71);                 // wide -> normal
  CHECK(!m.pending().wide && m.view().cells[0].value == "< Normal >");
  m.key(K::Down, 80);                   // Apply
  v = m.view();
  CHECK(v.buttons[0].focus && !v.buttons[0].dim && v.help.find("restart") != std::string::npos);
  CHECK(m.key(K::Center, 90) == GsMenuModel::Action::Apply);
  CHECK(!m.open() && !m.current().hop && m.current().channel == 116 && !m.current().wide);
  CHECK(m.take_settings_applied() && !m.take_settings_applied());   // consumed once
  CHECK(m.take_camera_applied() == mabur::cam::kNoWish);            // no camera asked
}

TEST(menu_model_closes_on_close_and_idle_and_wraps) {
  GsMenuModel m({36}, false, maburplay::GsMenuSettings{true, 36, false});
  m.key(K::Center, 0);
  m.key(K::Up, 1);                       // wraps from the tabs to the buttons
  CHECK(m.view().buttons[0].focus);
  m.key(K::Left, 2);                     // wraps to Close (Apply, Scan, Close on this tab)
  CHECK(m.view().buttons[2].focus);
  CHECK(m.key(K::Center, 3) == GsMenuModel::Action::None && !m.open());
  m.key(K::Center, 4);
  m.key(K::Right, 5);                    // Settings tab
  m.key(K::Down, 6);
  m.key(K::Center, 7);                   // Bandwidth fixed at 20/40 configs
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
// Camera tab: the running preset is grey (active), a chosen one carries the
// dot (marked), presets this drone has no mode for are dim and cannot be
// chosen. Apply hands the preset over once and leaves the link settings
// alone.
TEST(menu_model_camera_tab_requests_a_preset) {
  GsMenuModel m({36, 100}, false, maburplay::GsMenuSettings{false, 36, false});
  m.set_camera(0, 0b0011, 0b0100);      // runs 1080p 60; 90 now; 720p 100 after a driver switch
  m.key(K::Center, 0);
  m.key(K::Left, 1);                    // wraps Channel -> Camera
  CHECK(m.tab() == GsMenuModel::Tab::Camera);
  auto v = m.view();
  REQUIRE(v.tabs.size() == 3 && v.tabs[2].label == "Camera" && v.tabs[2].active);
  REQUIRE(v.cells.size() == mabur::cam::kNumPresets && v.cols == 5 && v.title == "Camera");
  CHECK(v.cells[0].label == "1080p 60" && v.cells[0].active && v.cells[0].value == "full view");
  CHECK(!v.cells[1].dim && v.cells[2].value == "new driver" && !v.cells[2].dim);
  CHECK(v.cells[3].dim && v.cells[3].value == "not here");
  CHECK(v.cells[4].label == "Crop 60" && v.cells[4].dim && v.cells[4].value == "not here");
  CHECK(v.buttons[0].label == "No change");
  m.key(K::Down, 2);                    // the preset row, on the running preset
  CHECK(m.view().cells[0].focus);
  CHECK(m.view().help.find("runs this now") != std::string::npos);
  m.key(K::Center, 3);                  // the running one: nothing to ask
  CHECK(m.pending_camera() == mabur::cam::kNoWish);
  m.key(K::Left, 4);                    // wraps to Crop 60: not on this drone
  CHECK(m.view().help.find("Not on this drone") != std::string::npos);
  CHECK(m.view().help.find("unbinned") != std::string::npos);   // its own description
  m.key(K::Center, 5);
  CHECK(m.pending_camera() == mabur::cam::kNoWish);
  m.key(K::Left, 5);                    // 544p 120: not on this drone either
  CHECK(m.view().help.find("Not on this drone") != std::string::npos);
  m.key(K::Left, 6);                    // 720p 100: needs the driver switch
  CHECK(m.view().help.find("driver") != std::string::npos);
  m.key(K::Center, 7);
  CHECK(m.pending_camera() == 2 && m.view().cells[2].marked);
  m.key(K::Center, 8);                  // pressed again: cleared
  CHECK(m.pending_camera() == mabur::cam::kNoWish);
  m.key(K::Left, 9);                    // 1080p 90
  m.key(K::Center, 10);
  CHECK(m.pending_camera() == 1);
  m.key(K::Down, 11);                   // Apply
  v = m.view();
  CHECK(v.buttons[0].label == "Apply" && v.help.find("30 s") != std::string::npos);
  CHECK(m.key(K::Center, 12) == GsMenuModel::Action::Apply);
  CHECK(!m.take_settings_applied());
  CHECK(m.take_camera_applied() == 1 && m.take_camera_applied() == mabur::cam::kNoWish);
  m.key(K::Center, 20);                 // reopened: no choice carried over
  CHECK(m.pending_camera() == mabur::cam::kNoWish);
}

TEST(menu_model_camera_tab_before_the_drone_reports) {
  GsMenuModel m({36}, false, maburplay::GsMenuSettings{false, 36, false});
  m.key(K::Center, 0);
  m.key(K::Left, 1);
  m.key(K::Down, 2);
  auto v = m.view();
  CHECK(v.cells[0].dim && v.cells[1].dim && !v.cells[0].active);
  CHECK(v.help.find("not reported") != std::string::npos);
  m.key(K::Center, 3);
  CHECK(m.pending_camera() == mabur::cam::kNoWish);
  const uint64_t ver = m.version();
  m.set_camera(mabur::cam::kUnknown, 0, 0);   // 0xFF from an older drone: still nothing
  m.set_camera(mabur::cam::kUnknown, 0, 0);
  CHECK(m.version() == ver + 1);              // a repeat does not repaint
}

TEST(menu_model_camera_result_lines) {
  CHECK(GsMenuModel::camera_result_text("").empty());
  CHECK(GsMenuModel::camera_result_text("sent").empty());
  for (const char* r : {"applying", "switched", "armed", "unavailable", "bad_config",
                        "rolled_back", "timeout"})
    CHECK(!GsMenuModel::camera_result_text(r).empty());
  CHECK(GsMenuModel::camera_result_text("armed").find("disarm") != std::string::npos);
}
// Settings tab: Calibrate and Remove cal are staged like any choice, one at
// a time; Remove needs a drone that says it is calibrated; Apply hands the
// action over once and leaves the link settings alone.
TEST(menu_model_settings_calibrate_and_remove) {
  using CA = GsMenuModel::CalAction;
  GsMenuModel m({36}, true, maburplay::GsMenuSettings{true, 36, true});
  m.set_calibration(std::nullopt, false, -1);
  m.key(K::Center, 0);
  m.key(K::Right, 1);                   // Settings
  m.key(K::Down, 2);                    // the cell row, on Bandwidth
  m.key(K::Right, 3);                   // Calibrate
  auto v = m.view();
  CHECK(v.cells[1].focus && v.cells[1].value == "measure TX");
  CHECK(v.cells[2].dim && v.cells[2].value == "no link");
  CHECK(v.help.find("2 min") != std::string::npos);
  m.key(K::Center, 4);
  CHECK(m.pending_cal() == CA::Start && m.view().cells[1].marked);
  m.key(K::Right, 5);                   // Remove cal: drone has not reported
  m.key(K::Center, 6);
  CHECK(m.pending_cal() == CA::Start);  // unchanged
  m.set_calibration(true, false, -1);
  CHECK(m.view().cells[2].value == "in use" && !m.view().cells[2].dim);
  m.key(K::Center, 7);                  // now it takes, replacing Calibrate
  CHECK(m.pending_cal() == CA::Remove && !m.view().cells[1].marked);
  m.key(K::Center, 8);                  // pressed again: cleared
  CHECK(m.pending_cal() == CA::None);
  m.key(K::Center, 9);
  m.key(K::Down, 10);                   // Apply
  CHECK(m.view().help.find("remove the calibration") != std::string::npos);
  CHECK(m.key(K::Center, 11) == GsMenuModel::Action::Apply);
  CHECK(!m.take_settings_applied());
  CHECK(m.take_cal_applied() == CA::Remove && m.take_cal_applied() == CA::None);
  // A run in progress: Calibrate shows it and cannot be staged again.
  m.set_calibration(true, true, 40);
  m.key(K::Center, 20);
  m.key(K::Right, 21);
  m.key(K::Down, 22);
  m.key(K::Right, 23);
  v = m.view();
  CHECK(v.cells[1].value == "running 40 %" && v.cells[1].dim && v.cells[2].dim);
  m.key(K::Center, 24);
  CHECK(m.pending_cal() == CA::None);
}

TEST(menu_model_calibration_event_lines) {
  using S = GsMenuModel::CalSeen;
  S was, now;
  CHECK(GsMenuModel::calibration_event_text(was, now).empty());
  now.start = "err not linked"; now.start_n = 1;
  CHECK(GsMenuModel::calibration_event_text(was, now) == "Calibration not started: not linked");
  was = now; now.start_n = 2;            // the same refusal again still shows
  CHECK(!GsMenuModel::calibration_event_text(was, now).empty());
  was = S{}; now = S{}; now.refused_armed = true;
  CHECK(GsMenuModel::calibration_event_text(was, now).find("armed") != std::string::npos);
  was = S{}; now = S{}; now.state = "await_ack";
  CHECK(GsMenuModel::calibration_event_text(was, now).find("2 min") != std::string::npos);
  was = now; now.state = "sweep"; now.pct = 5;
  CHECK(GsMenuModel::calibration_event_text(was, now) == "Calibrating: 5 %");
  was = now; now.pct = 8;                // same 10 % step: quiet
  CHECK(GsMenuModel::calibration_event_text(was, now).empty());
  was = now; now.pct = 12;
  CHECK(GsMenuModel::calibration_event_text(was, now) == "Calibrating: 12 %");
  was = now; now.state = "failed"; now.fail = "calibration ack timeout";
  CHECK(GsMenuModel::calibration_event_text(was, now) ==
        "Calibration failed: calibration ack timeout");
  was = S{}; now = S{}; now.off = "removed";
  CHECK(GsMenuModel::calibration_event_text(was, now) == "Calibration removed: stock TX power");
  was = S{}; was.calibrated = false; now = was; now.calibrated = true;
  CHECK(GsMenuModel::calibration_event_text(was, now) == "Calibration in use");
  was = S{}; now = S{}; now.calibrated = true;   // first report at link-up: quiet
  CHECK(GsMenuModel::calibration_event_text(was, now).empty());
}
// gs18 first-boot floor calibration: the banner follows maburgs' floorcal
// state, stays up (no 4 s expiry) and a status line shows over it.
TEST(menu_model_floorcal_banner_and_event) {
  using F = GsMenuModel::FloorCalSeen;
  F f;
  f.state = "test";
  f.width = 40;
  f.clean_s = 6;
  f.need_s = 10;
  CHECK(GsMenuModel::floorcal_banner(f) ==
        "First boot calibration: testing 40 MHz (6/10 s clean) - keep the drone disarmed");
  f.state = "unstick";
  f.floor = 0x24;
  CHECK(GsMenuModel::floorcal_banner(f) ==
        "First boot calibration: stall caught, floor now 0x24 - testing 40 MHz again");
  f.state = "paused";
  f.why = "armed";
  CHECK(GsMenuModel::floorcal_banner(f) == "First boot calibration paused: the drone is armed");
  F never = f;
  never.width = 0;                       // paused before it ever ran: nothing to say
  CHECK(GsMenuModel::floorcal_banner(never).empty());
  f.state = "done";
  CHECK(GsMenuModel::floorcal_banner(f) == "First boot calibration finished: floor 0x24");
  f.floor = 0;
  CHECK(GsMenuModel::floorcal_banner(f) == "First boot calibration finished: no floor needed");
  f.state = "idle";
  CHECK(GsMenuModel::floorcal_banner(f).empty());
  F was, now;
  was.state = "test";
  was.width = 80;
  now.state = "failed";
  now.why = "timeout";
  now.width = 80;
  CHECK(GsMenuModel::floorcal_event(was, now) ==
        "First boot calibration stopped: no clean 80 MHz in a minute (retries after a GS restart)");
  CHECK(GsMenuModel::floorcal_event(now, now).empty());
  now.why = "top";
  now.floor = 0x35;
  CHECK(GsMenuModel::floorcal_event(was, now) ==
        "First boot calibration stopped: the drone still stalls at the highest floor (0x35)");

  GsMenuModel m({36}, false, maburplay::GsMenuSettings{true, 36, false});
  CHECK(!m.visible());
  m.set_banner("First boot calibration: testing 40 MHz");
  CHECK(m.visible() && m.view().status == "First boot calibration: testing 40 MHz");
  m.tick(60000);                          // no expiry
  CHECK(m.view().status == "First boot calibration: testing 40 MHz");
  m.set_status("Camera: switched", 60000);
  CHECK(m.view().status == "Camera: switched");
  m.tick(64001);
  CHECK(m.view().status == "First boot calibration: testing 40 MHz");
  m.set_banner("");
  CHECK(!m.visible());
}
// gs18 Channel tab Scan: the button asks once and the menu stays open; while
// a scan runs it reads "Scanning" and does nothing; the ratings become bars
// on the cells (the link's own channel unrated) and the help names them.
// Revert (no Scan): the second button is Close and the press closes the menu.
TEST(menu_model_channel_scan_button_and_bars) {
  GsMenuModel m({36, 100}, true, maburplay::GsMenuSettings{true, 36, true});
  m.key(K::Center, 0);
  m.key(K::Up, 1);                       // the buttons row, on Apply
  m.key(K::Right, 2);                    // Scan
  auto v = m.view(36);
  CHECK(v.buttons[1].focus && v.help.find("Scan:") == 0);
  m.key(K::Center, 3);
  CHECK(m.open() && m.take_scan_request() && !m.take_scan_request());
  m.set_channel_scan("running", {}, -1);
  v = m.view(36);
  CHECK(v.buttons[1].label == "Scanning" && v.buttons[1].dim);
  m.key(K::Center, 4);
  CHECK(!m.take_scan_request());         // nothing while one runs
  m.set_channel_scan("done", {{36, -1, -1}, {100, 2, 31.4}}, 5);
  v = m.view(36);
  CHECK(v.cells[0].bar == -1 && v.cells[1].bar == 2);
  m.key(K::Up, 5);                       // the channel row
  m.key(K::Right, 6);                    // CH 100
  CHECK(m.view(36).help.find("Last scan: busy (31 % busy)") != std::string::npos);
  CHECK(GsMenuModel::scan_result_text("armed") == "Scan refused: the drone is armed - disarm first");
  CHECK(GsMenuModel::scan_result_text("started").find("about 5 s") != std::string::npos);
}
MTEST_MAIN
