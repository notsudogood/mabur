#include <string>
#include <vector>
#include "cal_off_request.h"
#include "camera_request.h"
#include "mtest.h"
#include "rec_control.h"
#include "mabur/rc_proto.h"

using maburgs::RecControl;

TEST(rec_control_starts_unknown) {
  RecControl c;
  CHECK(c.wire() == 0);   // unknown: the drone keeps whatever it is doing
}

TEST(rec_control_on_off_and_holds_without_decay) {
  RecControl c;
  CHECK(c.apply("vtx_rec on"));
  CHECK(c.wire() == (mabur::rc::kRecKnown | mabur::rc::kRecOn));
  CHECK(!c.apply("vtx_rec on\n"));   // same wish: no change
  CHECK(c.wire() == (mabur::rc::kRecKnown | mabur::rc::kRecOn));  // held, no timer
  CHECK(c.apply("vtx_rec off"));
  CHECK(c.wire() == mabur::rc::kRecKnown);
}

TEST(rec_control_ignores_garbage) {
  RecControl c;
  CHECK(!c.apply(""));
  CHECK(!c.apply("vtx_rec maybe"));
  CHECK(!c.apply("start"));
  CHECK(c.wire() == 0);
}

// `refresh <reason>` lines are queued verbatim and never touch the VTX
// record wish; malformed lines are ignored.
TEST(rec_control_refresh_lines) {
  maburgs::RecControl c;
  CHECK(!c.apply("refresh start"));         // not a wish change ...
  CHECK(!c.apply("refresh dvr\n"));         // ... trailing newline tolerated
  CHECK(!c.apply("refresh"));               // no reason: ignored
  CHECK(!c.apply("refresh  resync"));       // two spaces: ignored, the line is exact
  CHECK(!c.apply("refreshstart"));
  auto got = c.take_refresh();
  REQUIRE(got.size() == 2);
  CHECK(got[0] == "start" && got[1] == "dvr");
  CHECK(c.take_refresh().empty());
  CHECK(c.wire() == 0);                     // the VTX wish is still unknown
  CHECK(c.apply("vtx_rec on"));
  CHECK(!c.apply("refresh decoder"));
  CHECK(c.wire() == (mabur::rc::kRecKnown | mabur::rc::kRecOn));
}

// `phase <mfps>`: maburplay's phase-lock offset, held until the player goes
// quiet for kPhaseTimeoutMs, then 0 (the drone falls back to its trim).
TEST(rec_control_phase_line_held_with_timeout) {
  RecControl c;
  CHECK(c.snr_off(0) == 0);                   // nothing heard yet: no steering
  CHECK(!c.apply("phase -37", 1000));         // never a VTX wish change
  CHECK(c.snr_off(1000) == -37);
  CHECK(c.snr_off(1000 + RecControl::kPhaseTimeoutMs) == -37);
  CHECK(c.snr_off(1001 + RecControl::kPhaseTimeoutMs) == 0);
  CHECK(!c.apply("phase 200\n", 5000));       // the range edge, newline tolerated
  CHECK(c.snr_off(5000) == 200);
  CHECK(c.wire() == 0);                       // the VTX wish is untouched
}

TEST(rec_control_phase_rejects_bad_values) {
  RecControl c;
  c.apply("phase 12", 0);
  c.apply("phase 201", 10);
  c.apply("phase -201", 10);
  c.apply("phase abc", 10);
  c.apply("phase 5x", 10);
  CHECK(c.snr_off(10) == 12);   // the last good value stays
  CHECK(c.phase_rejects() == 4);
}

// "camera <id>": queued for take_camera(), never a VTX record change; ids
// outside the preset table and junk are dropped.
TEST(rec_control_camera_lines) {
  RecControl c;
  CHECK(!c.apply("camera 2"));
  CHECK(!c.apply("camera 0\n"));
  CHECK(!c.apply("camera 9"));
  CHECK(!c.apply("camera -1"));
  CHECK(!c.apply("camera x"));
  CHECK(!c.apply("camera "));
  const std::vector<uint8_t> got = c.take_camera();
  REQUIRE(got.size() == 2);
  CHECK(got[0] == 2 && got[1] == 0);
  CHECK(c.take_camera().empty());
  CHECK(c.wire() == 0);
}

namespace cam = mabur::cam;
using maburgs::CameraRequest;

TEST(camera_request_held_until_switched) {
  CameraRequest r;
  CHECK(r.wire(0) == cam::kNoWish && std::string(r.result()).empty());
  r.request(1, 1000);
  CHECK(r.wire(1000) == 1 && std::string(r.result()) == "sent");
  r.on_telem(0, cam::pack_state(cam::State::Applying, 1), 1500);
  CHECK(std::string(r.result()) == "applying" && r.wire(1500) == 1);   // still held
  r.on_telem(1, cam::pack_state(cam::State::Switched, 1), 40000);
  CHECK(std::string(r.result()) == "switched" && r.wire(40000) == cam::kNoWish);
  CHECK(r.last() == 1);
}

TEST(camera_request_refusals_after_the_grace_only) {
  CameraRequest r;
  r.request(2, 1000);
  // An "armed" about preset 2 inside the grace may answer an older request.
  r.on_telem(0, cam::pack_state(cam::State::NoArmState, 2), 1000 + CameraRequest::kGraceMs - 1);
  CHECK(r.wish() == 2);
  r.on_telem(0, cam::pack_state(cam::State::NoArmState, 2), 1000 + CameraRequest::kGraceMs);
  CHECK(r.wish() == cam::kNoWish && std::string(r.result()) == "armed" && r.last() == 2);
  // An outcome about another preset never settles this one.
  r.request(3, 5000);
  r.on_telem(0, cam::pack_state(cam::State::Unavailable, 2), 9000);
  CHECK(r.wish() == 3 && std::string(r.result()) == "sent");
  r.on_telem(0, cam::pack_state(cam::State::RolledBack, 3), 90000);
  CHECK(std::string(r.result()) == "rolled_back");
}

TEST(camera_request_clock_behind_the_request_settles_nothing) {
  CameraRequest r;
  r.request(2, 50000);
  r.on_telem(0, cam::pack_state(cam::State::NoArmState, 2), 10000);   // older stamp
  CHECK(r.wish() == 2);
  CHECK(r.wire(10000) == 2);
}

TEST(camera_request_times_out) {
  CameraRequest r;
  r.request(1, 0);
  CHECK(r.wire(CameraRequest::kTimeoutMs) == 1);
  CHECK(r.wire(CameraRequest::kTimeoutMs + 1) == cam::kNoWish);
  CHECK(std::string(r.result()) == "timeout" && r.last() == 1);
}
TEST(rec_control_calibration_lines) {
  RecControl c;
  CHECK(!c.apply("cal start"));
  CHECK(!c.apply("cal off\n"));
  CHECK(!c.apply("cal"));
  CHECK(!c.apply("cal now"));
  const std::vector<std::string> got = c.take_calibration();
  REQUIRE(got.size() == 2);
  CHECK(got[0] == "start" && got[1] == "off");
  CHECK(c.take_calibration().empty());
  CHECK(c.wire() == 0);
}

TEST(cal_off_request_held_until_the_drone_is_uncalibrated) {
  maburgs::CalOffRequest r;
  CHECK(r.wire(0) == 0 && std::string(r.result()).empty());
  r.request(1000);
  CHECK(r.wire(1000) == 1 && std::string(r.result()) == "sent");
  r.on_telem(true, 1500);                // still calibrated: held
  CHECK(r.wire(1500) == 1);
  r.on_telem(false, 2000);
  CHECK(r.wire(2000) == 0 && std::string(r.result()) == "removed");
  r.request(5000);
  CHECK(r.wire(5000 + maburgs::CalOffRequest::kTimeoutMs) == 1);
  CHECK(r.wire(5001 + maburgs::CalOffRequest::kTimeoutMs) == 0);
  CHECK(std::string(r.result()) == "timeout");
}
MTEST_MAIN
