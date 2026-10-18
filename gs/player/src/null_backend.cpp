#include "null_backend.h"

#include <utility>

#ifdef MABUR_PLAYER_MPP
#include "mpp_backend.h"
#endif
#ifdef MABUR_PLAYER_V4L2REQ
#include "v4l2req_backend.h"
#endif

namespace maburplay {

bool NullBackend::init(const BackendCfg&, FrameSink sink) {
  sink_ = std::move(sink);
  return true;
}

void NullBackend::submit_au(const uint8_t*, size_t, uint32_t) { ++submits_; }

void NullBackend::flush() { ++flushes_; }

void NullBackend::release_frame(const DmaFrame&) {}

// Factory: "null" is always available (host and cross). "mpp" and "v4l2req"
// exist only in the hardware build that carries that decoder; anything else
// is nullptr, which main.cpp turns into a clear exit(2).
std::unique_ptr<VideoBackend> make_backend(const std::string& name) {
  if (name == "null") return std::make_unique<NullBackend>();
#ifdef MABUR_PLAYER_MPP
  if (name == "mpp") return std::make_unique<MppBackend>();
#endif
#ifdef MABUR_PLAYER_V4L2REQ
  if (name == "v4l2req") return std::make_unique<V4l2ReqBackend>();
#endif
  return nullptr;
}

}  // namespace maburplay
