#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace maburgs {

// Feedback-repair shadow mode (docs/feedback-repair-rollout.md, phase 1).
//
// The feedback-repair design has the GS send, in a listen window after every
// drone burst, "layer s is short k repair symbols"; the drone answers with k
// fresh repairs. Before building any of that, this class answers the question
// that decides whether it is worth building, using nothing but the GS's own
// receive path: at each burst end, how many repair symbols short is each
// video layer, how long does a shortfall last, and does the FEC's in-band
// overlap (the next same-layer burst's repairs) fix it anyway or is it lost?
// It transmits nothing and changes no decoder state.
//
// Burst ends come from arrival order, not from any wire field:
//   * a video body of the OTHER layer (base and enh AUs alternate, one burst
//     each), stamped at the previous layer's last body;
//   * a probe body (it trails every AU as the burst's last PPDU);
//   * a same-layer body after an RX-stamp gap >= gap_ms (enh shed:
//     base-only bursts);
//   * tick() with no video body for quiet_ms (outage, end of stream).
// The decoder is sampled settle_ms after the end, so a second card's copy of
// the burst's tail can still land first (a body stamped inside that window
// counts as the ended burst's tail, not a new burst) -- or, if a new burst
// of the same layer starts sooner, immediately before its first body is
// decoded. A probe copy stamped before the current burst began is ignored.
//
// Pure logic: the owner supplies decoder/op readings through SnapFn, so this
// is testable without a radio. Core thread only.

struct ArqSnap {
  uint64_t deficit = 0;          // UepDecoder::deficit(sid): symbols short
  uint64_t abandoned = 0;        // LayerStats::syms_abandoned (monotonic)
  uint64_t abandoned_stale = 0;  // LayerStats::syms_abandoned_stale
  int mcs = 0;                   // op at the sample
  int bw = 20;
  double ov = 0.0;               // that layer's commanded overhead
  int bpb = 0;                   // FEC blocks (= symbols) per radio body
};

// One shortfall episode: a run of consecutive burst-end samples of one layer
// with deficit > 0, closed by the first sample back at 0.
struct ArqEpisode {
  double t_open_ms = 0;  // burst end of the first short sample
  int sid = 0, mcs = 0, bw = 20, bpb = 0;
  double ov = 0;         // op at open
  double dur_ms = 0;     // t_open -> the closing sample's burst end
  double grow_ms = 0;    // t_open -> the last burst end where the deficit grew
  uint32_t nack = 0;     // short samples = requests a live system would send
  uint64_t d0 = 0;       // deficit at open (the first request's size)
  uint64_t dpk = 0;      // peak deficit
  uint64_t aband = 0;    // symbols the decoder abandoned inside the episode
  uint64_t stale = 0;    // of aband, transition debris (ladder switch)
};

// Denominators: burst-end samples per layer per summary window.
struct ArqSummary {
  double t_ms = 0;
  int sid = 0;
  uint32_t bursts = 0;  // samples taken
  uint32_t short_bursts = 0;  // of those, deficit > 0
};

struct ArqShadowCfg {
  double settle_ms = 4.0;       // burst end -> decoder sample
  double gap_ms = 8.0;          // RX-stamp gap that ends a same-layer burst
  double quiet_ms = 15.0;       // tick(): processing-clock silence that ends one
  double summary_ms = 10000.0;  // S-record cadence
};

class ArqShadow {
 public:
  static constexpr int kLayers = 2;
  using SnapFn = std::function<ArqSnap(int sid)>;

  ArqShadow(const ArqShadowCfg& cfg, SnapFn snap);

  // Right before a CRC-clean video body of layer sid is decoded. t_ms is the
  // body's RX stamp (mono ms). Other sids are ignored.
  void on_video_body(int sid, double t_ms);
  // A probe body arrived (any card, parseable or not): the burst ended.
  void on_probe(double t_ms);
  // Core-loop tick, after each drain: silence detection, settled samples,
  // summaries.
  void tick(double now_ms);
  // Decoder continuity reset (new session): forget open episodes and
  // pending samples without emitting them -- their seq space is gone.
  void reset();

  std::vector<ArqEpisode> take_episodes();
  std::vector<ArqSummary> take_summaries();

  uint64_t bursts(int sid) const;
  uint64_t short_bursts(int sid) const;

 private:
  struct Layer {
    bool pending = false;
    double t_end = 0;
    bool open = false;
    ArqEpisode ep;
    uint64_t last_deficit = 0;
    uint64_t aband0 = 0, stale0 = 0;
    uint64_t bursts = 0, short_bursts = 0;
    uint32_t win_bursts = 0, win_short = 0;
  };

  void end_burst(int sid, double t_end);
  void sample(int sid);
  static bool valid(int sid) { return sid >= 0 && sid < kLayers; }

  ArqShadowCfg cfg_;
  SnapFn snap_;
  std::array<Layer, kLayers> layers_{};
  int cur_sid_ = -1;  // layer of the burst on air, -1 = none
  double burst_start_ms_ = 0;
  double last_body_ms_ = 0;
  double next_summary_ms_ = -1;
  std::vector<ArqEpisode> episodes_;
  std::vector<ArqSummary> summaries_;
};

}  // namespace maburgs
