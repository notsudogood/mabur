#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <string>

#include "mtest.h"
#include "camera_ctl.h"
#include "camera_switch.h"
#include "config.h"
#include "mabur/camera_presets.h"

using namespace mabur;

namespace {

// The stock OpenIPC sensor_imx415_mipi.ko list (docs/findings/2026-10-19-100fps.md).
constexpr CameraMode kStock[] = {
    {3840, 2160, 30}, {2560, 1440, 60}, {1920, 1080, 90}, {1472, 816, 120}};

std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string write_tmp(const std::string& text) {
  char path[] = "/tmp/mabur_cam_XXXXXX";
  const int fd = mkstemp(path);
  REQUIRE(fd >= 0);
  FILE* f = fdopen(fd, "w");
  std::fputs(text.c_str(), f);
  std::fclose(f);
  return path;
}

}  // namespace

TEST(presets_are_the_five_the_menu_offers) {
  REQUIRE(cam::kNumPresets == 5);
  for (size_t i = 0; i < cam::kNumPresets; ++i) CHECK(cam::kPresets[i].id == i);
  CHECK(cam::preset_of(1920, 1080, 60) == 0);
  CHECK(cam::preset_of(1920, 1080, 90) == 1);
  CHECK(cam::preset_of(1280, 720, 100) == 2);
  CHECK(cam::preset_of(1152, 544, 120) == 3);
  // d18: same size and rate as preset 0, told apart by venc.sensor_crop.
  CHECK(cam::preset_of(1920, 1080, 60, true) == 4);
  CHECK(cam::kPresets[4].crop && !cam::kPresets[0].crop && cam::kPresets[4].needs_star6e);
  CHECK(cam::preset_of(1920, 1080, 90, true) == cam::kCustom);
  CHECK(cam::preset_of(1280, 720, 60) == cam::kCustom);
  CHECK(cam::preset(5) == nullptr);
}

TEST(every_preset_fits_the_sensor_mode_its_fps_selects) {
  // The loader refuses a venc.size that does not keep the mode's aspect or
  // is larger (camera_mode_size_ok); a preset must never be one.
  for (const cam::Preset& p : cam::kPresets) {
    const CameraMode m = camera_mode_pick(p.fps, 0, 0, p.crop);
    CHECK(camera_mode_size_ok(m, p.w, p.h));
  }
}

// Crop 60 needs waybeam's 2816x1584@60: the stock driver's 1440-class crop
// (2560x1440@60) is not it, so the stock driver does not offer it.
TEST(stock_driver_offers_the_1080p_presets_and_waybeams_all_five) {
  CHECK(camera_offered_mask(kStock, 4) == 0x03);
  CHECK(camera_offered_mask(kStar6eImx415Modes, kNumStar6eImx415Modes) == 0x1F);
  CHECK(camera_offered_mask(nullptr, 0) == 0);
}

TEST(a_request_is_decided_once_per_change) {
  CameraSwitch s;
  // Preset 1 offered now, drone on preset 0, no FC: apply, once.
  auto d = s.on_wish(1, 0, 0x03, 0x0C, ArmView::NoFc);
  CHECK(d.action == CamAction::Apply && !d.driver_switch);
  CHECK(s.on_wish(1, 0, 0x03, 0x0C, ArmView::NoFc).action == CamAction::None);  // repeat RCF
  // The GS drops the request; the same preset asked again is decided again.
  CHECK(s.on_wish(cam::kNoWish, 0, 0x03, 0x0C, ArmView::NoFc).action == CamAction::None);
  CHECK(s.on_wish(1, 0, 0x03, 0x0C, ArmView::Disarmed).action == CamAction::Apply);
}

TEST(a_preset_only_waybeams_driver_has_asks_for_the_driver_switch) {
  CameraSwitch s;
  auto d = s.on_wish(2, 0, 0x03, 0x0C, ArmView::Disarmed);
  CHECK(d.action == CamAction::Apply && d.driver_switch);
  // No driver to switch to (not in the image): refused, not attempted.
  CameraSwitch t;
  auto r = t.on_wish(3, 0, 0x03, 0x00, ArmView::Disarmed);
  CHECK(r.action == CamAction::Refuse && r.refused == cam::State::Unavailable);
}

TEST(never_while_armed_or_with_a_stale_arm_state) {
  CameraSwitch a;
  auto d = a.on_wish(1, 0, 0x0F, 0, ArmView::Armed);
  CHECK(d.action == CamAction::Refuse && d.refused == cam::State::NoArmState);
  CameraSwitch b;
  CHECK(b.on_wish(1, 0, 0x0F, 0, ArmView::Stale).refused == cam::State::NoArmState);
}

TEST(the_running_preset_and_an_unknown_id_do_nothing_harmful) {
  CameraSwitch s;
  CHECK(s.on_wish(0, 0, 0x0F, 0, ArmView::NoFc).action == CamAction::None);
  CameraSwitch t;
  auto d = t.on_wish(7, 0, 0x0F, 0, ArmView::NoFc);
  CHECK(d.action == CamAction::Refuse && d.refused == cam::State::Unavailable);
}

TEST(set_venc_mode_rewrites_in_place_and_keeps_comments) {
  const std::string in =
      "[radio]\nchannels = [36]\n\n[venc]\nsensor_bin = \"/x#y.bin\"  # tuning\n"
      "size       = \"1920x1080\"\nfps        = 60            # comment here\n"
      "gop_s = 0\n\n[record]\nfps = 30\nsize = \"1280x720\"\n";
  const std::string out = set_venc_mode_text(in, 1280, 720, 100);
  CHECK(out.find("size       = \"1280x720\"\n") != std::string::npos);
  CHECK(out.find("fps        = 100           # comment here\n") != std::string::npos);
  CHECK(out.find("sensor_bin = \"/x#y.bin\"  # tuning\n") != std::string::npos);
  // [record]'s own fps/size are not [venc]'s.
  CHECK(out.find("[record]\nfps = 30\nsize = \"1280x720\"\n") != std::string::npos);
}

TEST(set_venc_mode_adds_missing_keys_and_a_missing_section) {
  const std::string out = set_venc_mode_text("[venc]\ngop_s = 0\n", 1152, 544, 120);
  CHECK(out == "[venc]\nsize = \"1152x544\"\nfps = 120\ngop_s = 0\n");
  const std::string none = set_venc_mode_text("[radio]\nchannels = [36]\n", 1920, 1080, 90);
  CHECK(none == "[radio]\nchannels = [36]\n\n[venc]\nsize = \"1920x1080\"\nfps = 90\n");
}

TEST(every_preset_written_into_the_bundle_still_loads) {
  // The drone validates with load_config before it writes; the shipped file
  // with each preset applied must pass (aspect, low_power.fps <= fps, ...).
  const std::string bundle = slurp(std::string(MABUR_BUNDLE_DIR) + "/mabur.default.toml");
  REQUIRE(!bundle.empty());
  const Config base = load_config(std::string(MABUR_BUNDLE_DIR) + "/mabur.default.toml");
  for (const cam::Preset& p : cam::kPresets) {
    const std::string path = write_tmp(preset_config_text(bundle, base, p));
    bool ok = true;
    try {
      const Config c = load_config(path);
      CHECK(c.venc.core.width == p.w && c.venc.core.height == p.h && c.venc.core.fps == p.fps);
      CHECK(c.venc.core.sensor_crop == p.crop);
      CHECK(cam::preset_of(c.venc.core.width, c.venc.core.height, c.venc.core.fps,
                           c.venc.core.sensor_crop) == p.id);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "preset %u: %s\n", p.id, e.what());
      ok = false;
    }
    CHECK(ok);
    std::remove(path.c_str());
  }
}

TEST(nearest_legal_slices_keeps_a_legal_count_and_moves_an_illegal_one) {
  CHECK(nearest_legal_slices(1080, 4) == 4);   // 17 CTU rows: 4 is legal
  CHECK(nearest_legal_slices(720, 4) == 4);    // 12 rows
  CHECK(nearest_legal_slices(544, 4) == 5);    // 9 rows: 1, 2, 3, 5, 9; tie 3/5 goes up
  CHECK(nearest_legal_slices(544, 1) == 1);
  CHECK(nearest_legal_slices(544, 0) == 1);
}

TEST(set_venc_mode_sets_slices_only_when_asked) {
  const std::string in = "[venc]\nsize = \"1920x1080\"\nfps = 60\nslices      = 4   # rows\n";
  CHECK(set_venc_mode_text(in, 1152, 544, 120) ==
        "[venc]\nsize = \"1152x544\"\nfps = 120\nslices      = 4   # rows\n");
  CHECK(set_venc_mode_text(in, 1152, 544, 120, 5) ==
        "[venc]\nsize = \"1152x544\"\nfps = 120\nslices      = 5   # rows\n");
}

TEST(a_recording_of_another_shape_takes_the_sensor_modes_size) {
  const std::string bundle = slurp(std::string(MABUR_BUNDLE_DIR) + "/mabur.default.toml");
  Config cur = load_config(std::string(MABUR_BUNDLE_DIR) + "/mabur.default.toml");
  cur.record.enable = true;
  cur.record.width = 1920;
  cur.record.height = 1080;
  auto record_section = [](const std::string& t) {
    const size_t a = t.find("\n[record]");
    if (a == std::string::npos) return std::string();
    const size_t b = t.find("\n[", a + 1);
    return t.substr(a, b == std::string::npos ? std::string::npos : b - a);
  };
  const std::string wide = record_section(preset_config_text(bundle, cur, cam::kPresets[3]));
  CHECK(wide.find("\"1728x816\"") != std::string::npos);
  // Same shape (720p 100 against a 16:9 recording): the recording is left alone.
  const std::string hd = record_section(preset_config_text(bundle, cur, cam::kPresets[2]));
  CHECK(hd == record_section(bundle));
}

namespace {

struct TmpDir {
  std::string dir;
  TmpDir() {
    char t[] = "/tmp/mabur_camctl_XXXXXX";
    REQUIRE(mkdtemp(t) != nullptr);
    dir = t;
  }
  ~TmpDir() { std::system(("rm -rf " + dir).c_str()); }
};

bool exists(const std::string& p) { return std::ifstream(p).good(); }

}  // namespace

TEST(apply_then_commit_leaves_the_new_file_and_no_marker) {
  TmpDir t;
  const std::string bundle = slurp(std::string(MABUR_BUNDLE_DIR) + "/mabur.default.toml");
  const CamFiles f(t.dir + "/mabur.toml", t.dir + "/sensor_star6e");
  {
    std::ofstream(f.conf) << bundle;
  }
  const Config cur = load_config(f.conf);
  std::string err;
  CHECK(cam_apply(f, cur, cam::kPresets[1], false, &err) == cam::State::Applying);
  CHECK(err.empty());
  CHECK(slurp(f.prev) == bundle);
  CHECK(load_config(f.conf).venc.core.fps == 90);
  CHECK(!exists(f.next));
  CHECK(!exists(f.star6e_flag));
  auto pend = cam_take_pending(f);
  REQUIRE(pend.has_value());
  CHECK(pend->preset == 1 && !pend->driver_switched);
  cam_commit(f);
  CHECK(!exists(f.pending));
  CHECK(cam::state_of(cam_take_result(f)) == cam::State::Idle);
}

TEST(rollback_restores_the_file_drops_the_driver_flag_and_reports_once) {
  TmpDir t;
  const std::string bundle = slurp(std::string(MABUR_BUNDLE_DIR) + "/mabur.default.toml");
  const CamFiles f(t.dir + "/mabur.toml", t.dir + "/sensor_star6e");
  {
    std::ofstream(f.conf) << bundle;
  }
  std::string err;
  REQUIRE(cam_apply(f, load_config(f.conf), cam::kPresets[2], true, &err) ==
          cam::State::Applying);
  CHECK(exists(f.star6e_flag));
  auto pend = cam_take_pending(f);
  REQUIRE(pend.has_value());
  CHECK(pend->driver_switched);
  cam_roll_back(f, *pend);
  CHECK(slurp(f.conf) == bundle);
  CHECK(!exists(f.star6e_flag));
  CHECK(!exists(f.pending));
  const uint8_t r = cam_take_result(f);
  CHECK(cam::state_of(r) == cam::State::RolledBack && cam::state_preset(r) == 2);
  CHECK(cam::state_of(cam_take_result(f)) == cam::State::Idle);  // reported once
}

TEST(a_file_that_would_not_load_is_refused_and_nothing_changes) {
  TmpDir t;
  std::string text = slurp(std::string(MABUR_BUNDLE_DIR) + "/mabur.default.toml");
  // Running 720p 100 with a low-power rate of 90: a switch down to 60 fps
  // would break low_power.fps <= venc.fps.
  text = set_venc_mode_text(text, 1280, 720, 100);
  text = set_toml_values(text, "low_power", {{"fps", "90"}});
  const CamFiles f(t.dir + "/mabur.toml", t.dir + "/sensor_star6e");
  {
    std::ofstream(f.conf) << text;
  }
  const Config cur = load_config(f.conf);
  std::string err;
  CHECK(cam_apply(f, cur, cam::kPresets[0], false, &err) == cam::State::BadConfig);
  CHECK(err.find("low_power.fps") != std::string::npos);
  CHECK(slurp(f.conf) == text);
  CHECK(!exists(f.prev) && !exists(f.pending) && !exists(f.next));
}

TEST(switch_mask_needs_the_module_and_the_stock_driver_loaded) {
  const std::string stock = "mi_venc 1 0 - Live 0x0\nsensor_imx415_mipi 30000 0 - Live 0x0\n";
  const std::string star = "sensor_imx415_star6e 19400 0 - Live 0x0\n";
  CHECK(cam_switch_mask(0x03, stock, true) == 0x1C);   // 720p 100, 544p 120, Crop 60
  CHECK(cam_switch_mask(0x03, stock, false) == 0);
  CHECK(cam_switch_mask(0x1F, star, true) == 0);
  CHECK(cam_switch_mask(0x01, "sensor_imx335_mipi 1 0 - Live\n", true) == 0);
}

TEST(pending_marker_round_trips_and_rejects_junk) {
  const CamPending p{2, true};
  auto back = parse_cam_pending(format_cam_pending(p));
  REQUIRE(back.has_value());
  CHECK(back->preset == 2 && back->driver_switched);
  CHECK(!parse_cam_pending("").has_value());
  CHECK(!parse_cam_pending("preset=9\n").has_value());
  CHECK(!parse_cam_pending("driver_switched=1\n").has_value());
}

// The boot after a rollback: the GS still asks for the preset that failed
// (its Telem has not told it yet), and that must not switch again. Once the
// GS drops the request, asking again is a new request.
TEST(camera_switch_seeded_preset_is_not_retried_until_dropped) {
  mabur::CameraSwitch s;
  s.seed_decided(2);
  CHECK(s.on_wish(2, 0, 0x03, 0x0C, mabur::ArmView::Disarmed).action == mabur::CamAction::None);
  CHECK(s.on_wish(2, 0, 0x03, 0x0C, mabur::ArmView::Disarmed).action == mabur::CamAction::None);
  CHECK(s.on_wish(mabur::cam::kNoWish, 0, 0x03, 0x0C, mabur::ArmView::Disarmed).action ==
        mabur::CamAction::None);
  const mabur::CamDecision d = s.on_wish(2, 0, 0x03, 0x0C, mabur::ArmView::Disarmed);
  CHECK(d.action == mabur::CamAction::Apply && d.driver_switch);
}
MTEST_MAIN
