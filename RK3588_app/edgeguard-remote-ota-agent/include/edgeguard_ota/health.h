#ifndef EDGEGUARD_OTA_HEALTH_H
#define EDGEGUARD_OTA_HEALTH_H
#include "rauc_adapter.h"

/* Paths are initialized to production values. Overrides exist for isolated tests,
 * not environment variables or extra agent.conf keys. Adapter strings are borrowed. */
typedef struct {
    const OtaRaucAdapter *rauc;
    const char *userdata;
    const char *backend;
    const char *system_conf;
    const char *keyring;
    const char *hook; /* empty disables; absolute executable with no arguments */
    uint32_t timeout_sec; /* per external health operation, monotonic */
} OtaHealth;

void ota_health_init(OtaHealth *health, const OtaRaucAdapter *rauc);
/* No mark/reboot/persistence side effects. False => HEALTH_FAILED. The caller
 * may call guarded mark-bad; only its success permits durable ROLLBACK + reboot.
 * True permits guarded ota_rauc_mark_good, which rechecks identity. */
gboolean ota_health_check(const OtaHealth *health, OtaSlot expected, OtaError *error);
#endif
