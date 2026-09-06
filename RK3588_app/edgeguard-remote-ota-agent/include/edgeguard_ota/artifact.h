#ifndef EDGEGUARD_OTA_ARTIFACT_H
#define EDGEGUARD_OTA_ARTIFACT_H

#include <glib.h>

/* The single R3 manifest/persistence artifact-path contract. */
gboolean ota_artifact_path_valid(const char *path);

#endif
