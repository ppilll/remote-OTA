#ifndef EDGEGUARD_OTA_REPORTING_H
#define EDGEGUARD_OTA_REPORTING_H
#include "config.h"
#include "time_source.h"
/* Pure mapping: legacy never emits statuses outside the frozen R2 set. */
const char *ota_report_status(OtaState state, OtaReportingMode mode);
gboolean ota_report_build(const char *device_id, const OtaRelease *local,
                          const OtaPersistentState *state, const OtaTimeSource *time,
                          OtaReport *out, OtaError *error);
/* Caller frees *json with g_free. Only explicit EXTENDED adds fields. */
gboolean ota_report_serialize(const OtaReport *report, OtaReportingMode mode,
                              char **json, OtaError *error);
gboolean ota_report_send(const OtaConfig *config, const OtaReport *report,
                         gboolean *retryable, OtaError *error);
#endif
