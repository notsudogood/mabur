#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "mabur/rc_proto.h"

namespace maburgs {

// maburplay's CoreControlClient sends here. Next to CalControl's 8400; the
// 830x block belongs to the stats sideport and the OSD feed.
constexpr int kRecControlPort = 8401;

// Loopback-only UDP listener for the record button's VTX wish.
// Commands: "vtx_rec on" / "vtx_rec off" (the record wish, held), "refresh
// <reason>" (a one-shot report that the player's picture needs a key
// frame; reasons are RefreshRequester's names, unknown ones are ignored),
// "phase <mfps>" (the phase lock's sensor-rate offset, held for
// kPhaseTimeoutMs: a silent player means no steering, unlike the record
// wish). The wish is HELD until the next command -- no decay: a crashed player
// must never stop the onboard recording by silence. It starts UNKNOWN
// (wire byte 0), so a maburgs restart does not stop it either; the player
// re-sends its wish every second. Non-blocking; polled from the core loop.
class RecControl {
 public:
  ~RecControl() { if (fd_ >= 0) close(fd_); }
  RecControl() = default;
  RecControl(const RecControl&) = delete;
  RecControl& operator=(const RecControl&) = delete;

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

  static constexpr uint64_t kPhaseTimeoutMs = 3000;

  // Drains every pending datagram. True iff the VTX wish changed.
  bool poll(uint64_t now_ms = 0) {
    if (fd_ < 0) return false;
    bool changed = false;
    char buf[64];
    for (;;) {
      const ssize_t n = recv(fd_, buf, sizeof(buf) - 1, 0);
      if (n < 0) break;
      buf[n] = '\0';
      const uint64_t rejects_before = phase_rejects_;
      if (apply(buf, now_ms)) {
        changed = true;
        std::fprintf(stderr, "maburgs: VTX record wish -> %s\n", on_ ? "on" : "off");
        std::fflush(stderr);
      }
      if (phase_rejects_ != rejects_before && rejects_before == 0) {
        std::fprintf(stderr, "maburgs: phase offset from the player rejected: %s\n", buf);
        std::fflush(stderr);
      }
    }
    return changed;
  }

  // Parses one command; true iff the VTX wish changed. A refresh line is
  // queued for take_refresh() and never changes the wish. I/O-free for tests.
  bool apply(const std::string& line, uint64_t now_ms = 0) {
    std::string s = line;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    static const std::string kRefresh = "refresh ";
    if (s.size() > kRefresh.size() && s.compare(0, kRefresh.size(), kRefresh) == 0) {
      const std::string reason = s.substr(kRefresh.size());
      if (!reason.empty() && reason.find(' ') == std::string::npos) refresh_.push_back(reason);
      return false;
    }
    static const std::string kPhase = "phase ";
    if (s.size() > kPhase.size() && s.compare(0, kPhase.size(), kPhase) == 0) {
      const std::string v = s.substr(kPhase.size());
      char* end = nullptr;
      errno = 0;
      const long mfps = std::strtol(v.c_str(), &end, 10);
      if (errno != 0 || end == v.c_str() || *end != '\0' ||
          mfps < -mabur::rc::kSnrOffRangeMfps || mfps > mabur::rc::kSnrOffRangeMfps) {
        ++phase_rejects_;
        return false;
      }
      phase_ = static_cast<int16_t>(mfps);
      phase_ms_ = now_ms;
      have_phase_ = true;
      return false;
    }
    bool on;
    if (s == "vtx_rec on") on = true;
    else if (s == "vtx_rec off") on = false;
    else return false;
    const bool changed = !known_ || on != on_;
    known_ = true;
    on_ = on;
    return changed;
  }

  // Refresh reasons received since the last call, in arrival order.
  std::vector<std::string> take_refresh() {
    std::vector<std::string> out;
    out.swap(refresh_);
    return out;
  }

  // The RCF byte: 0 until the first command, then kRecKnown | (kRecOn if on).
  uint8_t wire() const {
    if (!known_) return 0;
    return static_cast<uint8_t>(mabur::rc::kRecKnown | (on_ ? mabur::rc::kRecOn : 0));
  }

  // The RCF snr_off: the player's last offset, or 0 once it has been quiet
  // for longer than kPhaseTimeoutMs (or never spoke).
  int16_t snr_off(uint64_t now_ms) const {
    if (!have_phase_ || now_ms - phase_ms_ > kPhaseTimeoutMs) return 0;
    return phase_;
  }
  uint64_t phase_rejects() const { return phase_rejects_; }

 private:
  int fd_ = -1;
  bool known_ = false;
  bool on_ = false;
  std::vector<std::string> refresh_;
  int16_t phase_ = 0;
  uint64_t phase_ms_ = 0;
  bool have_phase_ = false;
  uint64_t phase_rejects_ = 0;
};

}  // namespace maburgs
