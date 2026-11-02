#pragma once
#include <cstdint>
#include <string>

namespace mabur {

// The encoder as a separate process: upstream waybeam_venc on the same
// board, publishing whole frames into the frame-shm ring maburd's hot thread
// reads (drone/vendor/venc_frame_ring.h) and taking its live settings over
// its local HTTP API. This is the SSC378QE image's arrangement
// (docs/ssc378qe-target.md): mabur's folded-in venc core is the SSC338Q's
// pipeline, and waybeam already has a working one for the infinity6c SDK.
// On the SSC338Q nothing here runs -- the core is in-process.
//
// Verbs (waybeam src/venc_api.c): GET /api/v1/live/set?<key>=<value>
// applies a MUT_LIVE field to the running config without a flash write
// (video0.bitrate, video0.fps, fpv.roi_qp ...), GET /request/idr forces a
// keyframe. Each call is a bounded HTTP/1.0 exchange on the agent thread:
// connect, send and receive are 200 ms each, so one call can never hold
// the thread past ~600 ms. Never throws; every failure is a false return
// and a count, logged at most once per 5 s.
struct ExtVencCfg {
  std::string host = "127.0.0.1";
  int port = 80;  // waybeam's system.webPort default
};

class ExtVenc {
 public:
  explicit ExtVenc(ExtVencCfg cfg);

  // "host:port" (or "host") from `spec`; the defaults when null or empty.
  static ExtVencCfg parse_spec(const char* spec);

  bool set_live(const std::string& key, const std::string& value);
  bool request_idr();

  uint64_t failures() const { return failures_; }
  const ExtVencCfg& cfg() const { return cfg_; }

 private:
  bool do_get(const std::string& path);

  ExtVencCfg cfg_;
  uint64_t failures_ = 0;
};

}  // namespace mabur
