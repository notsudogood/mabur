#include "genlock_client.h"
#include "genlock_control.h"
#include "mtest.h"

// Round trip over real loopback sockets on a test port (not 8402, so a
// running maburgs on the dev box cannot interfere).
TEST(client_setpoint_reaches_control) {
  constexpr int kPort = 18402;
  maburgs::GenlockControl ctl;
  REQUIRE(ctl.open(kPort));
  maburplay::GenlockClient cli;
  REQUIRE(cli.open(kPort));
  CHECK(!ctl.poll());  // nothing yet
  cli.send(59922);
  cli.send(59925);
  CHECK(cli.sent() == 2);
  CHECK(ctl.poll());
  CHECK(ctl.mfps() == 59925);  // the newest wins
  CHECK(ctl.received() == 2);
  CHECK(!ctl.poll());
}

TEST(control_rejects_garbage_and_out_of_range) {
  maburgs::GenlockControl c;
  CHECK(!c.apply(""));
  CHECK(!c.apply("genlock"));
  CHECK(!c.apply("genlock "));
  CHECK(!c.apply("genlock 59x"));
  CHECK(!c.apply("genlock -5"));
  CHECK(!c.apply("vtx_rec on"));
  CHECK(!c.apply("genlock 240001"));
  CHECK(c.received() == 0);
  CHECK(c.apply("genlock 0\n"));  // release
  CHECK(c.mfps() == 0);
  CHECK(c.apply("genlock 59940"));
  CHECK(c.mfps() == 59940);
}

MTEST_MAIN
