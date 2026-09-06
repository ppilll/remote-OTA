#include "edgeguard_ota/state_machine.h"
#include "edgeguard_ota/identity.h"
#include <string.h>

typedef struct { OtaState from, to; } Transition;
static const Transition transitions[] = {
    {OTA_STATE_IDLE, OTA_STATE_CHECK_NETWORK},
    {OTA_STATE_CHECK_NETWORK, OTA_STATE_CHECK_UPDATE},
    {OTA_STATE_CHECK_UPDATE, OTA_STATE_PRECHECK},
    {OTA_STATE_CHECK_UPDATE, OTA_STATE_IDLE}, /* HTTP 204: no active release */
    {OTA_STATE_PRECHECK, OTA_STATE_IDLE}, /* same version AND same build */
    {OTA_STATE_PRECHECK, OTA_STATE_DOWNLOADING},
    {OTA_STATE_DOWNLOADING, OTA_STATE_VERIFY_DOWNLOAD},
    {OTA_STATE_VERIFY_DOWNLOAD, OTA_STATE_RAUC_VERIFY},
    {OTA_STATE_RAUC_VERIFY, OTA_STATE_INSTALLING},
    {OTA_STATE_INSTALLING, OTA_STATE_REBOOT_PENDING},
    {OTA_STATE_REBOOT_PENDING, OTA_STATE_BOOT_NEW_SLOT},
    {OTA_STATE_BOOT_NEW_SLOT, OTA_STATE_HEALTH_CHECK},
    {OTA_STATE_BOOT_NEW_SLOT, OTA_STATE_ROLLBACK},
    {OTA_STATE_HEALTH_CHECK, OTA_STATE_MARK_GOOD},
    {OTA_STATE_HEALTH_CHECK, OTA_STATE_ROLLBACK},
    {OTA_STATE_MARK_GOOD, OTA_STATE_REPORT_SUCCESS},
    {OTA_STATE_REPORT_SUCCESS, OTA_STATE_IDLE}
};

gboolean ota_transition_allowed(OtaState from, OtaState to)
{
    if (!ota_state_name(from) || !ota_state_name(to) || from == to) return FALSE;
    if (to == OTA_STATE_ERROR) return TRUE;
    for (gsize i = 0; i < G_N_ELEMENTS(transitions); ++i)
        if (transitions[i].from == from && transitions[i].to == to) return TRUE;
    return FALSE;
}

gboolean ota_state_machine_open(OtaStateMachine *machine, const char *path,
                                const OtaPersistenceOps *ops, OtaError *error)
{
    memset(machine, 0, sizeof(*machine));
    machine->blocked = TRUE;
    if (!path || strlen(path) >= sizeof(machine->path)) {
        ota_error_set(error, OTA_ERROR_PERSISTENCE_FAILED, "Invalid state file path");
        return FALSE;
    }
    g_strlcpy(machine->path, path, sizeof(machine->path));
    if (ops) machine->persistence = *ops;
    gboolean missing;
    if (!ota_persistence_load(path, &machine->current, &missing, error)) return FALSE;
    if (missing) {
        ota_persistent_state_init(&machine->current);
        if (!ota_persistence_save(path, &machine->current, &machine->persistence, error)) return FALSE;
    }
    machine->blocked = FALSE;
    return TRUE;
}

static gboolean same_attempt(const OtaPersistentState *a, const OtaPersistentState *b)
{
    return !strcmp(a->attempt_id, b->attempt_id) && !strcmp(a->target_version, b->target_version) &&
           !strcmp(a->build_id, b->build_id) && !strcmp(a->artifact_url, b->artifact_url) &&
           !strcmp(a->expected_sha256, b->expected_sha256) && a->expected_size == b->expected_size;
}

gboolean ota_state_machine_transition(OtaStateMachine *machine,
                                      const OtaPersistentState *next, OtaError *error)
{
    if (machine->blocked) {
        ota_error_set(error, OTA_ERROR_PERSISTENCE_FAILED, "State machine blocked; restart and inspect durable state");
        return FALSE;
    }
    if (!ota_transition_allowed(machine->current.state, next->state)) {
        ota_error_set(error, OTA_ERROR_ILLEGAL_TRANSITION, "Rejected transition %d -> %d",
                      machine->current.state, next->state);
        g_printerr("ILLEGAL_TRANSITION: %d -> %d\n", machine->current.state, next->state);
        return FALSE;
    }
    if (!ota_persistent_state_validate(next, error)) return FALSE;
    const OtaPersistentState *old = &machine->current;
    gboolean reset = next->state == OTA_STATE_IDLE;
    if ((!reset && *old->attempt_id && !same_attempt(old, next)) ||
        (!reset && old->previous_slot != OTA_SLOT_UNKNOWN &&
         (old->previous_slot != next->previous_slot ||
          old->expected_candidate_slot != next->expected_candidate_slot ||
          strcmp(old->boot_id_before, next->boot_id_before))) ||
        (next->reboot_context != old->reboot_context && !reset &&
         !(old->state == OTA_STATE_RAUC_VERIFY && next->state == OTA_STATE_INSTALLING &&
           next->reboot_context == OTA_REBOOT_INSTALL) &&
         !(old->state == OTA_STATE_HEALTH_CHECK && next->state == OTA_STATE_ROLLBACK &&
           next->reboot_context == OTA_REBOOT_HEALTH_FAILURE))) {
        ota_error_set(error, OTA_ERROR_ILLEGAL_TRANSITION, "Attempt or reboot identity changed mid-attempt");
        g_printerr("ILLEGAL_TRANSITION: attempt/reboot identity mutation\n");
        return FALSE;
    }
    if (!ota_persistence_save(machine->path, next, &machine->persistence, error)) {
        machine->blocked = TRUE;
        return FALSE;
    }
    machine->current = *next;
    return TRUE;
}

gboolean ota_state_machine_fail(OtaStateMachine *machine, OtaErrorCode code,
                                const char *message, OtaError *error)
{
    OtaPersistentState next = machine->current;
    next.state = OTA_STATE_ERROR;
    ota_error_set(&next.last_error, code, "%s", message);
    return ota_state_machine_transition(machine, &next, error);
}

static gboolean recover_error(OtaStateMachine *machine, OtaErrorCode code,
                              const char *message, OtaError *error)
{
    if (!ota_state_machine_fail(machine, code, message, error)) return FALSE;
    ota_error_set(error, code, "%s", message);
    return TRUE; /* recovery completed into durable ERROR; caller may report it */
}

gboolean ota_state_machine_recover(OtaStateMachine *machine, const char *boot_id_now,
                                   OtaCurrentSlot current_slot, void *user, OtaError *error)
{
    if (machine->blocked) {
        ota_error_set(error, OTA_ERROR_PERSISTENCE_FAILED, "Cannot recover blocked state machine");
        return FALSE;
    }
    OtaState state = machine->current.state;
    if (state == OTA_STATE_IDLE || state == OTA_STATE_ERROR || state == OTA_STATE_ROLLBACK ||
        state == OTA_STATE_CHECK_NETWORK || state == OTA_STATE_CHECK_UPDATE ||
        state == OTA_STATE_PRECHECK || state == OTA_STATE_DOWNLOADING ||
        state == OTA_STATE_VERIFY_DOWNLOAD || state == OTA_STATE_RAUC_VERIFY ||
        state == OTA_STATE_REPORT_SUCCESS)
        return TRUE; /* replay-safe work resumes from its durable phase */
    if (state != OTA_STATE_REBOOT_PENDING && state != OTA_STATE_BOOT_NEW_SLOT) {
        OtaErrorCode code = state == OTA_STATE_INSTALLING ? OTA_ERROR_RAUC_INSTALL_FAILED :
                            state == OTA_STATE_MARK_GOOD ? OTA_ERROR_RAUC_MARK_GOOD_FAILED :
                            state == OTA_STATE_HEALTH_CHECK ? OTA_ERROR_HEALTH_FAILED :
                            OTA_ERROR_REBOOT_CONTEXT_INVALID;
        return recover_error(machine, code,
                             "Interrupted mutating/uncertain phase; automatic replay is disabled",
                             error);
    }
    if (!ota_uuid_valid(boot_id_now, TRUE))
        return recover_error(machine, OTA_ERROR_REBOOT_CONTEXT_INVALID,
                             "Invalid current boot ID", error);
    if (!strcmp(boot_id_now, machine->current.boot_id_before)) {
        if (state == OTA_STATE_REBOOT_PENDING) return TRUE; /* persist/retry reboot, never install */
        return recover_error(machine, OTA_ERROR_REBOOT_CONTEXT_INVALID,
                             "BOOT_NEW_SLOT without a changed boot ID", error);
    }
    if (state == OTA_STATE_REBOOT_PENDING) {
        OtaPersistentState next = machine->current;
        next.state = OTA_STATE_BOOT_NEW_SLOT;
        if (!ota_state_machine_transition(machine, &next, error)) return FALSE;
    }
    OtaSlot slot = OTA_SLOT_UNKNOWN;
    if (!current_slot || !current_slot(user, &slot, error) ||
        (slot != OTA_SLOT_A && slot != OTA_SLOT_B))
        return recover_error(machine, OTA_ERROR_REBOOT_CONTEXT_INVALID,
                             "Current slot is unavailable or ambiguous", error);
    OtaPersistentState next = machine->current;
    if (slot == next.expected_candidate_slot) next.state = OTA_STATE_HEALTH_CHECK;
    else if (slot == next.previous_slot) {
        next.state = OTA_STATE_ROLLBACK;
        ota_error_set(&next.last_error, OTA_ERROR_HEALTH_FAILED,
                      "Candidate fell back to the previous known-good slot");
    }
    else return recover_error(machine, OTA_ERROR_REBOOT_CONTEXT_INVALID,
                              "Current slot disagrees with reboot context", error);
    return ota_state_machine_transition(machine, &next, error);
}

gboolean ota_state_machine_reboot(OtaStateMachine *machine, const OtaRebootOps *ops, OtaError *error)
{
    if (machine->blocked) {
        ota_error_set(error, OTA_ERROR_PERSISTENCE_FAILED, "Reboot prohibited after persistence failure");
        return FALSE;
    }
    OtaError failure = {0};
    if (ota_reboot_controlled(machine->path, &machine->current, &machine->persistence, ops, &failure))
        return TRUE;
    if (failure.code == OTA_ERROR_PERSISTENCE_FAILED) machine->blocked = TRUE;
    else if (failure.code == OTA_ERROR_REBOOT_FAILED) {
        if (machine->current.last_error.code == OTA_ERROR_NONE) {
            if (!ota_state_machine_fail(machine, failure.code, failure.message, error)) return FALSE;
        }
        /* A secondary reboot request failure must not erase a durable OTA cause. */
    }
    if (error) *error = failure;
    return FALSE;
}
