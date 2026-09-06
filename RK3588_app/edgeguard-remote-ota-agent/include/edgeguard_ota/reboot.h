#ifndef EDGEGUARD_OTA_REBOOT_H
#define EDGEGUARD_OTA_REBOOT_H
#include "persistence.h"
typedef gboolean (*OtaRebootRequest)(void *user, OtaError *error);
typedef struct { OtaRebootRequest request; void *user; } OtaRebootOps;
/* Default production operation: sync() then execve /sbin/reboot, fixed argv. */
gboolean ota_reboot_production(void *user, OtaError *error);
/* Always re-persists and fsyncs before invoking even a fake reboot callback. */
gboolean ota_reboot_controlled(const char *state_path, const OtaPersistentState *state,
                               const OtaPersistenceOps *persistence,
                               const OtaRebootOps *reboot, OtaError *error);
#endif
