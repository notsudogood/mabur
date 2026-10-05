#pragma once

#include <string>

#include "config.h"
#include "log_writer.h"
#include "mabur/node.h"
#include "mabur/rc_proto.h"

namespace maburgs {

// Turnaround bench log (ta.log, rollout phase 2): raw sightings, paired
// offline by tools/flightreport.py's TURNAROUND section. Writes `ta.log`
// inside the session directory via the shared LogWriter, rotating with the
// session like arq.log.
//
// Record format is LOCKED (tests/test_ta_log.cpp pins the byte layout):
//
//   talog 1 rate_hz=<r> lanes=<a,b,..> frames=<n> bytes=<b>      # first line
//   S <seq> <lane> <card> <t_call_us> <t_done_us>
//   H <card> <seq> <tsfl> <t_us>
//   O <card> <seq> <lane> <idx> <n> <tsfl> <t_us> <hold_us> <txq> <pool> <backlog_100us> <rssi>
//   X <card> <type> <len>
//
// S: a ping handed to TX card <card>; t_call/t_done bracket the synchronous
// send call (GS mono us). H: card <card> heard the GS's own ping <seq> (the
// witness: a card other than the one that sent it); tsfl is that card's
// hardware RX TSF (us, low 32 bits, wraps every ~71.6 min), t_us its host
// stamp. O: card <card> heard pong frame <idx> of <n> for ping <seq>; the
// rest is what the drone stamped (TaPong) plus the frame's RSSI (dBm, the
// better chain; 0 when the frame carried no PHY status). On-air turnaround
// is O.tsfl - H.tsfl for the same seq on the same card. X: card <card> heard
// an FCS-clean T_TA_PING/T_TA_PONG (<type> 7/8) of <len> body bytes that did
// not parse -- a wire/parser mismatch, which would otherwise read as replies
// that never arrived.
//
// Every failure mode is non-fatal: ok() reads false and the writers no-op.
class TaLog {
 public:
  TaLog(LogWriter& w, const std::string& dir, const TurnaroundCfg& cfg);

  TaLog(const TaLog&) = delete;
  TaLog& operator=(const TaLog&) = delete;

  bool ok() const { return s_ != LogWriter::kBadStream; }
  void rotate(const std::string& dir) { w_.reopen(s_, dir); }
  const std::string& path() const { return w_.path(s_); }

  void sent(const mabur::rc::TaPing& p, int card, uint64_t t_call_us, uint64_t t_done_us);
  // A T_TA_PING or T_TA_PONG body heard on a card (CRC-clean). One that
  // does not parse is an X row; any other body is ignored.
  void heard(const mabur::node::RxBody& m);

  static std::string header(const TurnaroundCfg& cfg);

 private:
  LogWriter& w_;
  LogWriter::Stream s_;
};

}  // namespace maburgs
