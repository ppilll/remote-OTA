#ifndef EDGEGUARD_OTA_DOWNLOAD_H
#define EDGEGUARD_OTA_DOWNLOAD_H
#include "config.h"
/* Pure response gate used by production headers and deterministic tests. */
typedef enum { OTA_DOWNLOAD_REJECT, OTA_DOWNLOAD_WRITE_ZERO,
               OTA_DOWNLOAD_APPEND, OTA_DOWNLOAD_RESTART, OTA_DOWNLOAD_VERIFY } OtaDownloadAction;
OtaDownloadAction ota_download_response(uint64_t partial, uint64_t expected, long status,
                                        const char *content_range, OtaError *error);
/* Caller holds the Agent lock and associates .part with the persisted manifest.
 * Call again only with the same attempt metadata; discard stale .part when changing
 * releases. Success means size/hash checked, file fsynced, renamed and directory
 * fsynced. The orchestrator MUST still perform RAUC signature/compatible checks.
 * No automatic transport retry; one bounded recovery GET is permitted for 416. */
gboolean ota_download_bundle(const OtaConfig *config, const OtaManifest *manifest,
                             gboolean *retryable, OtaError *error);
/* Revalidate a completed bundle after a crash between durable publication and
 * state advancement. No network or mutation is performed. */
gboolean ota_download_validate_bundle(const OtaConfig *config,
                                      const OtaManifest *manifest, OtaError *error);
/* Called only from PRECHECK for a newly persisted attempt, before DOWNLOADING. */
gboolean ota_download_discard_partial(const OtaConfig *config, OtaError *error);
#endif
