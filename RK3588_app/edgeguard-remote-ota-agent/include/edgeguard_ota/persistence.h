#ifndef EDGEGUARD_OTA_PERSISTENCE_H
#define EDGEGUARD_OTA_PERSISTENCE_H
#include "model.h"

/* Fault/ordering seam; NULL in production. Called immediately before each step.
 * Returning false injects failure. Never use this hook for real disk I/O. */
typedef enum {
    OTA_IO_WRITE, OTA_IO_FILE_FSYNC, OTA_IO_RENAME, OTA_IO_PARENT_FSYNC
} OtaIoStep;
typedef gboolean (*OtaIoHook)(void *user, OtaIoStep step);
typedef struct { OtaIoHook before; void *user; } OtaPersistenceOps;

gboolean ota_storage_prepare(const char *directory, OtaError *error);
/* Read regular files only, bounded, no symlink following at the final component.
 * ENOENT alone sets missing; no other read failure is treated as first boot. */
gboolean ota_file_read(const char *path, gsize limit, char **data, gsize *length,
                       gboolean *missing, OtaError *error);
gboolean ota_atomic_write(const char *path, const void *data, gsize length,
                          const OtaPersistenceOps *ops, OtaError *error);
void ota_persistent_state_init(OtaPersistentState *state);
const char *ota_state_name(OtaState state);
gboolean ota_persistent_state_validate(const OtaPersistentState *state, OtaError *error);
gboolean ota_persistence_load(const char *path, OtaPersistentState *out,
                              gboolean *missing, OtaError *error);
gboolean ota_persistence_save(const char *path, const OtaPersistentState *state,
                              const OtaPersistenceOps *ops, OtaError *error);
#endif
