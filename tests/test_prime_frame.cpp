#include "mtest.h"
#include "prime_frame.h"

using maburplay::PrimeDesc;
using maburplay::PrimeReject;

// What Cedrus exports for a 1920x1080 picture: one buffer, luma then chroma,
// chroma at pitch * aligned height (1088).
static PrimeDesc cedrus_1080p() {
  PrimeDesc d;
  d.nb_objects = 1;
  d.objects[0] = {7, maburplay::kModLinear};
  d.nb_layers = 1;
  d.layer_format = maburplay::kFourccNV12;
  d.nb_planes = 2;
  d.planes[0] = {0, 0, 1920};
  d.planes[1] = {0, 1920L * 1088, 1920};
  return d;
}

TEST(accepts_single_object_linear_nv12) {
  maburplay::DmaFrame f;
  f.opaque = &f;  // must be left alone
  CHECK(maburplay::prime_to_dma(cedrus_1080p(), 1920, 1080, 12345, &f) == PrimeReject::kNone);
  CHECK(f.dmabuf_fd == 7);
  CHECK(f.fourcc == maburplay::kFourccNV12);
  CHECK(f.modifier == maburplay::kModLinear);
  CHECK(f.width == 1920 && f.height == 1080);
  CHECK(f.stride == 1920 && f.vstride == 1088);
  CHECK(f.pts_us == 12345u);
  CHECK(f.opaque == &f);
}

TEST(invalid_modifier_reads_as_linear) {
  PrimeDesc d = cedrus_1080p();
  d.objects[0].modifier = maburplay::kModInvalid;
  maburplay::DmaFrame f;
  CHECK(maburplay::prime_to_dma(d, 1920, 1080, 0, &f) == PrimeReject::kNone);
  CHECK(f.modifier == maburplay::kModLinear);
}

TEST(rejects) {
  maburplay::DmaFrame f;
  PrimeDesc d = cedrus_1080p();
  d.layer_format = 0x3132564Eu;  // NV21
  CHECK(maburplay::prime_to_dma(d, 1920, 1080, 0, &f) == PrimeReject::kFormat);

  d = cedrus_1080p();
  d.nb_objects = 2; d.objects[1] = {8, 0}; d.planes[1].object_index = 1; d.planes[1].offset = 0;
  CHECK(maburplay::prime_to_dma(d, 1920, 1080, 0, &f) == PrimeReject::kObjects);

  d = cedrus_1080p();
  d.objects[0].modifier = 0x0800000000000001ull;  // a tiled (AFBC-class) modifier
  CHECK(maburplay::prime_to_dma(d, 1920, 1080, 0, &f) == PrimeReject::kModifier);

  d = cedrus_1080p();
  d.nb_planes = 3;
  CHECK(maburplay::prime_to_dma(d, 1920, 1080, 0, &f) == PrimeReject::kPlanes);

  d = cedrus_1080p();
  d.planes[1].offset = 1920L * 1088 + 64;          // not a whole number of rows
  CHECK(maburplay::prime_to_dma(d, 1920, 1080, 0, &f) == PrimeReject::kOffset);

  d = cedrus_1080p();
  d.planes[1].offset = 1920L * 1000;               // chroma would overlap luma rows
  CHECK(maburplay::prime_to_dma(d, 1920, 1080, 0, &f) == PrimeReject::kOffset);

  d = cedrus_1080p();
  d.planes[0].offset = 4096;
  CHECK(maburplay::prime_to_dma(d, 1920, 1080, 0, &f) == PrimeReject::kOffset);

  d = cedrus_1080p();
  d.planes[0].pitch = 1280;                        // pitch narrower than the picture
  CHECK(maburplay::prime_to_dma(d, 1920, 1080, 0, &f) == PrimeReject::kOffset);
}

TEST(reject_names) {
  CHECK(std::string(maburplay::prime_reject_name(PrimeReject::kModifier)) == "modifier");
  CHECK(std::string(maburplay::prime_reject_name(PrimeReject::kNone)) == "none");
}

MTEST_MAIN
