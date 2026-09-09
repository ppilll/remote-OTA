#ifndef EDGEGUARD_PROVISIONING_STATUS_H
#define EDGEGUARD_PROVISIONING_STATUS_H

#include "store.h"

G_BEGIN_DECLS

#define EGP_STATUS_MAX_JSON 1024u
#define EGP_AGENT_STATE_PATH "/userdata/edgeguard-remote-ota/agent-state.json"
#define EGP_DEVICE_ID_PATH "/userdata/edgeguard-remote-ota/device-id"
#define EGP_RELEASE_PATH "/etc/edgeguard-ota/release.json"
#define EGP_AGENT_CONFIG_PATH "/etc/edgeguard-ota/agent.conf"
#define EGP_AGENT_CONTROL_SOCKET "/run/edgeguard-remote-ota/control.sock"

typedef struct _EgpStatus EgpStatus;

EgpStatus *egp_status_new(EgpStore *store);
void egp_status_free(EgpStatus *status);
void egp_status_set_provisioning(EgpStatus *status,
                                 EgpProvisioningState state,
                                 gboolean wifi_connected);

/* Refreshes bounded read-only sources. Missing/corrupt sources remain explicit
 * unavailable fields; no value is guessed and no Agent state is written. */
void egp_status_refresh(EgpStatus *status);
gboolean egp_status_agent_idle(EgpStatus *status, char observation[32],
                               EgpError *error);
gboolean egp_status_device_info_json(EgpStatus *status, char output[EGP_STATUS_MAX_JSON + 1u],
                                     gsize *length, EgpError *error);
gboolean egp_status_runtime_json(EgpStatus *status, char output[EGP_STATUS_MAX_JSON + 1u],
                                 gsize *length, EgpError *error);

/* Fixed-path, fixed-command IPC client. It never falls back to a signal,
 * restart, shell, or direct OTA operation. */
gboolean egp_status_check_update_now(EgpStatus *status,
                                     const guint8 transaction_id[16],
                                     EgpError *error);

G_END_DECLS

#endif
