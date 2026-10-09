#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "mabur/rc_proto.h"

namespace maburgs {

// maburplay's GenlockClient sends here; next to RecControl's 8401.
constexpr int kGenlockControlPort = 8402;

// Loopback-only UDP listener for the player's camera-rate setpoint
// (docs/efficient-link-plan.md step 2, gs/player/src/genlock.h). Command:
// "genlock <mfps>", about once a second while the player steers. Each new
// datagram is one setpoint to forward to the drone as a T_GENLOCK; maburgs
// keeps nothing and never repeats one on its own -- the player's cadence is
// the repeat, and silence (player stopped steering or died) leaves the drone
// holding its last rate, which is the safe state. Non-blocking; polled from
// the core loop.
class GenlockControl {
 public:
  ~GenlockControl() { if (fd_ >= 0) close(fd_); }
  GenlockControl() = default;
  GenlockControl(const GenlockControl&) = delete;
  GenlockControl& operator=(const GenlockControl&) = delete;

  bool ok() const { return fd_ >= 0; }

  bool open(int port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) return false;
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return false;
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      close(fd);
      return false;
    }
    fd_ = fd;
    return true;
  }

  // Drains every pending datagram. True iff at least one valid setpoint
  // arrived; the newest is in mfps().
  bool poll() {
    if (fd_ < 0) return false;
    bool got = false;
    char buf[64];
    for (;;) {
      const ssize_t n = recv(fd_, buf, sizeof(buf) - 1, 0);
      if (n < 0) break;
      buf[n] = '\0';
      if (apply(buf)) got = true;
    }
    return got;
  }

  // Parses one command; true iff it is a valid setpoint. I/O-free for tests.
  bool apply(const std::string& line) {
    static const std::string kVerb = "genlock ";
    if (line.compare(0, kVerb.size(), kVerb) != 0) return false;
    const char* s = line.c_str() + kVerb.size();
    char* end = nullptr;
    errno = 0;
    const unsigned long v = std::strtoul(s, &end, 10);
    if (end == s || errno == ERANGE) return false;
    while (*end == ' ' || *end == '\n' || *end == '\r') ++end;
    if (*end != '\0' || v > mabur::rc::kGenlockMaxMfps) return false;
    mfps_ = static_cast<uint32_t>(v);
    ++received_;
    return true;
  }

  uint32_t mfps() const { return mfps_; }
  uint64_t received() const { return received_; }

 private:
  int fd_ = -1;
  uint32_t mfps_ = 0;
  uint64_t received_ = 0;
};

}  // namespace maburgs
