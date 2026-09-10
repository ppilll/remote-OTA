#define _GNU_SOURCE
#include "edgeguard_provisioning/status.h"
#include "edgeguard_provisioning/protocol.h"
#include "edgeguard_provisioning/endpoint.h"

#include <errno.h>
#include <fcntl.h>
#include <json-glib/json-glib.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define EXTERNAL_SOURCE_TIMEOUT_MS 2000

struct _EgpStatus {
    GMutex lock;
    EgpStore *store;
    EgpProvisioningState provisioning_state;
    gboolean wifi_connected;
    gboolean device_available;
    char device_id[37];
    gboolean release_available;
    char release[EGP_MESSAGE_CAP];
    char build_id[EGP_MESSAGE_CAP];
    gboolean runtime_available;
    char endpoint[EGP_ENDPOINT_MAX_BYTES + 1u];
    char endpoint_source[32];
    gboolean runtime_config_invalid;
    gboolean agent_available;
    char agent_state[32];
    char attempt_id[37];
    char last_ota_error[64];
    gboolean slot_available;
    char current_slot[2];
    char daemon_session_id[37];
};

static gboolean status_fail(EgpError *error, EgpErrorCode code, const char *message)
{
    egp_error_set(error, code, TRUE, EGP_PERSISTENT_CHANGE_NONE, "%s", message);
    return FALSE;
}

static gboolean read_file(const char *path, gsize maximum, gboolean private_file,
                          char **output, gsize *length)
{
    *output = NULL;
    *length = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return FALSE;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
        (private_file && (st.st_mode & 0077)) || st.st_size < 0 ||
        (guint64)st.st_size > maximum) {
        close(fd);
        return FALSE;
    }
    char *data = g_malloc(maximum + 1u);
    gsize used = 0;
    gboolean ok = TRUE;
    while (used <= maximum) {
        ssize_t count = read(fd, data + used, maximum + 1u - used);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 || (count == 0 && used > maximum)) {
            ok = FALSE;
            break;
        }
        if (!count)
            break;
        used += (gsize)count;
        if (used > maximum) {
            ok = FALSE;
            break;
        }
    }
    if (close(fd) < 0)
        ok = FALSE;
    if (!ok || memchr(data, 0, used) || !g_utf8_validate(data, used, NULL)) {
        memset(data, 0, maximum + 1u);
        g_free(data);
        return FALSE;
    }
    data[used] = '\0';
    *output = data;
    *length = used;
    return TRUE;
}

typedef struct {
    GHashTable *members;
    gboolean duplicate;
} JsonGuard;

static void member_seen(JsonParser *parser, JsonObject *object,
                        const char *name, gpointer user_data)
{
    (void)parser;
    JsonGuard *guard = user_data;
    char *key = g_strdup_printf("%p:%s", (void *)object, name);
    if (g_hash_table_contains(guard->members, key)) {
        guard->duplicate = TRUE;
        g_free(key);
    } else {
        g_hash_table_add(guard->members, key);
    }
}

static JsonParser *parse_object(const char *data, gsize length, JsonObject **object)
{
    JsonParser *parser = json_parser_new();
    JsonGuard guard = {
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL), FALSE
    };
    g_signal_connect(parser, "object-member", G_CALLBACK(member_seen), &guard);
    gboolean ok = json_parser_load_from_data(parser, data, length, NULL) &&
                  !guard.duplicate;
    JsonNode *root = ok ? json_parser_get_root(parser) : NULL;
    ok = root && JSON_NODE_HOLDS_OBJECT(root);
    g_hash_table_unref(guard.members);
    if (!ok) {
        g_object_unref(parser);
        return NULL;
    }
    *object = json_node_get_object(root);
    return parser;
}

static gboolean get_string(JsonObject *object, const char *name,
                           char *output, gsize capacity)
{
    JsonNode *node = json_object_get_member(object, name);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING)
        return FALSE;
    const char *value = json_node_get_string(node);
    if (!value || strlen(value) >= capacity)
        return FALSE;
    g_strlcpy(output, value, capacity);
    return TRUE;
}

static gboolean get_schema_one(JsonObject *object)
{
    JsonNode *node = json_object_get_member(object, "schema_version");
    return node && JSON_NODE_HOLDS_VALUE(node) &&
           json_node_get_value_type(node) == G_TYPE_INT64 &&
           json_node_get_int(node) == EGP_SCHEMA_VERSION;
}

static gboolean frozen_agent_state(const char *state)
{
    static const char *const states[] = {
        "IDLE", "CHECK_NETWORK", "CHECK_UPDATE", "PRECHECK", "DOWNLOADING",
        "VERIFY_DOWNLOAD", "RAUC_VERIFY", "INSTALLING", "REBOOT_PENDING",
        "BOOT_NEW_SLOT", "HEALTH_CHECK", "MARK_GOOD", "REPORT_SUCCESS",
        "ROLLBACK", "ERROR"
    };
    for (guint i = 0; i < G_N_ELEMENTS(states); ++i)
        if (!strcmp(state, states[i]))
            return TRUE;
    return FALSE;
}

static void refresh_identity(EgpStatus *status)
{
    char *data = NULL;
    gsize length = 0;
    gboolean available = read_file(EGP_DEVICE_ID_PATH, 128, TRUE, &data, &length);
    if (available) {
        g_strchomp(data);
        available = strlen(data) == 36 && g_uuid_string_is_valid(data);
    }
    g_mutex_lock(&status->lock);
    status->device_available = available;
    memset(status->device_id, 0, sizeof(status->device_id));
    if (available)
        g_strlcpy(status->device_id, data, sizeof(status->device_id));
    g_mutex_unlock(&status->lock);
    if (data) {
        memset(data, 0, length);
        g_free(data);
    }
}

static void refresh_release(EgpStatus *status)
{
    char *data = NULL;
    gsize length = 0;
    char release[EGP_MESSAGE_CAP] = {0}, build_id[EGP_MESSAGE_CAP] = {0};
    gboolean available = read_file(EGP_RELEASE_PATH, 64u * 1024u, FALSE,
                                   &data, &length);
    JsonObject *object = NULL;
    JsonParser *parser = available ? parse_object(data, length, &object) : NULL;
    available = parser && get_schema_one(object) &&
                get_string(object, "version", release, sizeof(release)) &&
                get_string(object, "build_id", build_id, sizeof(build_id));
    g_mutex_lock(&status->lock);
    status->release_available = available;
    memset(status->release, 0, sizeof(status->release));
    memset(status->build_id, 0, sizeof(status->build_id));
    if (available) {
        g_strlcpy(status->release, release, sizeof(status->release));
        g_strlcpy(status->build_id, build_id, sizeof(status->build_id));
    }
    g_mutex_unlock(&status->lock);
    if (parser)
        g_object_unref(parser);
    g_free(data);
}

static void refresh_agent(EgpStatus *status)
{
    char *data = NULL;
    gsize length = 0;
    char state[32] = {0}, attempt[37] = {0}, last_error[64] = {0};
    gboolean available = read_file(EGP_AGENT_STATE_PATH, 64u * 1024u, TRUE,
                                   &data, &length);
    JsonObject *object = NULL;
    JsonParser *parser = available ? parse_object(data, length, &object) : NULL;
    available = parser && get_schema_one(object) &&
                get_string(object, "state", state, sizeof(state)) &&
                frozen_agent_state(state) &&
                get_string(object, "attempt_id", attempt, sizeof(attempt));
    if (available) {
        JsonNode *last_node = json_object_get_member(object, "last_error");
        JsonObject *last = last_node && JSON_NODE_HOLDS_OBJECT(last_node) ?
                           json_node_get_object(last_node) : NULL;
        available = last && get_string(last, "code", last_error, sizeof(last_error));
    }
    g_mutex_lock(&status->lock);
    status->agent_available = available;
    memset(status->agent_state, 0, sizeof(status->agent_state));
    memset(status->attempt_id, 0, sizeof(status->attempt_id));
    memset(status->last_ota_error, 0, sizeof(status->last_ota_error));
    if (available) {
        g_strlcpy(status->agent_state, state, sizeof(status->agent_state));
        g_strlcpy(status->attempt_id, attempt, sizeof(status->attempt_id));
        g_strlcpy(status->last_ota_error, last_error,
                  sizeof(status->last_ota_error));
    }
    g_mutex_unlock(&status->lock);
    if (parser)
        g_object_unref(parser);
    g_free(data);
}

static gboolean read_slot(char slot[2])
{
    int output_pipe[2];
    if (pipe2(output_pipe, O_CLOEXEC | O_NONBLOCK) < 0)
        return FALSE;
    pid_t child = fork();
    if (child < 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        return FALSE;
    }
    if (child == 0) {
        int null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
        dup2(output_pipe[1], STDOUT_FILENO);
        if (null_fd >= 0)
            dup2(null_fd, STDERR_FILENO);
        close(output_pipe[0]);
        close(output_pipe[1]);
        if (null_fd >= 0)
            close(null_fd);
        execl("/usr/bin/edgeguard-rk-abctl", "edgeguard-rk-abctl",
              "get-current", (char *)NULL);
        _exit(127);
    }
    close(output_pipe[1]);
    struct pollfd descriptor = { .fd = output_pipe[0], .events = POLLIN };
    char buffer[16] = {0};
    gsize used = 0;
    gint remaining = EXTERNAL_SOURCE_TIMEOUT_MS;
    gint64 started = g_get_monotonic_time();
    while (remaining > 0 && used < sizeof(buffer) - 1u) {
        int ready = poll(&descriptor, 1, remaining);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            break;
        ssize_t count = read(output_pipe[0], buffer + used,
                             sizeof(buffer) - 1u - used);
        if (count > 0)
            used += (gsize)count;
        if (count == 0 || (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)))
            break;
        remaining = MAX(0, EXTERNAL_SOURCE_TIMEOUT_MS -
                           (gint)((g_get_monotonic_time() - started) / 1000));
    }
    close(output_pipe[0]);
    int wait_status = 0;
    pid_t waited = waitpid(child, &wait_status, WNOHANG);
    if (waited == 0) {
        kill(child, SIGKILL);
        do {
            waited = waitpid(child, &wait_status, 0);
        } while (waited < 0 && errno == EINTR);
    }
    buffer[used] = '\0';
    g_strstrip(buffer);
    if (waited != child || !WIFEXITED(wait_status) || WEXITSTATUS(wait_status) ||
        (strcmp(buffer, "a") && strcmp(buffer, "b")))
        return FALSE;
    slot[0] = buffer[0];
    slot[1] = '\0';
    return TRUE;
}

EgpStatus *egp_status_new(EgpStore *store)
{
    if (!store)
        return NULL;
    EgpStatus *status = g_new0(EgpStatus, 1);
    g_mutex_init(&status->lock);
    status->store = store;
    status->provisioning_state = EGP_STATE_UNPROVISIONED;
    char *session_id = g_uuid_string_random();
    if (!session_id || !g_uuid_string_is_valid(session_id)) {
        g_free(session_id);
        g_mutex_clear(&status->lock);
        g_free(status);
        return NULL;
    }
    g_strlcpy(status->daemon_session_id, session_id,
              sizeof(status->daemon_session_id));
    g_free(session_id);
    return status;
}

void egp_status_free(EgpStatus *status)
{
    if (!status)
        return;
    g_mutex_clear(&status->lock);
    memset(status, 0, sizeof(*status));
    g_free(status);
}

void egp_status_set_provisioning(EgpStatus *status,
                                 EgpProvisioningState state,
                                 gboolean wifi_connected)
{
    if (!status)
        return;
    g_mutex_lock(&status->lock);
    status->provisioning_state = state;
    status->wifi_connected = wifi_connected;
    g_mutex_unlock(&status->lock);
}

void egp_status_refresh(EgpStatus *status)
{
    if (!status)
        return;
    refresh_identity(status);
    refresh_release(status);
    refresh_agent(status);
    EgpRuntimeConfig runtime = {0};
    gboolean present = FALSE;
    EgpError ignored = {0};
    gboolean runtime_ok = egp_store_load_runtime(status->store, &runtime,
                                                  &present, &ignored);
    gboolean endpoint_available = runtime_ok && present;
    const char *endpoint_source = endpoint_available ? "runtime_override" :
                                                       "immutable_default";
    char effective_endpoint[EGP_ENDPOINT_MAX_BYTES + 1u] = {0};
    if (endpoint_available) {
        g_strlcpy(effective_endpoint, runtime.ota_server_base_url,
                  sizeof(effective_endpoint));
    } else {
        char *config_data = NULL;
        gsize config_length = 0;
        if (read_file(EGP_AGENT_CONFIG_PATH, 64u * 1024u, FALSE,
                      &config_data, &config_length)) {
            GKeyFile *config = g_key_file_new();
            GError *config_error = NULL;
            if (g_key_file_load_from_data(config, config_data, config_length,
                                          G_KEY_FILE_NONE, &config_error)) {
                char *base_url = g_key_file_get_string(config, "server",
                                                       "base_url", &config_error);
                if (base_url) {
                    endpoint_available = egp_endpoint_canonicalize(
                        base_url, effective_endpoint, &ignored);
                    memset(base_url, 0, strlen(base_url));
                    g_free(base_url);
                }
            }
            g_clear_error(&config_error);
            g_key_file_unref(config);
            memset(config_data, 0, config_length);
            g_free(config_data);
        }
    }
    char slot[2] = {0};
    gboolean slot_available = read_slot(slot);
    g_mutex_lock(&status->lock);
    status->runtime_available = endpoint_available &&
                                strlen(effective_endpoint) <= EGP_ENDPOINT_MAX_BYTES;
    memset(status->endpoint, 0, sizeof(status->endpoint));
    memset(status->endpoint_source, 0, sizeof(status->endpoint_source));
    if (status->runtime_available)
        g_strlcpy(status->endpoint, effective_endpoint,
                  sizeof(status->endpoint));
    if (status->runtime_available)
        g_strlcpy(status->endpoint_source, endpoint_source,
                  sizeof(status->endpoint_source));
    status->runtime_config_invalid = !runtime_ok;
    status->slot_available = slot_available;
    memset(status->current_slot, 0, sizeof(status->current_slot));
    if (slot_available)
        g_strlcpy(status->current_slot, slot, sizeof(status->current_slot));
    g_mutex_unlock(&status->lock);
}

gboolean egp_status_agent_idle(EgpStatus *status, char observation[32],
                               EgpError *error)
{
    if (!status || !observation)
        return status_fail(error, EGP_ERROR_AGENT_UNAVAILABLE,
                           "Agent status source is invalid");
    refresh_agent(status);
    g_mutex_lock(&status->lock);
    gboolean available = status->agent_available;
    g_strlcpy(observation, status->agent_state, 32);
    g_mutex_unlock(&status->lock);
    if (!available)
        return status_fail(error, EGP_ERROR_AGENT_UNAVAILABLE,
                           "Agent state is unavailable or invalid");
    if (strcmp(observation, "IDLE"))
        return status_fail(error, EGP_ERROR_AGENT_BUSY,
                           "Agent is not at an IDLE commit boundary");
    egp_error_clear(error);
    return TRUE;
}

static gboolean generate_json(JsonBuilder *builder,
                              char output[EGP_STATUS_MAX_JSON + 1u],
                              gsize *length, EgpError *error)
{
    JsonNode *root = json_builder_get_root(builder);
    JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, root);
    gsize bytes = 0;
    char *data = json_generator_to_data(generator, &bytes);
    gboolean ok = data && bytes <= EGP_STATUS_MAX_JSON &&
                  !memchr(data, 0, bytes) && g_utf8_validate(data, bytes, NULL);
    if (ok) {
        memcpy(output, data, bytes);
        output[bytes] = '\0';
        if (length)
            *length = bytes;
        egp_error_clear(error);
    } else {
        status_fail(error, EGP_ERROR_INTERNAL_ERROR,
                    "Status serialization exceeded its bound");
    }
    g_free(data);
    json_node_free(root);
    g_object_unref(generator);
    return ok;
}

#define ADD_STRING(builder, name, value) do { \
    json_builder_set_member_name((builder), (name)); \
    json_builder_add_string_value((builder), (value)); \
} while (0)

gboolean egp_status_device_info_json(EgpStatus *status,
                                     char output[EGP_STATUS_MAX_JSON + 1u],
                                     gsize *length, EgpError *error)
{
    if (!status || !output)
        return status_fail(error, EGP_ERROR_INTERNAL_ERROR,
                           "DeviceInfo output is invalid");
    g_mutex_lock(&status->lock);
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "schema_version");
    json_builder_add_int_value(builder, EGP_SCHEMA_VERSION);
    json_builder_set_member_name(builder, "protocol_version");
    json_builder_add_int_value(builder, EGP_PROTOCOL_VERSION);
    json_builder_set_member_name(builder, "device_id_available");
    json_builder_add_boolean_value(builder, status->device_available);
    if (status->device_available)
        ADD_STRING(builder, "device_id", status->device_id);
    json_builder_set_member_name(builder, "release_available");
    json_builder_add_boolean_value(builder, status->release_available);
    if (status->release_available) {
        ADD_STRING(builder, "release", status->release);
        ADD_STRING(builder, "build_id", status->build_id);
    }
    ADD_STRING(builder, "provisioning_state",
               egp_provisioning_state_name(status->provisioning_state));
    json_builder_end_object(builder);
    g_mutex_unlock(&status->lock);
    gboolean ok = generate_json(builder, output, length, error);
    g_object_unref(builder);
    return ok;
}

gboolean egp_status_runtime_json(EgpStatus *status,
                                 char output[EGP_STATUS_MAX_JSON + 1u],
                                 gsize *length, EgpError *error)
{
    if (!status || !output)
        return status_fail(error, EGP_ERROR_INTERNAL_ERROR,
                           "RuntimeStatus output is invalid");
    g_mutex_lock(&status->lock);
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "schema_version");
    json_builder_add_int_value(builder, EGP_SCHEMA_VERSION);
    ADD_STRING(builder, "provisioning_state",
               egp_provisioning_state_name(status->provisioning_state));
    json_builder_set_member_name(builder, "wifi_connected");
    json_builder_add_boolean_value(builder, status->wifi_connected);
    json_builder_set_member_name(builder, "effective_endpoint_available");
    json_builder_add_boolean_value(builder, status->runtime_available);
    if (status->runtime_available)
        ADD_STRING(builder, "effective_endpoint", status->endpoint);
    if (status->runtime_available)
        ADD_STRING(builder, "effective_endpoint_source", status->endpoint_source);
    if (status->runtime_config_invalid)
        ADD_STRING(builder, "runtime_config_error", "ENDPOINT_CONFIG_INVALID");
    json_builder_set_member_name(builder, "agent_state_available");
    json_builder_add_boolean_value(builder, status->agent_available);
    if (status->agent_available) {
        ADD_STRING(builder, "agent_state", status->agent_state);
        ADD_STRING(builder, "attempt_id", status->attempt_id);
        ADD_STRING(builder, "last_ota_error", status->last_ota_error);
    }
    json_builder_set_member_name(builder, "current_slot_available");
    json_builder_add_boolean_value(builder, status->slot_available);
    if (status->slot_available)
        ADD_STRING(builder, "current_slot", status->current_slot);
    json_builder_end_object(builder);
    g_mutex_unlock(&status->lock);
    gboolean ok = generate_json(builder, output, length, error);
    g_object_unref(builder);
    return ok;
}

static gboolean write_all(int fd, const void *buffer, gsize length)
{
    gsize done = 0;
    while (done < length) {
        ssize_t count = send(fd, (const guint8 *)buffer + done, length - done,
                             MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return FALSE;
        done += (gsize)count;
    }
    return TRUE;
}

static gboolean read_all(int fd, void *buffer, gsize length)
{
    gsize done = 0;
    while (done < length) {
        ssize_t count = recv(fd, (guint8 *)buffer + done, length - done, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return FALSE;
        done += (gsize)count;
    }
    return TRUE;
}

gboolean egp_status_check_update_now(EgpStatus *status,
                                     const EgpProtocolRequest *protocol_request,
                                     EgpError *error)
{
    if (!status || !protocol_request)
        return status_fail(error, EGP_ERROR_INTERNAL_ERROR,
                           "Agent control request scope is invalid");
    struct stat st;
    if (lstat(EGP_AGENT_CONTROL_SOCKET, &st) < 0 || !S_ISSOCK(st.st_mode) ||
        st.st_uid != 0 || (st.st_mode & 0077))
        return status_fail(error, EGP_ERROR_AGENT_UNAVAILABLE,
                           "Agent control socket is unavailable or unsafe");
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return status_fail(error, EGP_ERROR_AGENT_UNAVAILABLE,
                           "Agent control socket could not be opened");
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    g_strlcpy(address.sun_path, EGP_AGENT_CONTROL_SOCKET, sizeof(address.sun_path));
    int connected = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (connected < 0 && errno == EINPROGRESS) {
        struct pollfd descriptor = { .fd = fd, .events = POLLOUT };
        int ready;
        do {
            ready = poll(&descriptor, 1, EXTERNAL_SOURCE_TIMEOUT_MS);
        } while (ready < 0 && errno == EINTR);
        int socket_error = 0;
        socklen_t error_length = sizeof(socket_error);
        if (ready > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR,
                                   &socket_error, &error_length) == 0 &&
            socket_error == 0)
            connected = 0;
    }
    int descriptor_flags = fcntl(fd, F_GETFL, 0);
    struct timeval timeout = { .tv_sec = 2 };
    if (connected < 0 || descriptor_flags < 0 ||
        fcntl(fd, F_SETFL, descriptor_flags & ~O_NONBLOCK) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        close(fd);
        return status_fail(error, EGP_ERROR_AGENT_UNAVAILABLE,
                           "Agent control socket connection failed");
    }
    char request_id[37];
    if (!egp_protocol_scoped_request_id(status->daemon_session_id,
                                        protocol_request, request_id, error)) {
        close(fd);
        return FALSE;
    }
    char *request = g_strdup_printf(
        "{\"version\":1,\"request_id\":\"%s\",\"command\":\"CHECK_UPDATE_NOW\"}",
        request_id);
    gsize request_length = strlen(request);
    guint8 header[4] = {
        (guint8)(request_length >> 24), (guint8)(request_length >> 16),
        (guint8)(request_length >> 8), (guint8)request_length
    };
    gboolean ok = request_length <= 1024 && write_all(fd, header, sizeof(header)) &&
                  write_all(fd, request, request_length) &&
                  read_all(fd, header, sizeof(header));
    guint32 response_length = ((guint32)header[0] << 24) |
                              ((guint32)header[1] << 16) |
                              ((guint32)header[2] << 8) | header[3];
    char response[1025] = {0};
    ok = ok && response_length >= 1 && response_length <= 1024 &&
         read_all(fd, response, response_length);
    struct pollfd trailing = { .fd = fd, .events = POLLIN };
    if (ok && poll(&trailing, 1, 0) > 0 && (trailing.revents & POLLIN)) {
        guint8 extra;
        if (recv(fd, &extra, 1, MSG_PEEK) > 0)
            ok = FALSE;
    }
    close(fd);
    memset(request, 0, request_length);
    g_free(request);
    if (!ok)
        return status_fail(error, EGP_ERROR_AGENT_UNAVAILABLE,
                           "Agent control response was unavailable");
    JsonObject *object = NULL;
    JsonParser *parser = parse_object(response, response_length, &object);
    char response_id[37] = {0}, response_status[16] = {0}, response_error[64] = {0};
    JsonNode *version = parser ? json_object_get_member(object, "version") : NULL;
    ok = parser && json_object_get_size(object) == 4 && version &&
         JSON_NODE_HOLDS_VALUE(version) &&
         json_node_get_value_type(version) == G_TYPE_INT64 &&
         json_node_get_int(version) == 1 &&
         get_string(object, "request_id", response_id, sizeof(response_id)) &&
         get_string(object, "status", response_status, sizeof(response_status)) &&
         get_string(object, "error", response_error, sizeof(response_error)) &&
         !strcmp(response_id, request_id);
    if (parser)
        g_object_unref(parser);
    if (!ok)
        return status_fail(error, EGP_ERROR_OTA_CONTROL_REJECTED,
                           "Agent control response was invalid");
    if (!strcmp(response_status, "ACCEPTED") && !strcmp(response_error, "NONE")) {
        egp_error_clear(error);
        return TRUE;
    }
    if (!strcmp(response_error, "AGENT_BUSY"))
        return status_fail(error, EGP_ERROR_AGENT_BUSY,
                           "Agent rejected the check while busy");
    return status_fail(error, EGP_ERROR_OTA_CONTROL_REJECTED,
                       "Agent rejected the check request");
}

#undef ADD_STRING
