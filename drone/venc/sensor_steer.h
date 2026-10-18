/* Phase-lock steering of the sensor rate. The GS measures where frames land
 * against its display's vblank and sends a signed offset in milli-fps; the
 * drone sets the sensor to the boot trim's request plus that offset, clamped
 * to a fixed range, once the trim has finished. Pure logic: the caller
 * applies the request (MI_SNR_SetFps) on the encoder thread, the sensor's
 * one writer. */
#ifndef MABUR_VENC_SENSOR_STEER_H
#define MABUR_VENC_SENSOR_STEER_H

#include <stdint.h>

#include "sensor_cal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Milli-fps either side of the trim request; matches the wire's
 * mabur::rc::kSnrOffRangeMfps. */
#define SENSOR_STEER_RANGE_MFPS 200

typedef struct {
	uint32_t request_mfps; /* apply this to the sensor; 0 = leave it alone */
	int32_t offset_mfps;   /* the clamped offset the target request carries */
	int clamped;           /* the GS asked for more than the range */
} SensorSteer;

/* st/trim_mfps: the trim's state and current request. off_mfps: the GS's
 * offset. applied_mfps: the request now on the sensor. refused_mfps: a
 * request the sensor refused (0 = none); it is not retried until another
 * request succeeds. */
SensorSteer sensor_steer_plan(SensorCalState st, uint32_t trim_mfps,
	int32_t off_mfps, uint32_t applied_mfps, uint32_t refused_mfps);

#ifdef __cplusplus
}
#endif
#endif
