#include "edgeguard_ota/reboot.h"
#include <errno.h>
#include <unistd.h>

gboolean ota_reboot_production(void *user, OtaError *error)
{
    (void)user;
    char *const argv[] = {"/sbin/reboot", NULL};
    char *const environment[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C", NULL};
    sync();
    execve(argv[0], argv, environment);
    ota_error_set(error, OTA_ERROR_REBOOT_FAILED, "execve /sbin/reboot failed: %s", g_strerror(errno));
    return FALSE;
}

gboolean ota_reboot_controlled(const char *state_path, const OtaPersistentState *state,
                               const OtaPersistenceOps *persistence,
                               const OtaRebootOps *reboot, OtaError *error)
{
    if (state->state != OTA_STATE_REBOOT_PENDING &&
        !(state->state == OTA_STATE_ROLLBACK && state->reboot_context == OTA_REBOOT_HEALTH_FAILURE)) {
        ota_error_set(error, OTA_ERROR_REBOOT_CONTEXT_INVALID, "State does not authorize reboot");
        return FALSE;
    }
    if (!ota_persistence_save(state_path, state, persistence, error)) return FALSE;
    OtaRebootRequest request = reboot && reboot->request ? reboot->request : ota_reboot_production;
    if (!request(reboot ? reboot->user : NULL, error)) {
        if (!error || error->code == OTA_ERROR_NONE)
            ota_error_set(error, OTA_ERROR_REBOOT_FAILED, "Controlled reboot request failed");
        return FALSE;
    }
    return TRUE;
}
