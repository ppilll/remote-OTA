#ifndef EDGEGUARD_OTA_RAUC_ADAPTER_H
#define EDGEGUARD_OTA_RAUC_ADAPTER_H
#include "model.h"

/* A runner executes exactly argv, never a shell. stdout is newly allocated with
 * g_malloc on success. Nonzero exit, signal, timeout and spawn errors fail.
 * stderr is inherited for diagnostics. The callback is a test injection seam. */
typedef gboolean (*OtaCommandRun)(void *user, const char *const argv[],
                                 uint32_t timeout_ms, char **output, GError **error);
typedef struct {
    OtaCommandRun run;
    void *user;
    const char *binary;
    const char *abctl;
    uint32_t query_timeout_ms;
    uint32_t install_timeout_ms;
} OtaRaucAdapter;

void ota_rauc_adapter_init(OtaRaucAdapter *adapter);
gboolean ota_command_run(void *user, const char *const argv[], uint32_t timeout_ms,
                         char **output, GError **error);
gboolean ota_rauc_current_slot(const OtaRaucAdapter *adapter, OtaSlot *slot, OtaError *error);
gboolean ota_rauc_verify(const OtaRaucAdapter *adapter, const char *bundle,
                          const OtaRelease *release, OtaError *error);
/* Exactly one invocation. Failure is RAUC_INSTALL_FAILED; no retry/restart. */
gboolean ota_rauc_install(const OtaRaucAdapter *adapter, const char *bundle, OtaError *error);
gboolean ota_rauc_status(const OtaRaucAdapter *adapter, OtaError *error);
/* Both mutations re-read current immediately before invoking RAUC. Orchestration
 * must also own expected_candidate and persist/reboot after successful mark-bad. */
gboolean ota_rauc_mark_good(const OtaRaucAdapter *adapter, OtaSlot expected, OtaError *error);
gboolean ota_rauc_mark_bad(const OtaRaucAdapter *adapter, OtaSlot expected, OtaError *error);
#endif
