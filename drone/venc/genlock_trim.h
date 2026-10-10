#pragma once
/* Genlock sensor-trim policy (efficient-link plan step 2), dependency-free
 * so host tests pin it; star6e_controls_apply_sensor_mfps() is the only
 * caller on the drone. */
#include <stdint.h>

/* True when trimming the sensor's rate keeps the VPE->VENC bind 1:1: the
 * encoder is fed every frame the sensor makes (no low-power decimation).
 * running_fps is the rate the sensor was set to (state->sensor.fps), NOT
 * the mode's maximum: the IMX415's 1920x1080 mode tops out at 90 fps and is
 * run at 60, and comparing against the maximum refused every setpoint
 * without a word (bench 2026-10-10). */
static inline int genlock_trim_allowed(uint32_t running_fps, uint32_t delivered_fps)
{
	return running_fps != 0 && delivered_fps == running_fps;
}

/* Clamps a setpoint to +-1% of the running rate, in milli-fps -- far below
 * one frame a second, so the bind ratio and the RC fpsNum stay as they
 * are. 0 (release: back to the configured rate) passes through. */
static inline uint32_t genlock_trim_clamp(uint32_t mfps, uint32_t running_fps)
{
	const uint32_t lo = running_fps * 990u, hi = running_fps * 1010u;
	if (mfps == 0)
		return 0;
	return mfps < lo ? lo : (mfps > hi ? hi : mfps);
}
