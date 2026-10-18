#ifndef MABUR_PLAYER_PRIME_FRAME_H_
#define MABUR_PLAYER_PRIME_FRAME_H_

#include <cstdint>

#include "video_backend.h"  // DmaFrame

namespace maburplay {

// A decoded DRM-PRIME frame's layout, copied field for field out of libav's
// AVDRMFrameDescriptor (layer 0 only). Kept free of libav and libdrm headers
// so the conversion below builds and tests on the host.
struct PrimeObject {
  int fd = -1;
  uint64_t modifier = 0;
};
struct PrimePlane {
  int object_index = 0;
  long offset = 0;
  long pitch = 0;
};
struct PrimeDesc {
  int nb_objects = 0;
  PrimeObject objects[4];
  int nb_layers = 0;
  uint32_t layer_format = 0;
  int nb_planes = 0;
  PrimePlane planes[4];
};

enum class PrimeReject : uint8_t { kNone, kFormat, kObjects, kModifier, kPlanes, kOffset };
const char* prime_reject_name(PrimeReject r);

constexpr uint32_t kFourccNV12 = 0x3231564Eu;           // DRM_FORMAT_NV12
constexpr uint64_t kModLinear = 0;                       // DRM_FORMAT_MOD_LINEAR
constexpr uint64_t kModInvalid = 0x00ffffffffffffffull;  // DRM_FORMAT_MOD_INVALID

// DrmPresenter imports a DmaFrame as one dmabuf holding linear NV12 with the
// chroma plane at stride * vstride. This accepts exactly that shape -- one
// object, linear (or unspecified) modifier, two planes on it, luma at 0,
// chroma a whole number of pitch-rows past the visible luma -- and fills
// `out` (opaque untouched). Anything else returns why, so the caller drops
// the frame instead of scanning out garbage.
PrimeReject prime_to_dma(const PrimeDesc& d, int width, int height, uint32_t pts_us,
                         DmaFrame* out);

}  // namespace maburplay

#endif  // MABUR_PLAYER_PRIME_FRAME_H_
