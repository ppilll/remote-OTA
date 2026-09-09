#define _GNU_SOURCE
#include "edgeguard_ota/control.h"
#include "edgeguard_ota/identity.h"

#include <errno.h>
#include <fcntl.h>
#include <json-glib/json-glib.h>
#include <poll.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define CONTROL_MAX_JSON 1024u
#define CONTROL_DEADLINE_SEC 2
#define CONTROL_CACHE_SIZE 16u
#define CONTROL_CACHE_TTL_US (10 * 60 * G_USEC_PER_SEC)

typedef struct {
    char request_id[OTA_UUID_CAP];
    gboolean accepted;
    gint64 expires_us;
} CachedRequest;

struct _OtaControl {
    int listener;
    int wake_read;
    int wake_write;
    GThread *thread;
    GMutex lock;
    volatile sig_atomic_t signal_stopping;
    gboolean stopping;
    gboolean idle_eligible;
    gboolean pending;
    CachedRequest cache[CONTROL_CACHE_SIZE];
    guint cache_next;
};

typedef struct {
    GHashTable *members;
    gboolean duplicate;
} ParseGuard;

static gboolean control_error(OtaError *error, const char *message)
{
    ota_error_set(error, OTA_ERROR_CONFIG_INVALID, "%s", message);
    return FALSE;
}

static void member_seen(JsonParser *parser, JsonObject *object,
                        const char *name, gpointer user)
{
    (void)parser;
    ParseGuard *guard = user;
    char *key = g_strdup_printf("%p:%s", (void *)object, name);
    if (g_hash_table_contains(guard->members, key)) {
        guard->duplicate = TRUE;
        g_free(key);
    } else {
        g_hash_table_add(guard->members, key);
    }
}

static gboolean string_member(JsonObject *object, const char *name,
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

static gboolean parse_request(const char *data, gsize length,
                              char request_id[OTA_UUID_CAP])
{
    if (!length || length > CONTROL_MAX_JSON || memchr(data, 0, length) ||
        !g_utf8_validate(data, length, NULL))
        return FALSE;
    JsonParser *parser = json_parser_new();
    ParseGuard guard = {
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL), FALSE
    };
    g_signal_connect(parser, "object-member", G_CALLBACK(member_seen), &guard);
    gboolean ok = json_parser_load_from_data(parser, data, length, NULL) &&
                  !guard.duplicate;
    JsonNode *root = ok ? json_parser_get_root(parser) : NULL;
    JsonObject *object = root && JSON_NODE_HOLDS_OBJECT(root)
                             ? json_node_get_object(root) : NULL;
    JsonNode *version = object ? json_object_get_member(object, "version") : NULL;
    char command[32] = {0};
    ok = object &&
         string_member(object, "request_id", request_id, OTA_UUID_CAP) &&
         ota_uuid_valid(request_id, TRUE) &&
         json_object_get_size(object) == 3 && version &&
         JSON_NODE_HOLDS_VALUE(version) &&
         json_node_get_value_type(version) == G_TYPE_INT64 &&
         json_node_get_int(version) == 1 &&
         string_member(object, "command", command, sizeof(command)) &&
         !strcmp(command, "CHECK_UPDATE_NOW");
    g_hash_table_unref(guard.members);
    g_object_unref(parser);
    return ok;
}

static gboolean read_exact(int fd, void *buffer, gsize length)
{
    gsize offset = 0;
    while (offset < length) {
        ssize_t count = recv(fd, (guint8 *)buffer + offset, length - offset, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return FALSE;
        offset += (gsize)count;
    }
    return TRUE;
}

static gboolean write_exact(int fd, const void *buffer, gsize length)
{
    gsize offset = 0;
    while (offset < length) {
        ssize_t count = send(fd, (const guint8 *)buffer + offset,
                             length - offset, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return FALSE;
        offset += (gsize)count;
    }
    return TRUE;
}

static void cache_decision(OtaControl *control, const char *request_id,
                           gboolean *accepted)
{
    gint64 now = g_get_monotonic_time();
    g_mutex_lock(&control->lock);
    for (guint i = 0; i < CONTROL_CACHE_SIZE; ++i) {
        CachedRequest *entry = &control->cache[i];
        if (entry->request_id[0] && entry->expires_us >= now &&
            !strcmp(entry->request_id, request_id)) {
            *accepted = entry->accepted;
            g_mutex_unlock(&control->lock);
            return;
        }
    }
    *accepted = control->idle_eligible && !control->pending &&
                !control->stopping && !control->signal_stopping;
    if (*accepted) {
        control->pending = TRUE;
        control->idle_eligible = FALSE;
    }
    CachedRequest *entry = &control->cache[control->cache_next];
    memset(entry, 0, sizeof(*entry));
    g_strlcpy(entry->request_id, request_id, sizeof(entry->request_id));
    entry->accepted = *accepted;
    entry->expires_us = now + CONTROL_CACHE_TTL_US;
    control->cache_next = (control->cache_next + 1u) % CONTROL_CACHE_SIZE;
    g_mutex_unlock(&control->lock);
    if (*accepted) {
        const guint8 event = 'C';
        (void)write(control->wake_write, &event, sizeof(event));
    }
}

static void send_response(int fd, const char *request_id,
                          const char *status, const char *error)
{
    const char *id = ota_uuid_valid(request_id, TRUE)
                         ? request_id : "00000000-0000-0000-0000-000000000000";
    char json[256];
    g_snprintf(json, sizeof(json),
               "{\"version\":1,\"request_id\":\"%s\",\"status\":\"%s\",\"error\":\"%s\"}",
               id, status, error);
    guint32 length = (guint32)strlen(json);
    guint8 header[4] = {
        (guint8)(length >> 24), (guint8)(length >> 16),
        (guint8)(length >> 8), (guint8)length
    };
    (void)(write_exact(fd, header, sizeof(header)) &&
           write_exact(fd, json, length));
}

static void handle_client(OtaControl *control, int client)
{
    struct timeval timeout = { .tv_sec = CONTROL_DEADLINE_SEC };
    struct ucred credentials;
    socklen_t credentials_length = sizeof(credentials);
    if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0 ||
        getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credentials,
                   &credentials_length) < 0 || credentials_length != sizeof(credentials) ||
        credentials.uid != 0)
        return;

    guint8 header[4];
    if (!read_exact(client, header, sizeof(header)))
        return;
    guint32 length = ((guint32)header[0] << 24) | ((guint32)header[1] << 16) |
                     ((guint32)header[2] << 8) | header[3];
    if (length < 1 || length > CONTROL_MAX_JSON)
        return;
    char request[CONTROL_MAX_JSON + 1u] = {0};
    if (!read_exact(client, request, length))
        return;
    guint8 extra;
    if (recv(client, &extra, sizeof(extra), MSG_PEEK | MSG_DONTWAIT) > 0)
        return;
    char request_id[OTA_UUID_CAP] = {0};
    if (!parse_request(request, length, request_id)) {
        if (ota_uuid_valid(request_id, TRUE))
            send_response(client, request_id, "REJECTED", "OTA_CONTROL_REJECTED");
        return;
    }
    gboolean accepted = FALSE;
    cache_decision(control, request_id, &accepted);
    send_response(client, request_id, accepted ? "ACCEPTED" : "REJECTED",
                  accepted ? "NONE" : "AGENT_BUSY");
    memset(request, 0, sizeof(request));
}

static gpointer server_thread(gpointer user)
{
    OtaControl *control = user;
    while (TRUE) {
        int client = accept4(control->listener, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR)
                continue;
            g_mutex_lock(&control->lock);
            gboolean stop = control->stopping;
            g_mutex_unlock(&control->lock);
            if (stop)
                break;
            continue;
        }
        handle_client(control, client);
        close(client);
    }
    return NULL;
}

static gboolean prepare_runtime_directory(OtaError *error)
{
    const char *directory = "/run/edgeguard-remote-ota";
    if (mkdir(directory, 0700) < 0 && errno != EEXIST)
        return control_error(error, "Control runtime directory could not be created");
    struct stat st;
    if (lstat(directory, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0 ||
        (st.st_mode & 0022))
        return control_error(error, "Control runtime directory is unsafe");
    if (chown(directory, 0, 0) < 0 || chmod(directory, 0700) < 0)
        return control_error(error, "Control runtime directory mode could not be set");
    return TRUE;
}

gboolean ota_control_open(OtaControl **out, OtaError *error)
{
    if (!out || *out)
        return control_error(error, "Control server output is invalid");
    if (!prepare_runtime_directory(error))
        return FALSE;
    struct stat st;
    if (lstat(OTA_CONTROL_SOCKET, &st) == 0) {
        if (!S_ISSOCK(st.st_mode) || st.st_uid != 0 || st.st_gid != 0)
            return control_error(error, "Refusing to replace unsafe control socket path");
        if (unlink(OTA_CONTROL_SOCKET) < 0)
            return control_error(error, "Stale control socket could not be removed");
    } else if (errno != ENOENT) {
        return control_error(error, "Control socket path could not be inspected");
    }

    OtaControl *control = g_new0(OtaControl, 1);
    control->listener = -1;
    control->wake_read = -1;
    control->wake_write = -1;
    g_mutex_init(&control->lock);
    int wake[2];
    if (pipe2(wake, O_CLOEXEC | O_NONBLOCK) < 0)
        goto failed;
    control->wake_read = wake[0];
    control->wake_write = wake[1];
    control->listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (control->listener < 0)
        goto failed;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    g_strlcpy(address.sun_path, OTA_CONTROL_SOCKET, sizeof(address.sun_path));
    mode_t previous_umask = umask(0077);
    int bound = bind(control->listener, (struct sockaddr *)&address,
                     offsetof(struct sockaddr_un, sun_path) + strlen(address.sun_path) + 1u);
    umask(previous_umask);
    if (bound < 0 || chown(OTA_CONTROL_SOCKET, 0, 0) < 0 ||
        chmod(OTA_CONTROL_SOCKET, 0600) < 0 ||
        listen(control->listener, 3) < 0)
        goto failed;
    control->thread = g_thread_new("ota-control", server_thread, control);
    if (!control->thread)
        goto failed;
    *out = control;
    return TRUE;

failed:
    control_error(error, "Control socket initialization failed");
    ota_control_close(control);
    return FALSE;
}

static void drain_wakeup(int fd)
{
    guint8 buffer[64];
    while (read(fd, buffer, sizeof(buffer)) > 0) {}
}

gboolean ota_control_idle_wait(OtaControl *control, uint32_t timeout_sec,
                               const volatile sig_atomic_t *stopping,
                               gboolean *check_requested, OtaError *error)
{
    if (!control || !stopping || !check_requested || timeout_sec > INT32_MAX / 1000u)
        return control_error(error, "Control wait arguments are invalid");
    *check_requested = FALSE;
    drain_wakeup(control->wake_read);
    g_mutex_lock(&control->lock);
    control->idle_eligible = !control->stopping &&
                             !control->signal_stopping && !*stopping;
    gboolean already_pending = control->pending;
    g_mutex_unlock(&control->lock);
    struct pollfd descriptor = { .fd = control->wake_read, .events = POLLIN };
    int ready = already_pending ? 1 : poll(&descriptor, 1, (int)(timeout_sec * 1000u));
    if (ready < 0 && errno != EINTR)
        return control_error(error, "Control wait failed");
    drain_wakeup(control->wake_read);
    g_mutex_lock(&control->lock);
    control->idle_eligible = FALSE;
    if (control->pending) {
        control->pending = FALSE;
        *check_requested = TRUE;
    }
    g_mutex_unlock(&control->lock);
    return TRUE;
}

void ota_control_signal_wakeup(OtaControl *control)
{
    if (!control || control->wake_write < 0)
        return;
    control->signal_stopping = 1;
    const guint8 event = 'S';
    (void)write(control->wake_write, &event, sizeof(event));
}

void ota_control_close(OtaControl *control)
{
    if (!control)
        return;
    g_mutex_lock(&control->lock);
    control->stopping = TRUE;
    control->idle_eligible = FALSE;
    g_mutex_unlock(&control->lock);
    if (control->listener >= 0)
        shutdown(control->listener, SHUT_RDWR);
    if (control->thread)
        g_thread_join(control->thread);
    if (control->listener >= 0)
        close(control->listener);
    if (control->wake_read >= 0)
        close(control->wake_read);
    if (control->wake_write >= 0)
        close(control->wake_write);
    struct stat st;
    if (lstat(OTA_CONTROL_SOCKET, &st) == 0 && S_ISSOCK(st.st_mode) && st.st_uid == 0)
        unlink(OTA_CONTROL_SOCKET);
    g_mutex_clear(&control->lock);
    memset(control, 0, sizeof(*control));
    g_free(control);
}
