#include "edgeguard_ota/state_machine.h"
#include "edgeguard_ota/identity.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static void stop_requested(int signal_number) { (void)signal_number; stopping = 1; }

#ifdef OTA_TEST_WEAK_SERVICES
__attribute__((weak))
gboolean ota_agent_services_init(OtaAgentServices *services, OtaError *error)
{
    (void)services;
    ota_error_set(error, OTA_ERROR_CONFIG_INVALID,
                  "Thread 2/3 service adapter is not linked; orchestration unavailable");
    return FALSE;
}
#endif

static int acquire_lock(const char *directory, OtaError *error)
{
    char *path = g_build_filename(directory, "agent.lock", NULL);
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    g_free(path);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || flock(fd, LOCK_EX | LOCK_NB) < 0) {
        if (fd >= 0) close(fd);
        ota_error_set(error, OTA_ERROR_PERSISTENCE_FAILED, "Agent lock unavailable; another agent may be running");
        return -1;
    }
    return fd;
}

static gboolean poll_wait(const OtaTimeSource *time, uint32_t seconds, OtaError *error)
{
    uint64_t deadline;
    if (!ota_deadline_after(time, (uint64_t)seconds * 1000, &deadline, error)) return FALSE;
    while (!stopping) {
        gboolean expired;
        if (!ota_deadline_expired(time, deadline, &expired, error)) return FALSE;
        if (expired) return TRUE;
        struct timespec pause = {0, 100000000L};
        if (nanosleep(&pause, NULL) < 0 && errno != EINTR) {
            ota_error_set(error, OTA_ERROR_TIME_SOURCE_FAILED, "Poll wait failed");
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean phase_may_retry(OtaState state)
{
    return state == OTA_STATE_CHECK_UPDATE || state == OTA_STATE_DOWNLOADING ||
           state == OTA_STATE_VERIFY_DOWNLOAD || state == OTA_STATE_RAUC_VERIFY ||
           state == OTA_STATE_REPORT_SUCCESS || state == OTA_STATE_ERROR ||
           state == OTA_STATE_ROLLBACK;
}

/* This skeleton owns ordering and durability. Service callbacks own the frozen
 * HTTP/version/download/RAUC/health/report operations, with typed results. */
static int run(OtaStateMachine *machine, const OtaConfig *config,
               const OtaRelease *release, const char *device_id,
               OtaAgentServices *services, OtaError *error)
{
    gboolean first_poll = TRUE;
    while (!stopping) {
        OtaState state = machine->current.state;
        if (state == OTA_STATE_REBOOT_PENDING)
            return ota_state_machine_reboot(machine, &services->reboot, error) ? 0 : 1;
        OtaPersistentState next = machine->current;
        if (state == OTA_STATE_IDLE) {
            if (!first_poll && !poll_wait(&services->time, config->poll_interval_sec, error)) return 1;
            if (stopping) break;
            first_poll = FALSE;
            next.state = OTA_STATE_CHECK_NETWORK;
        } else {
            OtaError phase_error = {0};
            gboolean terminal = state == OTA_STATE_ERROR || state == OTA_STATE_ROLLBACK;
            if (state == OTA_STATE_MARK_GOOD) {
                OtaSlot current = OTA_SLOT_UNKNOWN;
                if (!services->current_slot(services->user, &current, &phase_error) ||
                    current != machine->current.expected_candidate_slot) {
                    ota_error_set(&phase_error, OTA_ERROR_IDENTITY_AMBIGUOUS,
                                  "Current slot is not the expected candidate before mark-good");
                    if (!ota_state_machine_fail(machine, phase_error.code, phase_error.message, error)) return 1;
                    continue;
                }
            }
            OtaPhaseResult phase_result = services->phase(services->user, config, release,
                                                          device_id, &machine->current,
                                                          &next, &phase_error);
            if (phase_result == OTA_PHASE_RETRY) {
                if (!phase_may_retry(state)) {
                    ota_error_set(&phase_error, OTA_ERROR_ILLEGAL_TRANSITION,
                                  "State %s is not replay-safe", ota_state_name(state));
                    phase_result = OTA_PHASE_HARD_FAILURE;
                } else {
                    if (!poll_wait(&services->time, config->poll_interval_sec, error)) return 1;
                    continue;
                }
            }
            if (phase_result != OTA_PHASE_ADVANCE) {
                if (phase_error.code == OTA_ERROR_NONE)
                    ota_error_set(&phase_error, OTA_ERROR_ILLEGAL_TRANSITION, "Phase failed without a typed error");
                if (terminal) {
                    /* Reporting cannot mutate or replace the durable OTA cause. */
                    if (machine->current.last_error.code != OTA_ERROR_NONE)
                        *error = machine->current.last_error;
                    else *error = phase_error;
                    return 2;
                }
                if (state == OTA_STATE_REPORT_SUCCESS) {
                    *error = phase_error; /* keep durable report-pending state */
                    return 1;
                }
                if (!ota_state_machine_fail(machine, phase_error.code, phase_error.message, error)) return 1;
                continue; /* make terminal ERROR reporting reachable */
            }
            if (terminal) {
                if (machine->current.last_error.code != OTA_ERROR_NONE)
                    *error = machine->current.last_error;
                return 2; /* telemetry succeeded; terminal OTA state remains */
            }
            if (state == OTA_STATE_CHECK_UPDATE && next.state == OTA_STATE_PRECHECK &&
                !ota_uuid_kernel(NULL, next.attempt_id, error)) return 1;
            if (state == OTA_STATE_RAUC_VERIFY && next.state == OTA_STATE_INSTALLING) {
                OtaSlot current = OTA_SLOT_UNKNOWN;
                if (!services->current_slot(services->user, &current, error) ||
                    (current != OTA_SLOT_A && current != OTA_SLOT_B) ||
                    !ota_boot_id_read(next.boot_id_before, error)) {
                    OtaError cause = {0};
                    ota_error_set(&cause, OTA_ERROR_REBOOT_CONTEXT_INVALID,
                                  "Cannot rigorously capture pre-install slot and boot identity");
                    if (!ota_state_machine_fail(machine, cause.code, cause.message, error)) return 1;
                    continue;
                }
                next.previous_slot = current;
                next.expected_candidate_slot = current == OTA_SLOT_A ? OTA_SLOT_B : OTA_SLOT_A;
                next.reboot_context = OTA_REBOOT_INSTALL;
            }
            if (state == OTA_STATE_HEALTH_CHECK && next.state == OTA_STATE_ROLLBACK) {
                next.reboot_context = OTA_REBOOT_HEALTH_FAILURE; /* phase must have marked bad */
                if (next.last_error.code == OTA_ERROR_NONE)
                    ota_error_set(&next.last_error, OTA_ERROR_HEALTH_FAILED, "Candidate failed health checks; mark-bad succeeded");
            }
            if (next.state == OTA_STATE_IDLE) ota_persistent_state_init(&next);
        }
        if (!ota_state_machine_transition(machine, &next, error)) return 1;
        if (machine->current.state == OTA_STATE_ROLLBACK &&
            machine->current.reboot_context == OTA_REBOOT_HEALTH_FAILURE)
            return ota_state_machine_reboot(machine, &services->reboot, error) ? 0 : 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *config_path = "/etc/edgeguard-ota/agent.conf";
    if (argc == 3 && !strcmp(argv[1], "--config")) config_path = argv[2];
    else if (argc != 1) { g_printerr("Usage: %s [--config /absolute/agent.conf]\n", argv[0]); return 2; }
    OtaError error = {0};
    OtaConfig config;
    OtaRelease release = {0};
    OtaAgentServices services = {0};
    OtaStateMachine machine;
    char device_id[OTA_UUID_CAP], boot_id[OTA_UUID_CAP];
    char *state_path = NULL;
    int lock = -1, result = 1;
    if (!ota_config_load(config_path, &config, &error) ||
        !ota_storage_prepare(config.state_dir, &error)) goto done;
    lock = acquire_lock(config.state_dir, &error);
    if (lock < 0) goto done;
    if (!ota_identity_load_or_create(config.device_id_file, NULL, NULL, NULL, device_id, &error)) goto done;
    state_path = g_build_filename(config.state_dir, "agent-state.json", NULL);
    if (!ota_state_machine_open(&machine, state_path, NULL, &error) ||
        !ota_agent_services_init(&services, &error)) goto done;
    if (!services.load_release || !services.phase || !services.current_slot) {
        ota_error_set(&error, OTA_ERROR_CONFIG_INVALID, "Incomplete service adapter");
        goto done;
    }
    if (!services.load_release(services.user, config.release_file, &release, &error)) {
        ota_error_set(&error, OTA_ERROR_LOCAL_RELEASE_INVALID, "Missing or invalid local release identity");
        goto done;
    }
    if (!ota_boot_id_read(boot_id, &error) ||
        !ota_state_machine_recover(&machine, boot_id, services.current_slot, services.user, &error)) goto done;
    if (machine.current.state == OTA_STATE_ROLLBACK &&
        machine.current.reboot_context == OTA_REBOOT_HEALTH_FAILURE) {
        OtaSlot current = OTA_SLOT_UNKNOWN;
        if (!services.current_slot(services.user, &current, &error) ||
            (current != OTA_SLOT_A && current != OTA_SLOT_B)) {
            ota_error_set(&error, OTA_ERROR_REBOOT_CONTEXT_INVALID, "Cannot resolve persisted health rollback");
            goto done;
        }
        /* Recover a crash between durable mark-bad context and reboot request.
         * Once fallback is observed, never reboot the previous healthy slot. */
        if (current == machine.current.expected_candidate_slot) {
            result = ota_state_machine_reboot(&machine, &services.reboot, &error) ? 0 : 1;
            goto done;
        }
        if (current != machine.current.previous_slot) {
            ota_error_set(&error, OTA_ERROR_REBOOT_CONTEXT_INVALID,
                          "Persisted rollback slot identity is inconsistent");
            goto done;
        }
        /* Previous good slot is already active: report, never reboot it. */
    }
    struct sigaction action = {0};
    action.sa_handler = stop_requested;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    result = run(&machine, &config, &release, device_id, &services, &error);
    if (result == 2 && machine.current.last_error.code != OTA_ERROR_NONE) error = machine.current.last_error;
done:
    if (result && error.code != OTA_ERROR_NONE)
        g_printerr("%s: %s\n", ota_error_code_name(error.code), error.message);
    if (lock >= 0) close(lock);
    g_free(state_path);
    return result;
}
