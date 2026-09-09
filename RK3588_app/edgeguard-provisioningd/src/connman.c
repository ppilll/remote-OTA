#include "edgeguard_provisioning/connman.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CONNMAN_BUS "net.connman"
#define CONNMAN_MANAGER_PATH "/"
#define CONNMAN_MANAGER_IFACE "net.connman.Manager"
#define CONNMAN_TECH_IFACE "net.connman.Technology"
#define CONNMAN_SERVICE_IFACE "net.connman.Service"

struct _EgpConnman {
    GDBusConnection *connection;
    char *profile_path;
};

static gboolean connman_fail(EgpError *error, EgpErrorCode code,
                             gboolean retryable, const char *message)
{
    egp_error_set(error, code, retryable, EGP_PERSISTENT_CHANGE_NONE, "%s", message);
    return FALSE;
}

static gboolean absolute_clean_path(const char *path)
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

static gboolean private_regular(const struct stat *st)
{
    return S_ISREG(st->st_mode) && st->st_uid == 0 && (st->st_mode & 0077) == 0;
}

static gboolean cleanup_profile_temps(int directory, EgpError *error)
{
    int copy = dup(directory);
    if (copy < 0)
        return connman_fail(error, EGP_ERROR_CONNMAN_APPLY_FAILED, TRUE,
                            "ConnMan profile recovery scan failed");
    DIR *stream = fdopendir(copy);
    if (!stream) {
        close(copy);
        return connman_fail(error, EGP_ERROR_CONNMAN_APPLY_FAILED, TRUE,
                            "ConnMan profile recovery scan failed");
    }
    gboolean ok = TRUE, changed = FALSE;
    struct dirent *entry;
    while ((entry = readdir(stream)) != NULL) {
        if (!g_str_has_prefix(entry->d_name, ".egp-connman-"))
            continue;
        struct stat st;
        if (fstatat(directory, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0 ||
            !private_regular(&st) || unlinkat(directory, entry->d_name, 0) < 0) {
            ok = FALSE;
            break;
        }
        changed = TRUE;
    }
    closedir(stream);
    if (ok && changed && fsync(directory) < 0)
        ok = FALSE;
    if (!ok)
        return connman_fail(error, EGP_ERROR_CONNMAN_APPLY_FAILED, TRUE,
                            "ConnMan profile recovery cleanup failed");
    return TRUE;
}

static gboolean verify_file(int fd, const char *data, gsize length)
{
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
        (st.st_mode & 0777) != 0600 || st.st_size < 0 ||
        (guint64)st.st_size != length || lseek(fd, 0, SEEK_SET) < 0)
        return FALSE;
    char buffer[1024];
    gsize checked = 0;
    while (checked < length) {
        gsize wanted = MIN(sizeof(buffer), length - checked);
        ssize_t count = read(fd, buffer, wanted);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0 || memcmp(buffer, data + checked, (gsize)count))
            return FALSE;
        checked += (gsize)count;
    }
    return TRUE;
}

static gboolean profile_atomic_write(EgpConnman *connman, const char *data,
                                     gsize length, EgpError *error)
{
    char *parent = g_path_get_dirname(connman->profile_path);
    char *base = g_path_get_basename(connman->profile_path);
    int directory = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    g_free(parent);
    if (directory < 0) {
        g_free(base);
        return connman_fail(error, EGP_ERROR_CONNMAN_APPLY_FAILED, TRUE,
                            "ConnMan profile directory is unavailable");
    }
    struct stat directory_st;
    if (fstat(directory, &directory_st) < 0 || !S_ISDIR(directory_st.st_mode) ||
        directory_st.st_uid != 0 || (directory_st.st_mode & 0022) != 0) {
        close(directory);
        g_free(base);
        return connman_fail(error, EGP_ERROR_CONNMAN_APPLY_FAILED, TRUE,
                            "ConnMan profile directory metadata is unsafe");
    }
    if (!cleanup_profile_temps(directory, error)) {
        close(directory);
        g_free(base);
        return FALSE;
    }

    struct stat existing;
    if (fstatat(directory, base, &existing, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!private_regular(&existing)) {
            close(directory);
            g_free(base);
            return connman_fail(error, EGP_ERROR_CONNMAN_APPLY_FAILED, TRUE,
                                "Existing ConnMan profile is unsafe");
        }
    } else if (errno != ENOENT) {
        close(directory);
        g_free(base);
        return connman_fail(error, EGP_ERROR_CONNMAN_APPLY_FAILED, TRUE,
                            "ConnMan profile could not be inspected");
    }

    char temporary[96] = {0};
    int fd = -1;
    gboolean created = FALSE;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        g_snprintf(temporary, sizeof(temporary), ".egp-connman-%ld-%08x-%08x",
                   (long)getpid(), g_random_int(), g_random_int());
        fd = openat(directory, temporary,
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
        ssize_t count = write(fd, data + written, length - written);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            ok = FALSE;
            break;
        }
        written += (gsize)count;
    }
    if (ok && (fsync(fd) < 0 || !verify_file(fd, data, length)))
        ok = FALSE;
    if (fd >= 0 && close(fd) < 0)
        ok = FALSE;
    fd = -1;
    if (ok && renameat(directory, temporary, directory, base) == 0) {
        created = FALSE;
    } else if (ok) {
        ok = FALSE;
    }
    if (ok && fsync(directory) < 0)
        ok = FALSE;
    if (fd >= 0)
        close(fd);
    if (created)
        unlinkat(directory, temporary, 0);
    close(directory);
    g_free(base);
    if (!ok)
        return connman_fail(error, EGP_ERROR_CONNMAN_APPLY_FAILED, TRUE,
                            "ConnMan profile atomic replacement failed");
    egp_error_clear(error);
    return TRUE;
}

gboolean egp_connman_new(GDBusConnection *connection, const char *profile_path,
                         EgpConnman **out, EgpError *error)
{
    if (!out)
        return connman_fail(error, EGP_ERROR_INTERNAL_ERROR, FALSE,
                            "ConnMan adapter output is required");
    *out = NULL;
    if (!profile_path)
        profile_path = EGP_CONNMAN_PROFILE;
#ifndef EGP_ALLOW_TEST_PATHS
    if (strcmp(profile_path, EGP_CONNMAN_PROFILE))
        return connman_fail(error, EGP_ERROR_INTERNAL_ERROR, FALSE,
                            "Non-canonical ConnMan profile path is forbidden");
#endif
    if (!absolute_clean_path(profile_path))
        return connman_fail(error, EGP_ERROR_INTERNAL_ERROR, FALSE,
                            "ConnMan profile path is invalid");

    GError *bus_error = NULL;
    GDBusConnection *owned = connection ? g_object_ref(connection)
                                        : g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &bus_error);
    if (!owned) {
        g_clear_error(&bus_error);
        return connman_fail(error, EGP_ERROR_CONNMAN_UNAVAILABLE, TRUE,
                            "System D-Bus is unavailable");
    }
    EgpConnman *connman = g_new0(EgpConnman, 1);
    connman->connection = owned;
    connman->profile_path = g_strdup(profile_path);
    *out = connman;
    egp_error_clear(error);
    return TRUE;
}

void egp_connman_free(EgpConnman *connman)
{
    if (!connman)
        return;
    g_clear_object(&connman->connection);
    g_free(connman->profile_path);
    g_free(connman);
}

gboolean egp_connman_write_profile(EgpConnman *connman, const EgpWifiConfig *wifi,
                                   EgpError *error)
{
    if (!connman)
        return connman_fail(error, EGP_ERROR_INTERNAL_ERROR, FALSE,
                            "ConnMan adapter is invalid");
    if (!egp_wifi_validate(wifi, error))
        return FALSE;
    GKeyFile *key_file = g_key_file_new();
    g_key_file_set_string(key_file, "service_edgeguard", "Type", "wifi");
    g_key_file_set_string(key_file, "service_edgeguard", "Name", wifi->ssid);
    g_key_file_set_boolean(key_file, "service_edgeguard", "Hidden", wifi->hidden);
    g_key_file_set_string(key_file, "service_edgeguard", "Security", "psk");
    g_key_file_set_string(key_file, "service_edgeguard", "Passphrase", wifi->passphrase);
    gsize length = 0;
    char *data = g_key_file_to_data(key_file, &length, NULL);
    g_key_file_unref(key_file);
    gboolean ok = data && profile_atomic_write(connman, data, length, error);
    if (!data)
        connman_fail(error, EGP_ERROR_INTERNAL_ERROR, TRUE,
                     "ConnMan profile serialization failed");
    if (data) {
        volatile char *p = data;
        for (gsize i = 0; i < length; ++i)
            p[i] = 0;
        g_free(data);
    }
    return ok;
}

gboolean egp_connman_remove_profile(EgpConnman *connman, EgpError *error)
{
    if (!connman)
        return connman_fail(error, EGP_ERROR_INTERNAL_ERROR, FALSE,
                            "ConnMan adapter is invalid");
    char *parent = g_path_get_dirname(connman->profile_path);
    char *base = g_path_get_basename(connman->profile_path);
    int directory = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    g_free(parent);
    if (directory < 0) {
        g_free(base);
        return connman_fail(error, EGP_ERROR_CONNMAN_APPLY_FAILED, TRUE,
                            "ConnMan profile directory is unavailable");
    }
    struct stat directory_st;
    if (fstat(directory, &directory_st) < 0 || !S_ISDIR(directory_st.st_mode) ||
        directory_st.st_uid != 0 || (directory_st.st_mode & 0022) != 0) {
        close(directory);
        g_free(base);
        return connman_fail(error, EGP_ERROR_CONNMAN_APPLY_FAILED, TRUE,
                            "ConnMan profile directory metadata is unsafe");
    }
    if (!cleanup_profile_temps(directory, error)) {
        close(directory);
        g_free(base);
        return FALSE;
    }
    struct stat st;
    gboolean ok = TRUE;
    if (fstatat(directory, base, &st, AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno != ENOENT)
            ok = FALSE;
    } else if (!private_regular(&st) || unlinkat(directory, base, 0) < 0) {
        ok = FALSE;
    } else if (fsync(directory) < 0) {
        ok = FALSE;
    }
    close(directory);
    g_free(base);
    if (!ok)
        return connman_fail(error, EGP_ERROR_CONNMAN_APPLY_FAILED, TRUE,
                            "Owned ConnMan profile removal failed");
    egp_error_clear(error);
    return TRUE;
}

static gint remaining_ms(gint64 deadline_us)
{
    gint64 remaining = deadline_us - g_get_monotonic_time();
    if (remaining <= 0)
        return 0;
    return (gint)MIN((remaining + 999) / 1000, (gint64)G_MAXINT);
}

static GVariant *call_sync(EgpConnman *connman, const char *path,
                           const char *interface_name, const char *method,
                           GVariant *parameters, const GVariantType *reply_type,
                           gint64 deadline_us, GCancellable *cancellable,
                           GError **error)
{
    gint timeout = remaining_ms(deadline_us);
    if (!timeout) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                            "ConnMan operation timed out");
        if (parameters)
            g_variant_unref(g_variant_ref_sink(parameters));
        return NULL;
    }
    return g_dbus_connection_call_sync(connman->connection, CONNMAN_BUS, path,
                                       interface_name, method, parameters, reply_type,
                                       G_DBUS_CALL_FLAGS_NONE, timeout, cancellable, error);
}

static gboolean connman_has_owner(EgpConnman *connman, gint64 deadline_us,
                                  GCancellable *cancellable)
{
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        connman->connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", CONNMAN_BUS),
        G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE,
        MAX(1, remaining_ms(deadline_us)), cancellable, &error);
    gboolean owner = FALSE;
    if (reply) {
        g_variant_get(reply, "(b)", &owner);
        g_variant_unref(reply);
    }
    g_clear_error(&error);
    return owner;
}

static gboolean security_has_psk(GVariant *properties)
{
    GVariant *security = g_variant_lookup_value(properties, "Security",
                                                G_VARIANT_TYPE_STRING_ARRAY);
    if (!security)
        return FALSE;
    gboolean found = FALSE;
    GVariantIter iter;
    const char *item;
    g_variant_iter_init(&iter, security);
    while (g_variant_iter_loop(&iter, "&s", &item)) {
        if (!strcmp(item, "psk")) {
            found = TRUE;
            break;
        }
    }
    g_variant_unref(security);
    return found;
}

static gboolean properties_match_wifi(GVariant *properties, const EgpWifiConfig *wifi)
{
    const char *name = NULL;
    return g_variant_lookup(properties, "Name", "&s", &name) && name &&
           !strcmp(name, wifi->ssid) && security_has_psk(properties);
}

typedef struct {
    gboolean found;
    gboolean connected;
    gboolean provisioned;
    char path[EGP_DBUS_PATH_CAP];
    char state[32];
} ServiceObservation;

static gboolean has_ip_address(GVariant *properties)
{
    GVariant *ipv4 = g_variant_lookup_value(properties, "IPv4",
                                            G_VARIANT_TYPE("a{sv}"));
    GVariant *ipv6 = g_variant_lookup_value(properties, "IPv6",
                                            G_VARIANT_TYPE("a{sv}"));
    const char *address = NULL;
    gboolean found = (ipv4 && g_variant_lookup(ipv4, "Address", "&s", &address) &&
                      address && *address);
    address = NULL;
    if (!found)
        found = ipv6 && g_variant_lookup(ipv6, "Address", "&s", &address) &&
                address && *address;
    if (ipv4)
        g_variant_unref(ipv4);
    if (ipv6)
        g_variant_unref(ipv6);
    return found;
}

static gboolean observe_service(EgpConnman *connman, const EgpWifiConfig *wifi,
                                gint64 deadline_us, GCancellable *cancellable,
                                ServiceObservation *observation, GError **error)
{
    memset(observation, 0, sizeof(*observation));
    GVariant *reply = call_sync(connman, CONNMAN_MANAGER_PATH, CONNMAN_MANAGER_IFACE,
                                "GetServices", NULL, G_VARIANT_TYPE("(a(oa{sv}))"),
                                deadline_us, cancellable, error);
    if (!reply)
        return FALSE;
    GVariantIter *services = NULL;
    g_variant_get(reply, "(a(oa{sv}))", &services);
    const char *path;
    GVariant *properties;
    while (g_variant_iter_next(services, "(&o@a{sv})", &path, &properties)) {
        if (properties_match_wifi(properties, wifi)) {
            const char *state = NULL;
            gboolean immutable = FALSE, favorite = FALSE, autoconnect = FALSE;
            observation->found = TRUE;
            g_strlcpy(observation->path, path, sizeof(observation->path));
            if (g_variant_lookup(properties, "State", "&s", &state) && state)
                g_strlcpy(observation->state, state, sizeof(observation->state));
            g_variant_lookup(properties, "Immutable", "b", &immutable);
            g_variant_lookup(properties, "Favorite", "b", &favorite);
            g_variant_lookup(properties, "AutoConnect", "b", &autoconnect);
            observation->provisioned = immutable || favorite || autoconnect;
            observation->connected = (!strcmp(observation->state, "ready") ||
                                      !strcmp(observation->state, "online")) &&
                                     has_ip_address(properties);
            g_variant_unref(properties);
            break;
        }
        g_variant_unref(properties);
    }
    g_variant_iter_free(services);
    g_variant_unref(reply);
    return TRUE;
}

static gboolean find_wifi_technology(EgpConnman *connman, gint64 deadline_us,
                                     GCancellable *cancellable,
                                     char path[EGP_DBUS_PATH_CAP], GError **error)
{
    GVariant *reply = call_sync(connman, CONNMAN_MANAGER_PATH, CONNMAN_MANAGER_IFACE,
                                "GetTechnologies", NULL, G_VARIANT_TYPE("(a(oa{sv}))"),
                                deadline_us, cancellable, error);
    if (!reply)
        return FALSE;
    gboolean found = FALSE;
    GVariantIter *technologies = NULL;
    g_variant_get(reply, "(a(oa{sv}))", &technologies);
    const char *object_path;
    GVariant *properties;
    while (g_variant_iter_next(technologies, "(&o@a{sv})", &object_path, &properties)) {
        const char *type = NULL;
        gboolean powered = FALSE;
        if (g_variant_lookup(properties, "Type", "&s", &type) && type &&
            !strcmp(type, "wifi")) {
            g_variant_lookup(properties, "Powered", "b", &powered);
            if (powered) {
                found = TRUE;
                g_strlcpy(path, object_path, EGP_DBUS_PATH_CAP);
            }
            g_variant_unref(properties);
            break;
        }
        g_variant_unref(properties);
    }
    g_variant_iter_free(technologies);
    g_variant_unref(reply);
    return found;
}

static void result_init(EgpConnmanResult *result)
{
    memset(result, 0, sizeof(*result));
    result->disposition = EGP_CONNMAN_RETAIN_CANONICAL;
    result->state = EGP_STATE_FAILED_DEPENDENCY;
    result->code = EGP_ERROR_CONNMAN_APPLY_FAILED;
}

static gboolean dependency_error(EgpConnmanResult *result, EgpError *error)
{
    result->code = EGP_ERROR_CONNMAN_UNAVAILABLE;
    result->state = EGP_STATE_FAILED_DEPENDENCY;
    result->disposition = EGP_CONNMAN_RETAIN_CANONICAL;
    return connman_fail(error, result->code, TRUE, "ConnMan is unavailable");
}

static gboolean sleep_until_poll(gint64 deadline_us, GCancellable *cancellable)
{
    if (cancellable && g_cancellable_is_cancelled(cancellable))
        return FALSE;
    gint remaining = remaining_ms(deadline_us);
    if (!remaining)
        return FALSE;
    g_usleep((gulong)MIN(remaining, 200) * 1000u);
    return !cancellable || !g_cancellable_is_cancelled(cancellable);
}

static gboolean remote_error_is(GError *error, const char *suffix)
{
    if (!error || !g_dbus_error_is_remote_error(error))
        return FALSE;
    char *name = g_dbus_error_get_remote_error(error);
    gboolean matches = name && g_str_has_suffix(name, suffix);
    g_free(name);
    return matches;
}

gboolean egp_connman_apply(EgpConnman *connman, const EgpWifiConfig *wifi,
                           guint timeout_ms, GCancellable *cancellable,
                           EgpConnmanResult *result, EgpError *error)
{
    if (!connman || !result || timeout_ms == 0 || !egp_wifi_validate(wifi, error))
        return FALSE;
    result_init(result);
    result->state = EGP_STATE_APPLYING;
    if (!egp_connman_write_profile(connman, wifi, error))
        return FALSE;

    gint64 deadline_us = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
    if (!connman_has_owner(connman, deadline_us, cancellable))
        return dependency_error(result, error);

    char technology_path[EGP_DBUS_PATH_CAP] = {0};
    GError *call_error = NULL;
    if (!find_wifi_technology(connman, deadline_us, cancellable,
                              technology_path, &call_error)) {
        g_clear_error(&call_error);
        return dependency_error(result, error);
    }
    GVariant *reply = call_sync(connman, technology_path, CONNMAN_TECH_IFACE, "Scan",
                                NULL, G_VARIANT_TYPE("()"), deadline_us,
                                cancellable, &call_error);
    if (reply)
        g_variant_unref(reply);
    if (!reply && !remote_error_is(call_error, ".InProgress")) {
        g_clear_error(&call_error);
        return dependency_error(result, error);
    }
    g_clear_error(&call_error);

    ServiceObservation observed;
    gboolean observation_ok = FALSE;
    while (remaining_ms(deadline_us) > 0) {
        if (!observe_service(connman, wifi, deadline_us, cancellable,
                             &observed, &call_error))
            break;
        if (observed.found) {
            observation_ok = TRUE;
            break;
        }
        if (!sleep_until_poll(deadline_us, cancellable))
            break;
    }
    if (!observation_ok) {
        g_clear_error(&call_error);
        if (!connman_has_owner(connman, deadline_us, cancellable))
            return dependency_error(result, error);
        result->code = EGP_ERROR_WIFI_AP_NOT_FOUND;
        result->state = EGP_STATE_FAILED_NOT_FOUND;
        result->disposition = EGP_CONNMAN_ROLLBACK_ALLOWED;
        return connman_fail(error, result->code, TRUE, "Provisioned Wi-Fi service was not found");
    }
    g_strlcpy(result->service_path, observed.path, sizeof(result->service_path));
    result->state = EGP_STATE_ASSOCIATING;

    if (!observed.connected) {
        reply = call_sync(connman, observed.path, CONNMAN_SERVICE_IFACE, "Connect",
                          NULL, G_VARIANT_TYPE("()"), deadline_us,
                          cancellable, &call_error);
        if (reply)
            g_variant_unref(reply);
        if (!reply && !remote_error_is(call_error, ".AlreadyConnected") &&
            !remote_error_is(call_error, ".InProgress")) {
            if (remote_error_is(call_error, ".InvalidKey")) {
                result->code = EGP_ERROR_WIFI_AUTH_FAILED;
                result->state = EGP_STATE_FAILED_AUTH;
                result->disposition = EGP_CONNMAN_ROLLBACK_ALLOWED;
                g_clear_error(&call_error);
                return connman_fail(error, result->code, TRUE,
                                    "ConnMan rejected the Wi-Fi credential");
            }
            if (remote_error_is(call_error, ".NotFound")) {
                result->code = EGP_ERROR_WIFI_AP_NOT_FOUND;
                result->state = EGP_STATE_FAILED_NOT_FOUND;
                result->disposition = EGP_CONNMAN_ROLLBACK_ALLOWED;
                g_clear_error(&call_error);
                return connman_fail(error, result->code, TRUE,
                                    "Provisioned Wi-Fi service disappeared");
            }
            gboolean owner = connman_has_owner(connman, deadline_us, cancellable);
            g_clear_error(&call_error);
            if (!owner)
                return dependency_error(result, error);
            return connman_fail(error, EGP_ERROR_CONNMAN_APPLY_FAILED, TRUE,
                                "ConnMan could not start association");
        }
        g_clear_error(&call_error);
    }

    while (remaining_ms(deadline_us) > 0) {
        if (!observe_service(connman, wifi, deadline_us, cancellable,
                             &observed, &call_error))
            break;
        if (observed.found) {
            g_strlcpy(result->service_path, observed.path, sizeof(result->service_path));
            if (observed.connected) {
                result->code = EGP_ERROR_NONE;
                result->state = EGP_STATE_CONNECTED;
                result->disposition = EGP_CONNMAN_CONNECTED;
                egp_error_set(error, EGP_ERROR_NONE, FALSE,
                              EGP_PERSISTENT_CHANGE_NONE, "Wi-Fi connected");
                return TRUE;
            }
        }
        if (!sleep_until_poll(deadline_us, cancellable))
            break;
    }
    g_clear_error(&call_error);
    if (!connman_has_owner(connman, deadline_us, cancellable))
        return dependency_error(result, error);
    result->code = EGP_ERROR_WIFI_CONNECT_TIMEOUT;
    result->state = EGP_STATE_FAILED_TIMEOUT;
    result->disposition = EGP_CONNMAN_RETAIN_CANONICAL;
    return connman_fail(error, result->code, TRUE,
                        "Wi-Fi association outcome is uncertain after timeout");
}

gboolean egp_connman_revoke(EgpConnman *connman, const EgpWifiConfig *wifi,
                            guint timeout_ms, GCancellable *cancellable,
                            EgpConnmanResult *result, EgpError *error)
{
    if (!connman || !result || timeout_ms == 0 || !egp_wifi_validate(wifi, error))
        return FALSE;
    result_init(result);
    result->state = EGP_STATE_APPLYING;
    if (!egp_connman_remove_profile(connman, error))
        return FALSE;

    gint64 deadline_us = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
    if (!connman_has_owner(connman, deadline_us, cancellable))
        return dependency_error(result, error);
    GError *call_error = NULL;
    ServiceObservation observed;
    if (!observe_service(connman, wifi, deadline_us, cancellable,
                         &observed, &call_error)) {
        g_clear_error(&call_error);
        return dependency_error(result, error);
    }
    if (!observed.found) {
        result->code = EGP_ERROR_NONE;
        result->state = EGP_STATE_UNPROVISIONED;
        result->disposition = EGP_CONNMAN_CONNECTED;
        egp_error_set(error, EGP_ERROR_NONE, FALSE, EGP_PERSISTENT_CHANGE_NONE,
                      "Provisioned Wi-Fi service is absent");
        return TRUE;
    }
    g_strlcpy(result->service_path, observed.path, sizeof(result->service_path));

    GVariant *reply = call_sync(connman, observed.path, CONNMAN_SERVICE_IFACE,
                                "Disconnect", NULL, G_VARIANT_TYPE("()"),
                                deadline_us, cancellable, &call_error);
    if (reply)
        g_variant_unref(reply);
    if (!reply && !remote_error_is(call_error, ".NotConnected") &&
        !remote_error_is(call_error, ".NotFound")) {
        gboolean owner = connman_has_owner(connman, deadline_us, cancellable);
        g_clear_error(&call_error);
        if (!owner)
            return dependency_error(result, error);
        return connman_fail(error, EGP_ERROR_CONNMAN_APPLY_FAILED, TRUE,
                            "ConnMan could not disconnect the provisioned service");
    }
    g_clear_error(&call_error);

    reply = call_sync(connman, observed.path, CONNMAN_SERVICE_IFACE,
                      "Remove", NULL, G_VARIANT_TYPE("()"),
                      deadline_us, cancellable, &call_error);
    if (reply)
        g_variant_unref(reply);
    if (!reply && !remote_error_is(call_error, ".NotFound") &&
        !remote_error_is(call_error, ".NotSupported")) {
        gboolean owner = connman_has_owner(connman, deadline_us, cancellable);
        g_clear_error(&call_error);
        if (!owner)
            return dependency_error(result, error);
        return connman_fail(error, EGP_ERROR_CONNMAN_APPLY_FAILED, TRUE,
                            "ConnMan could not remove the provisioned service");
    }
    g_clear_error(&call_error);

    while (remaining_ms(deadline_us) > 0) {
        if (!observe_service(connman, wifi, deadline_us, cancellable,
                             &observed, &call_error))
            break;
        if (!observed.found || !observed.provisioned) {
            result->code = EGP_ERROR_NONE;
            result->state = EGP_STATE_UNPROVISIONED;
            result->disposition = EGP_CONNMAN_CONNECTED;
            egp_error_set(error, EGP_ERROR_NONE, FALSE,
                          EGP_PERSISTENT_CHANGE_NONE, "Provisioned Wi-Fi service revoked");
            return TRUE;
        }
        if (!sleep_until_poll(deadline_us, cancellable))
            break;
    }
    g_clear_error(&call_error);
    if (!connman_has_owner(connman, deadline_us, cancellable))
        return dependency_error(result, error);
    return connman_fail(error, EGP_ERROR_CONNMAN_APPLY_FAILED, TRUE,
                        "ConnMan revocation outcome is uncertain after timeout");
}

guint egp_connman_retry_delay_ms(guint attempt, guint32 random_value)
{
    guint shift = MIN(attempt, 6u);
    guint base = MIN(1000u << shift, 60000u);
    guint spread = base / 5u;
    guint range = spread * 2u + 1u;
    gint64 jitter = (gint64)(random_value % range) - spread;
    return (guint)CLAMP((gint64)base + jitter, 800, 60000);
}
