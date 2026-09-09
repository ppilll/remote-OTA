#include "edgeguard_provisioning/bluez.h"
#include "edgeguard_provisioning/input.h"
#include "edgeguard_provisioning/operations.h"

#include <glib-unix.h>
#include <signal.h>
#include <string.h>

typedef struct {
    GMainLoop *loop;
    GDBusConnection *connection;
    EgpStore *store;
    EgpConnman *connman;
    EgpStatus *status;
    EgpProtocol *protocol;
    EgpSecurity *security;
    EgpBluez *bluez;
    EgpInput *input;
    EgpOperations *operations;
    gboolean one_shot;
    gboolean active_valid;
    EgpProtocolRequest active_request;
    char result_peer[EGP_DBUS_PATH_CAP];
    char last_result[EGP_PROTOCOL_MAX_JSON + 1u];
    guint refresh_source;
    guint expire_source;
} Daemon;

static gboolean stop_daemon(gpointer user_data)
{
    Daemon *daemon = user_data;
    if (daemon->loop)
        g_main_loop_quit(daemon->loop);
    return G_SOURCE_REMOVE;
}

static void window_changed(gpointer user_data, gboolean open)
{
    Daemon *daemon = user_data;
    if (daemon->bluez)
        egp_bluez_set_window(daemon->bluez, open);
    if (!open) {
        egp_protocol_drop_all(daemon->protocol);
        memset(daemon->result_peer, 0, sizeof(daemon->result_peer));
        memset(daemon->last_result, 0, sizeof(daemon->last_result));
    }
}

static void physical_presence(gpointer user_data)
{
    Daemon *daemon = user_data;
    egp_security_open_window(daemon->security, g_get_monotonic_time());
}

static void input_lost(gpointer user_data)
{
    Daemon *daemon = user_data;
    egp_security_close_window(daemon->security);
}

static gboolean peer_security(Daemon *daemon, const char *peer_path,
                              EgpPeerSecurity *peer, EgpError *error)
{
    return egp_bluez_peer_security(daemon->bluez, peer_path, peer, error);
}

static gboolean authorize_commit(gpointer user_data, const char *peer_path,
                                 EgpError *error)
{
    Daemon *daemon = user_data;
    EgpPeerSecurity peer;
    return peer_security(daemon, peer_path, &peer, error) &&
           egp_security_authorize_sensitive(daemon->security, peer_path, &peer,
                                            FALSE, g_get_monotonic_time(), error);
}

static gboolean close_one_shot(gpointer user_data)
{
    Daemon *daemon = user_data;
    egp_security_close_window(daemon->security);
    return G_SOURCE_REMOVE;
}

static void result_published(gpointer user_data, const char *peer_path,
                             const char *result_json)
{
    Daemon *daemon = user_data;
    if (!daemon->active_valid || strcmp(peer_path, daemon->active_request.peer_path))
        return;
    EgpError ignored = {0};
    egp_protocol_cache_result_id(daemon->protocol,
                                 daemon->active_request.peer_path,
                                 daemon->active_request.characteristic,
                                 daemon->active_request.transaction_id,
                                 result_json, g_get_monotonic_time(), &ignored);
    g_strlcpy(daemon->result_peer, peer_path, sizeof(daemon->result_peer));
    g_strlcpy(daemon->last_result, result_json, sizeof(daemon->last_result));
    if (egp_security_owner(daemon->security) &&
        !strcmp(egp_security_owner(daemon->security), peer_path))
        egp_bluez_publish(daemon->bluez, EGP_BLUEZ_OPERATION_RESULT, result_json);
    if (strstr(result_json, "\"status\":\"SUCCEEDED\"")) {
        daemon->active_valid = FALSE;
        memset(&daemon->active_request, 0, sizeof(daemon->active_request));
        if (daemon->one_shot)
            g_idle_add(close_one_shot, daemon);
    } else if (strstr(result_json, "\"status\":\"FAILED\"")) {
        daemon->active_valid = FALSE;
        memset(&daemon->active_request, 0, sizeof(daemon->active_request));
    }
}

static gboolean read_value(gpointer user_data, EgpBluezCharacteristic characteristic,
                           const char *peer_path, GBytes **value, EgpError *error)
{
    Daemon *daemon = user_data;
    char json[EGP_STATUS_MAX_JSON + 1u] = {0};
    gsize length = 0;
    if (characteristic == EGP_BLUEZ_DEVICE_INFO) {
        if (!egp_status_device_info_json(daemon->status, json, &length, error))
            return FALSE;
    } else if (characteristic == EGP_BLUEZ_RUNTIME_STATUS) {
        EgpPeerSecurity peer;
        if (!peer_security(daemon, peer_path, &peer, error) ||
            !egp_security_can_read_runtime(daemon->security, &peer, error) ||
            !egp_status_runtime_json(daemon->status, json, &length, error))
            return FALSE;
    } else if (characteristic == EGP_BLUEZ_OPERATION_RESULT) {
        EgpPeerSecurity peer;
        if (!peer_security(daemon, peer_path, &peer, error) ||
            !egp_security_can_read_result(daemon->security, peer_path, &peer, error))
            return FALSE;
        if (!daemon->last_result[0] || strcmp(peer_path, daemon->result_peer)) {
            egp_error_set(error, EGP_ERROR_BLE_UNAUTHORIZED, TRUE,
                          EGP_PERSISTENT_CHANGE_NONE,
                          "No result belongs to this authorized peer");
            return FALSE;
        }
        length = strlen(daemon->last_result);
        memcpy(json, daemon->last_result, length + 1u);
    } else {
        egp_error_set(error, EGP_ERROR_BLE_PAYLOAD_INVALID, FALSE,
                      EGP_PERSISTENT_CHANGE_NONE,
                      "Characteristic is not readable");
        return FALSE;
    }
    *value = g_bytes_new(json, length);
    egp_error_clear(error);
    return TRUE;
}

static gboolean write_value(gpointer user_data, EgpBluezCharacteristic characteristic,
                            const char *peer_path, const guint8 *value, gsize length,
                            EgpError *error)
{
    Daemon *daemon = user_data;
    EgpWritableCharacteristic writable;
    if (characteristic == EGP_BLUEZ_PROVISIONING_REQUEST)
        writable = EGP_WRITABLE_PROVISIONING;
    else if (characteristic == EGP_BLUEZ_CONTROL_REQUEST)
        writable = EGP_WRITABLE_CONTROL;
    else {
        egp_error_set(error, EGP_ERROR_BLE_PAYLOAD_INVALID, FALSE,
                      EGP_PERSISTENT_CHANGE_NONE,
                      "Characteristic is not writable");
        return FALSE;
    }
    if (egp_operations_busy(daemon->operations)) {
        egp_error_set(error, EGP_ERROR_PROVISIONING_BUSY, TRUE,
                      EGP_PERSISTENT_CHANGE_NONE,
                      "Another provisioning operation is active");
        return FALSE;
    }
    EgpPeerSecurity peer;
    const char *owner_before = egp_security_owner(daemon->security);
    gboolean claiming = owner_before == NULL;
    if (!peer_security(daemon, peer_path, &peer, error) ||
        !egp_security_allow_pairing(daemon->security, peer_path,
                                    g_get_monotonic_time(), error))
        return FALSE;
    char agent_observation[32];
    if (!egp_status_agent_idle(daemon->status, agent_observation, error)) {
        egp_protocol_drop_peer(daemon->protocol, peer_path);
        return FALSE;
    }

    EgpProtocolOutput output = {0};
    if (!egp_protocol_accept(daemon->protocol, peer_path, writable,
                             value, length, g_get_monotonic_time(),
                             &output, error))
        return FALSE;
    if (!egp_security_authorize_sensitive(daemon->security, peer_path, &peer,
                                          TRUE, g_get_monotonic_time(), error)) {
        egp_protocol_drop_all(daemon->protocol);
        if (error->code == EGP_ERROR_BLE_WINDOW_CLOSED)
            egp_security_close_window(daemon->security);
        memset(&output, 0, sizeof(output));
        return FALSE;
    }
    if (claiming) {
        egp_bluez_set_result_owner(daemon->bluez, TRUE);
        egp_bluez_disconnect_non_owner(daemon->bluez, peer_path);
    }
    if (output.disposition == EGP_PROTOCOL_INCOMPLETE)
        return TRUE;
    if (output.disposition == EGP_PROTOCOL_REPLAY) {
        if (output.cached_result[0]) {
            g_strlcpy(daemon->result_peer, peer_path, sizeof(daemon->result_peer));
            g_strlcpy(daemon->last_result, output.cached_result,
                      sizeof(daemon->last_result));
            egp_bluez_publish(daemon->bluez, EGP_BLUEZ_OPERATION_RESULT,
                              output.cached_result);
        }
        memset(&output, 0, sizeof(output));
        return TRUE;
    }
    daemon->active_request = output.request;
    memset(daemon->active_request.json, 0,
           sizeof(daemon->active_request.json));
    daemon->active_request.json_length = 0;
    daemon->active_valid = TRUE;
    gboolean submitted = egp_operations_submit(daemon->operations,
                                                &output.request, error);
    if (!submitted) {
        daemon->active_valid = FALSE;
        memset(&daemon->active_request, 0, sizeof(daemon->active_request));
    }
    memset(&output, 0, sizeof(output));
    return submitted;
}

static gboolean allow_pairing(gpointer user_data, const char *peer_path,
                              EgpError *error)
{
    Daemon *daemon = user_data;
    return egp_security_allow_pairing(daemon->security, peer_path,
                                      g_get_monotonic_time(), error);
}

static void bluez_lost(gpointer user_data)
{
    Daemon *daemon = user_data;
    egp_security_bluez_lost(daemon->security);
    egp_protocol_drop_all(daemon->protocol);
    daemon->active_valid = FALSE;
    memset(&daemon->active_request, 0, sizeof(daemon->active_request));
    memset(daemon->result_peer, 0, sizeof(daemon->result_peer));
    memset(daemon->last_result, 0, sizeof(daemon->last_result));
}

static void peer_disconnected(gpointer user_data, const char *peer_path)
{
    Daemon *daemon = user_data;
    egp_protocol_drop_peer(daemon->protocol, peer_path);
}

static gboolean refresh_status(gpointer user_data)
{
    Daemon *daemon = user_data;
    egp_status_refresh(daemon->status);
    char json[EGP_STATUS_MAX_JSON + 1u];
    gsize length = 0;
    EgpError ignored = {0};
    if (egp_status_runtime_json(daemon->status, json, &length, &ignored))
        egp_bluez_publish(daemon->bluez, EGP_BLUEZ_RUNTIME_STATUS, json);
    return G_SOURCE_CONTINUE;
}

static gboolean expire_protocol(gpointer user_data)
{
    Daemon *daemon = user_data;
    egp_protocol_expire(daemon->protocol, g_get_monotonic_time());
    return G_SOURCE_CONTINUE;
}

static void startup_reconcile(Daemon *daemon)
{
    EgpError error = {0};
    EgpWifiConfig wifi = {0};
    gboolean pending = FALSE;
    if (egp_store_forget_pending(daemon->store, &wifi, &pending, &error) && pending) {
        EgpConnmanResult result = {0};
        if (egp_connman_revoke(daemon->connman, &wifi, 30000, NULL,
                               &result, &error) &&
            egp_store_forget_finish(daemon->store, &error))
            egp_status_set_provisioning(daemon->status,
                                        EGP_STATE_UNPROVISIONED, FALSE);
        else
            egp_status_set_provisioning(daemon->status,
                                        EGP_STATE_FAILED_DEPENDENCY, FALSE);
        egp_wifi_clear(&wifi);
        return;
    }
    egp_wifi_clear(&wifi);
    gboolean present = FALSE;
    if (!egp_store_load_wifi(daemon->store, &wifi, &present, &error)) {
        egp_status_set_provisioning(daemon->status, EGP_STATE_FAILED_CONFIG, FALSE);
    } else if (!present) {
        egp_status_set_provisioning(daemon->status, EGP_STATE_UNPROVISIONED, FALSE);
    } else if (!egp_connman_write_profile(daemon->connman, &wifi, &error)) {
        egp_status_set_provisioning(daemon->status,
                                    EGP_STATE_FAILED_DEPENDENCY, FALSE);
    } else {
        egp_status_set_provisioning(daemon->status, EGP_STATE_STORED, FALSE);
    }
    egp_wifi_clear(&wifi);
}

static gboolean parse_uint(const char *value, guint *output)
{
    if (!value || !*value || strspn(value, "0123456789") != strlen(value))
        return FALSE;
    guint64 parsed = g_ascii_strtoull(value, NULL, 10);
    if (parsed > G_MAXUINT)
        return FALSE;
    *output = (guint)parsed;
    return TRUE;
}

int main(int argc, char **argv)
{
    guint key_code = 0, hold_ms = 0;
    gboolean one_shot = FALSE;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--one-shot")) {
            one_shot = TRUE;
        } else if ((!strcmp(argv[i], "--key-code") || !strcmp(argv[i], "--hold-ms")) &&
                   i + 1 < argc) {
            guint parsed;
            if (!parse_uint(argv[++i], &parsed)) {
                g_printerr("Invalid numeric physical-presence option\n");
                return 2;
            }
            if (!strcmp(argv[i - 1], "--key-code"))
                key_code = parsed;
            else
                hold_ms = parsed;
        } else {
            g_printerr("Usage: %s [--key-code N --hold-ms N] [--one-shot]\n", argv[0]);
            return 2;
        }
    }
    if ((!key_code && hold_ms) || (key_code && !hold_ms)) {
        g_printerr("Both --key-code and --hold-ms are required together\n");
        return 2;
    }

    Daemon daemon = { .one_shot = one_shot };
    EgpError error = {0};
    GError *gerror = NULL;
    int result = 1;
    daemon.loop = g_main_loop_new(NULL, FALSE);
    daemon.connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &gerror);
    if (!daemon.connection) {
        g_printerr("BLUEZ_UNAVAILABLE: system bus unavailable\n");
        g_clear_error(&gerror);
        goto done;
    }
    if (!egp_store_open(NULL, &daemon.store, &error) ||
        !egp_connman_new(daemon.connection, NULL, &daemon.connman, &error))
        goto failed;
    daemon.status = egp_status_new(daemon.store);
    daemon.protocol = egp_protocol_new();
    daemon.security = egp_security_new(window_changed, &daemon);
    if (!daemon.status || !daemon.protocol || !daemon.security) {
        egp_error_set(&error, EGP_ERROR_INTERNAL_ERROR, TRUE,
                      EGP_PERSISTENT_CHANGE_NONE,
                      "Provisioning daemon initialization failed");
        goto failed;
    }
    startup_reconcile(&daemon);
    egp_status_refresh(daemon.status);
    EgpBluezHandlers handlers = {
        .user_data = &daemon,
        .read_value = read_value,
        .write_value = write_value,
        .allow_pairing = allow_pairing,
        .peer_disconnected = peer_disconnected,
        .bluez_lost = bluez_lost
    };
    daemon.bluez = egp_bluez_new(daemon.connection, &handlers, &error);
    if (!daemon.bluez)
        goto failed;
    daemon.operations = egp_operations_new(daemon.store, daemon.connman,
                                           daemon.status, authorize_commit,
                                           result_published, &daemon);
    if (!daemon.operations) {
        egp_error_set(&error, EGP_ERROR_INTERNAL_ERROR, TRUE,
                      EGP_PERSISTENT_CHANGE_NONE,
                      "Operation worker initialization failed");
        goto failed;
    }
    daemon.input = egp_input_new(key_code, hold_ms, physical_presence,
                                 input_lost, &daemon, &error);
    if (!daemon.input) {
        g_warning("Physical-presence input unavailable; provisioning window remains closed: %s",
                  error.message);
        egp_error_clear(&error);
    }
    daemon.refresh_source = g_timeout_add_seconds(5, refresh_status, &daemon);
    daemon.expire_source = g_timeout_add_seconds(1, expire_protocol, &daemon);
    g_unix_signal_add(SIGINT, stop_daemon, &daemon);
    g_unix_signal_add(SIGTERM, stop_daemon, &daemon);
    g_main_loop_run(daemon.loop);
    result = 0;
    goto done;

failed:
    g_printerr("%s: %s\n", egp_error_code_name(error.code), error.message);
done:
    if (daemon.refresh_source) g_source_remove(daemon.refresh_source);
    if (daemon.expire_source) g_source_remove(daemon.expire_source);
    egp_input_free(daemon.input);
    if (daemon.security) egp_security_close_window(daemon.security);
    egp_operations_free(daemon.operations);
    egp_bluez_free(daemon.bluez);
    egp_security_free(daemon.security);
    egp_protocol_free(daemon.protocol);
    egp_status_free(daemon.status);
    egp_connman_free(daemon.connman);
    egp_store_close(daemon.store);
    g_clear_object(&daemon.connection);
    if (daemon.loop) g_main_loop_unref(daemon.loop);
    memset(&daemon.active_request, 0, sizeof(daemon.active_request));
    memset(daemon.last_result, 0, sizeof(daemon.last_result));
    return result;
}
