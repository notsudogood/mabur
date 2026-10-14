#include "mabur/sw_decoder.h"

#include <algorithm>
#include <cstring>
#include <iterator>

#include "mabur/gf256.h"
#include "mabur/sw_wire.h"

namespace mabur {
namespace {
// A source seq this far from the newest is an encoder restart, not wrap
// (wrap-adjacent seqs land near 0 after int32 casting; a reboot lands
// ~2^32 away). ~1 minute of symbols at video rates.
constexpr int64_t kResetSpan = 1 << 20;
constexpr uint64_t kAnchor = 1ull << 32;  // headroom below the first vseq
uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
}  // namespace

SwDecoder::SwDecoder(const SwConfig& cfg, uint32_t seq_horizon, uint32_t arrival_guard)
    : cfg_(cfg),
      horizon_(seq_horizon ? seq_horizon : 4ull * static_cast<uint64_t>(cfg.window)),
      arr_guard_(arrival_guard ? arrival_guard : ArrivalTracker::kDefaultGuard),
      arr_(arr_guard_) {
  // Power of two above the live span (horizon_ + 1 seqs): 1024 slots at the
  // default seq_horizon 512, ~340 kB of payload at symbol_size 332.
  uint64_t ring = 64;
  while (ring < horizon_ + 2) ring <<= 1;
  ring_mask_ = ring - 1;
  known_tag_.assign(static_cast<size_t>(ring), kNoSeq);
  known_buf_.assign(static_cast<size_t>(ring) *
                        static_cast<size_t>(cfg_.symbol_size > 0 ? cfg_.symbol_size : 0),
                    0);
}

const uint8_t* SwDecoder::store_known(uint64_t v, const uint8_t* payload) {
  const size_t ss = static_cast<size_t>(cfg_.symbol_size);
  uint8_t* slot = known_buf_.data() + (v & ring_mask_) * ss;
  std::memcpy(slot, payload, ss);
  known_tag_[v & ring_mask_] = v;
  return slot;
}

uint64_t SwDecoder::unwrap(uint32_t s) const {
  const int64_t d = static_cast<int32_t>(s - static_cast<uint32_t>(newest_v_));
  return newest_v_ + static_cast<uint64_t>(d);
}

void SwDecoder::reset_state(uint64_t v) {
  std::fill(known_tag_.begin(), known_tag_.end(), kNoSeq);
  rows_.clear();
  recovered_await_src_.clear();
  retx_await_src_.clear();
  newest_v_ = v;
  base_ = v;
  arr_.reset(v);
  ++resets_;
  wm_open_ = false;
  wm_valid_ = false;
  repairs_seen_.clear();
  std::fill(repair_tab_.begin(), repair_tab_.end(), RepairSlot{});
  repair_untracked_ = 0;
  max_repair_ws_ = 0;
  ep_open_ = false;  // a half-built episode from the old seq space is meaningless
}

void SwDecoder::mark_transition() {
  if (!have_seq_) {  // no traffic yet: nothing to attribute
    wm_open_ = false;
    wm_valid_ = false;
    return;
  }
  wm_ = newest_v_;
  wm_valid_ = true;
  wm_open_ = true;
}

// The floor below which a seq is genuinely unrecoverable (older than the
// horizon behind the newest symbol seen). This — NOT base_ — is what the
// admit/drop checks must use. base_ only catches up to newest_v_ - horizon_
// after the first `horizon` symbols; for that opening window base_ sits AT
// the first source seen, so any earlier-or-reordered source (leading loss in
// a join burst, or merely a lower seq that arrived after a higher one at
// bpb=1 where each symbol is its own air frame) tested `< base_` and was
// dropped stale — never delivered, recovered, nor abandoned. Whole RTP
// packets vanished with every loss counter frozen (seen on hardware as
// bpb=1 big-symbol video corruption). Using the horizon floor admits them
// during the join window and is identical to base_ in steady state.
uint64_t SwDecoder::live_floor() const { return newest_v_ - horizon_; }

std::vector<uint32_t> SwDecoder::missing_sources(uint32_t lookback) const {
  std::vector<uint32_t> out;
  if (!have_seq_) return out;
  const uint64_t floor = live_floor();
  uint64_t from = newest_v_ > lookback ? newest_v_ - lookback : 0;
  if (from < floor) from = floor;
  if (from < base_) from = base_;
  for (uint64_t v = from; v < newest_v_; ++v)
    if (!is_known(v)) out.push_back(static_cast<uint32_t>(v));
  return out;
}

SwDecoder::SourceState SwDecoder::source_state(uint32_t wire_seq) const {
  if (!have_seq_) return SourceState::kUnknown;
  const uint64_t v = unwrap(wire_seq);
  if (v < live_floor() || v < base_) return SourceState::kBelowFloor;
  if (!is_known(v)) return SourceState::kUnknown;
  if (retx_await_src_.count(v)) return SourceState::kRetx;
  return recovered_await_src_.count(v) ? SourceState::kRecovered : SourceState::kDirect;
}

bool SwDecoder::fec_final(uint32_t wire_seq) const {
  if (!have_seq_ || max_repair_ws_ == 0) return false;
  const uint64_t v = unwrap(wire_seq);
  if (v < live_floor() || v < base_ || is_known(v)) return false;
  // Strictly past v, plus a one-symbol guard: the async FEC worker can
  // surface a repair a few positions out of order, never a whole window.
  return max_repair_ws_ > v + 1;
}

bool SwDecoder::source_covered(uint32_t wire_seq) const {
  if (!have_seq_) return false;
  const uint64_t v = unwrap(wire_seq);
  if (v < live_floor() || v < base_ || is_known(v)) return false;
  return rows_.count(v) != 0;
}

void SwDecoder::advance(uint64_t newest_candidate) {
  if (newest_candidate <= newest_v_) return;
  newest_v_ = newest_candidate;
  const uint64_t nb = newest_v_ - horizon_;  // vseqs start at 2^32 >> horizon
  if (nb <= base_) return;
  // Every seq in [base_, nb) that never became known is lost for good.
  // Evict all known seqs below nb, but only those in [base_, nb) offset the
  // abandoned count: seqs below base_ (delivered/recovered after arriving
  // reordered-earlier than the join anchor, via live_floor()) were never in
  // the [base_, nb) accounting span, so counting them would underflow it.
  uint64_t known_in_range = 0;
  uint64_t known_stale = 0;
  // Stale span: [base_, stale_end). Open boundary => the whole eviction
  // range is stale; closed+valid => seqs <= wm_; inactive => empty span.
  const uint64_t stale_end =
      wm_open_ ? nb : (wm_valid_ ? std::min(nb, wm_ + 1) : base_);
  // Loss episodes: every evicted seq the channel never delivered directly
  // (unknown, or recovered with no direct copy ever heard) is booked before
  // the state below is torn down. One seq per iteration in steady state.
  //
  // Eviction rides the same walk: a known seq in [base_, nb) is counted and
  // its slot freed. Known seqs below base_ (the reordered-earlier ones
  // above) need no walk: they are below nb = the new live floor, and every
  // lookup sits behind a live_floor guard, so their slots are already inert
  // and the next live seq that maps there overwrites them.
  for (uint64_t s = base_; s < nb; ++s) {
    const bool known = is_known(s);
    if (known) {
      ++known_in_range;
      if (s < stale_end) ++known_stale;
      forget_known(s);
    }
    const bool rec = known && recovered_await_src_.count(s);
    const bool rtx = known && retx_await_src_.count(s);
    if (known && !rec && !rtx) continue;  // heard directly
    note_missing(s, rtx ? Origin::kRetx : (rec ? Origin::kRepair : Origin::kSource),
                 s < stale_end);
  }
  settle_episodes(nb);
  syms_abandoned_ += (nb - base_) - known_in_range;
  if (stale_end > base_)
    syms_abandoned_stale_ += (stale_end - base_) - known_stale;
  // A recovered seq evicted before its direct copy showed stays "recovered"
  // for good: within the horizon the channel never delivered it.
  recovered_await_src_.erase(recovered_await_src_.begin(),
                             recovered_await_src_.lower_bound(nb));
  retx_await_src_.erase(retx_await_src_.begin(), retx_await_src_.lower_bound(nb));
  // Rows are keyed by pivot = their smallest referenced seq, so everything
  // that references an evicted seq is at the front of the map.
  while (!rows_.empty() && rows_.begin()->first < nb) rows_.erase(rows_.begin());
  base_ = nb;
}

void SwDecoder::unpack_symbol(const uint8_t* sym, std::vector<std::vector<uint8_t>>& out) {
  const size_t ss = static_cast<size_t>(cfg_.symbol_size);
  size_t pos = 0;
  while (pos + 2 <= ss) {
    const size_t ln = rd16(sym + pos);
    if (ln == 0) break;
    const size_t end = pos + 2 + ln;
    if (end > ss) break;
    out.emplace_back(sym + pos + 2, sym + end);
    ++packets_out_;
    pos = end;
  }
}

void SwDecoder::insert_row(Row r, std::vector<std::pair<uint64_t, std::vector<uint8_t>>>& solved) {
  const size_t ss = static_cast<size_t>(cfg_.symbol_size);
  for (;;) {
    if (r.coeffs.empty()) return;  // linearly dependent — nothing new
    const uint64_t pivot = r.coeffs.begin()->first;
    auto it = rows_.find(pivot);
    if (it == rows_.end()) {
      const uint8_t lead = r.coeffs.begin()->second;
      if (lead != 1) {
        const uint8_t ilead = gf::inv(lead);
        for (auto& [s, c] : r.coeffs) c = gf::mul(c, ilead);
        std::vector<uint8_t> scaled(ss, 0);
        gf::lincomb(scaled.data(), r.payload.data(), ilead, ss);
        r.payload = std::move(scaled);
      }
      if (r.coeffs.size() == 1) {
        solved.emplace_back(pivot, std::move(r.payload));
        return;
      }
      rows_.emplace(pivot, std::move(r));
      return;
    }
    // Eliminate pivot with the existing (lead-normalized) row.
    const Row& e = it->second;
    const uint8_t f = r.coeffs.begin()->second;
    for (const auto& [s, c] : e.coeffs) {
      const auto jt = r.coeffs.find(s);
      const uint8_t nv = static_cast<uint8_t>((jt == r.coeffs.end() ? 0 : jt->second) ^ gf::mul(f, c));
      if (nv)
        r.coeffs[s] = nv;
      else if (jt != r.coeffs.end())
        r.coeffs.erase(jt);
    }
    gf::lincomb(r.payload.data(), e.payload.data(), f, ss);
  }
}

void SwDecoder::ingest(uint64_t v, const uint8_t* sym, Origin origin,
                       std::vector<std::vector<uint8_t>>& out) {
  // The first symbol is read in place (no copy until store_known); only
  // repair cascades, rare, carry their own payload vectors.
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> queue;
  bool first = true;
  for (;;) {
    uint64_t s = v;
    const uint8_t* payload = sym;
    std::vector<uint8_t> held;
    if (!first) {
      if (queue.empty()) break;
      s = queue.back().first;
      held = std::move(queue.back().second);
      queue.pop_back();
      payload = held.data();
    }
    // Cascades are repair-recovered -- unless the symbol that set them off
    // was a retx: without it none of them would be known, so they inherit
    // the retx class (R20; retx_await_src_ then holds them, and a late
    // direct copy still books syms_retx_arrived).
    const Origin o =
        first ? origin : (origin == Origin::kRetx ? Origin::kRetx : Origin::kRepair);
    first = false;
    if (s < live_floor() || is_known(s)) continue;
    unpack_symbol(payload, out);
    for (size_t k = last_out_seqs_.size(); k < out.size(); ++k)
      last_out_seqs_.push_back(static_cast<uint32_t>(s));
    if (o == Origin::kSource) {
      ++syms_delivered_;
    } else if (o == Origin::kRetx) {
      ++syms_retx_;
      retx_await_src_.insert(s);
    } else {
      ++syms_recovered_;
      recovered_await_src_.insert(s);
    }
    const uint8_t* kp = store_known(s, payload);
    // Pull every row referencing s, reduce, re-insert (rows only ever
    // reference unknown seqs — this keeps that invariant).
    std::vector<Row> affected;
    for (auto it = rows_.begin(); it != rows_.end();) {
      if (it->second.coeffs.count(s)) {
        affected.push_back(std::move(it->second));
        it = rows_.erase(it);
      } else {
        ++it;
      }
    }
    for (auto& r : affected) {
      const uint8_t c = r.coeffs[s];
      gf::lincomb(r.payload.data(), kp, c, static_cast<size_t>(cfg_.symbol_size));
      r.coeffs.erase(s);
      std::vector<std::pair<uint64_t, std::vector<uint8_t>>> solved;
      insert_row(std::move(r), solved);
      for (auto& sv : solved) queue.push_back(std::move(sv));
    }
  }
}

std::vector<std::vector<uint8_t>> SwDecoder::add_symbol(const uint8_t* env, size_t len,
                                                        uint64_t now_ms, SwBoundary b,
                                                        bool clean, bool retx) {
  last_out_seqs_.clear();
  std::vector<std::vector<uint8_t>> out;
  sw::SwHeader h;
  if (!sw::parse_header(env, len, &h)) return out;
  if (h.symbol_size != cfg_.symbol_size ||
      len != sw::kSwHeaderLen + static_cast<size_t>(cfg_.symbol_size)) {
    ++symbols_dropped_bad_cfg_;
    return out;
  }
  ++symbols_in_;
  const uint8_t* payload = env + sw::kSwHeaderLen;
  const size_t ss = static_cast<size_t>(cfg_.symbol_size);

  if (!h.repair) {
    if (!have_seq_) {
      have_seq_ = true;
      newest_v_ = kAnchor + h.seq;
      base_ = newest_v_;
    }
    uint64_t v = unwrap(h.seq);
    if (v > newest_v_ + static_cast<uint64_t>(kResetSpan) ||
        v + static_cast<uint64_t>(kResetSpan) < newest_v_) {
      reset_state(kAnchor + h.seq);
      v = newest_v_;
    }
    // A retransmit is boundary-neutral: the drone re-sends old seqs at the
    // CURRENT rate, so its PHY rate says nothing about which op the seq
    // belongs to -- letting it close the boundary would leave the transition
    // gap booked as current loss (option A: retx never moves a ladder input).
    if (retx) b = SwBoundary::kNone;
    if (b == SwBoundary::kPre && wm_valid_ && v > wm_) wm_ = v;
    if (b == SwBoundary::kPost && wm_open_) {
      wm_open_ = false;
      if (wm_valid_ && v > 0 && v - 1 > wm_) wm_ = v - 1;
    }
    // Arrival accounting BEFORE the dedup/stale early-return: a second-card
    // copy still sets the heard bit (idempotent), a copy behind the settle
    // line counts late. A retransmit never touches the tracker (option A):
    // it is the drone answering a NACK, not the channel delivering.
    if (!retx) {
      arr_.on_source(v, arr_stale_end(), clean);
      arr_.advance(v, arr_stale_end());
    }
    if (v < live_floor() || is_known(v)) {
      // A retx of something we already have: nothing to learn.
      if (retx) {
        ++symbols_dropped_stale_;
        return out;
      }
      // First direct copy of a repair- or retx-filled symbol: the channel
      // did deliver it, the repair/retransmit just won the race. Not a
      // stale dup.
      if (recovered_await_src_.erase(v))
        ++syms_recovered_arrived_;
      else if (retx_await_src_.erase(v))
        ++syms_retx_arrived_;
      else
        ++symbols_dropped_stale_;
      return out;
    }
    advance(v);
    ingest(v, payload, retx ? Origin::kRetx : Origin::kSource, out);
    return out;
  }

  // Repair. Before the first source there is no seq anchor — drop (join-time
  // only; sources outnumber repairs, the next one anchors us).
  if (!have_seq_) {
    ++symbols_dropped_stale_;
    return out;
  }
  const uint64_t ws = unwrap(h.seq);
  const int wl = h.window_len;
  if (wl > repair_window_hwm_) repair_window_hwm_ = wl;
  const uint64_t wend = ws + static_cast<uint64_t>(wl);  // one past last covered
  if (wend > newest_v_ + static_cast<uint64_t>(kResetSpan) ||
      wend + static_cast<uint64_t>(kResetSpan) < newest_v_) {
    ++symbols_dropped_stale_;  // pre-reset stragglers after an encoder restart
    return out;
  }
  if (b == SwBoundary::kPre && wm_valid_ && wend - 1 > wm_) wm_ = wend - 1;
  advance(wend - 1);  // a repair implies its whole window was sent
  arr_.advance(wend - 1, arr_stale_end());
  if (ws > max_repair_ws_) max_repair_ws_ = ws;
  if (ws < live_floor()) {
    // References a seq older than the horizon — unsolvable, and (if any
    // covered seq is still unknown) that seq is already booked abandoned.
    ++symbols_dropped_stale_;
    return out;
  }

  // Repairs that can teach nothing are dropped before any GF work -- each
  // would cost up to window x symbol_size of lincomb only to reduce to a
  // linearly dependent row (bench 2026-10-12: ~80 % of the host replay's
  // instructions at 35 Mb/s, two cards). A second-card copy of an admitted
  // repair is the same row again.
  if (!note_repair(ws, wend, h.repair_key)) {
    ++repairs_dup_;
    return out;
  }
  // A window whose sources are all known yields no row either (the usual
  // case at low loss).
  {
    int i = 0;
    while (i < wl && is_known(ws + static_cast<uint64_t>(i))) ++i;
    if (i == wl) {
      ++repairs_redundant_;
      return out;
    }
  }

  Row r;
  r.payload.assign(payload, payload + ss);
  r.first_seen_ms = now_ms;
  uint8_t coeffs[sw::kMaxWindow];
  sw::repair_coeffs(h.repair_key, wl, coeffs);
  for (int i = 0; i < wl; ++i) {
    const uint64_t s = ws + static_cast<uint64_t>(i);
    if (is_known(s))
      gf::lincomb(r.payload.data(), known_payload(s), coeffs[i], ss);
    else
      r.coeffs.emplace(s, coeffs[i]);
  }
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> solved;
  insert_row(std::move(r), solved);
  for (auto& [sv, sym] : solved) ingest(sv, sym.data(), Origin::kRepair, out);
  return out;
}

// ---- loss episodes ----

uint64_t SwDecoder::episode_window() const {
  // The TX window as flown; the configured one only until a repair says.
  return repair_window_hwm_ > 0 ? static_cast<uint64_t>(repair_window_hwm_)
                                : static_cast<uint64_t>(cfg_.window);
}

bool SwDecoder::note_repair(uint64_t ws, uint64_t we, uint32_t key) {
  RepairSlot& t = repair_tab_[key & (kRepairTab - 1)];
  if (t.used && t.ws == ws && t.key == key) return false;
  if (repair_untracked_ > 0) {
    // A live entry lost its slot: only the scan is exact. A second-card copy
    // arrives within a few ms, so scanning backward finds it near the tail.
    for (auto it = repairs_seen_.rbegin(); it != repairs_seen_.rend(); ++it)
      if (it->ws == ws && it->key == key) return false;
  }
  if (t.used) ++repair_untracked_;  // the live entry there loses its slot
  t = RepairSlot{ws, key, true};
  repairs_seen_.push_back(RepairSpan{ws, we, key});
  return true;
}

void SwDecoder::note_missing(uint64_t v, Origin known_via, bool stale) {
  if (ep_open_ && v > ep_last_ + episode_window()) settle_episodes(v);  // too far: close first
  if (!ep_open_) {
    ep_ = LossEpisode{};
    ep_.first_seq = v;
    ep_open_ = true;
  }
  ep_last_ = v;
  ++ep_.missing;
  if (known_via == Origin::kRepair)
    ++ep_.recovered;
  else if (known_via == Origin::kRetx)
    ++ep_.retx;
  else
    ++ep_.abandoned;
  if (stale) ++ep_.stale;
}

void SwDecoder::settle_episodes(uint64_t evict_end) {
  const uint64_t w = episode_window();
  // Close once every seq that could still have joined (<= last + window)
  // has itself been evicted, i.e. evict_end - 1 > last + window.
  if (ep_open_ && evict_end > ep_last_ + w + 1) {
    ep_.span = static_cast<uint32_t>(ep_last_ - ep_.first_seq + 1);
    ep_.window = static_cast<uint32_t>(w);
    ep_.repairs = 0;
    for (const auto& r : repairs_seen_)
      if (r.ws <= ep_last_ && r.we > ep_.first_seq) ++ep_.repairs;
    if (episodes_.size() >= kMaxQueuedEpisodes)
      episodes_.erase(episodes_.begin());
    episodes_.push_back(ep_);
    ep_open_ = false;
  }
  // A repair can only matter to an episode holding a seq it covers; the
  // open episode's first seq is the oldest such seq, else nothing below
  // the eviction line can ever be booked again.
  const uint64_t keep_from = ep_open_ ? ep_.first_seq : evict_end;
  while (!repairs_seen_.empty() && repairs_seen_.front().we <= keep_from) {
    const RepairSpan& r = repairs_seen_.front();
    RepairSlot& t = repair_tab_[r.key & (kRepairTab - 1)];
    if (t.used && t.ws == r.ws && t.key == r.key)
      t.used = false;
    else
      --repair_untracked_;
    repairs_seen_.pop_front();
  }
}

std::vector<LossEpisode> SwDecoder::take_episodes() {
  std::vector<LossEpisode> out;
  out.swap(episodes_);
  return out;
}

uint64_t SwDecoder::deficit() const {
  if (!have_seq_ || newest_v_ < base_) return 0;
  // Below base_ everything is already booked (delivered/recovered/abandoned);
  // during the opening horizon the ring may hold reordered seqs below base_,
  // which the walk from base_ excludes. Rows never reference an evicted seq.
  const uint64_t span = newest_v_ - base_ + 1;
  uint64_t known_live = 0;
  for (uint64_t s = base_; s <= newest_v_; ++s)
    if (is_known(s)) ++known_live;
  const uint64_t unknown = span > known_live ? span - known_live : 0;
  const uint64_t rows = rows_.size();
  return unknown > rows ? unknown - rows : 0;
}

int SwDecoder::expire_rows_older_than(uint64_t deadline_ms, uint64_t now_ms) {
  // now_ms > guard against underflow — bodies routinely carry stamps newer
  // than the poll clock (seen on the bench).
  int dropped = 0;
  for (auto it = rows_.begin(); it != rows_.end();) {
    if (now_ms > it->second.first_seen_ms &&
        now_ms - it->second.first_seen_ms > deadline_ms) {
      it = rows_.erase(it);
      ++dropped;
    } else {
      ++it;
    }
  }
  return dropped;
}

}  // namespace mabur
