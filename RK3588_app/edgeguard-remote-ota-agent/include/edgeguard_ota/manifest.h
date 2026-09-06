#ifndef EDGEGUARD_OTA_MANIFEST_H
#define EDGEGUARD_OTA_MANIFEST_H
#include "config.h"
#include <curl/curl.h>

#define OTA_MANIFEST_MAX_BYTES 65536U
/* FALSE is an error. TRUE + available=FALSE means HTTP 204, not current. */
gboolean ota_manifest_fetch(const OtaConfig *config, OtaManifest *out,
                            gboolean *available, gboolean *retryable, OtaError *error);
gboolean ota_manifest_parse(const char *data, gsize length, OtaManifest *out, OtaError *error);
gboolean ota_manifest_validate(const OtaManifest *manifest, OtaError *error);
gboolean ota_release_parse(const char *data, gsize length, OtaRelease *out, OtaError *error);
gboolean ota_release_load(const char *path, OtaRelease *out, OtaError *error);
gboolean ota_release_validate(const OtaRelease *release, OtaError *error);
gboolean ota_artifact_path_valid(const char *path);

/* Shared transport plumbing, owned by Thread 2. Config must have passed
 * ota_config_load. No redirects/proxies: keep requests on the configured origin.
 * Caller owns the easy handle. No network-management side effects. */
CURL *ota_http_open(const OtaConfig *config, const char *path, OtaErrorCode code, OtaError *error);
gboolean ota_http_retryable(CURLcode result, long status);
#endif
