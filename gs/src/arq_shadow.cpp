#include "arq_shadow.h"

#include <algorithm>
#include <utility>

namespace maburgs {

namespace {
// An owner that never drains (tests, a future caller) stays bounded.
constexpr size_t kMaxQueued = 1024;

template <typename T>
void push_bounded(std::vector<T>& v, T x) {
  if (v.size() >= kMaxQueued) v.erase(v.begin());
  v.push_back(std::move(x));
}
}  // namespace

ArqShadow::ArqShadow(const ArqShadowCfg& cfg, SnapFn snap)
    : cfg_(cfg), snap_(std::move(snap)) {}

void ArqShadow::on_video_body(int sid, double t_ms) {
  if (!valid(sid)) return;
  Layer& l = layers_[static_cast<size_t>(sid)];
  if (l.pending) {
    // A copy of the settling burst's tail (the other card, a USB-late
    // aggregate): part of the burst being sampled, not a new one. It is
    // decoded before the sample, which is what settle_ms is for.
    if (t_ms < l.t_end + cfg_.settle_ms) return;
    // A genuinely new burst of this layer: sample before it is decoded.
    sample(sid);
  }
  if (cur_sid_ == sid) {
    if (t_ms - last_body_ms_ < cfg_.gap_ms) {
      // Two cards' stamps interleave slightly out of order; keep the latest.
      last_body_ms_ = std::max(last_body_ms_, t_ms);
      return;
    }
    // Same layer after a silence (enh shed: base-only bursts): the previous
    // burst ended at its last body; the gap already let every copy land.
    end_burst(sid, last_body_ms_);
    sample(sid);
  } else if (cur_sid_ >= 0) {
    end_burst(cur_sid_, last_body_ms_);
  }
  cur_sid_ = sid;
  burst_start_ms_ = t_ms;
  last_body_ms_ = t_ms;
}

void ArqShadow::on_probe(double t_ms) {
  // No burst on air (the other card's copy of a probe that already ended
  // it), or a probe copy stamped before the current burst began (it trails
  // the PREVIOUS burst): neither ends the current one.
  if (cur_sid_ < 0 || t_ms < burst_start_ms_) return;
  end_burst(cur_sid_, t_ms);
  cur_sid_ = -1;
}

void ArqShadow::tick(double now_ms) {
  // quiet_ms, not gap_ms: now_ms is the processing clock, which trails the
  // bodies' RX stamps by the RX pipeline's latency.
  if (cur_sid_ >= 0 && now_ms - last_body_ms_ >= cfg_.quiet_ms) {
    end_burst(cur_sid_, last_body_ms_);
    cur_sid_ = -1;
  }
  for (int sid = 0; sid < kLayers; ++sid) {
    const Layer& l = layers_[static_cast<size_t>(sid)];
    if (l.pending && now_ms >= l.t_end + cfg_.settle_ms) sample(sid);
  }
  if (next_summary_ms_ < 0) {
    next_summary_ms_ = now_ms + cfg_.summary_ms;
  } else if (now_ms >= next_summary_ms_) {
    for (int sid = 0; sid < kLayers; ++sid) {
      Layer& l = layers_[static_cast<size_t>(sid)];
      if (l.win_bursts > 0)
        push_bounded(summaries_, ArqSummary{now_ms, sid, l.win_bursts, l.win_short});
      l.win_bursts = 0;
      l.win_short = 0;
    }
    next_summary_ms_ = now_ms + cfg_.summary_ms;
  }
}

void ArqShadow::reset() {
  for (auto& l : layers_) {
    l.pending = false;
    l.open = false;
    l.last_deficit = 0;
  }
  cur_sid_ = -1;
}

void ArqShadow::end_burst(int sid, double t_end) {
  Layer& l = layers_[static_cast<size_t>(sid)];
  // Two ends without a sample between them (no tick ran): the first burst's
  // state is what the decoder holds now, so sample it before re-arming.
  if (l.pending) sample(sid);
  l.pending = true;
  l.t_end = t_end;
}

void ArqShadow::sample(int sid) {
  Layer& l = layers_[static_cast<size_t>(sid)];
  l.pending = false;
  const ArqSnap s = snap_(sid);
  ++l.bursts;
  ++l.win_bursts;
  if (s.deficit > 0) {
    ++l.short_bursts;
    ++l.win_short;
    if (!l.open) {
      l.open = true;
      l.ep = ArqEpisode{};
      l.ep.t_open_ms = l.t_end;
      l.ep.sid = sid;
      l.ep.mcs = s.mcs;
      l.ep.bw = s.bw;
      l.ep.bpb = s.bpb;
      l.ep.ov = s.ov;
      l.ep.d0 = s.deficit;
      l.ep.dpk = s.deficit;
      l.ep.nack = 1;
      l.aband0 = s.abandoned;
      l.stale0 = s.abandoned_stale;
    } else {
      ++l.ep.nack;
      l.ep.dpk = std::max(l.ep.dpk, s.deficit);
      if (s.deficit > l.last_deficit) l.ep.grow_ms = l.t_end - l.ep.t_open_ms;
    }
    l.last_deficit = s.deficit;
    return;
  }
  if (!l.open) return;
  l.ep.dur_ms = l.t_end - l.ep.t_open_ms;
  // Saturating: a decoder rebuilt under us restarts its counters at 0.
  l.ep.aband = s.abandoned >= l.aband0 ? s.abandoned - l.aband0 : 0;
  l.ep.stale = s.abandoned_stale >= l.stale0 ? s.abandoned_stale - l.stale0 : 0;
  push_bounded(episodes_, l.ep);
  l.open = false;
  l.last_deficit = 0;
}

std::vector<ArqEpisode> ArqShadow::take_episodes() {
  std::vector<ArqEpisode> out;
  out.swap(episodes_);
  return out;
}

std::vector<ArqSummary> ArqShadow::take_summaries() {
  std::vector<ArqSummary> out;
  out.swap(summaries_);
  return out;
}

uint64_t ArqShadow::bursts(int sid) const {
  return valid(sid) ? layers_[static_cast<size_t>(sid)].bursts : 0;
}

uint64_t ArqShadow::short_bursts(int sid) const {
  return valid(sid) ? layers_[static_cast<size_t>(sid)].short_bursts : 0;
}

}  // namespace maburgs
