#ifndef EDGEGUARD_OTA_TIME_SOURCE_H
#define EDGEGUARD_OTA_TIME_SOURCE_H
#include "model.h"
#include <time.h>
typedef int (*OtaClockRead)(void *user, clockid_t clock, struct timespec *value);
typedef struct { OtaClockRead read; void *user; } OtaTimeSource;
gboolean ota_monotonic_ms(const OtaTimeSource *source, uint64_t *out, OtaError *error);
gboolean ota_deadline_after(const OtaTimeSource *source, uint64_t delay_ms,
                            uint64_t *deadline, OtaError *error);
gboolean ota_deadline_expired(const OtaTimeSource *source, uint64_t deadline,
                              gboolean *expired, OtaError *error);
gboolean ota_time_telemetry(const OtaTimeSource *source, OtaReport *report, OtaError *error);
#endif
