#include "edgeguard_ota/runtime_endpoint.h"
#include "edgeguard_provisioning/endpoint.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void endpoint_diagnostic(OtaError *error, const char *message)
{
    ota_error_set(error, OTA_ERROR_ENDPOINT_CONFIG_INVALID, "%s", message);
}

static gboolean read_runtime(char **data, gsize *length, gboolean *missing,
                             OtaError *diagnostic)
{
    *data = NULL;
    *length = 0;
    *missing = FALSE;
    int fd = open(OTA_RUNTIME_ENDPOINT_PATH,
                  O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT) {
            *missing = TRUE;
            return TRUE;
        }
        endpoint_diagnostic(diagnostic, "Runtime endpoint override could not be opened");
        return TRUE;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
        (st.st_mode & 0777) != 0600 || st.st_size <= 0 ||
        (guint64)st.st_size > EGP_STORE_MAX_BYTES) {
        close(fd);
        endpoint_diagnostic(diagnostic, "Runtime endpoint override metadata is invalid");
        return TRUE;
    }
    char *buffer = g_malloc(EGP_STORE_MAX_BYTES + 1u);
    gsize used = 0;
    gboolean valid = TRUE;
    while (used <= EGP_STORE_MAX_BYTES) {
        ssize_t count = read(fd, buffer + used, EGP_STORE_MAX_BYTES + 1u - used);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            valid = FALSE;
            break;
        }
        if (count == 0)
            break;
        used += (gsize)count;
        if (used > EGP_STORE_MAX_BYTES) {
            valid = FALSE;
            break;
        }
    }
    if (close(fd) < 0)
        valid = FALSE;
    if (!valid) {
        memset(buffer, 0, EGP_STORE_MAX_BYTES + 1u);
        g_free(buffer);
        endpoint_diagnostic(diagnostic, "Runtime endpoint override read failed");
        return TRUE;
    }
    buffer[used] = '\0';
    *data = buffer;
    *length = used;
    return TRUE;
}

gboolean ota_runtime_endpoint_refresh(const OtaConfig *immutable_config,
                                      OtaConfig *cycle_config,
                                      gboolean *override_active,
                                      OtaError *diagnostic)
{
    if (!immutable_config || !cycle_config || !override_active || !diagnostic) {
        ota_error_set(diagnostic, OTA_ERROR_CONFIG_INVALID,
                      "Runtime endpoint refresh arguments are invalid");
        return FALSE;
    }
    memset(diagnostic, 0, sizeof(*diagnostic));
    *cycle_config = *immutable_config;
    *override_active = FALSE;

    char canonical[EGP_ENDPOINT_MAX_BYTES + 1u] = {0};
    EgpError endpoint_error = {0};
    if (!egp_endpoint_canonicalize(immutable_config->base_url, canonical,
                                   &endpoint_error) ||
        strcmp(canonical, immutable_config->base_url)) {
        ota_error_set(diagnostic, OTA_ERROR_CONFIG_INVALID,
                      "Immutable endpoint default is invalid or non-canonical");
        return FALSE;
    }

    char *data = NULL;
    gsize length = 0;
    gboolean missing = FALSE;
    if (!read_runtime(&data, &length, &missing, diagnostic))
        return FALSE;
    if (missing || diagnostic->code != OTA_ERROR_NONE)
        return TRUE;

    EgpRuntimeConfig runtime = {0};
    if (!egp_runtime_parse_json(data, length, &runtime, &endpoint_error)) {
        endpoint_diagnostic(diagnostic, "Runtime endpoint override is invalid; immutable default selected");
        memset(data, 0, length);
        g_free(data);
        return TRUE;
    }
    memset(data, 0, length);
    g_free(data);
    if (strlen(runtime.ota_server_base_url) >= sizeof(cycle_config->base_url)) {
        endpoint_diagnostic(diagnostic, "Runtime endpoint override exceeds Agent capacity");
        return TRUE;
    }
    g_strlcpy(cycle_config->base_url, runtime.ota_server_base_url,
              sizeof(cycle_config->base_url));
    *override_active = TRUE;
    memset(&runtime, 0, sizeof(runtime));
    return TRUE;
}
