#include "camera_modes.h"

/* Up to 90 fps: the 2x2-binned full view. Above it the sensor cannot read
 * the full height fast enough (a wide binned line needs ~4.9 us and the
 * full height ~2250 lines), so 91-100 and 101-120 use the widest binned
 * crops that stay clean at those rates. */
static const CameraMode kFullView = {1920, 1080, 90};
static const CameraMode kWide100 = {1728, 972, 100};
static const CameraMode kWide120 = {1728, 816, 120};
static const CameraMode k4K = {3840, 2160, 33};

CameraMode camera_mode_for(uint32_t venc_fps, uint32_t rec_w, uint32_t rec_h)
{
	if (rec_w > 1920 || rec_h > 1080)
		return k4K;
	if (venc_fps > kWide100.max_fps)
		return kWide120;
	if (venc_fps > kFullView.max_fps)
		return kWide100;
	return kFullView;
}

int camera_mode_find(const CameraMode *list, int n, CameraMode want)
{
	for (int i = 0; i < n; ++i)
		if (list[i].w == want.w && list[i].h == want.h &&
		    list[i].max_fps == want.max_fps)
			return i;
	return -1;
}

int camera_mode_size_ok(CameraMode mode, uint32_t w, uint32_t h)
{
	return (uint64_t)w * mode.h == (uint64_t)h * mode.w &&
		w <= mode.w && h <= mode.h;
}
