#include "ta_log.h"
#include "log_writer.h"
#include "mtest.h"
#include <fstream>
#include <sstream>
#include <sys/stat.h>
static std::string read_all(const std::string& p) {
  std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
static maburgs::TurnaroundCfg cfg() {
  maburgs::TurnaroundCfg c;
  c.rate_hz = 10;
  c.lanes = {0, 4, 5};
  c.frames = 2;
  c.bytes = 300;
  return c;
}
static mabur::node::RxBody rx(uint8_t card, std::vector<uint8_t> body, uint32_t tsfl,
                              uint64_t us) {
  mabur::node::RxBody m;
  m.card_id = card;
  m.body = std::move(body);
  m.tsfl = tsfl;
  m.mono_us = us;
  m.rssi[0] = 50;  // -60 dBm
  m.rssi[1] = 55;  // -55 dBm: the better chain is logged
  return m;
}
TEST(ta_log_header_rows_and_name) {
  std::string dir = "build_ta_log_test";
  (void)std::system(("rm -rf " + dir).c_str()); mkdir(dir.c_str(), 0755);
  maburgs::LogWriter w;
  maburgs::TaLog log(w, dir, cfg());
  REQUIRE(log.ok());
  CHECK(log.path() == dir + "/ta.log");

  mabur::rc::TaPing p;
  p.vtx_id = 1; p.seq = 17; p.lane = 4; p.n_frames = 2; p.frame_bytes = 300;
  log.sent(p, 0, 5000000, 5000420);
  log.heard(rx(1, mabur::rc::pack_ta_ping(p), 4000000000u, 5000700));

  mabur::rc::TaPong o;
  o.vtx_id = 1; o.seq = 17; o.lane = 4; o.idx = 1; o.n_frames = 2;
  o.hold_us = 1830; o.txq_depth = 41; o.pool_depth = 6; o.air_backlog_100us = 87;
  o.frame_bytes = 300;
  log.heard(rx(1, mabur::rc::pack_ta_pong(o), 4000004321u, 5004900));

  auto no_phy = rx(0, mabur::rc::pack_ta_pong(o), 7, 8);
  no_phy.phy_valid = false;
  log.heard(no_phy);

  auto corrupt = rx(1, mabur::rc::pack_ta_pong(o), 9, 9);
  corrupt.crc_ok = false;
  log.heard(corrupt);  // FCS-corrupt: not a sighting
  log.heard(rx(1, {1, 2, 3}, 9, 9));  // not a TA body: ignored
  w.flush_now();

  std::string text = read_all(log.path());
  CHECK(text.rfind("talog 1 rate_hz=10.00 lanes=0,4,5 frames=2 bytes=300\n", 0) == 0);
  CHECK(text.find("\nS 17 4 0 5000000 5000420\n") != std::string::npos);
  CHECK(text.find("\nH 1 17 4000000000 5000700\n") != std::string::npos);
  CHECK(text.find("\nO 1 17 4 1 2 4000004321 5004900 1830 41 6 87 -55\n") !=
        std::string::npos);
  CHECK(text.find("\nO 0 17 4 1 2 7 8 1830 41 6 87 0\n") != std::string::npos);
  // Exactly the four rows above after the header.
  size_t lines = 0;
  for (char ch : text) lines += ch == '\n';
  CHECK(lines == 5);
}
TEST(ta_log_bad_dir_is_nonfatal) {
  maburgs::LogWriter w;
  maburgs::TaLog log(w, "/nonexistent-dir-xyz", cfg());
  CHECK(!log.ok());
  log.sent(mabur::rc::TaPing{}, 0, 1, 2);
  log.heard(mabur::node::RxBody{});
}
MTEST_MAIN
