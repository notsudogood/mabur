#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "mabur/cpu_stat.h"
#include "mabur/rc_proto.h"

namespace mabur {

// Fed from maburd's rx_callback with pkt.RxAtrib of CRC-clean RC frames;
// thread-safe for one writer (RX thread) + one reader (main tick):
// doubles guarded by a std::mutex — 20 Hz writes, 1 Hz reads.
class UplinkTrack {
 public:
  void on_rc_frame(const uint8_t rssi[2], const int8_t snr[2]);  // kEmaAlpha 0.1, seed-then-EMA

  struct Snap {
    bool has = false;
    double rssi[2] = {0.0, 0.0};
    double snr[2] = {0.0, 0.0};
  };
  Snap snap() const;

 private:
  mutable std::mutex m_;
  bool has_ = false;
  double rssi_[2] = {0.0, 0.0};
  double snr_[2] = {0.0, 0.0};
};

// Pure: input struct -> rc::Telem. All clamping/saturation here.
struct TelemInputs {
  int state = 0;
  bool failsafe_shed = false;
  bool congestion_shed = false;  // RcAgent::congestion_shed() — flags bit4
  bool low_power = false;  // RcAgent::low_power() — flags bit7
  bool calibrated = false;   // live radio.power_mode == "offset" — flags bit5 (rc::kTelemCalibrated)
  bool arm_refused = false;  // a calibration start refused while armed, last 5 s — flags bit2
  // RcAgent::take_auth_reject() — flags bit1 (rc::kTelemAuthReject): a
  // control frame failed its tag since the last Telem.
  bool auth_reject = false;
  uint64_t rcf_age_ms = 0, rcf_rx = 0;
  // link-rtt: seq of the RCF rcf_age_ms ages against + the pts-domain clock
  // (MI timebase, µs) at telem build. Straight pass-through, no saturation.
  // echo_valid maps to flags bit3; false outside LINKED (failsafe rebase),
  // where the echo would be stale.
  uint16_t rcf_seq_echo = 0;
  bool rcf_seq_echo_valid = false;
  uint64_t pts_at_build_us = 0;
  int cmd_kbps = 0;
  uint64_t txq_drops = 0;
  // Per-telemetry-window max TxQueue wait (the txq_wait_max_ms atomic,
  // consumed via .exchange(0) at the 1 Hz tick) — saturating.
  uint64_t txq_wait_max_ms = 0;
  uint64_t usb_fail = 0;
  // fec-nack, per period: verified T_NACKs answered,
  // symbols re-sent, symbols the token bucket refused.
  uint64_t nack_rx = 0, retx_syms = 0, retx_refused = 0;
  // Camera tab (mabur/camera_presets.h): this boot's preset, the presets the
  // loaded driver offers / a driver switch would add, the last request's
  // outcome (cam::State).
  uint8_t cam_cur = 0xFF, cam_now = 0, cam_switch = 0, cam_state = 0;
  // RX-side channel view for this telemetry period (cca-on):
  // the RX callback's own / foreign / CRC-failed frame split.
  uint64_t rx_own = 0, rx_foreign = 0, rx_crcfail = 0;
  UplinkTrack::Snap uplink;
  int soc_temp_c = -128;
  // CpuStatSampler::sample() -- empty until two ticks have been read.
  std::optional<double> cpu_pct;
  // The same sample per core (index = core number); cores past the wire's
  // four are dropped, an empty entry goes out as rc::kCpuCoreNone.
  std::vector<std::optional<double>> cpu_core_pct;
  // VtxRecorder::status_byte(), straight pass-through.
  uint8_t rec_status = 0;
  // The floor block (rc::kTelemExtFloor, d19): the learned wide-TX IGI
  // floor and its first-boot calibration. has_floor = false sends none.
  bool has_floor = false;
  uint8_t floor_flags = 0;   // rc::kFloor* bits, arm view included
  uint8_t floor_val = 0;
  uint64_t floor_stalls = 0;
  uint64_t floor_clean_ms[2] = {0, 0};  // toward the 40 / 80 MHz marks
  uint64_t floor_need_ms = 0;
};

rc::Telem make_telem(uint16_t tlm_seq, const TelemInputs& in);

// Reads /sys/class/thermal/thermal_zone0/temp (millidegrees) and
// /proc/loadavg; -128 / 0.0 when unreadable. Trivial, separated for tests
// via path injection.
// Standard kernel thermal zone (millidegrees). -128 when unreadable.
int read_soc_temp_c(const char* path = "/sys/class/thermal/thermal_zone0/temp");
// SigmaStar fallback: cpufreq's "Temp=NN" (already degrees C) — the OpenIPC
// drone SoC ships no thermal_zone (found on hw). -128 when
// unreadable.
int read_soc_temp_c_sigmastar(
    const char* path = "/sys/devices/system/cpu/cpufreq/temp_out");

}  // namespace mabur
