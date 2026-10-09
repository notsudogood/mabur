#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>

namespace maburplay {

// Sends the genlock loop's camera-rate setpoint (genlock.h) to maburgs'
// GenlockControl (gs/src/genlock_control.h) on 127.0.0.1, which forwards it
// to the drone. Fire-and-forget UDP, one datagram per steering tick: the
// next tick is the retry.
class GenlockClient {
 public:
  ~GenlockClient() { if (fd_ >= 0) close(fd_); }
  GenlockClient() = default;
  GenlockClient(const GenlockClient&) = delete;
  GenlockClient& operator=(const GenlockClient&) = delete;

  bool open(int port) {
    fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd_ < 0) return false;
    addr_ = sockaddr_in{};
    addr_.sin_family = AF_INET;
    addr_.sin_port = htons(static_cast<uint16_t>(port));
    return inet_pton(AF_INET, "127.0.0.1", &addr_.sin_addr) == 1;
  }

  void send(uint32_t mfps) {
    if (fd_ < 0) return;
    char msg[32];
    const int n = std::snprintf(msg, sizeof(msg), "genlock %u", static_cast<unsigned>(mfps));
    if (n <= 0) return;
    (void)sendto(fd_, msg, static_cast<size_t>(n), 0, reinterpret_cast<const sockaddr*>(&addr_),
                 sizeof(addr_));
    ++sent_;
  }

  uint64_t sent() const { return sent_; }

 private:
  int fd_ = -1;
  sockaddr_in addr_{};
  uint64_t sent_ = 0;
};

}  // namespace maburplay
