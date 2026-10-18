#include "channel_core.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "hop_blank.h"
#include "hop_burst_gate.h"
#include "pair_pick.h"
#include "scout_pick.h"
#include "width_resync.h"

namespace maburgs {

std::string ChannelCore::logf_(const char* fmt, ...) {
  char b[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  return std::string(b);
}

ChannelCore::ChannelCore(ChannelCoreCfg cfg, std::vector<LinkCard*> cards, VrxController& vrx,
                         ChannelSink& sink, StoreFn store, NowMsFn now_ms, NowUsFn now_us,
                         SleepFn sleep_ms)
    : cfg_(std::move(cfg)), cards_(std::move(cards)), vrx_(vrx), sink_(sink),
      store_(std::move(store)), now_ms_(std::move(now_ms)), now_us_(std::move(now_us)),
      sleep_(std::move(sleep_ms)),
      n_cards_(static_cast<int>(cards_.size())),
      pinned_(cfg_.radio.pin.has_value()),
      reactive_(!cfg_.radio.pin.has_value()),
      saved_op_(cfg_.start_ch),
      plan_(ChannelPlanCfg{cfg_.start_ch, cfg_.radio.channels, n_cards_,
                           cfg_.radio.scan.search_after_ms}),
      boot_pick_(false),
      verdict_(cfg_.hop, cfg_.radio.scan.busy, n_cards_),
      ranker_(cfg_.hop, cfg_.radio.scan.busy, cfg_.radio.channels, 0),
      hopc_(cfg_.hop),
      cur_ch_(static_cast<size_t>(n_cards_), cfg_.start_ch),
      home_w_(home_width(cfg_.radio.width)),
      width_dynamic_(cfg_.radio.width == 80),
      can_widen_(static_cast<size_t>(n_cards_), false),
      width_fail_want_(static_cast<size_t>(n_cards_), 0),
      energy_last_(static_cast<size_t>(n_cards_)),
      dwell_stats_(static_cast<size_t>(n_cards_)),
      window_prev_(static_cast<size_t>(n_cards_)),
      window_prev_crc_(static_cast<size_t>(n_cards_), 0),
      window_prev_ok_(static_cast<size_t>(n_cards_), false),
      window_prev_ms_(static_cast<size_t>(n_cards_), 0),
      dwell_gen_(static_cast<size_t>(n_cards_)),
      tune_gen_(static_cast<size_t>(n_cards_)),
      card_blocked_(static_cast<size_t>(n_cards_)) {
  gs_start_ms_ = now_ms_();
  can_scout_.assign(static_cast<size_t>(n_cards_), false);
  can_sweep_.assign(static_cast<size_t>(n_cards_), false);
  for (int i = 0; i < n_cards_; ++i)
    can_sweep_[static_cast<size_t>(i)] = cards_[static_cast<size_t>(i)]->can_sweep();
  snr_ok_.assign(static_cast<size_t>(n_cards_), true);
  for (int i = 0; i < n_cards_; ++i)
    can_widen_[static_cast<size_t>(i)] =
        width_dynamic_ && cards_[static_cast<size_t>(i)]->max_width() >= 80;
  for (int i = 0; i < n_cards_; ++i) {
    can_scout_[static_cast<size_t>(i)] = cards_[static_cast<size_t>(i)]->can_scout();
    snr_ok_[static_cast<size_t>(i)] = cards_[static_cast<size_t>(i)]->caps().snr_ok;
  }
  one_card_ = cfg_.n_usb == 1;
  scout_card_ = pick_boot_scout(can_scout_);
  // Relay-only roster (no USB card): the last ready-able relay
  // still searches the set (DISC bursts, no energy reads). measure = false
  // below; the pick never opens. Scoped to cfg_.n_usb == 0 to agree with
  // scan_disc_targets's own n_usb == 0 special case -- with any USB card
  // present, RadioFrontend::can_scout() is statically true and
  // pick_boot_scout never returns -1, so this never fires there anyway.
  if (scout_card_ < 0 && cfg_.n_usb == 0 && n_cards_ > 0) {
    scout_card_ = n_cards_ - 1;
    relay_search_only_ = true;
  }
  if (scout_card_ >= 0) {
    ScoutCfg sc;
    sc.channels = cfg_.radio.channels;
    sc.measure = !pinned_ && !relay_search_only_;
    sc.dwell_ms = cfg_.radio.scan.dwell_ms;
    sc.settle_ms = cfg_.radio.scan.settle_ms;
    sc.min_rounds = cfg_.radio.scan.min_rounds;
    sc.search_ms = cfg_.radio.scan.search_ms;
    sc.op_window_ms = cfg_.radio.scan.op_window_ms;
    sc.beacon_period_ms = 20;
    sc.one_card_ms = cfg_.radio.scan.one_card_ms;
    sc.pick_margin = static_cast<uint32_t>(cfg_.radio.scan.pick_margin);
    sc.pick_busy_margin_pct = cfg_.radio.scan.pick_busy_margin_pct;
    sc.one_card = one_card_ || relay_search_only_;   // a sole relay interleaves op windows like a sole USB card
    sc.link_width_mhz = home_w_;   // the boot scout parks at the home width; 80 is per-rung
    sc.busy_dbm = cfg_.radio.scan.busy.busy_dbm;
    sc.blocked_pct = cfg_.radio.scan.busy.blocked_pct;
    sc.leak_per_frame = cfg_.leak_per_frame;
    scout_ = std::make_unique<ChannelScout>(
        sc, *cards_[static_cast<size_t>(scout_card_)],
        [this] { return static_cast<int64_t>(now_ms_()); },
        [this](int ms) { sleep_(ms); });
    scout_->set_op(cfg_.start_ch);
    // A search-only relay has no pick: boot_pick_ is born closed below and
    // never freezes the scout, so close the scout's pick here too. Left
    // open, pick_open() kept scout_owns_() true for life and may_send()
    // dropped every RCF on the relay -- the drone linked, then sat in
    // FAILSAFE (web GS on a CPE, auto). Search is unaffected:
    // freeze() only stops measuring, which measure=false already does.
    if (relay_search_only_) scout_->freeze();
    scout_->set_search(true);
    scout_search_req_ = true;
    // Built even for a relay scout: pick_burst_card/pick_inflight_scout key
    // off can_scout_, which is all-false for a relay-only roster, so neither
    // ever returns scout_card_ and inflight_ is never driven.
    inflight_ = std::make_unique<InflightScout>(
        InflightScoutCfg{cfg_.hop.dwell_observe_ms, cfg_.hop.dwell_period_ms,
                         cfg_.radio.channels, home_w_, cfg_.radio.scan.busy.busy_dbm},
        *cards_[static_cast<size_t>(scout_card_)],
        [this] { return static_cast<int64_t>(now_us_()); },
        [this](int ms) { sleep_(ms); });
  }
  boot_pick_ = BootPick(scout_ != nullptr && !pinned_ && !relay_search_only_);
  if (pinned_) frozen_pick_ = cfg_.start_ch;
  tx_card_now_.store(0);
  last_video_ch_ = cfg_.start_ch;   // old main.cpp: uint8_t last_video_ch = start_ch;
  op_atomic_.store(cfg_.start_ch, std::memory_order_relaxed);
  WindowSamplerCfg wsc;
  wsc.window_ms = cfg_.hop.window_ms;
  wsc.nhm_period_4us = nhm_period_4us(std::max(cfg_.hop.window_ms - 10, 1));
  wsc.busy_dbm = cfg_.radio.scan.busy.busy_dbm;
  sampler_ = std::make_unique<WindowSampler>(
      cards_, wsc,
      WindowGates{in_session_atomic_, cal_running_atomic_, op_atomic_, dwell_busy_, dwell_card_,
                  dwell_gen_, tune_gen_, card_blocked_});
}

ChannelCore::~ChannelCore() { shutdown(); }

bool ChannelCore::scout_owns_() const {
  return scout_ && (scout_->working() || scout_search_req_ || (!pinned_ && scout_->pick_open()));
}

ChannelSnapshot ChannelCore::snapshot(int tx_card) const {
  ChannelSnapshot s;
  s.channel = cards_[static_cast<size_t>(tx_card)]->channel();
  s.scan_state = (!scout_ || pinned_ || relay_search_only_) ? "off"
                 : !boot_pick_.open()        ? "frozen"
                 : boot_pick_.relocating()   ? "moving"
                                             : "scouting";
  s.scan_rounds = scout_ ? scout_->rounds() : 0;
  s.scan_pick = frozen_pick_;
  s.hop.verdict = to_string(last_verdict_out_.v);
  s.hop.evidence = last_verdict_out_.evidence;
  if (last_verdict_out_.ref_rung >= 0) s.hop.ref_rung = last_verdict_out_.ref_rung;
  s.hop.epoch = hopc_.epoch();
  s.hop.state = hop_state_name(hopc_.state());
  if (const uint8_t hc = hopc_.hop_ch(); hc != 0) s.hop.target = hc;
  s.hop.hops = hopc_.hops();
  s.hop.holds = hopc_.holds();
  s.hop.last_ms = last_hop_event_ms_;
  s.hop.sweep_timeouts = sweep_timeouts_;
  s.hop.sampler_dropped = sampler_dropped();
  s.energy = energy_last_;
  s.dwell = dwell_stats_;
  s.scout_gated_sends = scout_gated_sends_;
  return s;
}

ChannelTickOut ChannelCore::tick(const ChannelTickIn& in) {
  now_ms_cur_ = in.now_ms;
  now_ms_u_cur_ = static_cast<uint64_t>(in.now_ms);
  tx_card_now_.store(in.tx_card, std::memory_order_relaxed);
  in_session_atomic_.store(in.in_session, std::memory_order_relaxed);
  cal_running_atomic_.store(in.cal_running, std::memory_order_relaxed);
  op_bw_ = in.op_bw;
  probe_bw_ = in.probe_bw;
  in_session_ = in.in_session;
  // The scout thread starts once its card is up (threaded), or one step
  // runs here (tests).
  if (scout_ && !scout_started_ && cards_[static_cast<size_t>(scout_card_)]->ready()) {
    scout_started_ = true;
    if (cfg_.threaded) scout_thread_ = std::thread([this] { scout_->run(); });
  }
  if (!cfg_.threaded && scout_started_) run_scout_step();
  plan_.tick(in.now_ms, in.in_session || in.cal_running);
  step_scout_inputs_();
  step_boot_pick_(in);
  step_store_();
  step_gates_();
  step_hop_edge_and_window_(in);
  step_controller_(in);
  step_move_edge_(in);
  step_drains_();
  step_width_resync_();
  for (const auto& ev : plan_.take_events()) {
    sink_.move(ev);
    sink_.log(logf_("maburgs channel: %s card %d %u -> %u", to_string(ev.reason), ev.card,
                    static_cast<unsigned>(ev.from), static_cast<unsigned>(ev.to)));
  }
  step_mechanical_retune_();
  // Again at the end: a scout search or relay sweep started during this
  // tick is gated before the sampler's next pass, not a tick later.
  step_gates_();
  // s1_hop_loss is fed from the same base-sid arrival counters as the
  // assembler's s1_loss, on the same now_ms.
  // Fed at the end of tick() (old main.cpp: right after fstream.poll);
  // nothing in between touches decoder().stats(0), and the window samples
  // before this tick's add either way.
  if (in.agg) {
    const auto s1 = in.agg->decoder().stats(0);
    s1_hop_loss_.add(s1.arr_expected, s1.arr_arrived, in.now_ms);
  }
  ChannelTickOut out;
  out.dwell_busy = dwell_busy_.load();
  out.tx_frozen = tx_selection_frozen(out.dwell_busy || sweep_.on, plan_.hopping());
  return out;
}

void ChannelCore::run_scout_step() { if (scout_) scout_->run_once(); }
void ChannelCore::run_inflight_step() { inflight_body_(); }

void ChannelCore::step_gates_() {
  op_atomic_.store(plan_.op(), std::memory_order_relaxed);
  for (int i = 0; i < n_cards_; ++i)
    card_blocked_[static_cast<size_t>(i)].store(
        (scout_owns_() && i == scout_card_) || (sweep_.on && sweep_.card == i),
        std::memory_order_release);
}

void ChannelCore::run_sampler_step() { sampler_->step_now(now_ms_u_cur_); }

void ChannelCore::on_card_died(int card) {
  if (sweep_.on && sweep_.card == card) sweep_ = PendingSweep{};   // no result coming; not a timeout
  if (scout_ && card == scout_card_ && !scout_card_down_) {
    scout_card_down_ = true;
    if (scout_->working()) {
      scout_card_died_seen_ = true;
      sink_.log(logf_("maburgs channel: scout card %d died at %llu rounds; search held "
                      "until it reopens, card rejoins at %u MHz",
                      card, static_cast<unsigned long long>(scout_->rounds()),
                      static_cast<unsigned>(home_w_)));
    }
    scout_search_req_ = false;
    scout_->set_search(false);
  }
}

void ChannelCore::on_card_reopened(int card) {
  cur_ch_[static_cast<size_t>(card)] = cards_[static_cast<size_t>(card)]->channel();
  width_fail_want_[static_cast<size_t>(card)] = 0;
  if (card == scout_card_) scout_card_down_ = false;
}

void ChannelCore::shutdown() {
  if (shut_) return;
  shut_ = true;
  if (sampler_) sampler_->stop();
  scout_run_.store(false);
  if (inflight_started_ && scout_thread2_.joinable()) scout_thread2_.join();
  if (scout_) scout_->stop();
  if (scout_started_ && scout_thread_.joinable()) scout_thread_.join();
  if (width_thread_.joinable()) width_thread_.join();
}
void ChannelCore::on_rc_body(uint8_t rx_ch) { rc_body_rx_ch_ = rx_ch; }

void ChannelCore::on_session_opened(const mabur::rc::DiscAck& ack, double now_ms) {
  // The link forms where the drone is found. Only the ack
  // that OPENED the session -- our nonce, not key-mismatch flagged, so a
  // stranger's drone cannot drag the cards -- and only on the rx_channel of
  // this body. plan.link_found() moves op to it for every card; the
  // relocation to plan.want(), if any, is an ordinary hop order (BootPick).
  const uint8_t x = rc_body_rx_ch_;
  if (ack.vrx_nonce == vrx_.rz_nonce() && !(ack.flags & mabur::rc::kAckKeyMismatch) &&
      x != 0 && x != plan_.op() && plan_.member(x) && !plan_.hopping()) {
    sink_.log(logf_("maburgs channel: drone found on %u (op %u): the link forms there",
                    static_cast<unsigned>(x), static_cast<unsigned>(plan_.op())));
    plan_.link_found(now_ms, x);
    vrx_.set_proposal(plan_.op());   // a DISC proposes the channel it is sent on: stay
  }
}

std::vector<int> ChannelCore::disc_targets(int tx) const {
  if (!scout_owns_()) return {tx};
  std::vector<bool> ready(static_cast<size_t>(n_cards_));
  for (int i = 0; i < n_cards_; ++i) ready[static_cast<size_t>(i)] = cards_[static_cast<size_t>(i)]->ready();
  return scan_disc_targets(cfg_.n_usb, n_cards_, scout_card_, scout_->beaconing(), ready);
}

bool ChannelCore::may_send(int card) const {
  if (scout_owns_() && ((card == scout_card_ && !scout_->beaconing()) || scout_->quiet())) {
    ++scout_gated_sends_;   // mutable: the gate is a query the caller makes before sending
    return false;
  }
  return true;
}

std::vector<uint8_t> ChannelCore::disc_for_card(const std::vector<uint8_t>& frame, int card) const {
  // A DISC proposes the channel it is sent on.
  const uint8_t ch = cards_[static_cast<size_t>(card)]->channel();
  if (plan_.member(ch)) return disc_for_channel(frame, ch, cfg_.key);
  return frame;
}

void ChannelCore::note_sent(bool sent_ok, bool is_rcf) {
  if (sent_ok) ++ctrl_sent_total_;
  if (is_rcf) ++rcf_sent_total_;
}
void ChannelCore::freeze_pick_(double t, const char* why) {
  if (!scout_) return;
  scout_->freeze();
  const int mr = (one_card_ && scout_->prelude_done() &&
                  scout_->rounds() < static_cast<uint64_t>(cfg_.radio.scan.min_rounds))
                     ? 1
                     : cfg_.radio.scan.min_rounds;
  const auto all = scout_->ranking();
  bool any = false;
  if (home_w_ == 40) {
    any = any_pair_ranked(all, cfg_.radio.channels, mr);
  } else {
    for (const auto& e : all) any = any || e.visits >= static_cast<uint32_t>(mr);
  }
  const uint8_t pick = plan_.want();
  if (any) ranker_.set_boot_pick(pick);
  sink_.pick(t, any ? std::optional<uint8_t>(pick) : std::nullopt, scout_->rounds(), all, mr);
  sink_.log(logf_("maburgs channel: pick frozen on %u (%s) after %llu rounds",
                  static_cast<unsigned>(pick), why,
                  static_cast<unsigned long long>(scout_->rounds())));
}

void ChannelCore::apply_hop_action_(const HopAction& act, double now_ms) {
  switch (act.kind) {
    case HopAction::Order:
      verdict_.forget_loss();   // the verify window judges the target on its own loss
      vrx_.set_hop(act.target, act.epoch);
      vrx_.restore_rung(act.restore_rung, now_ms);
      vrx_.blank_store(now_ms + cfg_.hop.confirm_ms + 150.0);
      // Two-card: retune the lead now. One-card (lead < 0): the sole radio
      // stays on the OLD channel while the order rides one_card_repeats
      // RCFs; OneCardRetune below moves it.
      if (act.lead_card >= 0) plan_.hop_order(now_ms, act.target, act.lead_card);
      break;
    case HopAction::OneCardRetune:
      plan_.hop_order(now_ms, act.target, -1);
      break;
    case HopAction::Confirm:
      plan_.hop_confirmed(now_ms);
      break;
    case HopAction::Withdraw:
      vrx_.set_hop(act.target, act.epoch);
      plan_.hop_withdraw(now_ms);
      break;
    case HopAction::VerifyPass:
      verdict_.reset();   // thaw the frozen references
      break;
    case HopAction::Hold:
    case HopAction::None:
      break;
  }
}

void ChannelCore::dispatch_hop_action_(const HopAction& act, bool relocate_tick, double now_ms) {
  apply_hop_action_(act, now_ms);
  boot_pick_.note_hop_action(act.kind, relocate_tick);
  if (auto b = hop_verdict_loss_blank_until(act, now_ms)) s1_hop_loss_.blank_until(*b);
  switch (act.kind) {
    case HopAction::Order:
      hopping_atomic_.store(true);
      rcf_sent_at_order_ = rcf_sent_total_;
      break;
    case HopAction::Confirm:
    case HopAction::Withdraw:
      hopping_atomic_.store(false);
      break;
    default:
      break;
  }
  for (const auto& e : hopc_.take_events()) {
    sink_.hop(e);
    sink_.log(logf_("maburgs hop: %s epoch %u target %u score %u +%.0f ms", e.kind.c_str(),
                    e.epoch, e.target, e.score, e.elapsed_ms));
    last_hop_event_ms_ = static_cast<uint64_t>(e.elapsed_ms >= 0 ? e.elapsed_ms : 0.0);
  }
}

void ChannelCore::step_scout_inputs_() {
  if (!scout_) { scout_working_atomic_.store(false, std::memory_order_relaxed); return; }
  scout_->set_op(plan_.op());
  scout_search_req_ = plan_.release_scout() && !scout_card_down_ &&
                      !(dwell_busy_.load() && dwell_card_.load() == scout_card_);
  scout_->set_search(scout_search_req_);
  scout_->set_tx_frames(ctrl_sent_total_);
  const bool w = scout_owns_();
  if (scout_was_working_ && !w) {
    cur_ch_[static_cast<size_t>(scout_card_)] = cards_[static_cast<size_t>(scout_card_)]->channel();
    width_fail_want_[static_cast<size_t>(scout_card_)] = 0;
  }
  scout_was_working_ = w;
  scout_working_atomic_.store(scout_owns_(), std::memory_order_relaxed);
}

void ChannelCore::step_boot_pick_(const ChannelTickIn& in) {
  const double now_ms = in.now_ms;
  const bool linked = in.in_session || in.cal_running;
  BootPickIn bi;
  bi.now_ms = now_ms;
  bi.since_start_ms = now_ms_u_cur_ - gs_start_ms_;
  bi.max_ms = cfg_.radio.scan.max_ms;
  bi.in_session = in.in_session;
  bi.cal_running = in.cal_running;
  bi.one_card = one_card_;
  if (scout_) {
    bi.scout_mature = scout_->mature();
    bi.scout_op_ranked = scout_->op_ranked();
    bi.scout_prelude_done = scout_->prelude_done();
    bi.proposal = scout_->proposal();
  }
  bi.scout_owns = scout_owns_();
  bi.op = plan_.op();
  bi.want = plan_.want();
  bi.relocate_due = plan_.relocate_due();
  bi.plan_hopping = plan_.hopping();
  bi.hop_active = hop_active(in.in_session, in.cal_running);
  bi.hop_idle_or_hold = hopc_.state() == HopState::Idle || hopc_.state() == HopState::Hold;
  bi.link_edge = link_edge_seen_;
  bi.scout_card_died = scout_card_died_seen_;
  link_edge_seen_ = false;
  scout_card_died_seen_ = false;
  pending_relocate_.reset();
  last_bi_ = bi;
  const BootPickOut bo = boot_pick_.tick(bi);
  switch (bo.kind) {
    case BootPickOut::Commit:
      if (bo.ch != plan_.op())
        sink_.log(logf_("maburgs channel: commit %u -> %u (no link)",
                        static_cast<unsigned>(plan_.op()), static_cast<unsigned>(bo.ch)));
      plan_.commit(now_ms, bo.ch);
      freeze_pick_(now_ms, bo.reason);
      break;
    case BootPickOut::AckPrelude:
      sink_.log(logf_("maburgs channel: one-card prelude ranking picks %u (op %u)%s",
                      static_cast<unsigned>(bi.proposal), static_cast<unsigned>(plan_.op()),
                      linked ? ", linked: not committed" : ""));
      if (bo.ch != 0) plan_.commit(now_ms, bo.ch);
      scout_->ack_prelude(plan_.op());
      break;
    case BootPickOut::WantPick:
      plan_.set_want(now_ms, bo.ch);
      scout_->freeze();
      sink_.log(logf_("maburgs channel: boot pick wants %u, link on %u: relocating",
                      static_cast<unsigned>(bo.ch), static_cast<unsigned>(plan_.op())));
      break;
    case BootPickOut::Relocate:
      pending_relocate_ = bo.ch;
      break;
    case BootPickOut::Freeze:
      if (bo.accept_op) plan_.set_want(now_ms, plan_.op());
      freeze_pick_(now_ms, bo.reason);
      frozen_pick_ = plan_.want();
      break;
    case BootPickOut::AcceptOp:
      sink_.log(logf_("maburgs channel: relocation to %u did not land; staying on %u",
                      static_cast<unsigned>(plan_.want()), static_cast<unsigned>(plan_.op())));
      plan_.set_want(now_ms, plan_.op());
      break;
    case BootPickOut::None:
      break;
  }
  if (bo.kind == BootPickOut::Commit) frozen_pick_ = plan_.want();
}

void ChannelCore::step_store_() {
  if (plan_.op() != saved_op_) {
    saved_op_ = plan_.op();
    vrx_.set_proposal(plan_.op());
    if (!store_(saved_op_))
      sink_.log(logf_("maburgs channel: could not write %s", cfg_.store_name.c_str()));
  }
}

void ChannelCore::step_hop_edge_and_window_(const ChannelTickIn& in) {
  const double now_ms = in.now_ms;
  const uint64_t now_ms_u = now_ms_u_cur_;
  const bool active = hop_active(in.in_session, in.cal_running);
  if (active != hop_was_active_) {
    hop_was_active_ = active;
    if (!active) dispatch_hop_action_(hopc_.on_session_lost(now_ms, plan_.op()), false, now_ms);
    verdict_.reset();
    last_verdict_ = Verdict::Healthy;
    last_verdict_out_ = VerdictOut{};
    std::fill(window_prev_ok_.begin(), window_prev_ok_.end(), false);
    recovered_prev_window_ = in.agg ? in.agg->decoder().stats(0).syms_recovered +
                                          in.agg->decoder().stats(1).syms_recovered
                                    : 0;
    au_seq_prev_ = au_seq_.load(std::memory_order_relaxed);
    verdict_.new_session();
    sampler_->reset(now_ms_u);
  }
  if (!active) return;
  if (cfg_.threaded) {
    // One thread for the window's card reads, started once the hop is
    // live; it stays up across sessions and samples only while active.
    if (!sampler_started_) {
      sampler_started_ = true;
      sampler_->start(now_ms_, sleep_);
    }
  } else {
    sampler_->step(now_ms_u);
  }
  const auto rec = sampler_->take();
  if (!rec) return;
  const uint64_t t_ms = rec->t_ms;   // the sample instant: every per-window span is measured to it
  std::vector<VerdictCardIn> vc(static_cast<size_t>(n_cards_));
  int starved_valid = 0;
  bool starved_all_zero = true;
  for (int i = 0; i < n_cards_; ++i) {
    const size_t si = static_cast<size_t>(i);
    const CardSample& cs = rec->cards[si];
    if (!cs.valid || cs.channel != plan_.op()) {
      window_prev_ok_[si] = false;
      continue;
    }
    const uint64_t crc_fail = in.agg ? in.agg->card(i).crc_fail : 0;
    const double rssi_raw = in.agg ? in.agg->card(i).rssi_a_ema : 0.0;
    const double snr_raw = in.agg ? in.agg->card(i).snr_ema : 0.0;
    if (window_prev_ok_[si]) {
      vc[si].valid = true;
      ++starved_valid;
      if (cs.frames.own - window_prev_[si].own != 0) starved_all_zero = false;
      vc[si].fa = cs.energy.fa_ofdm;
      vc[si].cca = cs.energy.cca_ofdm;
      vc[si].foreign = static_cast<uint32_t>(cs.frames.foreign - window_prev_[si].foreign);
      vc[si].crc_fail = static_cast<uint32_t>(crc_fail - window_prev_crc_[si]);
      vc[si].rssi_dbm = rssi_raw_to_dbm(rssi_raw);
      vc[si].snr_db = snr_ok_[si] ? snr_raw_to_db(snr_raw) : std::nan("");
      vc[si].snr_valid = snr_ok_[si];
      const double win_us = static_cast<double>(t_ms - window_prev_ms_[si]) * 1000.0;
      const double own_pct = win_us > 0
          ? std::min(100.0, 100.0 * static_cast<double>(cs.frames.own_air_us - window_prev_[si].own_air_us) / win_us)
          : 0.0;
      // A relay reports op-channel airtime through SURVEY (busy %, rx %);
      // rx ~ our own video on the op, so it plays own_air.
      const auto busy_pct = cs.survey.valid ? std::optional<double>(cs.survey.busy_pct) : cs.nhm_busy_pct;
      const double own_used = cs.survey.valid ? cs.survey.rx_pct : own_pct;
      vc[si].busy_valid = busy_pct.has_value();
      vc[si].nhm_busy_pct = busy_pct.value_or(0.0);
      vc[si].own_air_pct = own_used;
      energy_last_[si] = StatsEnergyIn{cs.energy.cca_ofdm, cs.energy.fa_ofdm, cs.frames.own - window_prev_[si].own,
                                       cs.frames.foreign - window_prev_[si].foreign, std::nullopt, busy_pct, own_used};
    }
    window_prev_[si] = cs.frames;
    window_prev_crc_[si] = crc_fail;
    window_prev_ms_[si] = t_ms;
    window_prev_ok_[si] = true;
  }
  VerdictLinkIn vl;
  const auto s1 = s1_hop_loss_.sample(now_ms);
  vl.pre_fec_loss = s1.valid ? s1.loss : 0.0;
  vl.loss_valid = s1.valid;
  const uint64_t recovered_now = in.agg ? in.agg->decoder().stats(0).syms_recovered +
                                              in.agg->decoder().stats(1).syms_recovered
                                        : 0;
  vl.recovered = static_cast<uint32_t>(recovered_now - recovered_prev_window_);
  recovered_prev_window_ = recovered_now;
  vl.starved = starved_valid > 0 && starved_all_zero;
  const uint64_t au_now = au_seq_.load(std::memory_order_relaxed);
  vl.au_count = static_cast<uint32_t>(au_now - au_seq_prev_);
  au_seq_prev_ = au_now;
  const auto vo = verdict_.window(now_ms, vc, vl, vrx_.ctl().rung());
  if (vo.v != Verdict::Healthy || vo.v != last_verdict_) sink_.verdict(now_ms, vo, vc, vl);
  last_verdict_ = vo.v;
  last_verdict_out_ = vo;
  if (const auto blank = hop_store_blank_until(vo, reactive_, cfg_.hop.confirm_ms))
    vrx_.blank_store(*blank);
}

void ChannelCore::step_controller_(const ChannelTickIn& in) {
  const double now_ms = in.now_ms;
  if (!hop_active(in.in_session, in.cal_running)) { pending_relocate_.reset(); return; }
  poll_sweep_(now_ms);
  auto fill_hop_targets = [&](HopTick& k) {
    k.best = ranker_.best(now_ms, plan_.op(), hopc_.backed_off(now_ms), /*require_unblocked=*/true);
    k.escape = ranker_.best(now_ms, plan_.op(), hopc_.backed_off_failed(now_ms), /*require_unblocked=*/true);
    k.best_score = 0;
    k.escape_score = 0;
    for (const auto& e : ranker_.ranking(now_ms)) {
      if (k.best && e.ch == *k.best) k.best_score = e.score;
      if (k.escape && e.ch == *k.escape) k.escape_score = e.score;
    }
  };
  HopTick ht;
  ht.now_ms = now_ms;
  ht.verdict = last_verdict_out_;
  ht.cur_op = plan_.op();
  if (hopc_.state() != HopState::Ordered) {
    std::vector<bool> ready(static_cast<size_t>(n_cards_));
    for (int i = 0; i < n_cards_; ++i) ready[static_cast<size_t>(i)] = cards_[static_cast<size_t>(i)]->ready();
    hop_lead_latched_ = pick_hop_lead(ready, in.tx_card);
  }
  ht.lead_card = hop_lead_latched_;
  ht.n_cards = hop_lead_latched_ >= 0 ? n_cards_ : 1;
  fill_hop_targets(ht);
  ht.video_on_target = plan_.hopping() && last_video_ch_ == plan_.hop_target();
  ht.rcf_sent_since_order = static_cast<int>(rcf_sent_total_ - rcf_sent_at_order_);
  const bool relocation_owns = boot_pick_.relocating() || pending_relocate_.has_value() ||
                               boot_pick_.relocation_pending(last_bi_);
  // Sweep-capable is per tick: a relay that is down, booting or owned by
  // another client cannot take a SCAN (RemoteCard::start_sweep refuses it),
  // and picking it would starve the USB card of its burst. The static
  // can_sweep_ only says "speaks v4".
  std::vector<bool> sweepable(static_cast<size_t>(n_cards_));
  for (int i = 0; i < n_cards_; ++i)
    sweepable[static_cast<size_t>(i)] = can_sweep_[static_cast<size_t>(i)] && cards_[static_cast<size_t>(i)]->ready();
  const int burst_card = pick_burst_card(can_scout_, sweepable, in.tx_card);
  const bool relay_burst = burst_card >= 0 && !can_scout_[static_cast<size_t>(burst_card)] &&
                           sweepable[static_cast<size_t>(burst_card)];
  const int burst_period = relay_burst ? cfg_.hop.relay_burst_period_ms : cfg_.hop.dwell_period_ms;
  if (reactive_ && !scout_owns_() && !relocation_owns && !sweep_.on &&
      hop_burst_due(hopc_.state(), last_verdict_out_.trigger, now_ms, last_burst_ms_, burst_period)) {
    const double prev_burst_ms = last_burst_ms_;
    last_burst_ms_ = now_ms;
    if (relay_burst) {
      // Async: one SCAN for the set minus op; poll_sweep_ ranks the result.
      std::vector<uint8_t> chans;
      for (uint8_t c : cfg_.radio.channels) if (c != plan_.op()) chans.push_back(c);
      if (!chans.empty() &&
          cards_[static_cast<size_t>(burst_card)]->start_sweep(chans, kSweepPasses, kSweepObserveMs)) {
        const double need = static_cast<double>(kSweepPasses) * static_cast<double>(chans.size()) *
                                (kSweepObserveMs + kSweepPerChannelMs) + kSweepSlackMs;
        sweep_ = PendingSweep{true, burst_card, now_ms, std::max(kSweepTimeoutMinMs, need)};
      } else {
        // Nothing ran (a ready->down race, or a one-member set): the burst
        // is not spent. ready() gates sweepable, so this is rare; retrying
        // next tick is cheaper than a fall-through second pick.
        last_burst_ms_ = prev_burst_ms;
      }
      fill_hop_targets(ht);
    } else if (burst_card < 0 || !inflight_) {
      fill_hop_targets(ht);
    } else {
      std::lock_guard<std::mutex> ilk(inflight_mu_);
      auto& fe = *cards_[static_cast<size_t>(burst_card)];
      inflight_->set_radio(fe);
      // The burst leaves the op channel and returns to it, so only the
      // generation (not the channel number) can mark an NHM arm as stale
      // if it spans the burst; bump before and after so a read on either
      // side of the retune-away sees a generation newer than its arm.
      // The burst holds the card's device-use lock throughout: the
      // sampler's op-channel check cannot see the retune-away (the card
      // stores its channel only after each retune returns), and a pass
      // landing mid-burst would cut the dwells' FA/CCA delta and re-arm
      // NHM over theirs. Taken after inflight_mu_, the order every holder
      // of both follows.
      auto hold = sampler_->hold_card(static_cast<size_t>(burst_card));
      tune_gen_[static_cast<size_t>(burst_card)].fetch_add(1, std::memory_order_release);
      std::vector<ScoutDwell> recs;
      for (const auto& v : inflight_->burst(plan_.op(), recs)) ranker_.add(v);
      cur_ch_[static_cast<size_t>(burst_card)] = fe.channel();
      tune_gen_[static_cast<size_t>(burst_card)].fetch_add(1, std::memory_order_release);
      hold.unlock();
      for (const auto& d : recs) sink_.dwell(now_ms, burst_card, d);
      fill_hop_targets(ht);
    }
  }
  // A relay sweep in flight: the ranking is about to change; holding the
  // trigger back keeps the controller out of hold_exhausted until it lands.
  if (sweep_.on) {
    ht.verdict.trigger = false;
    ht.best.reset();
    ht.escape.reset();
  }
  // Pinned: the verdict is still measured (OSD/sideport read it) but the
  // controller never sees a reactive trigger, in any arm below -- no order,
  // no escape, no exhausted hold. Relocation re-arms it on its own tick.
  if (!reactive_) ht.verdict.trigger = false;
  if (scout_owns_()) {
    ht.best.reset();
    ht.escape.reset();
  } else if (pending_relocate_) {
    ht.verdict.trigger = true;
    ht.relocate = true;
    ht.no_verify = !reactive_;   // onto the pin: the confirm lands it, no verify
    ht.best = *pending_relocate_;
    ht.best_score = 0;
    ht.escape.reset();
    ht.escape_score = 0;
  } else if (relocation_owns) {
    ht.best.reset();
    ht.escape.reset();
  } else if (!reactive_) {
    ht.best.reset();
    ht.escape.reset();
  }
  const HopAction act = hopc_.tick(ht);
  dispatch_hop_action_(act, ht.relocate, now_ms);
  if (ht.relocate)
    sink_.log(logf_("maburgs channel: relocate %u -> %u %s", static_cast<unsigned>(plan_.op()),
                    static_cast<unsigned>(*pending_relocate_),
                    act.kind == HopAction::Order ? "placed" : "refused (hop cap)"));
  pending_relocate_.reset();
}

void ChannelCore::step_move_edge_(const ChannelTickIn& in) {
  const double now_ms = in.now_ms;
  vrx_.set_proposal(plan_.op());
  vrx_.set_keepalive_hold(hopc_.state() == HopState::Ordered);
  // The edge is held through a calibration run AND through a hop in flight
  // (relocate or reactive) and replayed when both are over; agreed is read at
  // replay time (the drone's latest accepted ack). on_ack() with agreed == op
  // is a no-op (channel_plan.cpp); a member agreed != op (e.g. after a
  // withdrawn hop) moves op as a normal ack move (Commit / AckOverride).
  // Held, not consumed: an edge landing on a hopping tick would otherwise be
  // dropped.
  if (cal_move_hold_.take(vrx_.take_move_edge(), in.cal_running || plan_.hopping())) {
    const uint8_t proposed = vrx_.proposal();
    const uint8_t agreed = vrx_.agreed_channel();
    if (!plan_.member(agreed))
      sink_.log(logf_("maburgs channel: drone acked %u, not in our set; ignored",
                      static_cast<unsigned>(agreed)));
    plan_.on_ack(now_ms, agreed, proposed);
    vrx_.set_proposal(plan_.op());
    link_edge_seen_ = true;
  }
}

void ChannelCore::step_drains_() {
  if (scout_)
    for (const auto& d : scout_->take_dwells()) sink_.dwell(now_ms_cur_, scout_card_, d);
  // In-flight scout thread: started once the boot pick is closed and the
  // boot scout owns no card. Two-card, auto mode only -- pinned never ranks
  // (nothing to move to), and the spare card stays on diversity.
  if (!inflight_started_ && reactive_ && !scout_owns_() && !boot_pick_.open() && n_cards_ >= 2) {
    inflight_started_ = true;
    if (cfg_.threaded) {
      scout_run_.store(true);
      scout_thread2_ = std::thread([this] { scout_loop_(); });
    }
  }
  std::vector<std::pair<int, ScoutDwell>> drained;
  std::vector<HopVisit> visits;
  {
    std::lock_guard<std::mutex> lk(dwell_mu_);
    drained.swap(dwell_recs_);
    visits.swap(dwell_visits_);
  }
  size_t vi = 0;
  for (auto& rec : drained) {
    const int card = rec.first;
    cur_ch_[static_cast<size_t>(card)] = cards_[static_cast<size_t>(card)]->channel();
    sink_.dwell(now_ms_cur_, card, rec.second);
    const bool ok = !(rec.second.survey.flags & devourer::chanmig::kFlagRetuneFailed);
    StatsDwellIn ds = dwell_stats_[static_cast<size_t>(card)].value_or(StatsDwellIn{});
    ++ds.visits;
    ds.cost_us = static_cast<uint32_t>(rec.second.to_us + rec.second.read_us + rec.second.back_us);
    if (ok && vi < visits.size()) ds.score = HopRanker::score(visits[vi]);
    dwell_stats_[static_cast<size_t>(card)] = ds;
    if (ok) ++vi;
  }
  for (const auto& v : visits) ranker_.add(v);
}

void ChannelCore::step_width_resync_() {
  // Results first: a finished switch updates this card's bookkeeping on the
  // core thread (the worker only tunes).
  drain_width_done_();
  if (!width_resync_open(!scout_owns_(), scout_ != nullptr, /*scout_frozen=*/!boot_pick_.open())) return;
  if (width_job_) return;   // one switch in flight at a time
  if (width_thread_.joinable()) width_thread_.join();
  // What the cards should be: the home width, plus (radio.width 80) one or
  // all cards at 80 while an 80 MHz rung is probed or flown.
  const WideNeed raw = width_dynamic_ ? wide_need(in_session_, op_bw_, probe_bw_) : WideNeed::None;
  wide_need_ = held_need(wide_hold_, raw, now_ms_cur_, kWideHoldMs);
  probe_card_ = wide_need_ == WideNeed::None
                    ? -1
                    : pick_probe_card(can_widen_, tx_card_now_.load(), probe_card_);
  probe_wide_.store(wide_need_ == WideNeed::Probe, std::memory_order_relaxed);
  const std::vector<uint8_t> want = width_targets(wide_need_, can_widen_, probe_card_, home_w_, 80);
  std::vector<uint8_t> cur(static_cast<size_t>(n_cards_));
  std::vector<bool> eligible(static_cast<size_t>(n_cards_));
  for (int i = 0; i < n_cards_; ++i) {
    const size_t k = static_cast<size_t>(i);
    auto& fe = *cards_[k];
    cur[k] = fe.width();
    const bool dwelling = dwell_busy_.load() && dwell_card_.load() == i;
    eligible[k] = fe.ready() && !dwelling && width_fail_want_[k] != want[k];
  }
  const int card = next_width_switch(cur, want, eligible, tx_card_now_.load());
  if (card < 0) return;
  // Spaced: the card just switched needs a moment back on the air before
  // the next one goes deaf. The boot fix-up (home width after the scout)
  // passes straight through: nothing switched before it.
  if (now_ms_cur_ - last_width_switch_ms_ < kWidthGapMs) return;
  const uint8_t w = want[static_cast<size_t>(card)];
  if (!cfg_.threaded) {
    last_width_switch_ms_ = now_ms_cur_;
    width_job_ = true;
    run_width_switch_(card, w);
    drain_width_done_();
    return;
  }
  // A full SetMonitorChannel takes tens of ms: off the core loop, which
  // keeps decoding the other card's video meanwhile. Marked as a dwell, so
  // every step that already steps around a dwelling card (energy windows,
  // mechanical retunes, TX selection) steps around this one. Started only
  // under inflight_mu_ with no dwell running: the in-flight scout sets the
  // same flags under that mutex, and checks width_pending_ there before it
  // dwells, so the two never overwrite each other's marks.
  std::unique_lock<std::mutex> ilk(inflight_mu_, std::try_to_lock);
  if (!ilk.owns_lock() || dwell_busy_.load()) return;   // a dwell is running: a later tick
  last_width_switch_ms_ = now_ms_cur_;
  width_job_ = true;
  width_pending_.store(true);
  dwell_card_.store(card);
  dwell_busy_.store(true);
  width_thread_ = std::thread([this, card, w] { run_width_switch_(card, w); });
}

void ChannelCore::run_width_switch_(int card, uint8_t want) {
  WidthDone d{card, want, false, 0, 0};
  {
    std::lock_guard<std::mutex> ilk(inflight_mu_);
    // The card's device-use lock, after inflight_mu_ (the order every holder
    // of both follows): set_width is a full retune, and a sampler pass
    // landing mid-switch would read and re-arm NHM across it.
    auto hold = sampler_->hold_card(static_cast<size_t>(card));
    auto& fe = *cards_[static_cast<size_t>(card)];
    d.ch = fe.channel();
    const uint64_t t0 = now_us_();
    d.ok = fe.set_width(d.ch, want);
    d.us = now_us_() - t0;
    // Energy windows spanning the retune are not one measurement, and an NHM
    // arm from before it is stale.
    dwell_gen_[static_cast<size_t>(card)].fetch_add(1, std::memory_order_release);
    tune_gen_[static_cast<size_t>(card)].fetch_add(1, std::memory_order_release);
  }
  {
    std::lock_guard<std::mutex> lk(width_mu_);
    width_done_.push_back(d);
  }
  if (cfg_.threaded) {
    dwell_busy_.store(false);
    dwell_card_.store(-1);
    width_pending_.store(false);
  }
}

void ChannelCore::drain_width_done_() {
  std::vector<WidthDone> done;
  {
    std::lock_guard<std::mutex> lk(width_mu_);
    done.swap(width_done_);
  }
  for (const auto& d : done) {
    width_job_ = false;
    const size_t k = static_cast<size_t>(d.card);
    if (d.ok) {
      cur_ch_[k] = cards_[k]->channel();   // the NHM arm went stale with tune_gen_
      width_fail_want_[k] = 0;
    } else {
      width_fail_want_[k] = d.want;
    }
    const char* why = d.want == home_w_ ? "home" : wide_need_ == WideNeed::All ? "80 MHz rung" : "80 MHz probe";
    sink_.log(logf_("maburgs radio: card %d width -> %u MHz on ch %u %s in %llu ms (%s)", d.card,
                    static_cast<unsigned>(d.want), static_cast<unsigned>(d.ch),
                    d.ok ? "done" : "FAILED", static_cast<unsigned long long>(d.us / 1000), why));
  }
}

void ChannelCore::step_mechanical_retune_() {
  for (int i = 0; i < n_cards_; ++i) {
    const bool scouting = scout_owns_() && i == scout_card_;
    const bool inflight_dwelling = dwell_busy_.load() && dwell_card_.load() == i;
    auto& fe = *cards_[static_cast<size_t>(i)];
    if (scouting || inflight_dwelling || !fe.ready()) continue;
    const uint8_t want = plan_.desired(i);
    if (cur_ch_[static_cast<size_t>(i)] != want && fe.retune(want)) {
      cur_ch_[static_cast<size_t>(i)] = want;
      tune_gen_[static_cast<size_t>(i)].fetch_add(1, std::memory_order_release);
    }
  }
}

void ChannelCore::poll_sweep_(double now_ms) {
  if (!sweep_.on) return;
  auto& fe = *cards_[static_cast<size_t>(sweep_.card)];
  // Expiry first: a sweep left pending across a session drop is a timeout,
  // and a result that sat on the card meanwhile is taken and discarded --
  // never ranked under the reconnect's timestamp.
  if (now_ms - sweep_.sent_ms >= sweep_.timeout_ms) {
    (void)fe.take_sweep_result();
    sweep_.on = false;
    ++sweep_timeouts_;
    sink_.log(logf_("maburgs hop: relay sweep_timeout on card %d", sweep_.card));
    return;
  }
  if (auto r = fe.take_sweep_result()) {
    sweep_.on = false;
    ++sweep_round_;
    for (const auto& e : r->entries) {
      if (!e.valid) continue;
      ranker_.add(sweep_visit(e, now_ms));
      sink_.dwell(now_ms, sweep_.card, sweep_dwell(e, sweep_round_));
    }
    if (r->status != 0)
      sink_.log(logf_("maburgs hop: relay sweep status %u, radio back on %u",
                      static_cast<unsigned>(r->status), static_cast<unsigned>(r->back_channel)));
  }
}

void ChannelCore::inflight_body_() {
  // Unlike main.cpp's always-present `inflight`, `inflight_` here is a
  // unique_ptr built only alongside a scout-capable card (ctor, scout_card_
  // >= 0): null on an all-relay roster, so this guard has no equivalent there.
  if (!inflight_ || !reactive_) return;
  if (!in_session_atomic_.load() || cal_running_atomic_.load() || hopping_atomic_.load() ||
      scout_working_atomic_.load() || n_cards_ < 2)
    return;
  // One card is wide for an 80 MHz probe and is the only one that can hear
  // it: a dwell would punch holes in the probe and fail it.
  if (probe_wide_.load()) return;
  const int card = pick_inflight_scout(can_scout_, tx_card_now_.load());
  if (card < 0) return;
  auto& fe = *cards_[static_cast<size_t>(card)];
  if (!fe.ready()) return;
  std::optional<uint8_t> dwell_ch;
  {
    std::lock_guard<std::mutex> ilk(inflight_mu_);
    dwell_ch = inflight_->next_candidate(fe.channel());
  }
  if (!dwell_ch) return;
  if (cfg_.threaded) {
    const uint64_t s0 = au_seq_.load();   // align to the next AU boundary (<= 17 ms wait)
    for (int i = 0; i < 20 && au_seq_.load() == s0; ++i) sleep_(1);
  }
  std::lock_guard<std::mutex> ilk(inflight_mu_);
  if (width_pending_.load()) return;   // a card width switch owns the busy marks
  ScoutDwell d;
  HopVisit v;
  bool ok = false;
  {
    // The card's device-use lock (after inflight_mu_, as the burst takes
    // it): the sampler skips a dwelling card, but its dwell_busy check and
    // this dwell are otherwise unordered; the lock makes a pass either
    // finish before the dwell starts or start after it has ended.
    auto hold = sampler_->hold_card(static_cast<size_t>(card));
    inflight_->set_radio(fe);
    dwell_card_.store(card);
    dwell_busy_.store(true);
    ok = inflight_->dwell(*dwell_ch, fe.channel(), d, v);
    dwell_gen_[static_cast<size_t>(card)].fetch_add(1, std::memory_order_release);
    dwell_busy_.store(false);
    dwell_card_.store(-1);
  }
  std::lock_guard<std::mutex> lk(dwell_mu_);
  dwell_recs_.emplace_back(card, d);
  if (ok) dwell_visits_.push_back(v);
}

void ChannelCore::scout_loop_() {
  while (scout_run_.load()) {
    sleep_(cfg_.hop.dwell_period_ms);
    if (!scout_run_.load()) break;
    inflight_body_();
  }
}

}  // namespace maburgs
