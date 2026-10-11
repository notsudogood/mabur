#include "mtest.h"
#include "rec_control.h"
#include "vtx_rec_client.h"

// Round trip over real loopback sockets on a test port (not 8401, so a
// running maburgs on the dev box cannot interfere).
TEST(client_sends_on_change_and_every_period) {
  constexpr int kPort = 18401;
  maburgs::RecControl ctl;
  REQUIRE(ctl.open(kPort));
  maburplay::VtxRecClient cli;
  REQUIRE(cli.open(kPort));
  cli.tick(true, 0);
  CHECK(cli.sent() == 1);
  ctl.poll();
  CHECK(ctl.wire() == (mabur::rc::kRecKnown | mabur::rc::kRecOn));
  cli.tick(true, 500);                 // same wish inside the period: quiet
  CHECK(cli.sent() == 1);
  cli.tick(true, 1000);                // period elapsed: re-send
  CHECK(cli.sent() == 2);
  cli.tick(false, 1001);               // change: immediate
  CHECK(cli.sent() == 3);
  ctl.poll();
  CHECK(ctl.wire() == mabur::rc::kRecKnown);
}

TEST(client_keyframe_requests_are_paced_and_reach_rec_control) {
  constexpr int kPort = 18402;
  maburgs::RecControl ctl;
  REQUIRE(ctl.open(kPort));
  maburplay::VtxRecClient cli;
  REQUIRE(cli.open(kPort));
  CHECK(cli.request_idr(0));           // the first ask goes out at once
  CHECK(!cli.request_idr(499));        // inside the period: quiet
  CHECK(cli.request_idr(500));
  CHECK(cli.idr_sent() == 2);
  CHECK(cli.sent() == 0);              // separate from the VTX wish
  ctl.poll();
  CHECK(ctl.idr_requests() == 2);
  CHECK(ctl.wire() == 0);
}

MTEST_MAIN
