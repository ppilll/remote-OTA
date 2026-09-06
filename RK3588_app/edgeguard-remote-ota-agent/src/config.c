#include "edgeguard_ota/config.h"
#include "edgeguard_ota/persistence.h"
#include <errno.h>
#include <string.h>

typedef struct { const char *group, *key; } ConfigKey;
static const ConfigKey keys[] = {
    {"server", "base_url"}, {"server", "manifest_path"}, {"server", "report_path"},
    {"server", "poll_interval_sec"}, {"server", "connect_timeout_sec"},
    {"server", "request_timeout_sec"}, {"agent", "state_dir"},
    {"agent", "release_file"}, {"agent", "max_manifest_bytes"},
    {"download", "part_file"}, {"download", "bundle_file"}, {"download", "reserve_bytes"},
    {"rauc", "binary"}, {"identity", "device_id_file"},
    {"health", "hook"}, {"health", "timeout_sec"}, {"reporting", "mode"}
};

/* Reject duplicate sections/keys before GKeyFile's last-value-wins behavior.
 * GKeyFile still owns the INI syntax and value decoding. */
static gboolean unique_entries(const char *text)
{
    char **lines = g_strsplit(text, "\n", -1);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char *group = NULL;
    gboolean ok = TRUE;
    for (gsize i = 0; lines[i] && ok; ++i) {
        char *line = g_strstrip(lines[i]);
        if (!*line || *line == '#') continue;
        char *entry = NULL;
        if (*line == '[') {
            char *end = strchr(line, ']');
            if (!end || end[1]) { ok = FALSE; break; }
            *end = 0;
            g_free(group);
            group = g_strdup(line + 1);
            entry = g_strdup_printf("group:%s", group);
        } else {
            char *equals = strchr(line, '=');
            if (!group || !equals) { ok = FALSE; break; }
            *equals = 0;
            entry = g_strdup_printf("key:%s:%s", group, g_strstrip(line));
        }
        if (g_hash_table_contains(seen, entry)) { g_free(entry); ok = FALSE; }
        else g_hash_table_add(seen, entry);
    }
    g_free(group);
    g_hash_table_unref(seen);
    g_strfreev(lines);
    return ok;
}

static gboolean known_entries(GKeyFile *file)
{
    gsize n;
    char **groups = g_key_file_get_groups(file, &n);
    gboolean ok = TRUE;
    for (gsize g = 0; g < n && ok; ++g) {
        gboolean known_group = FALSE;
        for (gsize i = 0; i < G_N_ELEMENTS(keys); ++i)
            if (!strcmp(groups[g], keys[i].group)) known_group = TRUE;
        if (!known_group) { ok = FALSE; break; }
        gsize count;
        char **names = g_key_file_get_keys(file, groups[g], &count, NULL);
        for (gsize k = 0; k < count; ++k) {
            gboolean known = FALSE;
            for (gsize i = 0; i < G_N_ELEMENTS(keys); ++i)
                if (!strcmp(groups[g], keys[i].group) && !strcmp(names[k], keys[i].key))
                    known = TRUE;
            if (!known) ok = FALSE;
        }
        g_strfreev(names);
    }
    g_strfreev(groups);
    return ok;
}

static gboolean read_string(GKeyFile *file, const char *group, const char *key,
                             char *out, gsize capacity, gboolean optional)
{
    if (optional && !g_key_file_has_key(file, group, key, NULL)) { out[0] = 0; return TRUE; }
    GError *error = NULL;
    char *value = g_key_file_get_string(file, group, key, &error);
    gboolean ok = !error && value && strlen(value) < capacity &&
                  (optional || *value) && g_utf8_validate(value, -1, NULL);
    if (ok) {
        for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
            if (*p < 0x20 || *p == 0x7f) ok = FALSE;
    }
    if (ok) g_strlcpy(out, value, capacity);
    g_clear_error(&error);
    g_free(value);
    return ok;
}

static gboolean read_number(GKeyFile *file, const char *group, const char *key,
                             uint64_t minimum, uint64_t maximum, uint64_t *out)
{
    char value[32];
    if (!read_string(file, group, key, value, sizeof(value), FALSE)) return FALSE;
    for (const char *p = value; *p; ++p) if (!g_ascii_isdigit(*p)) return FALSE;
    errno = 0;
    char *end;
    guint64 number = g_ascii_strtoull(value, &end, 10);
    if (errno || *end || number < minimum || number > maximum) return FALSE;
    *out = number;
    return TRUE;
}

static gboolean absolute_clean(const char *path)
{
    if (path[0] != '/' || path[1] == 0 || strchr(path, '\\')) return FALSE;
    char **parts = g_strsplit(path + 1, "/", -1);
    gboolean ok = TRUE;
    for (gsize i = 0; parts[i]; ++i)
        if (!*parts[i] || !strcmp(parts[i], ".") || !strcmp(parts[i], "..")) ok = FALSE;
    g_strfreev(parts);
    return ok;
}

static gboolean server_path(const char *path)
{
    return absolute_clean(path) && !strstr(path, "..") &&
           !strpbrk(path, " ?#%:");
}

static gboolean base_url_valid(const char *url)
{
    const char *authority;
    if (g_str_has_prefix(url, "http://")) authority = url + 7;
    else if (g_str_has_prefix(url, "https://")) authority = url + 8;
    else return FALSE;
    if (!*authority || strpbrk(authority, "/@?#\\% ")) return FALSE;
    const char *port = NULL;
    if (*authority == '[') {
        const char *end = strchr(authority, ']');
        if (!end || (end[1] && end[1] != ':')) return FALSE;
        char *host = g_strndup(authority + 1, end - authority - 1);
        gboolean ok = strchr(host, ':') && g_hostname_is_ip_address(host);
        g_free(host);
        if (!ok) return FALSE;
        if (end[1]) port = end + 2;
    } else {
        port = strchr(authority, ':');
        gsize size = port ? (gsize)(port - authority) : strlen(authority);
        if (!size || size > 253) return FALSE;
        for (gsize i = 0; i < size; ++i)
            if (!g_ascii_isalnum(authority[i]) && authority[i] != '-' && authority[i] != '.')
                return FALSE;
        if (port) ++port;
    }
    if (port) {
        if (!*port || strlen(port) > 5) return FALSE;
        unsigned number = 0;
        for (; *port; ++port) {
            if (!g_ascii_isdigit(*port)) return FALSE;
            number = number * 10 + (unsigned)(*port - '0');
        }
        if (!number || number > 65535) return FALSE;
    }
    return TRUE;
}

static gboolean child_is(const char *dir, const char *path, const char *name)
{
    char *expected = g_build_filename(dir, name, NULL);
    gboolean ok = !strcmp(expected, path);
    g_free(expected);
    return ok;
}

gboolean ota_config_load(const char *path, OtaConfig *out, OtaError *error)
{
    char *data = NULL;
    gsize length;
    gboolean missing;
    OtaConfig config = {0};
    GKeyFile *file = g_key_file_new();
    gboolean ok = FALSE;
    if (!ota_file_read(path, OTA_STATE_MAX_BYTES, &data, &length, &missing, error) || missing)
        goto done;
    if (memchr(data, 0, length) || !g_utf8_validate(data, length, NULL) || !unique_entries(data) ||
        !g_key_file_load_from_data(file, data, length, G_KEY_FILE_NONE, NULL) || !known_entries(file))
        goto done;
#define STR(group, key, field, optional) \
    if (!read_string(file, group, key, config.field, sizeof(config.field), optional)) goto done
#define NUM(group, key, field, min, max) do { \
    uint64_t n; if (!read_number(file, group, key, min, max, &n)) goto done; config.field = n; \
} while (0)
    STR("server", "base_url", base_url, FALSE);
    STR("server", "manifest_path", manifest_path, FALSE);
    STR("server", "report_path", report_path, FALSE);
    NUM("server", "poll_interval_sec", poll_interval_sec, 1, UINT32_MAX);
    NUM("server", "connect_timeout_sec", connect_timeout_sec, 1, INT32_MAX);
    NUM("server", "request_timeout_sec", request_timeout_sec, 1, INT32_MAX);
    STR("agent", "state_dir", state_dir, FALSE);
    STR("agent", "release_file", release_file, FALSE);
    NUM("agent", "max_manifest_bytes", max_manifest_bytes, 1, 65536);
    STR("download", "part_file", part_file, FALSE);
    STR("download", "bundle_file", bundle_file, FALSE);
    NUM("download", "reserve_bytes", reserve_bytes, 0, INT64_MAX);
    STR("rauc", "binary", rauc_binary, FALSE);
    STR("identity", "device_id_file", device_id_file, FALSE);
    STR("health", "hook", health_hook, TRUE);
    NUM("health", "timeout_sec", health_timeout_sec, 1, INT32_MAX);
#undef STR
#undef NUM
    char mode[32];
    if (!read_string(file, "reporting", "mode", mode, sizeof(mode), TRUE)) goto done;
    if (!*mode && g_key_file_has_key(file, "reporting", "mode", NULL)) goto done;
    if (!*mode || !strcmp(mode, "legacy")) config.reporting_mode = OTA_REPORT_LEGACY;
    else if (!strcmp(mode, "extended")) config.reporting_mode = OTA_REPORT_EXTENDED;
    else goto done;
    if (!base_url_valid(config.base_url) || !server_path(config.manifest_path) ||
        !server_path(config.report_path) || !absolute_clean(config.state_dir) ||
        !absolute_clean(config.release_file) ||
        (*config.health_hook && !absolute_clean(config.health_hook)) ||
        strcmp(config.rauc_binary, "/usr/bin/rauc") ||
        config.connect_timeout_sec > config.request_timeout_sec ||
        !child_is(config.state_dir, config.part_file, "update.raucb.part") ||
        !child_is(config.state_dir, config.bundle_file, "update.raucb") ||
        !child_is(config.state_dir, config.device_id_file, "device-id")) goto done;
    *out = config;
    ok = TRUE;
done:
    if (!ok) ota_error_set(error, OTA_ERROR_CONFIG_INVALID,
                          "Invalid, missing, duplicate or unsupported INI value in %s", path);
    g_free(data);
    g_key_file_unref(file);
    return ok;
}
