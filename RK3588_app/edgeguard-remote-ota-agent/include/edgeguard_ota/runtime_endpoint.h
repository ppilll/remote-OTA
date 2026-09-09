#ifndef EDGEGUARD_OTA_RUNTIME_ENDPOINT_H
#define EDGEGUARD_OTA_RUNTIME_ENDPOINT_H

#include "config.h"

#define OTA_RUNTIME_ENDPOINT_PATH "/userdata/edgeguard-provisioning/runtime.json"

/* Starts from the immutable configuration and consumes only the validated
 * base URL from Thread 1's runtime schema. Missing/invalid override data uses
 * the immutable default. Invalid override data is reported in diagnostic but
 * is not a fatal return; rejected file contents are never copied to errors. */
gboolean ota_runtime_endpoint_refresh(const OtaConfig *immutable_config,
                                      OtaConfig *cycle_config,
                                      gboolean *override_active,
                                      OtaError *diagnostic);

#endif
