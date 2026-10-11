#include "pair_pick.h"

#include <algorithm>
#include <optional>

#include "mabur/ht40.h"

namespace maburgs {

std::vector<uint8_t> scan_half_set(const std::vector<uint8_t>& set) {
  std::vector<uint8_t> out;
  auto add = [&out](uint8_t ch) {
    if (ch != 0 && std::find(out.begin(), out.end(), ch) == out.end()) out.push_back(ch);
  };
  auto add_pair = [&](uint8_t primary) {
    const uint8_t other = mabur::ht40_pair_other(primary);
    if (other == 0) return;
    add(std::min(primary, other));
    add(std::max(primary, other));
  };
  for (uint8_t c : set) add_pair(c);
  return out;
}

namespace {
struct PairScore {
  uint8_t primary;
  uint32_t score;
  bool blocked;
  double busy;
  bool busy_valid;   // both halves carry an NHM busy reading
};

// Tier first (an unblocked pair beats any blocked one; among blocked the
// less busy), then the existing worse-half event score.
bool better(const PairScore& a, const PairScore& b) {
  if (a.blocked != b.blocked) return !a.blocked;
  if (a.blocked && a.busy != b.busy) return a.busy < b.busy;
  return a.score < b.score;
}

// nullopt = unranked: no pair, a half never visited, or a half short of
// min_rounds. Both halves must be present in `all` -- a pair on half the
// evidence never wins.
std::optional<PairScore> score_pair(const std::vector<RankEntry>& all, uint8_t primary,
                                    int min_rounds, double blocked_pct) {
  const uint8_t other = mabur::ht40_pair_other(primary);
  if (other == 0) return std::nullopt;
  uint32_t worst = 0;
  bool blocked = false;
  double busy = 0.0;
  bool busy_valid = true;
  int seen = 0;
  for (const RankEntry& e : all) {
    if (e.ch != primary && e.ch != other) continue;
    if (e.visits < static_cast<uint32_t>(min_rounds)) return std::nullopt;
    worst = std::max(worst, e.worst_busy);
    blocked = blocked || (e.busy_valid && e.worst_busy_pct >= blocked_pct);
    busy = std::max(busy, e.busy_valid ? e.worst_busy_pct : 0.0);
    busy_valid = busy_valid && e.busy_valid;
    ++seen;
  }
  if (seen != 2) return std::nullopt;
  return PairScore{primary, worst, blocked, busy, busy_valid};
}
}  // namespace

uint8_t pair_proposal(const std::vector<RankEntry>& all, uint8_t current,
                      const std::vector<uint8_t>& set, int min_rounds,
                      uint32_t margin, double blocked_pct, double busy_margin_pct) {
  const std::optional<PairScore> h = score_pair(all, current, min_rounds, blocked_pct);
  std::optional<PairScore> best;
  for (uint8_t c : set) {
    if (c == current) continue;
    const std::optional<PairScore> s = score_pair(all, c, min_rounds, blocked_pct);
    if (s && (!best || better(*s, *best))) best = s;   // strict: config order on ties
  }
  if (!best) return current;
  if (!h) return best->primary;                                  // current unranked: best ranked candidate
  if (h->blocked && !best->blocked) return best->primary;         // margin applies within a tier
  // Airtime decides when there is a reading to decide on (header).
  if (busy_margin_pct > 0 && !h->blocked && h->busy_valid) {
    std::optional<PairScore> calm;
    for (uint8_t c : set) {
      if (c == current) continue;
      const std::optional<PairScore> s = score_pair(all, c, min_rounds, blocked_pct);
      if (!s || s->blocked || !s->busy_valid) continue;
      if (!calm || s->busy < calm->busy || (s->busy == calm->busy && s->score < calm->score)) calm = s;
    }
    if (calm) return calm->busy + busy_margin_pct <= h->busy ? calm->primary : current;
  }
  if (!better(*best, *h)) return current;                         // ties -> current
  // best->blocked != h->blocked is unreachable here: the (current blocked,
  // best unblocked) case already returned above, and the reverse (current
  // unblocked, best blocked) makes better(*best, *h) false, so the
  // !better(...) check above already returned current for it. Both
  // branches below this point therefore share a tier.
  return best->score + margin <= h->score ? best->primary : current;
}

bool any_pair_ranked(const std::vector<RankEntry>& all, const std::vector<uint8_t>& set,
                     int min_rounds) {
  for (uint8_t c : set)
    if (score_pair(all, c, min_rounds, 1e9)) return true;
  return false;
}

bool all_pairs_ranked(const std::vector<RankEntry>& all, const std::vector<uint8_t>& set,
                      int min_rounds) {
  for (uint8_t c : set) if (!score_pair(all, c, min_rounds, 1e9)) return false;
  return !set.empty();
}

std::vector<uint8_t> pair_ranking(const std::vector<RankEntry>& all,
                                  const std::vector<uint8_t>& set, int min_rounds,
                                  double blocked_pct) {
  std::vector<PairScore> ps;
  for (uint8_t c : set) if (auto s = score_pair(all, c, min_rounds, blocked_pct)) ps.push_back(*s);
  std::stable_sort(ps.begin(), ps.end(), [](const PairScore& a, const PairScore& b) { return better(a, b); });
  std::vector<uint8_t> out;
  for (const auto& p : ps) out.push_back(p.primary);
  return out;
}

}  // namespace maburgs
