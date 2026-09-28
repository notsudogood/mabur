#include "arq_log.h"

#include <algorithm>
#include <cstdio>

namespace maburgs {

ArqLog::ArqLog(LogWriter& w, const std::string& dir)
    : w_(w), s_(w.open(dir, "arq.log", "arqlog 1")) {}

void ArqLog::episode(const ArqEpisode& e) {
  if (s_ == LogWriter::kBadStream) return;
  char b[192];
  const int n = std::snprintf(
      b, sizeof(b), "E %.0f %d %d %d %.2f %d %.0f %.0f %u %llu %llu %llu %llu",
      e.t_open_ms, e.sid, e.mcs, e.bw, e.ov, e.bpb, e.dur_ms, e.grow_ms, e.nack,
      static_cast<unsigned long long>(e.d0), static_cast<unsigned long long>(e.dpk),
      static_cast<unsigned long long>(e.aband),
      static_cast<unsigned long long>(e.stale));
  if (n > 0) w_.line(s_, b, std::min(static_cast<size_t>(n), sizeof(b) - 1));
}

void ArqLog::summary(const ArqSummary& s) {
  if (s_ == LogWriter::kBadStream) return;
  char b[96];
  const int n = std::snprintf(b, sizeof(b), "S %.0f %d %u %u", s.t_ms, s.sid,
                              s.bursts, s.short_bursts);
  if (n > 0) w_.line(s_, b, std::min(static_cast<size_t>(n), sizeof(b) - 1));
}

}  // namespace maburgs
