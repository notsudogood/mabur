#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "mabur/channel_set.h"
#include "mabur/link_key.h"
#include "mabur/profile.h"
#include "mabur/uep_encoder.h"
#include "venc_cfg.h"  // VencCfg, VENC_RING_NAME — plain C99, host-safe

namespace mabur {

struct RadioCfg {
  uint16_t usb_vid = 0x0bda;
  uint16_t usb_pid = 0;  // 0 = scan
  uint8_t width = 20;
  // Width stepping (docs/next-build-backlog.md item 14): the radio sits at
  // home_width and widens to the flown/probed rung's width (up to `width`)
  // only while one needs it, back after width_hold_ms. Each step is
  // devourer's FastSetChannel on the same primary (batched, est. a few ms on
  // this host, unmeasured). false = the radio stays at `width` for the
  // session, as d8-d10 did.
  bool width_step = false;
  uint8_t home_width = 40;
  int width_hold_ms = 3000;
  // Hops: false = at 80 MHz, or when the 40 MHz pair side changes, a hop is
  // the full channel set (FastSetChannel) -- the 80 MHz lean hop is the
  // prime suspect for backlog item 0. true = every hop is FastRetune.
  bool fast_hop_at_80 = false;
  // devourer's batched channel set (tuning.batched_chset) for the width
  // steps and full-set hops; false = one USB round trip per register.
  bool batched_tune = true;
  // 10 MHz rung (backlog item 10, ordered by the GS): with no RCF heard for
  // this long at 10 MHz the radio leaves 10 on its own (the GS does the same
  // when it stops hearing the drone). 0 = never; the 3 s failsafe still
  // returns the link to MCS0/20.
  int narrow_rescue_ms = 1500;
  // The channel set: the drone parks on the remembered member (else the
  // first) and follows the GS to any member. Must be a superset of the GS's
  // list.
  std::vector<uint8_t> channels{40, 64, 112, 144};
  // How bring-up programs TX power:
  //   "offset" — program the wall-equalized per-rate diff table
  //              (SetTxPowerRateDiffs) once, then zero the global offset
  //              once. Power is constant for the life of the process.
  //   "none"   — never touch power (efuse table as-is, streamtx-proven).
  // There is no runtime power control: no per-op power, no thermal derate.
  std::string power_mode = "none";
  // Wall-equalization inputs: measured per-rate clean-air ceilings as
  // signed indices RELATIVE to the chip's per-channel anchor, [-64,63].
  // rate_walls_rel is REQUIRED when power_mode == "offset". See
  // power_plan.h for diff[r] = rel[r] - m. One table covers every channel.
  std::array<int, 8> rate_walls_rel = {0, 0, 0, 0, 0, 0, 0, 0};
  int legacy_wall_rel = 63;
  double wall_margin_db = 1.0;
  // Parallel USB sender threads (URBs in flight). The 8822E flow-controls
  // sync bulk-OUT URBs (~0.4 ms acceptance handshake + FIFO drain), so a
  // single blocking sender caps air throughput at ~26 Mbps regardless of
  // MCS; ~4 saturate (linkbench bisect, devourer
  // docs/aggregation.md). 1 = strict on-air frame order (>1 can swap
  // ≤3-frame URB batches, which the seq-addressed FEC datapath and
  // the GS max-seq delivery accounting both tolerate).
  int tx_threads = 4;
  // How many of the tx_threads USB senders run on the producer's core
  // (cpu1, beside mbr-hot) instead of cpu0 with venc and the rest. 0 =
  // all on cpu0, the 2026-09-01 placement. Bench lever for drone CPU
  // headroom (backlog item 3, lever 1): at 35 Mb/s cpu0 ran 83 % busy and
  // cpu1 35-40 %. Two-core targets only; ignored elsewhere.
  int tx_threads_hot = 0;
  // LDPC on video, probe and control frames. false = BCC, for a GS card
  // that cannot decode HT-LDPC (RTL8821AU, bench: 1/1000 LDPC vs
  // 900+/1000 BCC). Costs every GS the LDPC coding gain while off.
  bool ldpc = true;
};

struct FecCfg {
  // Per-layer symbol size (stream 0..1). JSON accepts a scalar (fans out)
  // or a 2-array. Big symbols suit the bulk enhance layer (1): fewer
  // symbols lost per body, window spans more airtime, cheaper GF per byte
  // (burst_sim table). Layer 0 (critical NALs) stays small so VPS/SPS/PPS
  // seal without padding/latency.
  std::array<int, 2> symbol_size = {64, 64};
  // Sliding-window burst budget: a layer at overhead ov survives a hole of
  // up to L <= window*ov/(1+ov) consecutive lost symbols. 128 lets the
  // ov-0.50 layer survive one full bpb-16 body loss with room to spare
  // (L=128*0.50/1.50≈42.7 vs 16 lost symbols).
  int window = 128;
  std::array<int, 2> blocks_per_body = {4, 8};
  // Literal air overhead: the fraction of repair bytes over source bytes
  // actually put on air, not a scaled command value — no uep_layer_overhead
  // ladder translation. Default 0.5 matches the flattened reference ladder.
  double base_overhead = 0.5;
  int flush_ms = 15;
  // Feed grouping: release sealed bodies to the TX writer in groups of N
  // (TxQueue wakeup batching + grouped pool submit) so URBs fill their
  // 3-descriptor cap and A-MPDU aggregates can form, instead of the
  // per-body trickle that ships every body alone (~360 µs metronome,
  // dq-spike findings §17). 0/1 = streaming push (per-body wakeups). Bench
  // lever — default off until the batching+agg compound is accepted
  // end-to-end (watch arrival-jitter EMA: coarser quanta, the 656-rollback
  // lesson).
  int feed_batch = 0;
  // Core of the async FEC worker (mbr-fecw): "rest" = cpu0 (the 2026-09-01
  // placement), "hot" = cpu1 beside the producer. The 2026-09-01 bench
  // measured "hot" strictly worse (flush-join 3.2-3.7 ms vs 0.4), but that
  // was before the 2026-09-22 change took mbr-hot from ~70 % of its core to
  // ~25 %; re-measure before trusting either. Two-core targets only.
  std::string worker_core = "rest";
  // Idle polls the FEC worker makes before it sleeps (FecWorker::kSpinIters
  // = 16384, ~50-100 us on the A7). 0 = sleep as soon as the queue is
  // empty: no spin on the busy core, but a futex wake per repair burst
  // instead. Bench lever (backlog item 3); measure, do not assume.
  int worker_spin = 16384;
};

// One [[encoder.rung]] entry (2026-10-11): the bitrate policy's budget and
// ceiling for one ladder rung, keyed by the BASE layer's (mcs, bw) -- the
// rung the GS commanded. A field left at -1 inherits the [encoder] global.
// max_kbps REPLACES encoder.bitrate_max_kbps for that rung (it may sit above
// it), so one rung can run faster than the rest without raising them all.
struct EncoderRungCfg {
  int mcs = 0;
  int bw = 20;
  double airtime_budget = -1.0;
  int max_kbps = -1;
};

// RcAgent's bitrate/ROI policy knobs (ex-WaybeamCfg; the HTTP fields
// host/port/idr_path went with the waybeam section they lived in — venc is
// in-process now, no control-plane HTTP left to address).
struct EncoderCfg {
  int bitrate_min_kbps = 2000;
  int bitrate_max_kbps = 10000;
  double airtime_budget = 0.60;
  int roi_threshold_kbps = 3000;
  // NEGATIVE, and that is the point: the ROI delta-QP is applied to the
  // centre region, and a LOWER QP means MORE bits there. -24 is the
  // hardware-validated value the shipped bundle carries (configs/, and
  // test_config's bundle assertions); the +8 that used to sit here was a
  // sign-flipped placeholder that would have spent fewer bits on the
  // centre of frame exactly when the link is poorest.
  int roi_qp_low = -24;
  int roi_qp_normal = 0;
  // Per-rung overrides ([[encoder.rung]]); empty = every rung uses the
  // globals above, exactly as before 2026-10-11. No compiled-in entries: a
  // config that predates the table must behave as it always did.
  std::vector<EncoderRungCfg> rungs;

  // The entry for this rung, or nullptr.
  const EncoderRungCfg* rung_for(int mcs, int bw) const {
    for (const auto& r : rungs)
      if (r.mcs == mcs && r.bw == bw) return &r;
    return nullptr;
  }
  // The highest bitrate any rung may run: what buffers sized "at the maximum
  // bitrate" (the NACK retransmit ring) must be sized for.
  int peak_max_kbps() const {
    int m = bitrate_max_kbps;
    for (const auto& r : rungs)
      if (r.max_kbps > m) m = r.max_kbps;
    return m;
  }
};

// Boot-time encoder pipeline config, handed to venc_core_start() as a
// VencCfg. `core` is the pure-mechanism struct venc_cfg.h defines;
// `debug_port` is mabur-side (the thin debug endpoint, debug_http.h), not
// part of the vendored VencCfg surface.
struct VencSectionCfg {
  VencCfg core{};
  int debug_port = 8301;
  // Not aggregate-initialised on purpose: VencCfg is a plain C struct, so
  // `VencCfg core{}` alone would zero every field and an absent venc key
  // would hand the encoder fps 0 / 0x0 / gop 0.0 rather than a fallback.
  // venc_cfg_defaults() (drone/venc/venc_cfg.c) is the one table of truth
  // for those values; it leaves sensor_bin empty, which parse_venc treats
  // as a boot failure.
  VencSectionCfg() { venc_cfg_defaults(&core); }
};

struct LinkCfg {
  int failsafe_ms = 1000;
  int rendezvous_ms = 30000;
  // After a GS-commanded retune, hear the GS within this or go home.
  int move_confirm_ms = 2000;
  // Housekeeping cadence for the agent loop's TickGate. Bounded [1,1000]
  // at load: behind the gate a non-positive value stops every per-tick job
  // silently (see parse_link in config.cpp).
  int tick_ms = 100;
  // Agent-loop wake period for draining queued RCFs (fade-demote). Decoupled
  // from tick_ms: op actuation latency is U(0, rc_drain_ms) while ALL
  // per-tick housekeeping (USB health polls, state timers, congestion guard,
  // watchdog, stats/telem) stays on tick_ms. Bounded [1,1000] AND required to
  // be <= tick_ms, since a drain slower than the tick would silently retime
  // the housekeeping to the drain period; == tick_ms reproduces the legacy
  // single-cadence loop.
  int rc_drain_ms = 5;
  // Pairing key: the key FILE path. The file is read at load; a missing file
  // means the compiled-in default (and a boot log line), a malformed one
  // fails boot naming the file.
  std::string key_file = "/etc/mabur.key";
  mabur::LinkKey key = mabur::kDefaultLinkKey;   // resolved at load
  bool key_is_default = true;
  std::string key_source;                        // path, "default", or "link.key" (GS overlay)
};

struct MspCfg {
  bool enable = false;
  std::string serial = "/dev/ttyS2";
  int baud = 115200;
  double update_rate_hz = 1.0;  // ceiling on forwarded snapshots
  int symbol_size = 1312;       // one snapshot per symbol
  int window = 16;
  double overhead = 1.0;
};

// A-MPDU TX aggregation. OFF by default per the bench verdict
// (dq-spike-findings §12): the depth sweep measured NO fec improvement at any
// depth — maburd's per-body tax is host/USB-side, not medium access — while
// aggregate subframes carry missing-or-untrustworthy PHY reports that degrade
// the GS's RF telemetry even with the phy_valid filtering in place. Enable
// (max_num > 0) only for experiments, e.g. after a USB feed rework. The
// QoS-Data wire header does NOT depend on this block — max_num 0 turns off
// only the aggregation (frames become QoS-Data singles, measured identical to
// the old pace).
struct AmpduCfg {
  int max_num = 0;    // MAX_AGG_NUM cap, 0 = aggregation off, max 31 (5-bit)
  int max_time = 32;  // raw 0x455 fill-timer value (0x20 ~= 0.8 ms);
                      // 1..8 is a hardware cliff (disables aggregation) and
                      // is rejected at load; 0 keeps the chip bring-up
                      // default (0x70 ~= 3 ms — too slow, but valid for A/B)
  // Lowest op MCS that aggregates, PER WIDTH; rungs below fly QoS-Data
  // singles (drone/src/ampdu_policy.h). 20 MHz: agg6 delivers 6-13 points
  // LESS than singles at mcs0-2 (docs/findings/2026-09-17-bandwidth-sweep.md),
  // the bundle ships 4. 40 MHz: frames are half as long on air and
  // aggregation pays from mcs2 (docs/findings/2026-09-23-bw40-sweep.md),
  // the bundle ships 2. 0 = aggregate at every rung of that width.
  int min_mcs_20 = 0;
  int min_mcs_40 = 0;
  int min_mcs_80 = 0;   // VHT 80 MHz rungs; frames are a quarter as long on air as 20
};

// Drone air clock: per-frame virtual air-serialization model + enh admission.
// shed_ms 0 = observe only (the model runs and exports, nothing is dropped);
// > 0 drops an enh AU whose arrival finds the modelled backlog at or past
// shed_ms. efficiency is the fraction of nominal PHY rate treated as
// capacity; body_us a fixed per-body cost. Both are calibrated on the bench,
// not derived.
struct AirClockCfg {
  int shed_ms = 0;
  // Delivered/nominal air capacity per HT MCS 0..7, measured at saturation,
  // one table per width (drone/src/air_rate.h picks by LayerTxSpec.bw).
  // 20 MHz: docs/findings/2026-09-17-bandwidth-sweep.md (singles at
  // mcs0-3 under ampdu.min_mcs_20 4, agg6 above). 40 MHz:
  // docs/findings/2026-09-23-bw40-sweep.md (agg6, flat 0.74-0.77, no
  // rung-0 exception). Priced into BOTH run_bitrate_policy
  // (encoder.airtime_budget is a fraction of this, not of nominal) and the
  // air clock. All ones = nominal.
  std::array<double, 8> efficiency_20 = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
  std::array<double, 8> efficiency_40 = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
  // 80 MHz (VHT 1SS, mcs 0..7): measured, not all ones, because no flight
  // config predates it -- the bench 2026-10-17 linkbench run carried 0.76 at
  // mcs3 (agg6) and >= 0.67 at mcs4 with the drone's cpu0 at 94 % (a floor,
  // not the air's ceiling). mcs0-2 are unmeasured and take mcs3's 0.76;
  // mcs5-7 take mcs4's 0.67 (findings 2026-10-17-bw80-bench).
  std::array<double, 8> efficiency_80 = {0.76, 0.76, 0.76, 0.76, 0.67, 0.67, 0.67, 0.67};
  int body_us = 0;
};

// Low-power (disarmed) mode. While the FC reports DISARMED (MSP_STATUS over
// the OSD UART, polled at 2 Hz), RcAgent clamps the encoder bitrate to
// bitrate_kbps and switches the frame rate to fps; an ARMED report returns
// full power. The mode follows the FC's current state both ways, so a disarm
// after a flight re-enters it — that is the recovery case, a downed but
// still-powered aircraft falling back to a thin stream the degraded link can
// still carry. A DISARMED report older than stale_ms, or none at all, means
// full power (fail open: a dead UART can never fly you at 1 Mb/s — and
// equally, a crash that kills the FC gives full rate, not this mode). Struct
// default is OFF; the bundle enables it.
struct LowPowerCfg {
  bool enable = false;
  int bitrate_kbps = 1000;
  int fps = 15;
  int stale_ms = 2000;
};

// VTX onboard SD recorder. enable creates the record VENC channel at boot;
// the GS record button starts/stops it.
struct RecordCfg {
  bool enable = false;
  std::string dir = "/mnt/mmcblk0p1";  // mdev automount point of the SD card
  int bitrate_kbps = 40000;            // CBR
  int fps = 60;
  int min_free_mb = 512;               // refuse to start / stop below this
  // Recording size; 0x0 = venc.size. A size other than venc.size records
  // from the second VPE scaler (port 1) instead of the link's port 0; above
  // 1920x1080 the sensor switches to its 3840x2160@30 mode.
  int width = 0, height = 0;
  // Placement of the two recorder threads (mbr-rec, mbr-recdrain): "rest"
  // = CPU 0 beside the USB senders and FEC worker (the old layout), "hot" =
  // CPU 1 beside the video packer. low_priority runs both at nice 19 so the
  // live link's threads always preempt them: a busy core then costs
  // recorded frames (the recorder's own drop counter), not the link.
  std::string core = "rest";
  bool low_priority = true;
};

// Software NACK (fec-nack): how long the drone keeps recently sent base-layer
// source envelopes for re-sending, and the air budget a GS's T_NACK requests
// may spend.
struct NackDroneCfg {
  int ring_ms = 150;
  int air_pct = 5;
  int burst_ms = 20;  // bucket depth: this much air of re-sends at the current op
  // Hardware queue the re-sends ride: "vo" (voice, airs ahead of video
  // already inside the chip; turnaround bench 2026-10-06: p99 6.0 ms vs
  // 11.8 on video's queue, close range) or "video" (video's own queue,
  // upstream's behaviour).
  std::string queue = "vo";
  // Also keep and re-send the enhance (in-between frame) layer (2026-10-10):
  // a GS with [link.nack] enh asks for it.
  bool enh = true;
};

struct Config {
  RadioCfg radio;
  FecCfg fec;
  EncoderCfg encoder;
  VencSectionCfg venc;
  LinkCfg link;
  MspCfg msp;
  AmpduCfg ampdu;
  AirClockCfg air_clock;
  LowPowerCfg low_power;
  RecordCfg record;
  NackDroneCfg nack;
  std::array<UepLayerCfg, 2> uep_layers() const;
};

// Loads and validates a mabur.toml config file. Missing keys fall back to
// the struct defaults above; unknown keys and out-of-range values throw
// std::runtime_error("config: <file>:<line>: <field>: <why>"). Missing file
// throws too.
//
// `defaulted`, when non-null, receives "dotted.key=value" for every known
// key the file did not set. main() prints it once at startup so a
// hand-transcribed config shows its gaps in the log, not in the air.
Config load_config(const std::string& path,
                   std::vector<std::string>* defaulted = nullptr);

}  // namespace mabur
