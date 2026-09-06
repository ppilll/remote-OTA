#define _GNU_SOURCE
#include "edgeguard_ota/health.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void ota_health_init(OtaHealth *health, const OtaRaucAdapter *rauc)
{
    *health = (OtaHealth){rauc, "/userdata", "/usr/libexec/rauc/edgeguard-rk-ab-backend",
        "/etc/rauc/system.conf", "/etc/rauc/keyring.pem", "", 30};
}

static gboolean regular_access(const char *path, int mode)
{
    struct stat st;
    return path && path[0] == '/' && stat(path, &st) == 0 &&
        S_ISREG(st.st_mode) && access(path, mode) == 0;
}

static gboolean userdata_writable(const char *path)
{
    struct stat st;
    if (!path || path[0] != '/' || stat(path, &st) < 0 || !S_ISDIR(st.st_mode) ||
        access(path, W_OK | X_OK) < 0) return FALSE;
    char *temporary = g_build_filename(path, ".edgeguard-health-XXXXXX", NULL);
    int fd = g_mkstemp_full(temporary, O_RDWR | O_CLOEXEC, 0600);
    gboolean ok = fd >= 0;
    if (fd >= 0) {
        ssize_t written;
        do { written = write(fd, "1", 1); } while (written < 0 && errno == EINTR);
        ok = written == 1 && fsync(fd) == 0;
        if (close(fd) < 0) ok = FALSE;
        if (unlink(temporary) < 0) ok = FALSE;
    }
    g_free(temporary); return ok;
}

gboolean ota_health_check(const OtaHealth *health, OtaSlot expected, OtaError *error)
{
    if (!health || !health->rauc || !health->rauc->run || !health->timeout_sec ||
        health->timeout_sec > UINT32_MAX / 1000 ||
        (expected != OTA_SLOT_A && expected != OTA_SLOT_B)) {
        ota_error_set(error, OTA_ERROR_HEALTH_FAILED, "Invalid health configuration/candidate"); return FALSE;
    }
    OtaRaucAdapter adapter = *health->rauc;
    adapter.query_timeout_ms = health->timeout_sec * 1000;
    OtaError cause = {0};
    OtaSlot current = OTA_SLOT_UNKNOWN;
    if (!ota_rauc_current_slot(&adapter, &current, &cause) || current != expected) {
        ota_error_set(error, OTA_ERROR_HEALTH_FAILED, "Candidate identity failed: %s",
                      cause.code ? cause.message : "unexpected current slot"); return FALSE;
    }
    if (!userdata_writable(health->userdata)) {
        ota_error_set(error, OTA_ERROR_HEALTH_FAILED, "userdata write/fsync probe failed"); return FALSE;
    }
    const char *executables[] = {adapter.binary, adapter.abctl, health->backend};
    for (gsize i = 0; i < G_N_ELEMENTS(executables); ++i) {
        if (!regular_access(executables[i], X_OK)) {
            ota_error_set(error, OTA_ERROR_HEALTH_FAILED, "Required executable unavailable: %s",
                          executables[i] ? executables[i] : "(unset)"); return FALSE;
        }
    }
    if (!regular_access(health->system_conf, R_OK) || !regular_access(health->keyring, R_OK)) {
        ota_error_set(error, OTA_ERROR_HEALTH_FAILED, "RAUC config or keyring unreadable"); return FALSE;
    }
    if (!ota_rauc_status(&adapter, error)) return FALSE;
    if (health->hook && *health->hook) {
        if (!regular_access(health->hook, X_OK)) {
            ota_error_set(error, OTA_ERROR_HEALTH_FAILED, "Health hook must be an absolute executable"); return FALSE;
        }
        const char *argv[] = {health->hook, NULL};
        GError *failure = NULL;
        char *output = NULL;
        gboolean ok = adapter.run(adapter.user, argv, adapter.query_timeout_ms, &output, &failure);
        g_free(output);
        if (!ok) ota_error_set(error, OTA_ERROR_HEALTH_FAILED, "Health hook failed: %s",
                               failure ? failure->message : "runner failure");
        g_clear_error(&failure);
        if (!ok) return FALSE;
    }
    return TRUE;
}
