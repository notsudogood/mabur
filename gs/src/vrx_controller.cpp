#include "vrx_controller.h"

#include <algorithm>

#include "mabur/rc_proto.h"
#include "mabur/profile.h"

namespace maburgs {

namespace {
// Rebuild an OpPoint from the ladder's current rung. Power is constant and
// is not part of the operating point (spec 2026-08-12-constant-txpower).
OpPoint op_from_rung(const Rung& r) {
  return OpPoint{false, r.mcs, 20, false, r.overhead_base, r.overhead_enh, 0.0};
}
}  // namespace

VrxController::VrxController(VrxCfg cfg)
    : cfg_(cfg),
      ctrl_(cfg.ladder),
      ov_base_(cfg.overhead),
      ov_enh_(cfg.overhead),
      // link_lost_ms 1000 / beacon_period_ms 20 are deliberately fixed, not
      // config: every hw validation ran with these, and a slower fallback to
      // BEACONING after video loss only delays re-rendezvous. The removed
      // link.video_silence_ms key claimed to tune the 1000 but never did.
      rz_(VrxRzConfig{cfg.vtx_id, 1000, 20, cfg.op_channel}),
      cur_op_(op_from_rung(ctrl_.op())) {}

void VrxController::on_video(double now_ms) { rz_.feed_video(now_ms); }

void VrxController::on_rc_frame(const uint8_t* buf, size_t len, double now_ms) {
  if (mabur::rc::frame_type(buf, len) != mabur::rc::T_DISC_ACK) return;
  auto ack = mabur::rc::parse_disc_ack(buf, len);
  if (ack && rz_.feed_disc_ack(*ack, now_ms)) {
    agreed_channel_ = ack->agreed_channel;
    ack_edge_ = true;
    peer_caps_ = ack->chip_caps;
    peer_acked_ = true;
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
    cur_op_ = OpPoint{false, cfg_.pin_mcs, 20, false,
                     cfg_.pin_overhead_base, cfg_.pin_overhead_enh, 0.0};
  } else if (ctrl_.on_tick(now_ms)) {
    cur_op_ = op_from_rung(ctrl_.op());
  }
  const VrxAction act = rz_.tick(now_ms);
  if (act == VrxAction::Beacon)
    return Out{mabur::rc::pack_disc(rz_.beacon()), true};
  if (act != VrxAction::TxFeedback) return std::nullopt;

  // Fix (a): SESSION keep-alive DISC, replacing this tick's RCF slot.
  // Fast cadence until the peer's caps are known (stale-caps fix).
  const int keepalive_ms =
      peer_acked_ ? cfg_.beacon_keepalive_ms : cfg_.unacked_keepalive_ms;
  if (now_ms - last_keepalive_ms_ >= keepalive_ms) {
    last_keepalive_ms_ = now_ms;
    return Out{mabur::rc::pack_disc(rz_.beacon()), true};
  }
  if (now_ms - last_fb_ms_ < cfg_.feedback_ms) return std::nullopt;
  last_fb_ms_ = now_ms;

  if (cfg_.pin_mcs < 0) {
    if (ctrl_.update(health, now_ms)) cur_op_ = op_from_rung(ctrl_.op());
    apply_overhead_policy(health, now_ms);
  }
  mabur::rc::Rcf r = build_rcf();
  // No repeat copies of an op-changing RCF: the 2026-08-14 repeat burst
  // (3 copies 10 ms apart) was removed 2026-09-05 -- with the RCF slotter
  // the drone hears ~90 % of single sends, and the slotter released the
  // three copies as one batch at an AU completion, which overran the
  // inter-AU idle and killed the next aggregate on both cards on a quarter
  // to a third of all rung changes (bench A/B, ctl-0296/0298/0300,
  // docs/switch-loss-findings-2026-09-05.md).
  note_cmd(r);
  return Out{mabur::rc::pack_rcf(r), false};
}

// Tier 1 (docs/link-adaptation-v2-proposal.md §3): replace the rung's
// constant overhead pair with one sized to the loss actually being measured.
// Runs AFTER the rung is settled for the tick, so the policy always sees the
// op it is modifying.
//
// Fed with RAW pre-FEC loss, never the controller's `u` -- u's denominator is
// this very actuator, so driving the loop with it would make the loop
// converge on nothing (trap 1 in overhead_policy.h). health.pre_fec_loss and
// health.s3_pre_fec_loss are both raw fractions, which is exactly what
// feed() wants.
void VrxController::apply_overhead_policy(const LinkHealth& health,
                                          double now_ms) {
  // A window with no traffic measured no loss; feeding its 0.0 would walk
  // both layers down to the floor on silence, which is the opposite of what
  // silence should do.
  if (!health.sample_valid) return;
  const double b = ov_base_.feed(health.pre_fec_loss, cur_op_.overhead_base,
                                 now_ms);
  // The enh layer only has a measurement when its own window saw traffic --
  // it is shed under congestion and under the air gate, and s3_usable()
  // treats that silence as "no information", never as loss. Hold its
  // overhead where it is rather than reading the shed as a clean link.
  const double e = health.s3_valid
                       ? ov_enh_.feed(health.s3_pre_fec_loss,
                                      cur_op_.overhead_enh, now_ms)
                       : cur_op_.overhead_enh;
  if (!cfg_.overhead.enable) return;
  // Operator rule (uep-base-protection-constraint): base protection must
  // never fall below enh. Applied here, across the two independent policies,
  // rather than inside either -- and by RAISING base, never by lowering enh,
  // so enforcing the invariant can never strip protection from a layer.
  cur_op_.overhead_base = std::max(b, e);
  cur_op_.overhead_enh = e;
}

mabur::rc::Rcf VrxController::build_rcf() {
  seq_ = static_cast<uint16_t>(seq_ + 1);
  mabur::rc::Rcf r;
  r.vtx_id = cfg_.vtx_id;
  r.seq = seq_;
  r.profile = mabur::rc::encode_profile(
      cur_op_.vht ? mabur::rc::PhyMode::VHT : mabur::rc::PhyMode::HT,
      static_cast<uint8_t>(cur_op_.mcs), static_cast<uint8_t>(cur_op_.bw));
  r.fec_overhead_base = cur_op_.overhead_base;
  r.fec_overhead_enh = cur_op_.overhead_enh;
  // Probe stream MCS (spec 2026-09-04): the ladder names a rung to probe on
  // every RCF, or none. In static-pin mode the ladder is out of the loop, so
  // the probe follows the dedicated pin instead.
  r.probe_profile = mabur::rc::kNoProbeProfile;
  const auto mode = cur_op_.vht ? mabur::rc::PhyMode::VHT : mabur::rc::PhyMode::HT;
  if (cfg_.pin_mcs >= 0) {
    if (cfg_.probe_pin_mcs >= 0)
      r.probe_profile = mabur::rc::encode_profile(mode, static_cast<uint8_t>(cfg_.probe_pin_mcs),
                                                  static_cast<uint8_t>(cur_op_.bw));
  } else if (const int pr = ctrl_.probe_rung(); pr >= 0) {
    r.probe_profile = mabur::rc::encode_profile(
        mode, static_cast<uint8_t>(cfg_.ladder.ladder[static_cast<std::size_t>(pr)].mcs),
        static_cast<uint8_t>(cur_op_.bw));
  }
  return r;
}

void VrxController::note_cmd(const mabur::rc::Rcf& r) {
  last_cmd_probe_profile_ = r.probe_profile;
  last_cmd_probe_profile_dn_ = r.probe_profile_dn;
}

const OpPoint& VrxController::cur_op() const { return cur_op_; }
VrxState VrxController::link_state() const { return rz_.state(); }
uint16_t VrxController::rcf_seq() const { return seq_; }

}  // namespace maburgs
