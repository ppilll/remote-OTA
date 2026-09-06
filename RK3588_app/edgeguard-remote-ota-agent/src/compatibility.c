#include "edgeguard_ota/compatibility.h"
#include "edgeguard_ota/manifest.h"
#include "edgeguard_ota/version.h"
#include <string.h>

gboolean ota_compatibility_precheck(const OtaRelease *local, const OtaManifest *remote,
                                   gboolean *update, OtaError *error)
{
    if (update) *update = FALSE;
    if (!update || !ota_release_validate(local, error) || !ota_manifest_validate(remote, error))
        return FALSE;
    if (strcmp(local->device_compatible, remote->device_compatible)) {
        ota_error_set(error, OTA_ERROR_DEVICE_COMPAT_MISMATCH, "Manifest device compatibility differs from local release");
        return FALSE;
    }
    OtaVersion l, r;
    if (!ota_version_parse(local->version, &l, error) || !ota_version_parse(remote->version, &r, error))
        return FALSE;
    int order = ota_version_compare(&r, &l);
    if (order < 0) {
        ota_error_set(error, OTA_ERROR_DOWNGRADE_REJECTED, "R3 does not permit downgrades");
        return FALSE;
    }
    if (!order && strcmp(local->build_id, remote->build_id)) {
        ota_error_set(error, OTA_ERROR_VERSION_COLLISION, "Equal versions have different build IDs");
        return FALSE;
    }
    *update = order > 0;
    return TRUE;
}

gboolean ota_compatibility_rauc(const OtaRelease *local, const char *bundle_compatible, OtaError *error)
{
    if (!ota_release_validate(local, error)) return FALSE;
    if (!bundle_compatible || !*bundle_compatible || strcmp(local->rauc_compatible, bundle_compatible)) {
        ota_error_set(error, OTA_ERROR_RAUC_COMPAT_MISMATCH, "Bundle RAUC compatibility differs from local release");
        return FALSE;
    }
    return TRUE;
}
