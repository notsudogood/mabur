#include "sensor_steer.h"

SensorSteer sensor_steer_plan(SensorCalState st, uint32_t trim_mfps,
	int32_t off_mfps, uint32_t applied_mfps, uint32_t refused_mfps,
	uint32_t max_mfps)
{
	SensorSteer s = {0, 0, 0};
	int32_t off = off_mfps;
	uint32_t want;

	if (off > SENSOR_STEER_RANGE_MFPS) {
		off = SENSOR_STEER_RANGE_MFPS;
		s.clamped = 1;
	} else if (off < -SENSOR_STEER_RANGE_MFPS) {
		off = -SENSOR_STEER_RANGE_MFPS;
		s.clamped = 1;
	}
	s.offset_mfps = off;
	/* The trim owns the sensor until it has locked (or given up and gone
	 * back to the boot request). */
	if (st != SENSOR_CAL_STATE_LOCKED && st != SENSOR_CAL_STATE_GAVE_UP)
		return s;
	if (trim_mfps == 0)
		return s;
	want = (uint32_t)((int64_t)trim_mfps + off);
	/* Never above the mode's ceiling: the driver refuses it, and a refusal
	 * leaves the sensor on its last, slower request. */
	if (max_mfps && want > max_mfps) {
		want = max_mfps;
		s.offset_mfps = (int32_t)((int64_t)max_mfps - trim_mfps);
		s.clamped = 1;
	}
	if (want == applied_mfps || want == refused_mfps)
		return s;
	s.request_mfps = want;
	return s;
}
