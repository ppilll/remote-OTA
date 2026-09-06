#ifndef EDGEGUARD_OTA_COMPATIBILITY_H
#define EDGEGUARD_OTA_COMPATIBILITY_H
#include "model.h"
/* TRUE + update=FALSE only for identical version AND build_id. */
gboolean ota_compatibility_precheck(const OtaRelease *local, const OtaManifest *remote,
                                   gboolean *update, OtaError *error);
/* Invoke only after successful RAUC signature/metadata inspection. This equality
 * gate does not replace rauc info or final rauc install verification. */
gboolean ota_compatibility_rauc(const OtaRelease *local, const char *bundle_compatible,
                              OtaError *error);
#endif
