#include "edgeguard_ota/artifact.h"
#include "edgeguard_ota/model.h"
#include <string.h>

gboolean ota_artifact_path_valid(const char *path)
{
    if (!path || path[0] != '/' || !path[1] || path[1] == '/' ||
        strlen(path) >= OTA_PATH_CAP || !g_utf8_validate(path, -1, NULL) ||
        strstr(path, "..") || strpbrk(path, "\\%:?# "))
        return FALSE;
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p)
        if (*p < 0x20 || *p == 0x7f) return FALSE;
    return TRUE;
}
