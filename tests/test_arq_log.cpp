#include "arq_log.h"
#include "log_writer.h"
#include "mtest.h"
#include <fstream>
#include <sstream>
#include <sys/stat.h>
static std::string read_all(const std::string& p) {
  std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
TEST(arq_log_header_rows_and_name) {
  std::string dir = "build_arq_log_test";
  (void)std::system(("rm -rf " + dir).c_str()); mkdir(dir.c_str(), 0755);
  maburgs::LogWriter w;
  maburgs::ArqLog log(w, dir);
  REQUIRE(log.ok());
  CHECK(log.path() == dir + "/arq.log");
  maburgs::ArqEpisode e;
  e.t_open_ms = 1234.6; e.sid = 1; e.mcs = 5; e.bw = 40; e.ov = 0.5; e.bpb = 4;
  e.dur_ms = 66.4; e.grow_ms = 33.2; e.nack = 3; e.d0 = 12; e.dpk = 20;
  e.aband = 7; e.stale = 2;
  log.episode(e);
  log.summary(maburgs::ArqSummary{10000.2, 0, 600, 9});
  w.flush_now();
  std::string text = read_all(log.path());
  CHECK(text.rfind("arqlog 1\n", 0) == 0);
  CHECK(text.find("\nE 1235 1 5 40 0.50 4 66 33 3 12 20 7 2\n") != std::string::npos);
  CHECK(text.find("\nS 10000 0 600 9\n") != std::string::npos);
}
TEST(arq_log_bad_dir_is_nonfatal) {
  maburgs::LogWriter w;
  maburgs::ArqLog log(w, "/nonexistent-dir-xyz");
  CHECK(!log.ok());
  log.episode(maburgs::ArqEpisode{});
  log.summary(maburgs::ArqSummary{});
}
MTEST_MAIN
