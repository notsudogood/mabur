#include "prime_frame.h"

namespace maburplay {

const char* prime_reject_name(PrimeReject r) {
  switch (r) {
    case PrimeReject::kNone: return "none";
    case PrimeReject::kFormat: return "format";
    case PrimeReject::kObjects: return "objects";
    case PrimeReject::kModifier: return "modifier";
    case PrimeReject::kPlanes: return "planes";
    case PrimeReject::kOffset: return "offset";
  }
  return "?";
}

PrimeReject prime_to_dma(const PrimeDesc& d, int width, int height, uint32_t pts_us,
                         DmaFrame* out) {
  if (d.nb_layers < 1 || d.layer_format != kFourccNV12) return PrimeReject::kFormat;
  if (d.nb_objects != 1) return PrimeReject::kObjects;
  const uint64_t mod = d.objects[0].modifier;
  if (mod != kModLinear && mod != kModInvalid) return PrimeReject::kModifier;
  if (d.nb_planes != 2 || d.planes[0].object_index != 0 || d.planes[1].object_index != 0)
    return PrimeReject::kPlanes;
  const long pitch = d.planes[0].pitch;
  const long c_off = d.planes[1].offset;
  if (d.planes[0].offset != 0 || pitch < width || d.planes[1].pitch != pitch ||
      c_off % pitch != 0 || c_off / pitch < height)
    return PrimeReject::kOffset;

  out->dmabuf_fd = d.objects[0].fd;
  out->fourcc = kFourccNV12;
  out->modifier = kModLinear;
  out->width = width;
  out->height = height;
  out->stride = static_cast<int>(pitch);
  out->vstride = static_cast<int>(c_off / pitch);
  out->pts_us = pts_us;
  return PrimeReject::kNone;
}

}  // namespace maburplay
