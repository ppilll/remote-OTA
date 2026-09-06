#include "edgeguard_ota/version.h"

gboolean ota_version_parse(const char *text, OtaVersion *out, OtaError *error)
{
    uint32_t parts[3] = {0};
    const char *p = text;
    if (!p || !out) goto invalid;
    for (unsigned i = 0; i < 3; ++i) {
        if (*p < '0' || *p > '9' || (*p == '0' && p[1] >= '0' && p[1] <= '9'))
            goto invalid;
        while (*p >= '0' && *p <= '9') {
            unsigned digit = (unsigned)(*p++ - '0');
            if (parts[i] > (UINT32_MAX - digit) / 10) goto invalid;
            parts[i] = parts[i] * 10 + digit;
        }
        if (i < 2) { if (*p++ != '.') goto invalid; }
        else if (*p) goto invalid;
    }
    *out = (OtaVersion){parts[0], parts[1], parts[2]};
    return TRUE;
invalid:
    ota_error_set(error, OTA_ERROR_VERSION_MALFORMED, "Version must be three canonical uint32 decimal components");
    return FALSE;
}

int ota_version_compare(const OtaVersion *left, const OtaVersion *right)
{
#define CMP(field) if (left->field != right->field) return left->field > right->field ? 1 : -1
    CMP(major); CMP(minor); CMP(patch);
#undef CMP
    return 0;
}
