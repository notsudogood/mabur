// ext_venc.h: the encoder as its own process (the SSC378QE image), driven
// over waybeam_venc's local HTTP API. A loopback server stands in for
// waybeam and records the request lines.
#include "ext_venc.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "mtest.h"

namespace {

// Accepts `n` connections, answers each with `status`, keeps the first
// request line of each.
struct FakeWaybeam {
  int lfd = -1;
  int port = 0;
  std::vector<std::string> lines;
  std::thread th;
  FakeWaybeam(int n, const char* status) {
    lfd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(lfd >= 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    REQUIRE(bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
    socklen_t len = sizeof a;
    REQUIRE(getsockname(lfd, reinterpret_cast<sockaddr*>(&a), &len) == 0);
    port = ntohs(a.sin_port);
    REQUIRE(listen(lfd, 4) == 0);
    th = std::thread([this, n, status] {
      for (int i = 0; i < n; ++i) {
        const int c = accept(lfd, nullptr, nullptr);
        if (c < 0) return;
        char buf[1024];
        const ssize_t got = recv(c, buf, sizeof buf - 1, 0);
        std::string req(buf, got > 0 ? static_cast<size_t>(got) : 0);
        const auto eol = req.find("\r\n");
        lines.push_back(eol == std::string::npos ? req : req.substr(0, eol));
        const std::string reply = std::string("HTTP/1.0 ") + status + "\r\n\r\n{}";
        (void)!send(c, reply.data(), reply.size(), 0);
        close(c);
      }
    });
  }
  ~FakeWaybeam() {
    if (th.joinable()) th.join();
    close(lfd);
  }
};

}  // namespace

TEST(set_live_and_idr_hit_waybeams_routes) {
  FakeWaybeam wb(3, "200 OK");
  mabur::ExtVenc v(mabur::ExtVenc::parse_spec(("127.0.0.1:" + std::to_string(wb.port)).c_str()));
  CHECK(v.set_live("video0.bitrate", "12345"));
  CHECK(v.set_live("video0.fps", "60"));
  CHECK(v.request_idr());
  CHECK(v.failures() == 0);
  wb.th.join();
  REQUIRE(wb.lines.size() == 3);
  CHECK(wb.lines[0] == "GET /api/v1/live/set?video0.bitrate=12345 HTTP/1.0");
  CHECK(wb.lines[1] == "GET /api/v1/live/set?video0.fps=60 HTTP/1.0");
  CHECK(wb.lines[2] == "GET /request/idr HTTP/1.0");
}

TEST(a_refusal_is_a_failure_not_a_throw) {
  FakeWaybeam wb(1, "400 Bad Request");
  mabur::ExtVenc v(mabur::ExtVenc::parse_spec(("127.0.0.1:" + std::to_string(wb.port)).c_str()));
  CHECK(!v.set_live("video0.bitrate", "x"));
  CHECK(v.failures() == 1);
}

// Before waybeam is up (it starts after the MI modules, maburd first)
// every verb fails fast and is counted; RcAgent retries on its next tick.
TEST(nothing_listening_fails_within_the_timeout) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  REQUIRE(bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
  socklen_t len = sizeof a;
  REQUIRE(getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) == 0);
  const int free_port = ntohs(a.sin_port);
  close(fd);  // bound, never listened, now released: nothing answers here
  mabur::ExtVenc v(mabur::ExtVenc::parse_spec(("127.0.0.1:" + std::to_string(free_port)).c_str()));
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(!v.request_idr());
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));
  CHECK(v.failures() == 1);
}

TEST(spec_parsing_defaults_to_waybeams_port) {
  CHECK(mabur::ExtVenc::parse_spec(nullptr).host == "127.0.0.1");
  CHECK(mabur::ExtVenc::parse_spec(nullptr).port == 80);
  CHECK(mabur::ExtVenc::parse_spec("").port == 80);
  CHECK(mabur::ExtVenc::parse_spec("10.0.0.2").host == "10.0.0.2");
  CHECK(mabur::ExtVenc::parse_spec("10.0.0.2").port == 80);
  CHECK(mabur::ExtVenc::parse_spec("127.0.0.1:8080").port == 8080);
  CHECK(mabur::ExtVenc::parse_spec(":99").host == "127.0.0.1");
  CHECK(mabur::ExtVenc::parse_spec("127.0.0.1:0").port == 80);  // out of range: default
}
MTEST_MAIN
