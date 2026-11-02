#include "ext_venc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>  // timeval: not pulled in by <sys/socket.h> on musl
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mabur {

namespace {

constexpr int kTimeoutMs = 200;

void log_rate_limited(const std::string& msg) {
  using clock = std::chrono::steady_clock;
  static clock::time_point last{};
  const auto now = clock::now();
  if (last.time_since_epoch().count() == 0 || now - last >= std::chrono::seconds(5)) {
    std::fprintf(stderr, "ext_venc: %s\n", msg.c_str());
    last = now;
  }
}

// A connected fd within timeout_ms, else -1. Loopback connects at once;
// the non-blocking path is for a server that is not listening yet.
int connect_with_timeout(const std::string& host, int port, int timeout_ms) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    close(fd);
    return -1;
  }
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    close(fd);
    return -1;
  }
  int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
  if (rc != 0) {
    if (errno != EINPROGRESS) {
      close(fd);
      return -1;
    }
    pollfd p{fd, POLLOUT, 0};
    if (poll(&p, 1, timeout_ms) <= 0) {
      close(fd);
      return -1;
    }
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
      close(fd);
      return -1;
    }
  }
  if (fcntl(fd, F_SETFL, flags) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

}  // namespace

ExtVenc::ExtVenc(ExtVencCfg cfg) : cfg_(std::move(cfg)) {}

ExtVencCfg ExtVenc::parse_spec(const char* spec) {
  ExtVencCfg c;
  if (!spec || !*spec) return c;
  const std::string s(spec);
  const auto colon = s.rfind(':');
  if (colon == std::string::npos) {
    c.host = s;
    return c;
  }
  c.host = s.substr(0, colon);
  const int port = std::atoi(s.c_str() + colon + 1);
  if (port > 0 && port < 65536) c.port = port;
  if (c.host.empty()) c.host = "127.0.0.1";
  return c;
}

bool ExtVenc::set_live(const std::string& key, const std::string& value) {
  return do_get("/api/v1/live/set?" + key + "=" + value);
}

bool ExtVenc::request_idr() { return do_get("/request/idr"); }

bool ExtVenc::do_get(const std::string& path) {
  const std::string where = cfg_.host + ":" + std::to_string(cfg_.port);
  const int fd = connect_with_timeout(cfg_.host, cfg_.port, kTimeoutMs);
  if (fd < 0) {
    ++failures_;
    log_rate_limited("connect failed to " + where + " (is waybeam up?)");
    return false;
  }
  timeval tv{};
  tv.tv_sec = kTimeoutMs / 1000;
  tv.tv_usec = (kTimeoutMs % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

  const std::string request = "GET " + path + " HTTP/1.0\r\n\r\n";
  const ssize_t sent = send(fd, request.data(), request.size(), 0);
  if (sent < 0 || static_cast<size_t>(sent) != request.size()) {
    close(fd);
    ++failures_;
    log_rate_limited("write failed to " + where);
    return false;
  }
  // To EOF (HTTP/1.0: the server closes when done), capped.
  std::string response;
  char buf[512];
  while (response.size() < 8192) {
    const ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n <= 0) break;
    response.append(buf, static_cast<size_t>(n));
  }
  close(fd);
  if (response.empty()) {
    ++failures_;
    log_rate_limited("no reply from " + where);
    return false;
  }
  const auto eol = response.find('\n');
  const std::string status = eol == std::string::npos ? response : response.substr(0, eol);
  if (status.find(" 200") == std::string::npos) {
    ++failures_;
    log_rate_limited(path + " -> " + status);
    return false;
  }
  return true;
}

}  // namespace mabur
