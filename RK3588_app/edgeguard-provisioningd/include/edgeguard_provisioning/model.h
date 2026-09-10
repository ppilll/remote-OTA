#ifndef EDGEGUARD_PROVISIONING_MODEL_H
#define EDGEGUARD_PROVISIONING_MODEL_H

#include <glib.h>
#include <stdarg.h>
#include <stdint.h>

G_BEGIN_DECLS

#define EGP_SCHEMA_VERSION 1
#define EGP_STORE_MAX_BYTES (64u * 1024u)
#define EGP_SSID_MAX_BYTES 32u
#define EGP_PASSPHRASE_MAX_BYTES 64u
#define EGP_ENDPOINT_MAX_BYTES 512u
#define EGP_MESSAGE_CAP 256u
#define EGP_DBUS_PATH_CAP 512u
#define EGP_JSON_UINT_MAX G_MAXINT64

#define EGP_STORE_DIRECTORY "/userdata/edgeguard-provisioning"
#define EGP_WIFI_FILENAME "wifi.json"
#define EGP_RUNTIME_FILENAME "runtime.json"
#define EGP_FORGET_FILENAME ".wifi.json.forget"
#define EGP_RUNTIME_PATH EGP_STORE_DIRECTORY "/" EGP_RUNTIME_FILENAME
#define EGP_CONNMAN_PROFILE "/var/lib/connman/edgeguard-provisioning.config"

/* These names are protocol ABI. Append only; never reorder existing values. */
#define EGP_ERROR_CODES(X) \
    X(NONE) \
    X(BLE_UNAUTHORIZED) \
    X(BLE_NOT_PAIRED) \
    X(BLE_WINDOW_CLOSED) \
    X(BLE_PROTOCOL_UNSUPPORTED) \
    X(BLE_PAYLOAD_INVALID) \
    X(BLE_PAYLOAD_TOO_LARGE) \
    X(BLE_FRAGMENT_CONFLICT) \
    X(BLE_TRANSACTION_TIMEOUT) \
    X(BLE_REPLAY_REJECTED) \
    X(PROVISIONING_BUSY) \
    X(PROVISIONING_RACE) \
    X(WIFI_CONFIG_INVALID) \
    X(WIFI_AUTH_FAILED) \
    X(WIFI_AP_NOT_FOUND) \
    X(WIFI_CONNECT_TIMEOUT) \
    X(PERSISTENCE_READ_FAILED) \
    X(PERSISTENCE_WRITE_FAILED) \
    X(PERSISTENCE_SCHEMA_UNSUPPORTED) \
    X(ENDPOINT_INVALID) \
    X(ENDPOINT_CONFIG_INVALID) \
    X(CONNMAN_UNAVAILABLE) \
    X(CONNMAN_APPLY_FAILED) \
    X(BLUEZ_UNAVAILABLE) \
    X(GATT_REGISTRATION_FAILED) \
    X(ADVERTISEMENT_FAILED) \
    X(AGENT_BUSY) \
    X(AGENT_UNAVAILABLE) \
    X(OTA_CONTROL_REJECTED) \
    X(FACTORY_RESET_BLOCKED) \
    X(INTERNAL_ERROR)

typedef enum {
#define EGP_ERROR_ENUM(name) EGP_ERROR_##name,
    EGP_ERROR_CODES(EGP_ERROR_ENUM)
#undef EGP_ERROR_ENUM
    EGP_ERROR_COUNT
} EgpErrorCode;

typedef enum {
    EGP_PERSISTENT_CHANGE_NONE,
    EGP_PERSISTENT_CHANGE_COMMITTED,
    EGP_PERSISTENT_CHANGE_ROLLED_BACK,
    EGP_PERSISTENT_CHANGE_UNCERTAIN
} EgpPersistentChange;

typedef struct {
    EgpErrorCode code;
    gboolean retryable;
    EgpPersistentChange persistent_change;
    char message[EGP_MESSAGE_CAP];
} EgpError;

typedef enum {
    EGP_STATE_UNPROVISIONED,
    EGP_STATE_STORED,
    EGP_STATE_APPLYING,
    EGP_STATE_ASSOCIATING,
    EGP_STATE_CONNECTED,
    EGP_STATE_FAILED_CONFIG,
    EGP_STATE_FAILED_AUTH,
    EGP_STATE_FAILED_NOT_FOUND,
    EGP_STATE_FAILED_TIMEOUT,
    EGP_STATE_FAILED_DEPENDENCY
} EgpProvisioningState;

/* Fixed-size structs own all bytes. A copied EgpWifiConfig is another secret
 * copy and must be cleared with egp_wifi_clear() as soon as it is no longer
 * needed. generation is assigned by the store, never trusted from a request. */
typedef struct {
    guint64 generation;
    char ssid[EGP_SSID_MAX_BYTES + 1u];
    char security[4]; /* R5 v1: exactly "psk". */
    char passphrase[EGP_PASSPHRASE_MAX_BYTES + 1u];
    gboolean hidden;
} EgpWifiConfig;

typedef struct {
    guint64 generation;
    char ota_server_base_url[EGP_ENDPOINT_MAX_BYTES + 1u];
} EgpRuntimeConfig;

const char *egp_error_code_name(EgpErrorCode code);
const char *egp_provisioning_state_name(EgpProvisioningState state);
void egp_error_clear(EgpError *error);
void egp_error_set(EgpError *error, EgpErrorCode code, gboolean retryable,
                   EgpPersistentChange change, const char *format, ...)
    G_GNUC_PRINTF(5, 6);
void egp_wifi_clear(EgpWifiConfig *wifi);
gboolean egp_wifi_validate(const EgpWifiConfig *wifi, EgpError *error);

G_END_DECLS

#endif
