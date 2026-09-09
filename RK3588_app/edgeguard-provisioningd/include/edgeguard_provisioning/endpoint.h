#ifndef EDGEGUARD_PROVISIONING_ENDPOINT_H
#define EDGEGUARD_PROVISIONING_ENDPOINT_H

#include "model.h"

G_BEGIN_DECLS

/* Validates a complete UTF-8 R5 base URL and writes its canonical form. The
 * output is lowercase scheme/DNS host, bracketed IPv6, no trailing slash, and
 * no default port. input and output may alias. Raw rejected input is never
 * copied into error. */
gboolean egp_endpoint_canonicalize(const char *input,
                                   char output[EGP_ENDPOINT_MAX_BYTES + 1u],
                                   EgpError *error);

/* Strictly parses the complete runtime.json bytes: exact v1 members, no
 * duplicates/unknowns, exact integer range, and an already-canonical URL.
 * This function performs no file I/O, so the OTA Agent can combine it with its
 * own bounded O_NOFOLLOW read without acquiring the provisioning writer lock. */
gboolean egp_runtime_parse_json(const char *data, gsize length,
                                EgpRuntimeConfig *out, EgpError *error);

G_END_DECLS

#endif
