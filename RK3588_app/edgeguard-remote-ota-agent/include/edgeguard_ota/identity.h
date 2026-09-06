#ifndef EDGEGUARD_OTA_IDENTITY_H
#define EDGEGUARD_OTA_IDENTITY_H
#include "persistence.h"
/* Canonical lowercase UUID; require_v4 additionally enforces version/variant. */
gboolean ota_uuid_valid(const char *value, gboolean require_v4);
typedef gboolean (*OtaUuidSource)(void *user, char out[OTA_UUID_CAP], OtaError *error);
gboolean ota_uuid_kernel(void *user, char out[OTA_UUID_CAP], OtaError *error);
gboolean ota_boot_id_read(char out[OTA_UUID_CAP], OtaError *error);
gboolean ota_identity_load_or_create(const char *path, OtaUuidSource source,
                                     void *user, const OtaPersistenceOps *ops,
                                     char out[OTA_UUID_CAP], OtaError *error);
#endif
