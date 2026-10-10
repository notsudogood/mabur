// Genlock sensor-trim policy (drone/venc/genlock_trim.h): the one piece of
// star6e_controls_apply_sensor_mfps that decides whether a GS setpoint
// reaches the sensor. It was untested ARM-only C until the 2026-10-10
// bench, where it refused every setpoint silently.
#include "genlock_trim.h"
#include "mtest.h"

TEST(trim_allowed_on_a_1to1_bind_whatever_the_mode_maximum) {
  // Bench 2026-10-10: IMX415 mode [2] "1920x1080@90fps" (max 90) run at 60,
  // encoder fed 60. The old check compared the mode maximum (90) with the
  // running rate (60) and refused.
  CHECK(genlock_trim_allowed(60, 60));
  CHECK(genlock_trim_allowed(90, 90));
}

TEST(trim_refused_when_the_bind_decimates_or_nothing_runs) {
  CHECK(!genlock_trim_allowed(60, 30));  // low power: every other frame
  CHECK(!genlock_trim_allowed(0, 0));
  CHECK(!genlock_trim_allowed(60, 0));
}

TEST(clamp_is_one_percent_of_the_running_rate_and_release_passes) {
  CHECK(genlock_trim_clamp(0, 60) == 0);            // release
  CHECK(genlock_trim_clamp(59940, 60) == 59940);    // in band
  CHECK(genlock_trim_clamp(59000, 60) == 59400);    // -1%
  CHECK(genlock_trim_clamp(61000, 60) == 60600);    // +1%
  // Against the RUNNING rate, not the mode maximum: 90 would allow 89100.
  CHECK(genlock_trim_clamp(59400, 60) == 59400);
}

MTEST_MAIN
