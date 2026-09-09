#ifndef EDGEGUARD_PROVISIONING_STORE_H
#define EDGEGUARD_PROVISIONING_STORE_H

#include "model.h"

G_BEGIN_DECLS

typedef struct _EgpStore EgpStore;

/* Store calls serialize individual reads/commits. The Thread 2 orchestrator
 * owns serialization across the wider state-check -> commit -> ConnMan apply ->
 * result transaction; the store does not lock or modify OTA Agent state. */

/* Opens one exclusive process lock in directory. Production accepts NULL or
 * EGP_STORE_DIRECTORY only; EGP_ALLOW_TEST_PATHS permits an absolute fixture
 * directory in backend tests. The returned store is the sole writer. */
gboolean egp_store_open(const char *directory, EgpStore **out, EgpError *error);
void egp_store_close(EgpStore *store);

/* Loads immutable validated snapshots. present is false only for ENOENT.
 * Callers own returned values; Wi-Fi snapshots must be cleared. */
gboolean egp_store_load_wifi(EgpStore *store, EgpWifiConfig *out,
                             gboolean *present, EgpError *error);
gboolean egp_store_load_runtime(EgpStore *store, EgpRuntimeConfig *out,
                                gboolean *present, EgpError *error);

/* Atomically commits a replacement and assigns previous_generation + 1 (or 1
 * for the first value). previous receives the validated pre-commit snapshot.
 * Invalid input or failure before rename leaves the old value authoritative;
 * a post-rename directory-fsync failure is reported as uncertain. */
gboolean egp_store_replace_wifi(EgpStore *store, const EgpWifiConfig *candidate,
                                EgpWifiConfig *previous, gboolean *previous_present,
                                guint64 *committed_generation, EgpError *error);
gboolean egp_store_replace_runtime(EgpStore *store, const char *candidate_url,
                                   EgpRuntimeConfig *previous, gboolean *previous_present,
                                   EgpRuntimeConfig *committed, EgpError *error);

/* Used only after a deterministic ConnMan apply failure. The supplied previous
 * snapshot is restored with a new generation; previous_present=false restores
 * canonical absence. This never applies or revokes ConnMan by itself. */
gboolean egp_store_restore_wifi(EgpStore *store, const EgpWifiConfig *previous,
                                gboolean previous_present, guint64 *restored_generation,
                                EgpError *error);

/* Forget is a two-phase durable tombstone. begin returns the credential solely
 * so ConnMan can identify/revoke the owned service. On restart, pending returns
 * the tombstoned snapshot; it must never be re-applied. finish is called only
 * after derived-profile revocation succeeds (including observed absence). A
 * dependency/error outcome retains the tombstone for bounded restart retry. */
gboolean egp_store_forget_begin(EgpStore *store, EgpWifiConfig *forgotten,
                                gboolean *had_wifi, EgpError *error);
gboolean egp_store_forget_pending(EgpStore *store, EgpWifiConfig *forgotten,
                                  gboolean *pending, EgpError *error);
gboolean egp_store_forget_finish(EgpStore *store, EgpError *error);

G_END_DECLS

#endif
