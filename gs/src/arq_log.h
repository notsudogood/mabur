#pragma once

#include <string>

#include "arq_shadow.h"
#include "log_writer.h"

namespace maburgs {

// Feedback-repair shadow log (arq.log, 2026-09-28): what ArqShadow saw --
// the per-burst repair-symbol shortfall a live feedback-repair loop would
// have requested (docs/feedback-repair-rollout.md). Read by
// tools/flightreport.py's ARQ SHADOW section. Writes `arq.log` inside the
// session directory (DebugSession::dir()) via the shared LogWriter, rotating
// with the session like fec.log.
//
// Record format is LOCKED (tests/test_arq_log.cpp pins the byte layout):
//
//   arqlog 1                                                    # first line
//   E <t_open_ms> <sid> <mcs> <bw> <ov> <bpb> <dur_ms> <grow_ms> <nack> <d0> <dpk> <aband> <stale>
//   S <t_ms> <sid> <bursts> <short>
//
// E: one shortfall episode (ArqEpisode verbatim; times mono ms, deficits in
// FEC symbols -- bpb converts them to radio bodies). S: burst-end samples
// taken on one layer since the previous S line (every ~10 s, only for a
// layer that had any), and how many of them were short.
//
// Every failure mode is non-fatal: ok() reads false and the writers no-op.
class ArqLog {
 public:
  ArqLog(LogWriter& w, const std::string& dir);

  ArqLog(const ArqLog&) = delete;
  ArqLog& operator=(const ArqLog&) = delete;

  bool ok() const { return s_ != LogWriter::kBadStream; }
  void rotate(const std::string& dir) { w_.reopen(s_, dir); }
  const std::string& path() const { return w_.path(s_); }

  void episode(const ArqEpisode& e);
  void summary(const ArqSummary& s);

 private:
  LogWriter& w_;
  LogWriter::Stream s_;
};

}  // namespace maburgs
