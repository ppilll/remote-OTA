#ifndef EDGEGUARD_OTA_VERSION_H
#define EDGEGUARD_OTA_VERSION_H
#include "model.h"
/* Components use the uint32_t limits defined by Thread 1's OtaVersion. */
gboolean ota_version_parse(const char *text, OtaVersion *out, OtaError *error);
int ota_version_compare(const OtaVersion *left, const OtaVersion *right);
#endif
