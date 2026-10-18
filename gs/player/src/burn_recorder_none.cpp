// BurnRecorder for a build with no hardware encoder (the V4L2-request
// decoder build). main.cpp drives the recorder through `if (burn)` sites
// across its run loop; this keeps them compiling unchanged. start() always
// refuses, so `burn` is never set -- and a config asking for the burned DVR
// on this build is rejected at startup before it gets here.
#include "burn_recorder.h"

#include <cstdio>

namespace maburplay {

struct BurnRecorder::Impl {};

BurnRecorder::BurnRecorder() : impl_(new Impl) {}
BurnRecorder::~BurnRecorder() = default;
void BurnRecorder::set_palette(const OsdPalette&) {}
bool BurnRecorder::start(const BurnCfg&, const std::string&, const VideoBackend*) {
  std::fprintf(stderr, "BurnRecorder: this build has no hardware encoder\n");
  return false;
}
void BurnRecorder::submit(const DmaFrame&) {}
void BurnRecorder::set_osd(const Surface&, const DirtyRect*, size_t) {}
void BurnRecorder::request_idr() {}
void BurnRecorder::drop_pending() {}
void BurnRecorder::stop() {}
bool BurnRecorder::running() const { return false; }
uint64_t BurnRecorder::frames_in() const { return 0; }
uint64_t BurnRecorder::frames_encoded() const { return 0; }
uint64_t BurnRecorder::frames_dropped() const { return 0; }
uint64_t BurnRecorder::frames_flushed() const { return 0; }
uint64_t BurnRecorder::encode_errors() const { return 0; }
uint64_t BurnRecorder::osd_rejects() const { return 0; }
uint64_t BurnRecorder::colortrans_fallbacks() const { return 0; }
BurnRecorder::StageUs BurnRecorder::stage_us() const { return {}; }

}  // namespace maburplay
