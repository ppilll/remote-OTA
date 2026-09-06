#ifndef EDGEGUARD_OTA_STATE_MACHINE_H
#define EDGEGUARD_OTA_STATE_MACHINE_H
#include "config.h"
#include "reboot.h"
#include "time_source.h"

typedef enum {
    OTA_PHASE_ADVANCE,
    OTA_PHASE_RETRY,
    OTA_PHASE_HARD_FAILURE
} OtaPhaseResult;

typedef struct {
    OtaPersistentState current;
    char path[OTA_PATH_CAP];
    OtaPersistenceOps persistence;
    gboolean blocked; /* any ambiguous durability outcome requires restart */
} OtaStateMachine;

gboolean ota_transition_allowed(OtaState from, OtaState to);
gboolean ota_state_machine_open(OtaStateMachine *machine, const char *path,
                                const OtaPersistenceOps *ops, OtaError *error);
/* Caller supplies a full proposed snapshot; memory changes only after durability.
 * Same-state changes are forbidden. ERROR/ROLLBACK are terminal for this attempt. */
gboolean ota_state_machine_transition(OtaStateMachine *machine,
                                      const OtaPersistentState *next, OtaError *error);
gboolean ota_state_machine_fail(OtaStateMachine *machine, OtaErrorCode code,
                                const char *message, OtaError *error);
/* Slot callback MUST wrap /usr/bin/edgeguard-rk-abctl get-current (Thread 3).
 * No slot guessing or direct metadata reads are permitted. */
typedef gboolean (*OtaCurrentSlot)(void *user, OtaSlot *out, OtaError *error);
gboolean ota_state_machine_recover(OtaStateMachine *machine, const char *boot_id_now,
                                   OtaCurrentSlot current_slot, void *user,
                                   OtaError *error);
gboolean ota_state_machine_reboot(OtaStateMachine *machine,
                                  const OtaRebootOps *ops, OtaError *error);

/* Merge adapter contract for the main skeleton; no HTTP/RAUC implementation here.
 * phase executes ONLY the action belonging to current.state, returns proposed next.
 * It must enforce the frozen gates (including last-second current-slot checks).
 * INSTALLING is durably recorded before phase may run rauc install.
 * HEALTH_CHECK -> ROLLBACK is permitted only after successful mark-bad.
 * REPORT_SUCCESS must finish reporting before IDLE. Do not retry install in phase.
 * load_release must strictly validate the local JSON identity before any phase. */
typedef struct {
    void *user;
    gboolean (*load_release)(void *user, const char *path, OtaRelease *out, OtaError *error);
    OtaPhaseResult (*phase)(void *user, const OtaConfig *config,
                           const OtaRelease *release, const char *device_id,
                           const OtaPersistentState *current,
                           OtaPersistentState *next, OtaError *error);
    OtaCurrentSlot current_slot;
    OtaRebootOps reboot;
    OtaTimeSource time;
} OtaAgentServices;
/* Strong production definition is in services.c. Omitting it must fail to link. */
gboolean ota_agent_services_init(OtaAgentServices *services, OtaError *error);
#endif
