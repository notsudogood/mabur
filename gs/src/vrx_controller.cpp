#include "vrx_controller.h"

#include <cassert>

#include "mabur/rc_proto.h"
#include "mabur/profile.h"

namespace maburgs {

namespace {
// Rebuild an OpPoint from the ladder's current rung. Power is constant and
// is not part of the operating point.
// 80 MHz exists only as VHT; the 20/40 rungs stay HT (their frames, rate
// tables and per-MCS walls were all measured as HT).
OpPoint op_from_rung(const Rung& r) {
  return OpPoint{r.bw == 80, r.mcs, r.bw, false, r.overhead_base, r.overhead_enh, 0.0};
}
}  // namespace

std::vector<uint8_t> disc_for_channel(const std::vector<uint8_t>& disc, uint8_t ch,
                                      const mabur::LinkKey& key) {
  if (ch == 0 || mabur::rc::frame_type(disc.data(), disc.size()) != mabur::rc::T_DISC) return disc;
  auto d = mabur::rc::parse_disc(disc.data(), disc.size());
  if (!d || d->op_channel == ch) return disc;
  d->op_channel = ch;
  return mabur::rc::pack_disc(*d, key);
}

VrxController::VrxController(VrxCfg cfg)
    : cfg_(cfg),
      ctrl_(cfg.ladder),
      // link_lost_ms 1000 / beacon_period_ms 20 are deliberately fixed, not
      // config: every hw validation ran with these, and a slower fallback to
      // BEACONING after video loss only delays re-rendezvous. The removed
      // link.video_silence_ms key claimed to tune the 1000 but never did.
      // nonce 0 = random per process: a restarted GS is a new session.
      rz_(VrxRzConfig{1000, 20, cfg.op_channel, cfg.rz_nonce}),
      cur_op_(op_from_rung(ctrl_.op())) {}

void VrxController::sync_op_() { cur_op_ = op_from_rung(ctrl_.op()); }

void VrxController::on_video(double now_ms) { rz_.feed_video(now_ms); }

void VrxController::on_rc_frame(const uint8_t* buf, size_t len, double now_ms) {
  if (mabur::rc::frame_type(buf, len) != mabur::rc::T_DISC_ACK) return;
  auto ack = mabur::rc::parse_disc_ack(buf, len);
  if (!ack) return;
  bool adopted_new = false;
  if (!rz_.feed_disc_ack(*ack, now_ms, &adopted_new)) return;
  if (ack->flags & mabur::rc::kAckKeyMismatch) return;   // nothing learned about the peer
  agreed_channel_ = ack->agreed_channel;
  peer_caps_ = ack->chip_caps;
  peer_acked_ = true;
  if (adopted_new) {
    seq32_ = 0;
    rcfs_since_adopt_ = 0;
    move_armed_ = true;
    move_edge_ = false;
  }
}

void VrxController::note_drone_state(uint8_t telem_state) {
  if (telem_state == 2 && move_armed_) {
    move_edge_ = true;
    move_armed_ = false;
  }
}

std::optional<VrxController::Out> VrxController::step(double now_ms,
                                                      const LinkHealth& health) {
  // Blind-side failsafe: with no feedback the ladder's own on_tick() forces
  // rung 0 after feedback_timeout_ms, so the first RCF after recovery
  // commands the conservative floor, not the last aggressive point.
  if (cfg_.pin_mcs >= 0) {
    // Static-link mode: fixed op, ladder fully out of the loop (never
    // ticked/updated — health is ignored entirely).
    cur_op_ = OpPoint{cfg_.pin_bw == 80, cfg_.pin_mcs, cfg_.pin_bw, false,
                     cfg_.pin_overhead_base, cfg_.pin_overhead_enh, 0.0};
  } else if (narrow_) {
    // Below the ladder: the 10 MHz op stands, the ladder is not ticked.
  } else if (forced_rung_ >= 0) {
    // First-boot floor calibration holds a rung: the ladder waits.
  } else if (ctrl_.on_tick(now_ms)) {
    sync_op_();
  }
  const VrxAction act = rz_.tick(now_ms);
  if (act == VrxAction::Beacon)
    return Out{mabur::rc::pack_disc(rz_.beacon(), cfg_.key), true};
  if (act != VrxAction::TxFeedback) return std::nullopt;

  // Fix (a): SESSION keep-alive DISC, replacing this tick's RCF slot.
  // SESSION needs an accepted DiscAck (link pairing),
  // so the peer's caps are always known here; a GS that lacks them is still
  // BEACONING at beacon_period_ms, which is the stale-caps re-teach.
  if (!keepalive_hold_ && now_ms - last_keepalive_ms_ >= cfg_.beacon_keepalive_ms) {
    last_keepalive_ms_ = now_ms;
    return Out{mabur::rc::pack_disc(rz_.beacon(), cfg_.key), true};
  }
  if (now_ms - last_fb_ms_ < cfg_.feedback_ms) return std::nullopt;
  last_fb_ms_ = now_ms;

  if (cfg_.pin_mcs < 0 && !narrow_ && forced_rung_ < 0) {
    if (ctrl_.update(health, now_ms)) sync_op_();
  }
  mabur::rc::Rcf r = build_rcf();
  // No repeat copies of an op-changing RCF (a repeat burst of 3 copies
  // 10 ms apart): with the RCF slotter the drone hears ~90 % of single
  // sends, and the slotter released the three copies as one batch at an AU completion, which overran the
  // inter-AU idle and killed the next aggregate on both cards on a quarter
  // to a third of all rung changes (bench A/B, ctl-0296/0298/0300,
  // docs/findings/2026-09-05-switch-loss.md).
  note_cmd(r);
  if (move_armed_ && ++rcfs_since_adopt_ >= kMoveAfterRcfs) {
    move_edge_ = true;
    move_armed_ = false;
  }
  // SESSION implies a held vtx_nonce (rendezvous: only an unflagged ack
  // adopts one, and video re-enters SESSION only when one is held).
  assert(rz_.vtx_nonce());
  return Out{mabur::rc::pack_rcf(r, cfg_.key,
                                 mabur::rc::TagCtx{rz_.nonce(), *rz_.vtx_nonce(), seq32_}),
             false};
}

mabur::rc::Rcf VrxController::build_rcf() {
  ++seq32_;
  mabur::rc::Rcf r;
  r.seq = static_cast<uint16_t>(seq32_);
  r.profile = mabur::rc::encode_profile(
      cur_op_.vht ? mabur::rc::PhyMode::VHT : mabur::rc::PhyMode::HT,
      static_cast<uint8_t>(cur_op_.mcs), static_cast<uint8_t>(cur_op_.bw));
  r.fec_overhead_base = cur_op_.overhead_base;
  r.fec_overhead_enh = cur_op_.overhead_enh;
  r.hop_ch = hop_ch_;
  r.hop_epoch = hop_epoch_;
  r.rec = rec_wish_;
  r.idr_epoch = idr_epoch_;
  r.snr_off = snr_off_;
  r.cam = cam_wish_;
  r.cal_off = cal_off_;
  // Probe stream MCS: the ladder names a rung to probe on
  // every RCF, or none. In static-pin mode the ladder is out of the loop, so
  // the probe follows the dedicated pin instead.
  r.probe_profile = mabur::rc::kNoProbeProfile;
  const auto mode = cur_op_.vht ? mabur::rc::PhyMode::VHT : mabur::rc::PhyMode::HT;
  if (cfg_.pin_mcs >= 0) {
    if (cfg_.probe_pin_mcs >= 0)
      r.probe_profile = mabur::rc::encode_profile(mode, static_cast<uint8_t>(cfg_.probe_pin_mcs),
                                                  static_cast<uint8_t>(cur_op_.bw));
  } else if (narrow_ || forced_rung_ >= 0) {
    // Nothing can be probed: a 10 MHz radio neither sends nor hears 20 MHz;
    // a held calibration rung flies alone.
  } else if (const int pr = ctrl_.probe_rung(); pr >= 0) {
    // The PROBE rung's own width, not the current op's: sitting on 20/4
    // with 40/3 above, the probe must fly 40 MHz.
    // Mode from the probe rung too: a 40 -> 80 probe flies VHT.
    const Rung& prung = cfg_.ladder.ladder[static_cast<std::size_t>(pr)];
    r.probe_profile = mabur::rc::encode_profile(
        prung.bw == 80 ? mabur::rc::PhyMode::VHT : mabur::rc::PhyMode::HT,
        static_cast<uint8_t>(prung.mcs), static_cast<uint8_t>(prung.bw));
  }
  return r;
}

void VrxController::set_narrow(bool on, double ov_base, double ov_enh, double now_ms) {
  if (on) {
    narrow_ = true;
    cur_op_ = OpPoint{false, 0, 10, false, ov_base, ov_enh, 0.0};
    return;
  }
  if (!narrow_) return;
  narrow_ = false;
  ctrl_.restore(0, now_ms);
  sync_op_();
}

void VrxController::restore_rung(int rung, double now_ms) {
  ctrl_.restore(rung, now_ms);
  sync_op_();
}

void VrxController::set_forced_rung(int rung, double now_ms) {
  if (cfg_.pin_mcs >= 0 || narrow_) rung = -1;
  if (rung >= static_cast<int>(cfg_.ladder.ladder.size())) rung = -1;
  if (rung == forced_rung_) return;
  forced_rung_ = rung;
  if (rung >= 0) {
    ctrl_.restore(rung, now_ms);
    sync_op_();
  }
}

void VrxController::note_cmd(const mabur::rc::Rcf& r) {
  last_cmd_probe_profile_ = r.probe_profile;
}

const OpPoint& VrxController::cur_op() const { return cur_op_; }
VrxState VrxController::link_state() const { return rz_.state(); }
uint16_t VrxController::rcf_seq() const { return static_cast<uint16_t>(seq32_); }

}  // namespace maburgs
