#include "edgeguard_ota/identity.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

gboolean ota_uuid_valid(const char *value, gboolean require_v4)
{
    if (!value || strnlen(value, OTA_UUID_CAP) != 36) return FALSE;
    for (unsigned i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return FALSE;
        } else if (!g_ascii_isdigit(value[i]) && (value[i] < 'a' || value[i] > 'f')) return FALSE;
    }
    /* Kernel boot IDs also use RFC 4122 variant, version 4. */
    if (require_v4 && (value[14] != '4' || !strchr("89ab", value[19]))) return FALSE;
    return TRUE;
}

static gboolean uuid_from_file(const char *path, gboolean v4, char out[OTA_UUID_CAP],
                               gboolean *missing, OtaError *error)
{
    char *data;
    gsize length;
    if (!ota_file_read(path, 37, &data, &length, missing, error)) return FALSE;
    if (*missing) return TRUE;
    if (length == 37 && data[36] == '\n') data[--length] = 0;
    gboolean valid = length == 36 && !memchr(data, 0, length) && ota_uuid_valid(data, v4);
    if (valid) memcpy(out, data, OTA_UUID_CAP);
    else ota_error_set(error, OTA_ERROR_IDENTITY_INVALID, "Invalid UUID in %s", path);
    g_free(data);
    return valid;
}

gboolean ota_uuid_kernel(void *user, char out[OTA_UUID_CAP], OtaError *error)
{
    (void)user;
    gboolean missing;
    if (!uuid_from_file("/proc/sys/kernel/random/uuid", TRUE, out, &missing, error) || missing) {
        ota_error_set(error, OTA_ERROR_IDENTITY_INVALID, "Kernel UUIDv4 source unavailable or invalid");
        return FALSE;
    }
    return TRUE;
}

gboolean ota_boot_id_read(char out[OTA_UUID_CAP], OtaError *error)
{
    gboolean missing;
    if (!uuid_from_file("/proc/sys/kernel/random/boot_id", TRUE, out, &missing, error) || missing) {
        ota_error_set(error, OTA_ERROR_REBOOT_CONTEXT_INVALID, "Kernel boot ID unavailable or invalid");
        return FALSE;
    }
    return TRUE;
}

gboolean ota_identity_load_or_create(const char *path, OtaUuidSource source,
                                     void *user, const OtaPersistenceOps *ops,
                                     char out[OTA_UUID_CAP], OtaError *error)
{
    /* Serialize first-boot creators across processes. Main also holds agent.lock. */
    char *lock_path = g_strconcat(path, ".lock", NULL);
    int lock = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    g_free(lock_path);
    struct stat st;
    if (lock < 0 || fstat(lock, &st) < 0 || !S_ISREG(st.st_mode) || flock(lock, LOCK_EX) < 0) {
        if (lock >= 0) close(lock);
        ota_error_set(error, OTA_ERROR_PERSISTENCE_FAILED, "Cannot lock device identity");
        return FALSE;
    }
    gboolean missing;
    char value[OTA_UUID_CAP] = {0};
    gboolean ok = uuid_from_file(path, TRUE, value, &missing, error);
    if (ok && missing) {
        if (!source) source = ota_uuid_kernel;
        ok = source(user, value, error);
        if (ok && !ota_uuid_valid(value, TRUE)) {
            ota_error_set(error, OTA_ERROR_IDENTITY_INVALID, "UUID source did not return canonical UUIDv4");
            ok = FALSE;
        }
        if (ok) {
            char line[OTA_UUID_CAP + 1];
            g_snprintf(line, sizeof(line), "%s\n", value);
            ok = ota_atomic_write(path, line, 37, ops, error);
        }
    }
    if (ok) memcpy(out, value, sizeof(value));
    flock(lock, LOCK_UN);
    close(lock);
    return ok;
}
