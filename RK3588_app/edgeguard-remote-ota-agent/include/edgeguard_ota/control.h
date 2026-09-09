#ifndef EDGEGUARD_OTA_CONTROL_H
#define EDGEGUARD_OTA_CONTROL_H

#include "model.h"
#include <signal.h>

typedef struct _OtaControl OtaControl;

#define OTA_CONTROL_SOCKET "/run/edgeguard-remote-ota/control.sock"

/* Creates the root-only v1 control socket and its bounded handler thread. */
gboolean ota_control_open(OtaControl **out, OtaError *error);

/* Makes CHECK_UPDATE_NOW eligible only for the duration of this IDLE wait.
 * Returns immediately for an accepted request and otherwise after timeout. */
gboolean ota_control_idle_wait(OtaControl *control, uint32_t timeout_sec,
                               const volatile sig_atomic_t *stopping,
                               gboolean *check_requested, OtaError *error);

/* Async-signal-safe wakeup used by the Agent's SIGINT/SIGTERM handler. */
void ota_control_signal_wakeup(OtaControl *control);

/* Stops the handler, removes only the owned socket, and releases resources. */
void ota_control_close(OtaControl *control);

#endif
