#include "mabur/rc_proto.h"
#include "mtest.h"
#include "sensor_steer.h"

static_assert(SENSOR_STEER_RANGE_MFPS == mabur::rc::kSnrOffRangeMfps,
              "drone and wire steering ranges must match");

TEST(waits_for_the_trim) {
  // Measuring or idle: the trim owns the sensor; no steering yet.
  CHECK(sensor_steer_plan(SENSOR_CAL_STATE_IDLE, 59718, 20, 60000, 0, 0).request_mfps == 0);
  CHECK(sensor_steer_plan(SENSOR_CAL_STATE_MEASURING, 59718, 20, 59718, 0, 0).request_mfps == 0);
}

TEST(trim_plus_offset_once_locked) {
  const SensorSteer s = sensor_steer_plan(SENSOR_CAL_STATE_LOCKED, 59718, 20, 59718, 0, 0);
  CHECK(s.request_mfps == 59738);
  CHECK(s.offset_mfps == 20);
  CHECK(!s.clamped);
  // Already applied: nothing to do.
  CHECK(sensor_steer_plan(SENSOR_CAL_STATE_LOCKED, 59718, 20, 59738, 0, 0).request_mfps == 0);
  // Gave up: steer around the boot request the trim fell back to.
  CHECK(sensor_steer_plan(SENSOR_CAL_STATE_GAVE_UP, 60000, -15, 60000, 0, 0).request_mfps == 59985);
}

TEST(zero_restores_the_trim_request) {
  const SensorSteer s = sensor_steer_plan(SENSOR_CAL_STATE_LOCKED, 59718, 0, 59738, 0, 0);
  CHECK(s.request_mfps == 59718);
  CHECK(s.offset_mfps == 0);
}

TEST(clamps_to_the_range) {
  const SensorSteer hi = sensor_steer_plan(SENSOR_CAL_STATE_LOCKED, 59718, 900, 59718, 0, 0);
  CHECK(hi.clamped);
  CHECK(hi.offset_mfps == SENSOR_STEER_RANGE_MFPS);
  CHECK(hi.request_mfps == 59718 + SENSOR_STEER_RANGE_MFPS);
  const SensorSteer lo = sensor_steer_plan(SENSOR_CAL_STATE_LOCKED, 59718, -900, 59718, 0, 0);
  CHECK(lo.clamped);
  CHECK(lo.request_mfps == 59718 - SENSOR_STEER_RANGE_MFPS);
}

TEST(refused_value_is_not_retried) {
  // The sensor refused 59738 once: asking again every frame would spam the
  // driver and the log. A different value is tried.
  CHECK(sensor_steer_plan(SENSOR_CAL_STATE_LOCKED, 59718, 20, 59718, 59738, 0).request_mfps == 0);
  CHECK(sensor_steer_plan(SENSOR_CAL_STATE_LOCKED, 59718, 21, 59718, 59738, 0).request_mfps == 59739);
}

// A mode whose ceiling is its nominal rate (1728x972@100: maxFps 100) has
// the driver refuse anything above 100000 mfps, and a refused request
// leaves the sensor on its last (slower) one. The steer clamps to the
// ceiling instead of asking.
TEST(clamps_to_the_mode_ceiling) {
  const SensorSteer s = sensor_steer_plan(SENSOR_CAL_STATE_LOCKED, 99958, 150, 99958, 0, 100000);
  CHECK(s.request_mfps == 100000);
  CHECK(s.clamped);
  // Already on the ceiling: nothing to send.
  CHECK(sensor_steer_plan(SENSOR_CAL_STATE_LOCKED, 99958, 150, 100000, 0, 100000).request_mfps == 0);
  // Under the ceiling: unchanged.
  CHECK(sensor_steer_plan(SENSOR_CAL_STATE_LOCKED, 99958, -40, 99958, 0, 100000).request_mfps == 99918);
}

MTEST_MAIN
