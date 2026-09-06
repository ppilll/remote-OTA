/* Exercise the private skeleton dispatcher with fake services. No HTTP, RAUC,
 * command fixtures or real reboot. The production entrypoint is never called. */
#define main ota_unused_program_main
#define OTA_TEST_WEAK_SERVICES 1
#include "../src/main.c"
#undef main
#include <glib/gstdio.h>

typedef struct {
    char *dir, *path;
    OtaStateMachine machine;
    OtaAgentServices services;
    OtaSlot slot;
    int phase_calls, install_calls, reboot_calls;
} Fixture;

static gboolean fake_slot(void *user, OtaSlot *out, OtaError *error)
{
    (void)error;
    *out = ((Fixture *)user)->slot;
    return TRUE;
}
static OtaPhaseResult fake_phase(void *user, const OtaConfig *config,
                                 const OtaRelease *release, const char *id,
                                 const OtaPersistentState *current,
                                 OtaPersistentState *next, OtaError *error)
{
    (void)config; (void)release; (void)id;
    Fixture *f = user;
    ++f->phase_calls;
    if (current->state == OTA_STATE_INSTALLING) {
        ++f->install_calls;
        ota_error_set(error, OTA_ERROR_RAUC_INSTALL_FAILED, "fake install failure");
        return OTA_PHASE_HARD_FAILURE;
    }
    if (current->state == OTA_STATE_ERROR || current->state == OTA_STATE_ROLLBACK)
        return OTA_PHASE_ADVANCE; /* terminal report succeeds; state is retained */
    if (current->state == OTA_STATE_HEALTH_CHECK) {
        /* Represents successful health-module mark-bad, without running a tool. */
        next->state = OTA_STATE_ROLLBACK;
        return OTA_PHASE_ADVANCE;
    }
    if (current->state == OTA_STATE_CHECK_UPDATE) {
        next->state = OTA_STATE_IDLE; /* represents HTTP 204 */
        stopping = 1;
        return OTA_PHASE_ADVANCE;
    }
    g_assert_not_reached();
    return OTA_PHASE_HARD_FAILURE;
}
static gboolean fake_reboot(void *user, OtaError *error)
{
    Fixture *f = user;
    OtaPersistentState durable;
    gboolean missing;
    g_assert_true(ota_persistence_load(f->path, &durable, &missing, error));
    g_assert_false(missing);
    g_assert_cmpint(durable.state, ==, OTA_STATE_ROLLBACK);
    g_assert_cmpint(durable.reboot_context, ==, OTA_REBOOT_HEALTH_FAILURE);
    ++f->reboot_calls;
    return TRUE;
}
static void setup(Fixture *f, gconstpointer unused)
{
    (void)unused;
    stopping = 0;
    f->dir = g_dir_make_tmp("ota-orchestration-XXXXXX", NULL);
    g_assert_nonnull(f->dir);
    f->path = g_build_filename(f->dir, "agent-state.json", NULL);
    f->slot = OTA_SLOT_B;
    f->services.user = f;
    f->services.current_slot = fake_slot;
    f->services.phase = fake_phase;
    f->services.reboot = (OtaRebootOps){fake_reboot, f};
}
static void teardown(Fixture *f, gconstpointer unused)
{
    (void)unused;
    g_assert_cmpint(g_unlink(f->path), ==, 0);
    g_assert_cmpint(g_rmdir(f->dir), ==, 0);
    g_free(f->path); g_free(f->dir);
}
static void start_at(Fixture *f, OtaState state)
{
    OtaPersistentState snapshot;
    ota_persistent_state_init(&snapshot);
    snapshot.state = state;
    if (state >= OTA_STATE_INSTALLING) {
        g_strlcpy(snapshot.attempt_id, "12345678-1234-4123-8123-123456789abc", sizeof(snapshot.attempt_id));
        g_strlcpy(snapshot.boot_id_before, snapshot.attempt_id, sizeof(snapshot.boot_id_before));
        g_strlcpy(snapshot.target_version, "1.2.0", sizeof(snapshot.target_version));
        g_strlcpy(snapshot.build_id, "candidate", sizeof(snapshot.build_id));
        g_strlcpy(snapshot.artifact_url, "/update.raucb", sizeof(snapshot.artifact_url));
        memset(snapshot.expected_sha256, 'a', 64);
        snapshot.expected_size = 1024;
        snapshot.previous_slot = OTA_SLOT_A;
        snapshot.expected_candidate_slot = OTA_SLOT_B;
        snapshot.reboot_context = OTA_REBOOT_INSTALL;
    }
    OtaError error = {0};
    g_assert_true(ota_persistence_save(f->path, &snapshot, NULL, &error));
    g_assert_true(ota_state_machine_open(&f->machine, f->path, NULL, &error));
}
static int dispatch(Fixture *f, OtaError *error)
{
    OtaConfig config = {0};
    OtaRelease release = {0};
    return run(&f->machine, &config, &release, "fake-device", &f->services, error);
}
static void no_reinstall(Fixture *f, gconstpointer unused)
{
    (void)unused;
    OtaError error = {0};
    start_at(f, OTA_STATE_INSTALLING);
    g_assert_cmpint(dispatch(f, &error), ==, 2);
    g_assert_cmpint(f->machine.current.state, ==, OTA_STATE_ERROR);
    g_assert_cmpint(error.code, ==, OTA_ERROR_RAUC_INSTALL_FAILED);
    g_assert_cmpint(f->install_calls, ==, 1);
    g_assert_true(ota_state_machine_open(&f->machine, f->path, NULL, &error));
    g_assert_cmpint(dispatch(f, &error), ==, 2);
    g_assert_cmpint(f->install_calls, ==, 1);
}
static void guard_mark_good(Fixture *f, gconstpointer unused)
{
    (void)unused;
    OtaError error = {0};
    start_at(f, OTA_STATE_MARK_GOOD);
    f->slot = OTA_SLOT_A;
    g_assert_cmpint(dispatch(f, &error), ==, 2);
    g_assert_cmpint(f->phase_calls, ==, 0);
    g_assert_cmpint(f->machine.current.state, ==, OTA_STATE_ERROR);
    g_assert_cmpint(error.code, ==, OTA_ERROR_IDENTITY_AMBIGUOUS);
}
static void health_rollback(Fixture *f, gconstpointer unused)
{
    (void)unused;
    OtaError error = {0};
    start_at(f, OTA_STATE_HEALTH_CHECK);
    g_assert_cmpint(dispatch(f, &error), ==, 0);
    g_assert_cmpint(f->phase_calls, ==, 1);
    g_assert_cmpint(f->reboot_calls, ==, 1);
}
static void no_release(Fixture *f, gconstpointer unused)
{
    (void)unused;
    OtaError error = {0};
    start_at(f, OTA_STATE_CHECK_UPDATE);
    g_assert_cmpint(dispatch(f, &error), ==, 0);
    g_assert_cmpint(f->phase_calls, ==, 1);
    g_assert_cmpint(f->machine.current.state, ==, OTA_STATE_IDLE);
    g_assert_cmpstr(f->machine.current.attempt_id, ==, "");
}
static gboolean fail_fsync(void *user, OtaIoStep step)
{
    (void)user;
    return step != OTA_IO_FILE_FSYNC;
}
static void block_reboot(Fixture *f, gconstpointer unused)
{
    (void)unused;
    OtaError error = {0};
    start_at(f, OTA_STATE_REBOOT_PENDING);
    f->machine.persistence.before = fail_fsync;
    g_assert_cmpint(dispatch(f, &error), ==, 1);
    g_assert_cmpint(f->phase_calls, ==, 0);
    g_assert_cmpint(f->reboot_calls, ==, 0);
    g_assert_true(f->machine.blocked);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
#define TEST(name, function) g_test_add(name, Fixture, NULL, setup, function, teardown)
    TEST("/orchestration/no-reinstall", no_reinstall);
    TEST("/orchestration/guard-mark-good", guard_mark_good);
    TEST("/orchestration/health-rollback", health_rollback);
    TEST("/orchestration/no-active-release", no_release);
    TEST("/orchestration/persistence-blocks-reboot", block_reboot);
#undef TEST
    return g_test_run();
}
