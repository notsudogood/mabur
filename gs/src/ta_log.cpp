#include "ta_log.h"

#include <algorithm>
#include <cstdio>

namespace maburgs {

std::string TaLog::header(const TurnaroundCfg& cfg) {
  std::string lanes;
  for (size_t i = 0; i < cfg.lanes.size(); ++i) {
    if (i) lanes += ',';
    lanes += std::to_string(cfg.lanes[i]);
  }
  char b[160];
  std::snprintf(b, sizeof(b), "talog 1 rate_hz=%.2f lanes=%s frames=%d bytes=%d",
                cfg.rate_hz, lanes.c_str(), cfg.frames, cfg.bytes);
  return b;
}

TaLog::TaLog(LogWriter& w, const std::string& dir, const TurnaroundCfg& cfg)
    : w_(w), s_(w.open(dir, "ta.log", header(cfg))) {}

void TaLog::sent(const mabur::rc::TaPing& p, int card, uint64_t t_call_us,
                 uint64_t t_done_us) {
  if (s_ == LogWriter::kBadStream) return;
  char b[128];
  const int n = std::snprintf(b, sizeof(b), "S %u %u %d %llu %llu",
                              static_cast<unsigned>(p.seq), static_cast<unsigned>(p.lane),
                              card, static_cast<unsigned long long>(t_call_us),
                              static_cast<unsigned long long>(t_done_us));
  if (n > 0) w_.line(s_, b, std::min(static_cast<size_t>(n), sizeof(b) - 1));
}

void TaLog::heard(const mabur::node::RxBody& m) {
  if (s_ == LogWriter::kBadStream || !m.crc_ok) return;
  const uint8_t* d = m.body.data();
  const size_t len = m.body.size();
  char b[192];
  int n = 0;
  if (auto ping = mabur::rc::parse_ta_ping(d, len)) {
    n = std::snprintf(b, sizeof(b), "H %u %u %u %llu", static_cast<unsigned>(m.card_id),
                      static_cast<unsigned>(ping->seq), m.tsfl,
                      static_cast<unsigned long long>(m.mono_us));
  } else if (auto pong = mabur::rc::parse_ta_pong(d, len)) {
    const int rssi =
        m.phy_valid ? static_cast<int>(std::max(m.rssi[0], m.rssi[1])) - 110 : 0;
    n = std::snprintf(b, sizeof(b), "O %u %u %u %u %u %u %llu %u %u %u %u %d",
                      static_cast<unsigned>(m.card_id), static_cast<unsigned>(pong->seq),
                      static_cast<unsigned>(pong->lane), static_cast<unsigned>(pong->idx),
                      static_cast<unsigned>(pong->n_frames), m.tsfl,
                      static_cast<unsigned long long>(m.mono_us), pong->hold_us,
                      static_cast<unsigned>(pong->txq_depth),
                      static_cast<unsigned>(pong->pool_depth),
                      static_cast<unsigned>(pong->air_backlog_100us), rssi);
  }
  if (n > 0) w_.line(s_, b, std::min(static_cast<size_t>(n), sizeof(b) - 1));
}

}  // namespace maburgs
