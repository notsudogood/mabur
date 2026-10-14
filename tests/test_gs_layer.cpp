#include "mtest.h"

#include <string>

#include "gs_draw.h"  // premul
#include "gs_layer.h"

using namespace maburplay;

// The layout-independent half of the GS OSD (gs_layer.h): formatters and
// the burned-DVR palette seeds. The bar itself is test_gs_compact.

// --- formatting -------------------------------------------------------

TEST(fmt_int_rounds_to_nearest) {
  CHECK(fmt_int(0.0) == "0");
  CHECK(fmt_int(60.4) == "60");
  CHECK(fmt_int(60.5) == "61");
  CHECK(fmt_int(-0.4) == "0");
}

TEST(fmt_one_dp_always_keeps_the_decimal) {
  CHECK(fmt_one_dp(0.0) == "0.0");
  CHECK(fmt_one_dp(2.14) == "2.1");
  CHECK(fmt_one_dp(2.15) == "2.2");
  CHECK(fmt_one_dp(24.6) == "24.6");
  CHECK(fmt_one_dp(100.0) == "100.0");
}

TEST(fmt_clock_is_mm_ss_and_saturates) {
  CHECK(fmt_clock(0) == "00:00");
  CHECK(fmt_clock(9) == "00:09");
  CHECK(fmt_clock(767) == "12:47");
  CHECK(fmt_clock(3599) == "59:59");
  // Past an hour it keeps counting minutes rather than wrapping to 00:00,
  // which would read as "recording just started" mid-flight.
  CHECK(fmt_clock(3600) == "60:00");
  CHECK(fmt_clock(5999) == "99:59");
  CHECK(fmt_clock(6000) == "99:59");   // saturate, never widen the box
  CHECK(fmt_clock(-5) == "00:00");
}

// --- palette seeds ----------------------------------------------------

TEST(palette_seeds_cover_every_token) {
  size_t n = 0;
  const uint32_t* seeds = gs_palette_seeds(&n);
  REQUIRE(seeds != nullptr);
  CHECK(n > 0);
  // Each token must appear at full alpha somewhere in the seed set, or the
  // burned DVR quantizes GS pixels to the nearest MSP-atlas colour.
  const uint32_t want[] = {tok::kTextPrimary, tok::kTextSecondary, tok::kTextLabel,
                           tok::kTrack, tok::kStatusOk, tok::kStatusCaution,
                           tok::kStatusRec};
  for (uint32_t w : want) {
    bool found = false;
    for (size_t i = 0; i < n; ++i)
      if (seeds[i] == premul(w, 255)) { found = true; break; }
    CHECK(found);
  }
}

MTEST_MAIN
