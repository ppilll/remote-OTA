#define main ota_integration_unused_main
#include "../src/main.c"
#undef main
#include "edgeguard_ota/services.h"
#include "edgeguard_ota/manifest.h"
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

static const char *uuid_a = "12345678-1234-4123-8123-123456789abc";
static const char *uuid_b = "87654321-4321-4321-9321-cba987654321";

typedef struct {
    char *dir, *state_path;
    OtaConfig config;
    OtaRelease local, candidate;
    OtaManifest manifest;
    OtaStateMachine machine;
    OtaAgentServiceContext context;
    OtaAgentServices services;
    const char *slot;
    const char *signed_compatible, *signed_version, *signed_build;
    const char *runner_fail;
    int manifest_mode, manifest_calls;
    int download_failures, download_calls;
    OtaErrorCode download_error;
    gboolean download_retryable;
    gboolean validate_ok, health_ok;
    int report_failures, report_calls;
    gboolean report_retryable, stop_on_report;
    int info_calls, install_calls, mark_good_calls, mark_bad_calls, reboot_calls;
    uint64_t clock_ms;
} Matrix;

static Matrix *active;

static OtaPersistentState snapshot(OtaState state)
{
    OtaPersistentState value;
    ota_persistent_state_init(&value);
    value.state = state;
    if (state >= OTA_STATE_PRECHECK) {
        g_strlcpy(value.attempt_id, uuid_a, sizeof(value.attempt_id));
        g_strlcpy(value.target_version, "2.0.0", sizeof(value.target_version));
        g_strlcpy(value.build_id, "candidate", sizeof(value.build_id));
        g_strlcpy(value.artifact_url, "/update.raucb", sizeof(value.artifact_url));
        value.expected_size = 3;
        g_strlcpy(value.expected_sha256,
                  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                  sizeof(value.expected_sha256));
    }
    if (state >= OTA_STATE_INSTALLING) {
        value.previous_slot = OTA_SLOT_A;
        value.expected_candidate_slot = OTA_SLOT_B;
        g_strlcpy(value.boot_id_before, uuid_a, sizeof(value.boot_id_before));
        value.reboot_context = OTA_REBOOT_INSTALL;
    }
    if (state == OTA_STATE_ERROR)
        ota_error_set(&value.last_error, OTA_ERROR_RAUC_INSTALL_FAILED, "preserved install failure");
    if (state == OTA_STATE_ROLLBACK)
        ota_error_set(&value.last_error, OTA_ERROR_HEALTH_FAILED, "preserved health failure");
    return value;
}

static gboolean fake_manifest(const OtaConfig *config, OtaManifest *out,
                              gboolean *available, gboolean *retryable,
                              OtaError *error)
{
    (void)config;
    ++active->manifest_calls;
    *available = FALSE;
    *retryable = FALSE;
    if (active->manifest_mode == 1) return TRUE;
    if (active->manifest_mode == 2 && active->manifest_calls == 1) {
        *retryable = TRUE;
        ota_error_set(error, OTA_ERROR_MANIFEST_HTTP, "fixture HTTP 503");
        return FALSE;
    }
    *out = active->manifest;
    *available = TRUE;
    return TRUE;
}

static gboolean fake_download(const OtaConfig *config, const OtaManifest *manifest,
                              gboolean *retryable, OtaError *error)
{
    (void)config; (void)manifest;
    ++active->download_calls;
    *retryable = FALSE;
    if (active->download_failures > 0) {
        --active->download_failures;
        *retryable = active->download_retryable;
        ota_error_set(error, active->download_error, "fixture download failure");
        return FALSE;
    }
    return TRUE;
}

static gboolean fake_validate(const OtaConfig *config, const OtaManifest *manifest,
                              OtaError *error)
{
    (void)config; (void)manifest;
    if (active->validate_ok) return TRUE;
    ota_error_set(error, OTA_ERROR_DOWNLOAD_HASH_MISMATCH, "fixture hash mismatch");
    return FALSE;
}

static gboolean fake_health(const OtaHealth *health, OtaSlot expected, OtaError *error)
{
    (void)health; (void)expected;
    if (active->health_ok) return TRUE;
    ota_error_set(error, OTA_ERROR_HEALTH_FAILED, "fixture health failure");
    return FALSE;
}

static gboolean fake_report(const OtaConfig *config, const OtaReport *report,
                            gboolean *retryable, OtaError *error)
{
    (void)config;
    ++active->report_calls;
    *retryable = FALSE;
    if (active->report_failures > 0) {
        --active->report_failures;
        *retryable = active->report_retryable;
        ota_error_set(error, OTA_ERROR_REPORT_FAILED, "fixture report transport");
        return FALSE;
    }
    if (report->agent_state == OTA_STATE_ERROR)
        g_assert_cmpint(report->error_code, ==, active->machine.current.last_error.code);
    if (active->stop_on_report) stopping = 1;
    return TRUE;
}

static gboolean fake_command(void *user, const char *const argv[], uint32_t timeout,
                             char **output, GError **error)
{
    (void)user; (void)timeout;
    const char *action = argv[1] ? argv[1] : "";
    if (!strcmp(action, "status") && argv[2]) action = argv[2];
    if (!strcmp(action, "install")) ++active->install_calls;
    else if (!strcmp(action, "mark-good")) ++active->mark_good_calls;
    else if (!strcmp(action, "mark-bad")) ++active->mark_bad_calls;
    if (active->runner_fail && !strcmp(active->runner_fail, action)) {
        g_set_error_literal(error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED, "fixture command failure");
        *output = NULL;
        return FALSE;
    }
    if (!strcmp(action, "get-current")) *output = g_strdup(active->slot);
    else if (!strcmp(action, "info")) {
        ++active->info_calls;
        *output = g_strdup_printf("RAUC_MF_COMPATIBLE='%s'\nRAUC_MF_VERSION='%s'\n"
                                  "RAUC_MF_BUILD='%s'\n",
                                  active->signed_compatible, active->signed_version,
                                  active->signed_build);
    } else {
        *output = g_strdup("");
    }
    return TRUE;
}

static int fake_clock(void *user, clockid_t clock, struct timespec *out)
{
    Matrix *matrix = user;
    matrix->clock_ms += 2000;
    out->tv_sec = clock == CLOCK_REALTIME ? 1700000000 : (time_t)(matrix->clock_ms / 1000);
    out->tv_nsec = 0;
    return 0;
}

static gboolean fake_reboot(void *user, OtaError *error)
{
    Matrix *matrix = user;
    OtaPersistentState durable;
    gboolean missing;
    g_assert_true(ota_persistence_load(matrix->state_path, &durable, &missing, error));
    g_assert_false(missing);
    g_assert_true(durable.state == OTA_STATE_REBOOT_PENDING ||
                  (durable.state == OTA_STATE_ROLLBACK &&
                   durable.reboot_context == OTA_REBOOT_HEALTH_FAILURE));
    ++matrix->reboot_calls;
    return TRUE;
}

static void setup(Matrix *m, gconstpointer data)
{
    (void)data;
    memset(m, 0, sizeof(*m));
    stopping = 0;
    active = m;
    m->dir = g_dir_make_tmp("ota-merged-XXXXXX", NULL);
    g_assert_nonnull(m->dir);
    m->state_path = g_build_filename(m->dir, "agent-state.json", NULL);
    g_snprintf(m->config.part_file, sizeof(m->config.part_file), "%s/update.raucb.part", m->dir);
    g_snprintf(m->config.bundle_file, sizeof(m->config.bundle_file), "%s/update.raucb", m->dir);
    g_strlcpy(m->config.base_url, "http://127.0.0.1:9", sizeof(m->config.base_url));
    g_strlcpy(m->config.report_path, "/device/report", sizeof(m->config.report_path));
    m->config.connect_timeout_sec = 1;
    m->config.request_timeout_sec = 1;
    m->config.poll_interval_sec = 1;
    m->config.health_timeout_sec = 1;
    m->local = (OtaRelease){.schema_version=1, .device_compatible="atk-dlrk3588",
        .rauc_compatible="EdgeGuard-ATK-DLRK3588-RK3588", .version="1.0.0", .build_id="installed"};
    m->candidate = m->local;
    g_strlcpy(m->candidate.version, "2.0.0", sizeof(m->candidate.version));
    g_strlcpy(m->candidate.build_id, "candidate", sizeof(m->candidate.build_id));
    m->manifest = (OtaManifest){.schema_version=1, .device_compatible="atk-dlrk3588",
        .version="2.0.0", .build_id="candidate", .artifact_url="/update.raucb", .size=3,
        .sha256="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"};
    m->slot = "a\n";
    m->signed_compatible = m->local.rauc_compatible;
    m->signed_version = "2.0.0";
    m->signed_build = "candidate";
    m->download_error = OTA_ERROR_DOWNLOAD_HTTP;
    m->download_retryable = TRUE;
    m->validate_ok = TRUE;
    m->health_ok = TRUE;
    ota_agent_service_context_init(&m->context);
    m->context.manifest_fetch = fake_manifest;
    m->context.download_bundle = fake_download;
    m->context.download_validate = fake_validate;
    m->context.health_check = fake_health;
    m->context.report_send = fake_report;
    m->context.rauc.run = fake_command;
    m->context.rauc.user = m;
    m->context.time = (OtaTimeSource){fake_clock, m};
    ota_agent_services_bind(&m->services, &m->context);
    m->services.reboot = (OtaRebootOps){fake_reboot, m};
}

static void teardown(Matrix *m, gconstpointer data)
{
    (void)data;
    GDir *directory = g_dir_open(m->dir, 0, NULL);
    const char *name;
    while (directory && (name = g_dir_read_name(directory))) {
        char *path = g_build_filename(m->dir, name, NULL);
        g_assert_cmpint(g_unlink(path), ==, 0);
        g_free(path);
    }
    if (directory) g_dir_close(directory);
    g_assert_cmpint(g_rmdir(m->dir), ==, 0);
    g_free(m->state_path);
    g_free(m->dir);
    active = NULL;
}

static void open_at(Matrix *m, OtaState state)
{
    OtaError error = {0};
    OtaPersistentState value = snapshot(state);
    g_assert_true(ota_persistence_save(m->state_path, &value, NULL, &error));
    g_assert_true(ota_state_machine_open(&m->machine, m->state_path, NULL, &error));
}

static void reopen(Matrix *m)
{
    OtaError error = {0};
    OtaStateMachine recovered;
    g_assert_true(ota_state_machine_open(&recovered, m->state_path, NULL, &error));
    m->machine = recovered;
}

static int dispatch(Matrix *m, const OtaRelease *release, OtaError *error)
{
    return run(&m->machine, &m->config, release,
               "12345678-1234-4123-8123-123456789abc", &m->services, error);
}

static void assert_terminal(Matrix *m, OtaState start, const OtaRelease *release,
                            OtaErrorCode code)
{
    OtaError error = {0};
    open_at(m, start);
    g_assert_cmpint(dispatch(m, release, &error), ==, 2);
    g_assert_cmpint(m->machine.current.state, ==, OTA_STATE_ERROR);
    g_assert_cmpint(m->machine.current.last_error.code, ==, code);
    g_assert_cmpint(m->report_calls, ==, 1);
}

static gboolean fail_step(void *user, OtaIoStep step)
{
    return step != GPOINTER_TO_INT(user);
}

static void matrix_case(Matrix *m, gconstpointer data)
{
    unsigned which = GPOINTER_TO_UINT(data);
    OtaError error = {0};
    switch (which) {
    case 1:
        open_at(m, OTA_STATE_IDLE);
        g_assert_cmpint(dispatch(m, &m->local, &error), ==, 0);
        g_assert_cmpint(m->install_calls, ==, 1);
        g_assert_cmpint(m->reboot_calls, ==, 1);
        g_assert_cmpint(m->machine.current.state, ==, OTA_STATE_REBOOT_PENDING);
        break;
    case 2: {
        open_at(m, OTA_STATE_REBOOT_PENDING);
        reopen(m); /* a new process reads only the durable handoff */
        m->slot = "b\n";
        g_assert_true(ota_state_machine_recover(&m->machine, uuid_b,
                                                m->services.current_slot,
                                                m->services.user, &error));
        g_assert_cmpint(m->machine.current.state, ==, OTA_STATE_HEALTH_CHECK);
        m->stop_on_report = TRUE;
        g_assert_cmpint(dispatch(m, &m->candidate, &error), ==, 0);
        g_assert_cmpint(m->mark_good_calls, ==, 1);
        break;
    }
    case 3: {
        OtaPersistentState next;
        m->manifest_mode = 1; open_at(m, OTA_STATE_CHECK_UPDATE);
        g_assert_cmpint(m->services.phase(m->services.user, &m->config, &m->local,
            uuid_a, &m->machine.current, &next, &error), ==, OTA_PHASE_ADVANCE);
        g_assert_cmpint(next.state, ==, OTA_STATE_IDLE);
        break;
    }
    case 4:
        m->manifest.version[0] = 0; g_strlcpy(m->manifest.version, "1.0.0", sizeof(m->manifest.version));
        g_strlcpy(m->manifest.build_id, "installed", sizeof(m->manifest.build_id));
        open_at(m, OTA_STATE_CHECK_UPDATE); {
            OtaPersistentState next;
            g_assert_cmpint(m->services.phase(m->services.user, &m->config, &m->local,
                uuid_a, &m->machine.current, &next, &error), ==, OTA_PHASE_ADVANCE);
            g_assert_cmpint(next.state, ==, OTA_STATE_IDLE);
        } break;
    case 5:
        g_strlcpy(m->manifest.version, "0.9.0", sizeof(m->manifest.version));
        assert_terminal(m, OTA_STATE_CHECK_UPDATE, &m->local, OTA_ERROR_DOWNGRADE_REJECTED); break;
    case 6:
        g_strlcpy(m->manifest.version, "1.0.0", sizeof(m->manifest.version));
        assert_terminal(m, OTA_STATE_CHECK_UPDATE, &m->local, OTA_ERROR_VERSION_COLLISION); break;
    case 7:
        g_strlcpy(m->manifest.device_compatible, "other", sizeof(m->manifest.device_compatible));
        assert_terminal(m, OTA_STATE_CHECK_UPDATE, &m->local, OTA_ERROR_DEVICE_COMPAT_MISMATCH); break;
    case 8:
        m->signed_compatible = "other";
        assert_terminal(m, OTA_STATE_RAUC_VERIFY, &m->local, OTA_ERROR_RAUC_COMPAT_MISMATCH); break;
    case 9:
        m->signed_version = "2.0.1";
        assert_terminal(m, OTA_STATE_RAUC_VERIFY, &m->local, OTA_ERROR_RAUC_IDENTITY_MISMATCH); break;
    case 10:
        m->signed_build = "other";
        assert_terminal(m, OTA_STATE_RAUC_VERIFY, &m->local, OTA_ERROR_RAUC_IDENTITY_MISMATCH); break;
    case 11:
        assert_terminal(m, OTA_STATE_HEALTH_CHECK, &m->local, OTA_ERROR_RAUC_IDENTITY_MISMATCH);
        g_assert_cmpint(m->mark_good_calls, ==, 0); break;
    case 12:
    case 14:
        m->download_failures = 1; open_at(m, OTA_STATE_DOWNLOADING);
        g_assert_cmpint(dispatch(m, &m->local, &error), ==, 0);
        g_assert_cmpint(m->download_calls, ==, 2); break;
    case 13:
        m->manifest_mode = 2; open_at(m, OTA_STATE_CHECK_UPDATE);
        g_assert_cmpint(dispatch(m, &m->local, &error), ==, 0);
        g_assert_cmpint(m->manifest_calls, ==, 2); break;
    case 15:
        open_at(m, OTA_STATE_DOWNLOADING);
        reopen(m); /* retain the same durable attempt metadata */
        g_assert_true(ota_state_machine_recover(&m->machine, uuid_a,
                                                m->services.current_slot,
                                                m->services.user, &error));
        g_assert_cmpint(dispatch(m, &m->local, &error), ==, 0);
        g_assert_cmpint(m->download_calls, ==, 1); break;
    case 16:
        g_assert_true(g_file_set_contents(m->config.part_file, "stale", -1, NULL));
        open_at(m, OTA_STATE_PRECHECK);
        g_assert_cmpint(dispatch(m, &m->local, &error), ==, 0);
        g_assert_false(g_file_test(m->config.part_file, G_FILE_TEST_EXISTS)); break;
    case 17:
        g_assert_cmpint(ota_download_response(1, 3, 200, NULL, &error), ==,
                        OTA_DOWNLOAD_WRITE_ZERO); break;
    case 18:
        g_assert_cmpint(ota_download_response(1, 3, 206, "bytes 0-2/3", &error), ==,
                        OTA_DOWNLOAD_REJECT);
        g_assert_cmpint(error.code, ==, OTA_ERROR_DOWNLOAD_RANGE_MISMATCH); break;
    case 19:
        g_assert_cmpint(ota_download_response(1, 3, 416, "bytes */3", &error), ==,
                        OTA_DOWNLOAD_RESTART); break;
    case 20: {
        g_assert_true(g_file_set_contents(m->config.bundle_file, "xyz", 3, NULL));
        g_assert_false(ota_download_validate_bundle(&m->config, &m->manifest, &error));
        g_assert_cmpint(error.code, ==, OTA_ERROR_DOWNLOAD_HASH_MISMATCH); break;
    }
    case 21:
        m->runner_fail = "info";
        assert_terminal(m, OTA_STATE_RAUC_VERIFY, &m->local, OTA_ERROR_RAUC_VERIFY_FAILED); break;
    case 22:
    case 23:
        m->runner_fail = "install";
        assert_terminal(m, OTA_STATE_INSTALLING, &m->local, OTA_ERROR_RAUC_INSTALL_FAILED);
        g_assert_cmpint(m->install_calls, ==, 1); break;
    case 24:
        m->runner_fail = "install"; open_at(m, OTA_STATE_INSTALLING);
        g_assert_cmpint(dispatch(m, &m->local, &error), ==, 2);
        g_assert_cmpint(m->info_calls, ==, 0);
        g_assert_cmpint(dispatch(m, &m->local, &error), ==, 2);
        g_assert_cmpint(m->install_calls, <=, 1); break;
    case 25:
        m->slot = "b\n"; m->stop_on_report = TRUE; open_at(m, OTA_STATE_HEALTH_CHECK);
        g_assert_cmpint(dispatch(m, &m->candidate, &error), ==, 0);
        g_assert_cmpint(m->mark_good_calls, ==, 1); break;
    case 26:
        open_at(m, OTA_STATE_MARK_GOOD);
        g_assert_cmpint(dispatch(m, &m->candidate, &error), ==, 2);
        g_assert_cmpint(m->mark_good_calls, ==, 0); break;
    case 27:
    case 30:
        m->slot = "b\n"; m->health_ok = FALSE; open_at(m, OTA_STATE_HEALTH_CHECK);
        g_assert_cmpint(dispatch(m, &m->candidate, &error), ==, 0);
        g_assert_cmpint(m->mark_bad_calls, ==, 1);
        g_assert_cmpint(m->reboot_calls, ==, 1); break;
    case 28:
        m->health_ok = FALSE; open_at(m, OTA_STATE_HEALTH_CHECK);
        g_assert_cmpint(dispatch(m, &m->candidate, &error), ==, 2);
        g_assert_cmpint(m->mark_bad_calls, ==, 0);
        g_assert_cmpint(m->reboot_calls, ==, 0); break;
    case 29:
        m->slot = "b\n"; m->health_ok = FALSE; m->runner_fail = "mark-bad";
        open_at(m, OTA_STATE_HEALTH_CHECK);
        g_assert_cmpint(dispatch(m, &m->candidate, &error), ==, 2);
        g_assert_cmpint(m->machine.current.last_error.code, ==,
                        OTA_ERROR_RAUC_MARK_BAD_FAILED);
        g_assert_cmpint(m->reboot_calls, ==, 0); break;
    case 31:
        open_at(m, OTA_STATE_ROLLBACK); m->slot = "a\n";
        m->report_failures = 1; m->report_retryable = TRUE;
        g_assert_cmpint(dispatch(m, &m->local, &error), ==, 2);
        g_assert_cmpint(m->report_calls, ==, 2);
        g_assert_cmpint(m->machine.current.state, ==, OTA_STATE_ROLLBACK);
        g_assert_cmpint(m->reboot_calls, ==, 0); break;
    case 32:
        m->slot = "b\n"; m->report_failures = 1; m->report_retryable = TRUE;
        m->stop_on_report = TRUE; open_at(m, OTA_STATE_MARK_GOOD);
        g_assert_cmpint(dispatch(m, &m->candidate, &error), ==, 0);
        g_assert_cmpint(m->mark_good_calls, ==, 1);
        g_assert_cmpint(m->report_calls, ==, 2); break;
    case 33:
        m->report_failures = 1; m->report_retryable = FALSE;
        open_at(m, OTA_STATE_REPORT_SUCCESS);
        g_assert_cmpint(dispatch(m, &m->candidate, &error), ==, 1);
        g_assert_cmpint(m->machine.current.state, ==, OTA_STATE_REPORT_SUCCESS);
        m->stop_on_report = TRUE;
        g_assert_cmpint(dispatch(m, &m->candidate, &error), ==, 0);
        g_assert_cmpint(m->machine.current.state, ==, OTA_STATE_IDLE);
        g_assert_cmpint(m->report_calls, ==, 2);
        g_assert_cmpint(m->install_calls + m->mark_good_calls + m->mark_bad_calls + m->reboot_calls, ==, 0); break;
    case 34:
        for (int step = OTA_IO_WRITE; step <= OTA_IO_PARENT_FSYNC; ++step) {
            if (g_file_test(m->state_path, G_FILE_TEST_EXISTS)) g_unlink(m->state_path);
            open_at(m, OTA_STATE_IDLE);
            m->machine.persistence = (OtaPersistenceOps){fail_step, GINT_TO_POINTER(step)};
            OtaPersistentState next = snapshot(OTA_STATE_CHECK_NETWORK);
            g_assert_false(ota_state_machine_transition(&m->machine, &next, &error));
            g_assert_true(m->machine.blocked);
        } break;
    case 35: {
        g_assert_true(g_file_set_contents(m->state_path, "{malformed", -1, NULL));
        char *before = NULL, *after = NULL;
        g_assert_true(g_file_get_contents(m->state_path, &before, NULL, NULL));
        g_assert_false(ota_state_machine_open(&m->machine, m->state_path, NULL, &error));
        g_assert_true(g_file_get_contents(m->state_path, &after, NULL, NULL));
        g_assert_cmpstr(before, ==, after); g_free(before); g_free(after); break;
    }
    case 36: {
        const char *json = "{\"schema_version\":1,\"device_compatible\":\"atk-dlrk3588\","
            "\"version\":\"2.0.0\",\"build_id\":\"candidate\","
            "\"artifact_url\":\"/releases/update.raucb\","
            "\"sha256\":\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\","
            "\"size\":3,\"mandatory\":false}";
        OtaManifest manifest;
        g_assert_true(ota_manifest_parse(json, strlen(json), &manifest, &error));
        OtaPersistentState value = snapshot(OTA_STATE_PRECHECK), loaded;
        g_strlcpy(value.artifact_url, manifest.artifact_url, sizeof(value.artifact_url));
        gboolean missing;
        g_assert_true(ota_persistence_save(m->state_path, &value, NULL, &error));
        g_assert_true(ota_persistence_load(m->state_path, &loaded, &missing, &error));
        g_assert_cmpstr(loaded.artifact_url, ==, manifest.artifact_url);
        const char *bad[] = {"/releases/update%20file.raucb", "//host/x", "/../x", "/x?q"};
        for (gsize i = 0; i < G_N_ELEMENTS(bad); ++i) {
            char *changed = g_strdup(json);
            char *where = strstr(changed, "/releases/update.raucb");
            char *invalid = g_strdup_printf("%.*s%s%s", (int)(where - changed), changed,
                                            bad[i], where + strlen("/releases/update.raucb"));
            g_assert_false(ota_manifest_parse(invalid, strlen(invalid), &manifest, &error));
            value = snapshot(OTA_STATE_PRECHECK);
            g_strlcpy(value.artifact_url, bad[i], sizeof(value.artifact_url));
            g_assert_false(ota_persistence_save(m->state_path, &value, NULL, &error));
            g_free(invalid); g_free(changed);
        }
        break;
    }
    default: g_assert_not_reached();
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    const char *names[] = {
        "happy-reboot-pending", "new-slot-recovery", "http-204", "same-version-build",
        "downgrade", "version-collision", "device-compatible", "signed-compatible",
        "signed-version", "signed-build", "new-slot-release", "interrupted-range",
        "http-503", "timeout", "restart-resume", "stale-part", "range-ignored-200",
        "wrong-206", "http-416", "hash-mismatch", "rauc-verify", "rauc-install",
        "install-timeout", "install-once", "health-success", "guarded-mark-good",
        "health-failure", "guarded-mark-bad", "mark-bad-failure", "rollback-durable",
        "terminal-report", "post-good-report-retry", "report-no-replay", "persistence-faults",
        "malformed-state", "artifact-path-roundtrip"
    };
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        char *path = g_strdup_printf("/merged/%02u-%s", i + 1, names[i]);
        g_test_add(path, Matrix, GUINT_TO_POINTER(i + 1), setup, matrix_case, teardown);
        g_free(path);
    }
    return g_test_run();
}
