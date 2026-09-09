#include "edgeguard_provisioning/store.h"
#include "edgeguard_provisioning/endpoint.h"

#include <json-glib/json-glib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

struct _EgpStore {
    char *directory;
    char *wifi_path;
    char *runtime_path;
    char *forget_path;
    int directory_fd;
    int lock_fd;
    GMutex mutex;
};

static void secure_clear(void *data, gsize length)
{
    volatile unsigned char *p = data;
    while (length--)
        *p++ = 0;
}

void egp_wifi_clear(EgpWifiConfig *wifi)
{
    if (wifi)
        secure_clear(wifi, sizeof(*wifi));
}

static gboolean bounded_string(const char *value, gsize capacity, gsize *length)
{
    const char *end = memchr(value, 0, capacity);
    if (!end)
        return FALSE;
    if (length)
        *length = (gsize)(end - value);
    return TRUE;
}

gboolean egp_wifi_validate(const EgpWifiConfig *wifi, EgpError *error)
{
    gsize ssid_length, passphrase_length;
    if (!wifi || !bounded_string(wifi->ssid, sizeof(wifi->ssid), &ssid_length) ||
        !bounded_string(wifi->security, sizeof(wifi->security), NULL) ||
        !bounded_string(wifi->passphrase, sizeof(wifi->passphrase), &passphrase_length) ||
        ssid_length < 1 || ssid_length > EGP_SSID_MAX_BYTES ||
        strcmp(wifi->security, "psk") || !g_utf8_validate(wifi->ssid, ssid_length, NULL) ||
        (wifi->hidden != FALSE && wifi->hidden != TRUE))
        goto invalid;

    for (const char *p = wifi->ssid; *p; p = g_utf8_next_char(p)) {
        gunichar ch = g_utf8_get_char(p);
        if (ch == 0 || g_unichar_iscntrl(ch))
            goto invalid;
    }

    if (passphrase_length == 64) {
        for (gsize i = 0; i < passphrase_length; ++i)
            if (!g_ascii_isxdigit(wifi->passphrase[i]))
                goto invalid;
    } else {
        if (passphrase_length < 8 || passphrase_length > 63)
            goto invalid;
        for (gsize i = 0; i < passphrase_length; ++i) {
            unsigned char ch = (unsigned char)wifi->passphrase[i];
            if (ch < 0x20 || ch > 0x7e)
                goto invalid;
        }
    }
    egp_error_clear(error);
    return TRUE;

invalid:
    egp_error_set(error, EGP_ERROR_WIFI_CONFIG_INVALID, TRUE,
                  EGP_PERSISTENT_CHANGE_NONE, "Wi-Fi configuration is invalid");
    return FALSE;
}

static gboolean path_is_absolute_clean(const char *path)
{
    if (!path || path[0] != '/' || path[1] == '\0' || strchr(path, '\\'))
        return FALSE;
    char **parts = g_strsplit(path + 1, "/", -1);
    gboolean ok = TRUE;
    for (gsize i = 0; parts[i]; ++i) {
        if (!*parts[i] || !strcmp(parts[i], ".") || !strcmp(parts[i], "..")) {
            ok = FALSE;
            break;
        }
    }
    g_strfreev(parts);
    return ok;
}

static gboolean set_read_error(EgpError *error, EgpErrorCode code, const char *message)
{
    egp_error_set(error, code, TRUE, EGP_PERSISTENT_CHANGE_NONE, "%s", message);
    return FALSE;
}

static gboolean set_write_error(EgpError *error, gboolean renamed, const char *message)
{
    egp_error_set(error, EGP_ERROR_PERSISTENCE_WRITE_FAILED, TRUE,
                  renamed ? EGP_PERSISTENT_CHANGE_UNCERTAIN : EGP_PERSISTENT_CHANGE_NONE,
                  "%s", message);
    return FALSE;
}

static gboolean metadata_is_private_regular(const struct stat *st)
{
    return S_ISREG(st->st_mode) && st->st_uid == 0 && (st->st_mode & 0077) == 0;
}

static gboolean read_private_file(const char *path, char **data, gsize *length,
                                  gboolean *missing, EgpError *error)
{
    *data = NULL;
    *length = 0;
    *missing = FALSE;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT) {
            *missing = TRUE;
            return TRUE;
        }
        return set_read_error(error, EGP_ERROR_PERSISTENCE_READ_FAILED,
                              "Persistent file could not be opened");
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || !metadata_is_private_regular(&st) || st.st_size < 0 ||
        (guint64)st.st_size > EGP_STORE_MAX_BYTES) {
        close(fd);
        return set_read_error(error, EGP_ERROR_PERSISTENCE_READ_FAILED,
                              "Persistent file metadata is unsafe or oversized");
    }

    char *buffer = g_malloc(EGP_STORE_MAX_BYTES + 1u);
    gsize used = 0;
    gboolean ok = TRUE;
    while (used <= EGP_STORE_MAX_BYTES) {
        ssize_t count = read(fd, buffer + used, EGP_STORE_MAX_BYTES + 1u - used);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            ok = FALSE;
            break;
        }
        if (count == 0)
            break;
        used += (gsize)count;
        if (used > EGP_STORE_MAX_BYTES) {
            ok = FALSE;
            break;
        }
    }
    if (close(fd) < 0)
        ok = FALSE;
    if (!ok) {
        secure_clear(buffer, EGP_STORE_MAX_BYTES + 1u);
        g_free(buffer);
        return set_read_error(error, EGP_ERROR_PERSISTENCE_READ_FAILED,
                              "Persistent file read failed or exceeded its bound");
    }
    buffer[used] = '\0';
    *data = buffer;
    *length = used;
    return TRUE;
}

static gboolean verify_written_file(int fd, const void *data, gsize length)
{
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
        (st.st_mode & 0777) != 0600 || st.st_size < 0 || (guint64)st.st_size != length ||
        lseek(fd, 0, SEEK_SET) < 0)
        return FALSE;

    unsigned char buffer[4096];
    gsize checked = 0;
    while (checked < length) {
        gsize wanted = MIN(sizeof(buffer), length - checked);
        ssize_t count = read(fd, buffer, wanted);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0 || memcmp(buffer, (const unsigned char *)data + checked,
                                 (gsize)count))
            return FALSE;
        checked += (gsize)count;
    }
    return TRUE;
}

static gboolean atomic_write_at(EgpStore *store, const char *basename,
                                const void *data, gsize length, EgpError *error)
{
    struct stat final_st;
    if (fstatat(store->directory_fd, basename, &final_st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!metadata_is_private_regular(&final_st))
            return set_write_error(error, FALSE, "Existing persistent file is unsafe");
    } else if (errno != ENOENT) {
        return set_write_error(error, FALSE, "Persistent destination could not be inspected");
    }

    char temporary[96] = {0};
    int fd = -1;
    gboolean created = FALSE;
    gboolean renamed = FALSE;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        g_snprintf(temporary, sizeof(temporary), ".egp-tmp-%ld-%08x-%08x",
                   (long)getpid(), g_random_int(), g_random_int());
        fd = openat(store->directory_fd, temporary,
                    O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) {
            created = TRUE;
            break;
        }
        if (errno != EEXIST)
            break;
    }

    gboolean ok = fd >= 0;
    gsize written = 0;
    while (ok && written < length) {
        ssize_t count = write(fd, (const unsigned char *)data + written, length - written);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            ok = FALSE;
            break;
        }
        written += (gsize)count;
    }
    if (ok && (fsync(fd) < 0 || !verify_written_file(fd, data, length)))
        ok = FALSE;
    if (fd >= 0 && close(fd) < 0)
        ok = FALSE;
    fd = -1;
    if (ok && renameat(store->directory_fd, temporary,
                       store->directory_fd, basename) == 0) {
        renamed = TRUE;
        created = FALSE;
    } else if (ok) {
        ok = FALSE;
    }
    if (ok && fsync(store->directory_fd) < 0)
        ok = FALSE;

    if (fd >= 0)
        close(fd);
    if (created)
        unlinkat(store->directory_fd, temporary, 0);
    if (!ok)
        return set_write_error(error, renamed,
                               "Atomic persistence failed; durability may be uncertain after rename");
    egp_error_clear(error);
    return TRUE;
}

typedef struct {
    GHashTable *seen;
    gboolean duplicate;
} JsonGuard;

static void json_member_seen(JsonParser *parser, JsonObject *object,
                             const char *member_name, gpointer user_data)
{
    (void)parser;
    JsonGuard *guard = user_data;
    char *key = g_strdup_printf("%p:%s", (void *)object, member_name);
    if (g_hash_table_contains(guard->seen, key)) {
        guard->duplicate = TRUE;
        g_free(key);
    } else {
        g_hash_table_add(guard->seen, key);
    }
}

static gboolean json_lexically_safe(const char *data, gsize length)
{
    unsigned depth = 0;
    gboolean quoted = FALSE;
    for (gsize i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)data[i];
        if (quoted) {
            if (ch == '\\') {
                if (++i >= length)
                    return FALSE;
            } else if (ch == '"') {
                quoted = FALSE;
            } else if (ch < 0x20) {
                return FALSE;
            }
            continue;
        }
        if (ch == '"') {
            quoted = TRUE;
        } else if (ch == '{') {
            if (++depth > 1)
                return FALSE;
        } else if (ch == '}') {
            if (!depth)
                return FALSE;
            --depth;
        } else if (ch == '[' || ch == ']' || ch == '-' || ch == '+' || ch == '.') {
            return FALSE;
        } else if (g_ascii_isdigit(ch)) {
            if (ch == '0' && i + 1 < length && g_ascii_isdigit(data[i + 1]))
                return FALSE;
            guint64 number = 0;
            do {
                guint digit = (guint)(data[i] - '0');
                if (number > ((guint64)EGP_JSON_UINT_MAX - digit) / 10u)
                    return FALSE;
                number = number * 10u + digit;
                ++i;
            } while (i < length && g_ascii_isdigit(data[i]));
            if (i < length && !strchr(",} \t\r\n", data[i]))
                return FALSE;
            --i;
        }
    }
    return !quoted && depth == 0;
}

static gboolean exact_string(JsonObject *object, const char *name,
                             char *out, gsize capacity)
{
    JsonNode *node = json_object_get_member(object, name);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING)
        return FALSE;
    const char *value = json_node_get_string(node);
    if (!value || strlen(value) >= capacity)
        return FALSE;
    g_strlcpy(out, value, capacity);
    return TRUE;
}

static gboolean exact_integer(JsonObject *object, const char *name, gint64 *out)
{
    JsonNode *node = json_object_get_member(object, name);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_INT64)
        return FALSE;
    *out = json_node_get_int(node);
    return TRUE;
}

static gboolean exact_boolean(JsonObject *object, const char *name, gboolean *out)
{
    JsonNode *node = json_object_get_member(object, name);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_BOOLEAN)
        return FALSE;
    *out = json_node_get_boolean(node);
    return TRUE;
}

static gboolean parse_root(const char *data, gsize length, JsonParser **parser_out,
                           JsonObject **object_out)
{
    if (!length || memchr(data, 0, length) || !g_utf8_validate(data, length, NULL) ||
        g_strstr_len(data, length, "\\u0000") || !json_lexically_safe(data, length))
        return FALSE;
    JsonParser *parser = json_parser_new();
    JsonGuard guard = {
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL), FALSE
    };
    g_signal_connect(parser, "object-member", G_CALLBACK(json_member_seen), &guard);
    gboolean ok = json_parser_load_from_data(parser, data, length, NULL) && !guard.duplicate;
    JsonNode *root = ok ? json_parser_get_root(parser) : NULL;
    ok = root && JSON_NODE_HOLDS_OBJECT(root);
    g_hash_table_unref(guard.seen);
    if (!ok) {
        g_object_unref(parser);
        return FALSE;
    }
    *parser_out = parser;
    *object_out = json_node_get_object(root);
    return TRUE;
}

static gboolean parse_wifi_data(char *data, gsize length, EgpWifiConfig *out,
                                EgpError *error)
{
    JsonParser *parser = NULL;
    JsonObject *object = NULL;
    EgpWifiConfig wifi = {0};
    gint64 schema = 0, generation = 0;
    gboolean ok = parse_root(data, length, &parser, &object);
    if (ok && exact_integer(object, "schema_version", &schema) &&
        schema != EGP_SCHEMA_VERSION) {
        egp_error_set(error, EGP_ERROR_PERSISTENCE_SCHEMA_UNSUPPORTED, FALSE,
                      EGP_PERSISTENT_CHANGE_NONE, "Wi-Fi store schema is unsupported");
        ok = FALSE;
    }
    if (ok) {
        ok = exact_integer(object, "schema_version", &schema) &&
             schema == EGP_SCHEMA_VERSION && json_object_get_size(object) == 6 &&
             exact_integer(object, "generation", &generation) && generation > 0 &&
             exact_string(object, "ssid", wifi.ssid, sizeof(wifi.ssid)) &&
             exact_string(object, "security", wifi.security, sizeof(wifi.security)) &&
             exact_string(object, "passphrase", wifi.passphrase, sizeof(wifi.passphrase)) &&
             exact_boolean(object, "hidden", &wifi.hidden);
        wifi.generation = generation > 0 ? (guint64)generation : 0;
    }
    if (ok)
        ok = egp_wifi_validate(&wifi, error);
    if (ok) {
        *out = wifi;
        egp_error_clear(error);
    } else if (!error || error->code == EGP_ERROR_NONE ||
               error->code == EGP_ERROR_WIFI_CONFIG_INVALID) {
        egp_error_set(error, EGP_ERROR_WIFI_CONFIG_INVALID, TRUE,
                      EGP_PERSISTENT_CHANGE_NONE, "Canonical Wi-Fi configuration is invalid");
    }
    if (parser)
        g_object_unref(parser);
    if (!ok)
        egp_wifi_clear(&wifi);
    return ok;
}

static gboolean parse_runtime_data(char *data, gsize length, EgpRuntimeConfig *out,
                                   EgpError *error)
{
    return egp_runtime_parse_json(data, length, out, error);
}

static gboolean load_wifi_path(const char *path, EgpWifiConfig *out,
                               gboolean *present, EgpError *error)
{
    char *data = NULL;
    gsize length = 0;
    gboolean missing = FALSE;
    memset(out, 0, sizeof(*out));
    if (!read_private_file(path, &data, &length, &missing, error))
        return FALSE;
    if (missing) {
        *present = FALSE;
        egp_error_clear(error);
        return TRUE;
    }
    gboolean ok = parse_wifi_data(data, length, out, error);
    secure_clear(data, length);
    g_free(data);
    *present = ok;
    return ok;
}

static gboolean load_runtime_path(const char *path, EgpRuntimeConfig *out,
                                  gboolean *present, EgpError *error)
{
    char *data = NULL;
    gsize length = 0;
    gboolean missing = FALSE;
    memset(out, 0, sizeof(*out));
    if (!read_private_file(path, &data, &length, &missing, error))
        return FALSE;
    if (missing) {
        *present = FALSE;
        egp_error_clear(error);
        return TRUE;
    }
    gboolean ok = parse_runtime_data(data, length, out, error);
    secure_clear(data, length);
    g_free(data);
    *present = ok;
    return ok;
}

static char *serialize_wifi(const EgpWifiConfig *wifi, gsize *length)
{
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
#define MEMBER_INT(name, value) do { json_builder_set_member_name(builder, name); \
    json_builder_add_int_value(builder, (gint64)(value)); } while (0)
#define MEMBER_STRING(name, value) do { json_builder_set_member_name(builder, name); \
    json_builder_add_string_value(builder, value); } while (0)
    MEMBER_INT("schema_version", EGP_SCHEMA_VERSION);
    MEMBER_INT("generation", wifi->generation);
    MEMBER_STRING("ssid", wifi->ssid);
    MEMBER_STRING("security", wifi->security);
    MEMBER_STRING("passphrase", wifi->passphrase);
    json_builder_set_member_name(builder, "hidden");
    json_builder_add_boolean_value(builder, wifi->hidden);
    json_builder_end_object(builder);
#undef MEMBER_INT
#undef MEMBER_STRING
    JsonNode *root = json_builder_get_root(builder);
    JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, root);
    char *data = json_generator_to_data(generator, length);
    json_node_free(root);
    g_object_unref(generator);
    g_object_unref(builder);
    return data;
}

static char *serialize_runtime(const EgpRuntimeConfig *runtime, gsize *length)
{
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "schema_version");
    json_builder_add_int_value(builder, EGP_SCHEMA_VERSION);
    json_builder_set_member_name(builder, "generation");
    json_builder_add_int_value(builder, (gint64)runtime->generation);
    json_builder_set_member_name(builder, "ota_server_base_url");
    json_builder_add_string_value(builder, runtime->ota_server_base_url);
    json_builder_end_object(builder);
    JsonNode *root = json_builder_get_root(builder);
    JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, root);
    char *data = json_generator_to_data(generator, length);
    json_node_free(root);
    g_object_unref(generator);
    g_object_unref(builder);
    return data;
}

static gboolean cleanup_recovery_files(EgpStore *store, EgpError *error)
{
    int copy = dup(store->directory_fd);
    if (copy < 0)
        return set_write_error(error, FALSE, "Store recovery scan could not start");
    DIR *directory = fdopendir(copy);
    if (!directory) {
        close(copy);
        return set_write_error(error, FALSE, "Store recovery scan could not start");
    }
    gboolean changed = FALSE;
    gboolean ok = TRUE;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        gboolean owned_temp = g_str_has_prefix(entry->d_name, ".egp-tmp-") ||
                              !strcmp(entry->d_name, ".wifi.json.rollback");
        if (!owned_temp)
            continue;
        struct stat st;
        if (fstatat(store->directory_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0 ||
            !metadata_is_private_regular(&st) ||
            unlinkat(store->directory_fd, entry->d_name, 0) < 0) {
            ok = FALSE;
            break;
        }
        changed = TRUE;
    }
    closedir(directory);
    if (ok && changed && fsync(store->directory_fd) < 0)
        ok = FALSE;
    if (!ok)
        return set_write_error(error, FALSE, "Store recovery cleanup failed");
    return TRUE;
}

gboolean egp_store_open(const char *directory, EgpStore **out, EgpError *error)
{
    if (!out)
        return set_write_error(error, FALSE, "Store output is required");
    *out = NULL;
    if (!directory)
        directory = EGP_STORE_DIRECTORY;
#ifndef EGP_ALLOW_TEST_PATHS
    if (strcmp(directory, EGP_STORE_DIRECTORY))
        return set_write_error(error, FALSE, "Non-canonical store path is forbidden");
#endif
    if (!path_is_absolute_clean(directory))
        return set_write_error(error, FALSE, "Store directory is not an absolute clean path");

    char *parent = g_path_get_dirname(directory);
    char *directory_base = g_path_get_basename(directory);
    int parent_fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    g_free(parent);
    if (parent_fd < 0)
        goto parent_failed;

    gboolean created = FALSE;
    if (mkdirat(parent_fd, directory_base, 0700) == 0)
        created = TRUE;
    else if (errno != EEXIST) {
        close(parent_fd);
        g_free(directory_base);
        return set_write_error(error, FALSE, "Store directory could not be created");
    }
    int directory_fd = openat(parent_fd, directory_base,
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    g_free(directory_base);
    struct stat directory_st;
    if (directory_fd < 0 || fstat(directory_fd, &directory_st) < 0 ||
        !S_ISDIR(directory_st.st_mode) || directory_st.st_uid != 0 ||
        (directory_st.st_mode & 0777) != 0700 ||
        (created && (fsync(directory_fd) < 0 || fsync(parent_fd) < 0))) {
        if (directory_fd >= 0)
            close(directory_fd);
        close(parent_fd);
        return set_write_error(error, FALSE, "Store directory metadata is unsafe");
    }
    close(parent_fd);

    int lock_fd = openat(directory_fd, ".lock",
                         O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    struct stat lock_st;
    if (lock_fd < 0 || fstat(lock_fd, &lock_st) < 0 ||
        !metadata_is_private_regular(&lock_st) ||
        flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
        if (lock_fd >= 0)
            close(lock_fd);
        close(directory_fd);
        return set_write_error(error, FALSE, "Provisioning store lock is unavailable");
    }

    EgpStore *store = g_new0(EgpStore, 1);
    store->directory = g_strdup(directory);
    store->wifi_path = g_build_filename(directory, EGP_WIFI_FILENAME, NULL);
    store->runtime_path = g_build_filename(directory, EGP_RUNTIME_FILENAME, NULL);
    store->forget_path = g_build_filename(directory, EGP_FORGET_FILENAME, NULL);
    store->directory_fd = directory_fd;
    store->lock_fd = lock_fd;
    g_mutex_init(&store->mutex);
    if (!cleanup_recovery_files(store, error)) {
        egp_store_close(store);
        return FALSE;
    }
    *out = store;
    egp_error_clear(error);
    return TRUE;

parent_failed:
    g_free(directory_base);
    return set_write_error(error, FALSE, "Store parent is unavailable");
}

void egp_store_close(EgpStore *store)
{
    if (!store)
        return;
    if (store->lock_fd >= 0)
        close(store->lock_fd);
    if (store->directory_fd >= 0)
        close(store->directory_fd);
    g_mutex_clear(&store->mutex);
    g_free(store->forget_path);
    g_free(store->runtime_path);
    g_free(store->wifi_path);
    g_free(store->directory);
    g_free(store);
}

gboolean egp_store_load_wifi(EgpStore *store, EgpWifiConfig *out,
                             gboolean *present, EgpError *error)
{
    if (!store || !out || !present)
        return set_read_error(error, EGP_ERROR_PERSISTENCE_READ_FAILED,
                              "Wi-Fi store arguments are invalid");
    g_mutex_lock(&store->mutex);
    gboolean ok = load_wifi_path(store->wifi_path, out, present, error);
    g_mutex_unlock(&store->mutex);
    return ok;
}

gboolean egp_store_load_runtime(EgpStore *store, EgpRuntimeConfig *out,
                                gboolean *present, EgpError *error)
{
    if (!store || !out || !present)
        return set_read_error(error, EGP_ERROR_PERSISTENCE_READ_FAILED,
                              "Runtime store arguments are invalid");
    g_mutex_lock(&store->mutex);
    gboolean ok = load_runtime_path(store->runtime_path, out, present, error);
    g_mutex_unlock(&store->mutex);
    return ok;
}

gboolean egp_store_replace_wifi(EgpStore *store, const EgpWifiConfig *candidate,
                                EgpWifiConfig *previous, gboolean *previous_present,
                                guint64 *committed_generation, EgpError *error)
{
    if (!store || !candidate || !previous || !previous_present || !committed_generation ||
        !egp_wifi_validate(candidate, error))
        return FALSE;
    memset(previous, 0, sizeof(*previous));
    *previous_present = FALSE;
    *committed_generation = 0;
    g_mutex_lock(&store->mutex);
    gboolean ok = load_wifi_path(store->wifi_path, previous, previous_present, error);
    if (ok && *previous_present && previous->generation >= (guint64)EGP_JSON_UINT_MAX) {
        egp_error_set(error, EGP_ERROR_PERSISTENCE_SCHEMA_UNSUPPORTED, FALSE,
                      EGP_PERSISTENT_CHANGE_NONE, "Wi-Fi generation cannot be incremented");
        ok = FALSE;
    }
    EgpWifiConfig next = *candidate;
    next.generation = *previous_present ? previous->generation + 1u : 1u;
    char *data = NULL;
    gsize length = 0;
    if (ok) {
        data = serialize_wifi(&next, &length);
        ok = length <= EGP_STORE_MAX_BYTES &&
             atomic_write_at(store, EGP_WIFI_FILENAME, data, length, error);
        *committed_generation = next.generation;
    }
    if (data) {
        secure_clear(data, length);
        g_free(data);
    }
    egp_wifi_clear(&next);
    g_mutex_unlock(&store->mutex);
    if (ok) {
        egp_error_set(error, EGP_ERROR_NONE, FALSE,
                      EGP_PERSISTENT_CHANGE_COMMITTED, "Wi-Fi configuration committed");
    }
    return ok;
}

gboolean egp_store_replace_runtime(EgpStore *store, const char *candidate_url,
                                   EgpRuntimeConfig *previous, gboolean *previous_present,
                                   EgpRuntimeConfig *committed, EgpError *error)
{
    if (!store || !candidate_url || !previous || !previous_present || !committed)
        return set_write_error(error, FALSE, "Runtime store arguments are invalid");
    EgpRuntimeConfig next = {0};
    if (!egp_endpoint_canonicalize(candidate_url, next.ota_server_base_url, error))
        return FALSE;
    memset(previous, 0, sizeof(*previous));
    memset(committed, 0, sizeof(*committed));
    *previous_present = FALSE;
    g_mutex_lock(&store->mutex);
    gboolean ok = load_runtime_path(store->runtime_path, previous, previous_present, error);
    if (ok && *previous_present && previous->generation >= (guint64)EGP_JSON_UINT_MAX) {
        egp_error_set(error, EGP_ERROR_PERSISTENCE_SCHEMA_UNSUPPORTED, FALSE,
                      EGP_PERSISTENT_CHANGE_NONE, "Runtime generation cannot be incremented");
        ok = FALSE;
    }
    next.generation = *previous_present ? previous->generation + 1u : 1u;
    char *data = NULL;
    gsize length = 0;
    if (ok) {
        data = serialize_runtime(&next, &length);
        ok = length <= EGP_STORE_MAX_BYTES &&
             atomic_write_at(store, EGP_RUNTIME_FILENAME, data, length, error);
    }
    if (data) {
        secure_clear(data, length);
        g_free(data);
    }
    if (ok) {
        *committed = next;
        egp_error_set(error, EGP_ERROR_NONE, FALSE,
                      EGP_PERSISTENT_CHANGE_COMMITTED, "Runtime endpoint committed");
    }
    g_mutex_unlock(&store->mutex);
    return ok;
}

static gboolean restore_absence(EgpStore *store, EgpError *error)
{
    struct stat st;
    if (fstatat(store->directory_fd, EGP_WIFI_FILENAME, &st, AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno == ENOENT) {
            egp_error_clear(error);
            return TRUE;
        }
        return set_write_error(error, FALSE, "Wi-Fi rollback destination could not be inspected");
    }
    if (!metadata_is_private_regular(&st))
        return set_write_error(error, FALSE, "Wi-Fi rollback source is unsafe");
    if (renameat(store->directory_fd, EGP_WIFI_FILENAME,
                 store->directory_fd, ".wifi.json.rollback") < 0)
        return set_write_error(error, FALSE, "Wi-Fi rollback rename failed");
    if (fsync(store->directory_fd) < 0)
        return set_write_error(error, TRUE, "Wi-Fi rollback durability is uncertain");
    if (unlinkat(store->directory_fd, ".wifi.json.rollback", 0) < 0 ||
        fsync(store->directory_fd) < 0)
        return set_write_error(error, TRUE, "Wi-Fi rollback cleanup is uncertain");
    egp_error_clear(error);
    return TRUE;
}

gboolean egp_store_restore_wifi(EgpStore *store, const EgpWifiConfig *previous,
                                gboolean previous_present, guint64 *restored_generation,
                                EgpError *error)
{
    if (!store || !restored_generation || (previous_present &&
        (!previous || !egp_wifi_validate(previous, error))))
        return FALSE;
    *restored_generation = 0;
    g_mutex_lock(&store->mutex);
    gboolean ok;
    if (!previous_present) {
        ok = restore_absence(store, error);
    } else {
        EgpWifiConfig current = {0};
        gboolean current_present = FALSE;
        ok = load_wifi_path(store->wifi_path, &current, &current_present, error);
        guint64 base = MAX(current.generation, previous->generation);
        if (ok && base >= (guint64)EGP_JSON_UINT_MAX) {
            egp_error_set(error, EGP_ERROR_PERSISTENCE_SCHEMA_UNSUPPORTED, FALSE,
                          EGP_PERSISTENT_CHANGE_NONE, "Wi-Fi rollback generation cannot be incremented");
            ok = FALSE;
        }
        EgpWifiConfig restored = *previous;
        restored.generation = base + 1u;
        char *data = NULL;
        gsize length = 0;
        if (ok) {
            data = serialize_wifi(&restored, &length);
            ok = atomic_write_at(store, EGP_WIFI_FILENAME, data, length, error);
            *restored_generation = restored.generation;
        }
        if (data) {
            secure_clear(data, length);
            g_free(data);
        }
        egp_wifi_clear(&restored);
        egp_wifi_clear(&current);
    }
    g_mutex_unlock(&store->mutex);
    if (ok) {
        egp_error_set(error, EGP_ERROR_NONE, FALSE,
                      EGP_PERSISTENT_CHANGE_ROLLED_BACK,
                      "Previous Wi-Fi configuration restored");
    }
    return ok;
}

gboolean egp_store_forget_begin(EgpStore *store, EgpWifiConfig *forgotten,
                                gboolean *had_wifi, EgpError *error)
{
    if (!store || !forgotten || !had_wifi)
        return set_write_error(error, FALSE, "Forget arguments are invalid");
    memset(forgotten, 0, sizeof(*forgotten));
    *had_wifi = FALSE;
    g_mutex_lock(&store->mutex);

    gboolean pending = FALSE;
    gboolean ok = load_wifi_path(store->forget_path, forgotten, &pending, error);
    if (ok && pending) {
        struct stat live;
        if (fstatat(store->directory_fd, EGP_WIFI_FILENAME, &live,
                    AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
            egp_wifi_clear(forgotten);
            ok = set_write_error(error, FALSE,
                                 "Live Wi-Fi and forget tombstone conflict");
        } else {
            *had_wifi = TRUE;
        }
        g_mutex_unlock(&store->mutex);
        return ok;
    }
    if (!ok) {
        g_mutex_unlock(&store->mutex);
        return FALSE;
    }

    gboolean live = FALSE;
    ok = load_wifi_path(store->wifi_path, forgotten, &live, error);
    if (ok && live) {
        if (renameat(store->directory_fd, EGP_WIFI_FILENAME,
                     store->directory_fd, EGP_FORGET_FILENAME) < 0) {
            ok = set_write_error(error, FALSE, "Wi-Fi forget rename failed");
        } else if (fsync(store->directory_fd) < 0) {
            ok = set_write_error(error, TRUE, "Wi-Fi forget durability is uncertain");
        }
        *had_wifi = TRUE;
    }
    if (ok)
        egp_error_set(error, EGP_ERROR_NONE, FALSE,
                      live ? EGP_PERSISTENT_CHANGE_COMMITTED : EGP_PERSISTENT_CHANGE_NONE,
                      "Wi-Fi forget tombstone prepared");
    g_mutex_unlock(&store->mutex);
    return ok;
}

gboolean egp_store_forget_pending(EgpStore *store, EgpWifiConfig *forgotten,
                                  gboolean *pending, EgpError *error)
{
    if (!store || !forgotten || !pending)
        return set_read_error(error, EGP_ERROR_PERSISTENCE_READ_FAILED,
                              "Forget recovery arguments are invalid");
    g_mutex_lock(&store->mutex);
    gboolean ok = load_wifi_path(store->forget_path, forgotten, pending, error);
    if (ok && *pending) {
        struct stat live;
        if (fstatat(store->directory_fd, EGP_WIFI_FILENAME, &live,
                    AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
            egp_wifi_clear(forgotten);
            *pending = FALSE;
            ok = set_read_error(error, EGP_ERROR_PERSISTENCE_READ_FAILED,
                                "Live Wi-Fi and forget tombstone conflict");
        }
    }
    g_mutex_unlock(&store->mutex);
    return ok;
}

gboolean egp_store_forget_finish(EgpStore *store, EgpError *error)
{
    if (!store)
        return set_write_error(error, FALSE, "Forget store is invalid");
    g_mutex_lock(&store->mutex);
    struct stat st;
    gboolean ok = TRUE;
    if (fstatat(store->directory_fd, EGP_FORGET_FILENAME, &st,
                AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno != ENOENT)
            ok = set_write_error(error, FALSE, "Forget tombstone could not be inspected");
    } else if (!metadata_is_private_regular(&st)) {
        ok = set_write_error(error, FALSE, "Forget tombstone is unsafe");
    } else if (unlinkat(store->directory_fd, EGP_FORGET_FILENAME, 0) < 0) {
        ok = set_write_error(error, FALSE, "Forget tombstone removal failed");
    } else if (fsync(store->directory_fd) < 0) {
        ok = set_write_error(error, TRUE, "Forget completion durability is uncertain");
    }
    if (ok)
        egp_error_set(error, EGP_ERROR_NONE, FALSE,
                      EGP_PERSISTENT_CHANGE_COMMITTED, "Wi-Fi forget completed");
    g_mutex_unlock(&store->mutex);
    return ok;
}
