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
    char last_result[EGP_GATT_VALUE_MAX_BYTES + 1u];
    guint refresh_source;
    guint expire_source;
    guint connman_watch;
    guint connman_retry_source;
    guint connman_retry_attempt;
    gboolean connman_present;
    gboolean shutting_down;
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
    /* Opening an already-open window also advances the authorization epoch.
     * Never carry a transaction or result binding across that boundary. */
    egp_protocol_drop_all(daemon->protocol);
    daemon->active_valid = FALSE;
    memset(&daemon->active_request, 0, sizeof(daemon->active_request));
    memset(daemon->result_peer, 0, sizeof(daemon->result_peer));
    memset(daemon->last_result, 0, sizeof(daemon->last_result));
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
                              gboolean encrypted_gatt_gate,
                              EgpPeerSecurity *peer, EgpError *error)
{
    return egp_bluez_peer_security(daemon->bluez, peer_path,
                                   encrypted_gatt_gate, peer, error);
}

static gboolean authorize_commit(gpointer user_data, const char *peer_path,
                                  guint64 window_epoch,
                                  guint64 connection_epoch,
                                  EgpError *error)
{
    Daemon *daemon = user_data;
    EgpPeerSecurity peer;
    return peer_security(daemon, peer_path, FALSE, &peer, error) &&
           egp_security_authorize_commit(daemon->security, peer_path, &peer,
                                         window_epoch, connection_epoch,
                                         g_get_monotonic_time(), error);
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
    gsize result_length = result_json ?
        strnlen(result_json, EGP_GATT_VALUE_MAX_BYTES + 1u) : 0;
    if (!daemon->active_valid || strcmp(peer_path, daemon->active_request.peer_path) ||
        !result_length || result_length > EGP_GATT_VALUE_MAX_BYTES)
        return;
    egp_bluez_invalidate_read(daemon->bluez, EGP_BLUEZ_OPERATION_RESULT, NULL);
    EgpError ignored = {0};
    egp_protocol_cache_result_id(daemon->protocol,
                                 daemon->active_request.peer_path,
                                 daemon->active_request.characteristic,
                                 daemon->active_request.transaction_id,
                                 result_json, g_get_monotonic_time(), &ignored);
    g_strlcpy(daemon->result_peer, peer_path, sizeof(daemon->result_peer));
    g_strlcpy(daemon->last_result, result_json, sizeof(daemon->last_result));
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
    char json[EGP_GATT_VALUE_MAX_BYTES + 1u] = {0};
    gsize length = 0;
    if (characteristic == EGP_BLUEZ_DEVICE_INFO) {
        if (!egp_status_device_info_json(daemon->status, json, &length, error))
            return FALSE;
    } else if (characteristic == EGP_BLUEZ_RUNTIME_STATUS) {
        EgpPeerSecurity peer;
        if (!peer_security(daemon, peer_path, TRUE, &peer, error) ||
            !egp_security_can_read_runtime(daemon->security, &peer, error) ||
            !egp_status_runtime_json(daemon->status, json, &length, error))
            return FALSE;
    } else if (characteristic == EGP_BLUEZ_OPERATION_RESULT) {
        EgpPeerSecurity peer;
        if (!peer_security(daemon, peer_path, TRUE, &peer, error) ||
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
    if (!peer_security(daemon, peer_path, TRUE, &peer, error) ||
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
        egp_bluez_disconnect_non_owner(daemon->bluez, peer_path);
    }
    if (output.disposition == EGP_PROTOCOL_INCOMPLETE)
        return TRUE;
    if (output.disposition == EGP_PROTOCOL_REPLAY) {
        if (output.cached_result[0]) {
            gsize cached_length = strnlen(output.cached_result,
                                          EGP_GATT_VALUE_MAX_BYTES + 1u);
            if (cached_length && cached_length <= EGP_GATT_VALUE_MAX_BYTES) {
                egp_bluez_invalidate_read(daemon->bluez,
                                           EGP_BLUEZ_OPERATION_RESULT, NULL);
                g_strlcpy(daemon->result_peer, peer_path,
                          sizeof(daemon->result_peer));
                g_strlcpy(daemon->last_result, output.cached_result,
                          sizeof(daemon->last_result));
            }
        }
        memset(&output, 0, sizeof(output));
        return TRUE;
    }
    output.request.window_epoch = egp_security_window_epoch(daemon->security);
    output.request.connection_epoch = peer.connection_epoch;
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
    egp_bluez_invalidate_read(daemon->bluez, EGP_BLUEZ_DEVICE_INFO, peer_path);
    egp_bluez_invalidate_read(daemon->bluez, EGP_BLUEZ_RUNTIME_STATUS, peer_path);
    egp_bluez_invalidate_read(daemon->bluez, EGP_BLUEZ_OPERATION_RESULT, peer_path);
}

static gboolean connman_reconcile_retry(gpointer user_data);

static void schedule_connman_reconcile(Daemon *daemon)
{
    if (!daemon || daemon->shutting_down || !daemon->connman_present ||
        daemon->connman_retry_source)
        return;
    guint delay = egp_connman_retry_delay_ms(daemon->connman_retry_attempt,
                                             g_random_int());
    daemon->connman_retry_source = g_timeout_add(delay,
                                                 connman_reconcile_retry,
                                                 daemon);
}

static void connman_reconciled(gpointer user_data, EgpErrorCode code,
                               gboolean retryable)
{
    Daemon *daemon = user_data;
    if (!daemon || daemon->shutting_down)
        return;
    if (code == EGP_ERROR_NONE) {
        daemon->connman_retry_attempt = 0;
        return;
    }
    if (retryable)
        schedule_connman_reconcile(daemon);
}

static gboolean connman_reconcile_retry(gpointer user_data)
{
    Daemon *daemon = user_data;
    daemon->connman_retry_source = 0;
    if (daemon->shutting_down || !daemon->connman_present)
        return G_SOURCE_REMOVE;
    if (daemon->connman_retry_attempt < G_MAXUINT)
        daemon->connman_retry_attempt++;
    EgpError error = {0};
    if (!egp_operations_reconcile(daemon->operations, connman_reconciled,
                                  daemon, &error))
        connman_reconciled(daemon, error.code, error.retryable);
    return G_SOURCE_REMOVE;
}

static void connman_appeared(GDBusConnection *connection, const char *name,
                             const char *owner, gpointer user_data)
{
    (void)connection;
    (void)name;
    (void)owner;
    Daemon *daemon = user_data;
    daemon->connman_present = TRUE;
    daemon->connman_retry_attempt = 0;
    schedule_connman_reconcile(daemon);
}

static void connman_vanished(GDBusConnection *connection, const char *name,
                             gpointer user_data)
{
    (void)connection;
    (void)name;
    Daemon *daemon = user_data;
    daemon->connman_present = FALSE;
    daemon->connman_retry_attempt = 0;
    if (daemon->connman_retry_source) {
        g_source_remove(daemon->connman_retry_source);
        daemon->connman_retry_source = 0;
    }
    egp_operations_connman_lost(daemon->operations);
    egp_status_set_provisioning(daemon->status,
                                EGP_STATE_FAILED_DEPENDENCY, FALSE);
}

static gboolean refresh_status(gpointer user_data)
{
    Daemon *daemon = user_data;
    egp_status_refresh(daemon->status);
    return G_SOURCE_CONTINUE;
}

static gboolean expire_protocol(gpointer user_data)
{
    Daemon *daemon = user_data;
    egp_protocol_expire(daemon->protocol, g_get_monotonic_time());
    return G_SOURCE_CONTINUE;
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
    daemon.connman_watch = g_bus_watch_name_on_connection(
        daemon.connection, "net.connman", G_BUS_NAME_WATCHER_FLAGS_NONE,
        connman_appeared, connman_vanished, &daemon, NULL);
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
    daemon.shutting_down = TRUE;
    if (daemon.connman_watch) g_bus_unwatch_name(daemon.connman_watch);
    if (daemon.connman_retry_source) g_source_remove(daemon.connman_retry_source);
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
