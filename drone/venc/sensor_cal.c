#include "sensor_cal.h"

#include <stdlib.h>
#include <string.h>

/* A window must span SENSOR_CAL_WINDOW_US and hold this many clean steps
 * before its median is trusted. */
#define WINDOW_US SENSOR_CAL_WINDOW_US
#define MIN_STEPS 32
/* Steps skipped after a request: the driver applies new timing at a frame
 * boundary and the first intervals straddle both. */
#define SETTLE_STEPS 10
/* A step this long is a producer restart, not a frame interval. */
#define RESYNC_US 1000000u
/* Band under the target, in milli-Hz: the real rate must stay below the
 * target (the next stage drops anything above it) but no further than a
 * couple of sensor lines, so the band is wider than one line's 18 mHz
 * step and always reachable. */
#define BAND_LO_MHZ 30
#define BAND_HI_MHZ 4
/* The proportional step aims at the band's TOP edge, not its middle: the
 * driver rounds the request to a whole line (~18 mHz at 60 fps), so an
 * aim at the middle lands one line low half the time and every extra
 * line below the target shortens the beat against the GS panel. Aiming
 * at the top, the rounding lands on the nearest line below the target or
 * one line above it, and the above case costs one nudge down. */
#define BAND_AIM_MHZ BAND_HI_MHZ
/* One sensor line is ~18 mfps at 60 fps: the fallback nudge when the
 * proportional step rounds to the same request. */
#define NUDGE_MFPS 18

static void start_window(SensorCal *c, uint32_t pts, int settle)
{
	c->win_start_pts = pts;
	c->nsteps = 0;
	c->settle_left = settle;
}

void sensor_cal_init(SensorCal *c, uint32_t target_fps, uint32_t initial_mfps)
{
	memset(c, 0, sizeof(*c));
	c->target_fps = target_fps;
	c->request_mfps = initial_mfps;
	c->prev_request_mfps = initial_mfps;
	c->initial_mfps = initial_mfps;
	c->state = SENSOR_CAL_STATE_IDLE;
}

void sensor_cal_arm(SensorCal *c)
{
	c->state = SENSOR_CAL_STATE_MEASURING;
	c->rounds = 0;
	c->have_prev = 0;
	c->measured_hz = 0.0;
	start_window(c, 0, 0);
}

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return x < y ? -1 : x > y;
}

static double window_median_hz(SensorCal *c)
{
	uint32_t sorted[SENSOR_CAL_MAX_STEPS];
	memcpy(sorted, c->steps, (size_t)c->nsteps * sizeof(uint32_t));
	qsort(sorted, (size_t)c->nsteps, sizeof(uint32_t), cmp_u32);
	return 1e6 / (double)sorted[c->nsteps / 2];
}

SensorCalAction sensor_cal_feed(SensorCal *c, uint32_t pts_us)
{
	if (c->state != SENSOR_CAL_STATE_MEASURING)
		return SENSOR_CAL_NONE;
	if (!c->have_prev) {
		c->have_prev = 1;
		c->prev_pts = pts_us;
		start_window(c, pts_us, c->settle_left);
		return SENSOR_CAL_NONE;
	}
	const uint32_t delta = pts_us - c->prev_pts;
	c->prev_pts = pts_us;
	if (delta == 0 || delta > RESYNC_US) {
		start_window(c, pts_us, SETTLE_STEPS);
		return SENSOR_CAL_NONE;
	}
	if (c->settle_left > 0) {
		--c->settle_left;
		c->win_start_pts = pts_us;
		return SENSOR_CAL_NONE;
	}
	if (c->nsteps < SENSOR_CAL_MAX_STEPS)
		c->steps[c->nsteps++] = delta;
	if (pts_us - c->win_start_pts < WINDOW_US || c->nsteps < MIN_STEPS)
		return SENSOR_CAL_NONE;

	const double hz = window_median_hz(c);
	c->measured_hz = hz;
	const double target = (double)c->target_fps;
	const double lo = target - BAND_LO_MHZ / 1000.0;
	const double hi = target - BAND_HI_MHZ / 1000.0;
	if (hz >= lo && hz <= hi) {
		c->state = SENSOR_CAL_STATE_LOCKED;
		return SENSOR_CAL_LOCKED;
	}
	const double aim = target - BAND_AIM_MHZ / 1000.0;
	const double err = hz - aim;
	/* Not converging means the driver does not answer the request the
	 * way this model expects: leave the sensor exactly as the integer
	 * boot request set it rather than at some half-trimmed point. */
	if (++c->rounds >= SENSOR_CAL_MAX_ROUNDS) {
		c->state = SENSOR_CAL_STATE_GAVE_UP;
		c->prev_request_mfps = c->request_mfps;
		c->request_mfps = c->initial_mfps;
		return SENSOR_CAL_GAVE_UP;
	}
	/* Proportional step: the driver's rate is close to linear in the
	 * request over this tiny range. A step that rounds to the same
	 * request moves by one line instead. */
	uint32_t next = (uint32_t)((double)c->request_mfps * aim / hz + 0.5);
	if (next == c->request_mfps)
		next += err > 0 ? -NUDGE_MFPS : NUDGE_MFPS;
	const uint32_t floor_mfps = c->target_fps * 950, ceil_mfps = c->target_fps * 1050;
	if (next < floor_mfps) next = floor_mfps;
	if (next > ceil_mfps) next = ceil_mfps;
	c->prev_request_mfps = c->request_mfps;
	c->request_mfps = next;
	start_window(c, pts_us, SETTLE_STEPS);
	return SENSOR_CAL_REQUEST;
}

void sensor_cal_hold(SensorCal *c)
{
	if (c->state != SENSOR_CAL_STATE_MEASURING)
		return;
	c->have_prev = 0;
	start_window(c, 0, SETTLE_STEPS);
}

void sensor_cal_request_rejected(SensorCal *c)
{
	c->request_mfps = c->prev_request_mfps;
	c->state = SENSOR_CAL_STATE_GAVE_UP;
}

uint32_t sensor_cal_request_mfps(const SensorCal *c) { return c->request_mfps; }
double sensor_cal_measured_hz(const SensorCal *c) { return c->measured_hz; }
int sensor_cal_rounds(const SensorCal *c) { return c->rounds; }
SensorCalState sensor_cal_state(const SensorCal *c) { return c->state; }

uint32_t sensor_cal_bind_dst_fps(uint32_t requested_fps, uint32_t full_fps,
	uint32_t src_fps)
{
	if (src_fps == 0)
		return requested_fps;
	if (full_fps && requested_fps >= full_fps)
		return src_fps;
	return requested_fps;
}
