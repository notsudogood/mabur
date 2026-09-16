#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace maburgs {

// Rolling mode of the RX HT MCS the drone is actually transmitting at
// (docs/link-adaptation-v2-proposal.md §4, "Fight B"). Fed from
// RxBody::mcs, which comes off devourer's rx_pkt_attrib.data_rate and which
// node.h documents as "valid independent of the body CRC" — so it keeps
// reading through a fade, which is precisely when the drone may have changed
// rate on its own authority (apply_max_range() today, tier 0 later).
//
// MODE, not mean. The value is categorical: averaging mcs2 and mcs4 would
// produce mcs3, a rung that may not exist and that the drone never sent. The
// unknown code (255 — a legacy/VHT rate, or frame-file replay) is skipped
// entirely rather than folded, the same discipline aggregator.h applies to
// the EVM zero/-128 sentinels.
//
// A short ring rather than a decaying histogram: a rate change should show up
// as a clean switch after `kWindow` frames, not as a slow blend between the
// old rung and the new one. At the shipped ~2200 video frames/s per card the
// window is a few milliseconds of traffic, well inside the RCF period the
// ladder decides on.
class McsMode {
 public:
  static constexpr int kWindow = 32;
  static constexpr uint8_t kUnknown = 255;

  void feed(uint8_t mcs) {
    if (mcs > 7) return;  // unknown/legacy/VHT: never folded
    ring_[head_] = mcs;
    head_ = (head_ + 1) % kWindow;
    if (n_ < kWindow) ++n_;
  }

  // The most common MCS in the window, or -1 when nothing usable has been
  // seen. Ties break toward the LOWER mcs: during a change the window holds
  // both rungs, and reporting the lower one early is the safe direction —
  // it can only make the controller score against more FEC budget than the
  // drone really has, never less.
  int mode() const {
    if (n_ == 0) return -1;
    std::array<int, 8> bins{};
    for (int i = 0; i < n_; ++i) ++bins[ring_[static_cast<size_t>(i)]];
    int best = -1, best_n = 0;
    for (int m = 0; m < 8; ++m)
      if (bins[static_cast<size_t>(m)] > best_n) { best_n = bins[static_cast<size_t>(m)]; best = m; }
    return best;
  }

  int samples() const { return n_; }
  void reset() { n_ = 0; head_ = 0; }

 private:
  std::array<uint8_t, kWindow> ring_{};
  int head_ = 0;
  int n_ = 0;
};

}  // namespace maburgs
