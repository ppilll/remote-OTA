#include "edgeguard_ota/health.h"
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    GPtrArray *calls;
    const char *info;
    const char *slot;
    const char *fail;
    uint32_t timeout;
} Fake;

static gboolean fake_run(void *user, const char *const argv[], uint32_t timeout,
                         char **output, GError **error)
{
    Fake *fake = user;
    g_ptr_array_add(fake->calls, g_strdupv((char **)argv));
    fake->timeout = timeout;
    *output = NULL;
    const char *action = argv[1] ? argv[1] : "hook";
    if (argv[1] && !strcmp(argv[1], "status") && argv[2]) action = argv[2];
    if (fake->fail && !strcmp(fake->fail, action)) {
        g_set_error_literal(error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED, "fake exit 23"); return FALSE;
    }
    *output = g_strdup(!strcmp(action, "info") ? fake->info :
                       !strcmp(action, "get-current") ? fake->slot : "");
    return TRUE;
}

static OtaRaucAdapter adapter_for(Fake *fake)
{
    *fake = (Fake){g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev),
        "RAUC_MF_COMPATIBLE='EdgeGuard-ATK-DLRK3588-RK3588'\n"
        "RAUC_MF_VERSION='1.2.0'\nRAUC_MF_BUILD='candidate'\n", "a\n", NULL, 0};
    OtaRaucAdapter adapter;
    ota_rauc_adapter_init(&adapter); adapter.run = fake_run; adapter.user = fake;
    return adapter;
}

static void assert_call(Fake *fake, guint index, const char *const expected[])
{
    char **actual = g_ptr_array_index(fake->calls, index);
    guint i = 0;
    for (; expected[i]; ++i) g_assert_cmpstr(actual[i], ==, expected[i]);
    g_assert_null(actual[i]);
}

static OtaRelease release_identity(void)
{
    OtaRelease release = {.schema_version=1, .device_compatible="atk-dlrk3588",
                          .version="1.0.0", .build_id="installed"};
    g_strlcpy(release.rauc_compatible, "EdgeGuard-ATK-DLRK3588-RK3588", sizeof(release.rauc_compatible));
    return release;
}

static OtaPersistentState attempt_identity(void)
{
    OtaPersistentState attempt = {.schema_version=1, .state=OTA_STATE_RAUC_VERIFY,
        .attempt_id="12345678-1234-4123-8123-123456789abc",
        .target_version="1.2.0", .build_id="candidate"};
    return attempt;
}

static void argv_and_failures(void)
{
    Fake fake;
    OtaRaucAdapter adapter = adapter_for(&fake);
    OtaRelease release = release_identity();
    OtaPersistentState attempt = attempt_identity();
    OtaError error = {0};
    const char *bundle = "/tmp/bundle $(touch sentinel); ' quoted.raucb";
    g_assert_true(ota_rauc_verify(&adapter, bundle, &release, &attempt, &error));
    const char *info[] = {"/usr/bin/rauc", "info", "--output-format=shell", bundle, NULL};
    assert_call(&fake, 0, info);
    g_assert_true(ota_rauc_install(&adapter, bundle, &error));
    const char *install[] = {"/usr/bin/rauc", "install", bundle, NULL};
    assert_call(&fake, 1, install);
    fake.fail = "install";
    guint before = fake.calls->len;
    g_assert_false(ota_rauc_install(&adapter, bundle, &error));
    g_assert_cmpint(error.code, ==, OTA_ERROR_RAUC_INSTALL_FAILED);
    g_assert_cmpuint(fake.calls->len, ==, before + 1);
    g_assert_cmpuint(fake.timeout, ==, adapter.install_timeout_ms);
    fake.fail = "info";
    g_assert_false(ota_rauc_verify(&adapter, bundle, &release, &attempt, &error));
    g_assert_cmpint(error.code, ==, OTA_ERROR_RAUC_VERIFY_FAILED);
    before = fake.calls->len;
    g_assert_false(ota_rauc_install(&adapter, "--anything", &error));
    g_assert_false(ota_rauc_verify(&adapter, "relative.raucb", &release, &attempt, &error));
    g_assert_cmpuint(fake.calls->len, ==, before);
    fake.fail = NULL;
    g_assert_true(ota_rauc_status(&adapter, &error));
    const char *status[] = {"/usr/bin/rauc", "status", NULL};
    assert_call(&fake, fake.calls->len - 1, status);
    fake.fail = "status";
    g_assert_false(ota_rauc_status(&adapter, &error));
    g_assert_cmpint(error.code, ==, OTA_ERROR_HEALTH_FAILED);
    g_ptr_array_unref(fake.calls);
}

static void info_parser(void)
{
    Fake fake;
    OtaRaucAdapter adapter = adapter_for(&fake);
    OtaRelease release = release_identity();
    OtaPersistentState attempt = attempt_identity();
    OtaError error = {0};
    const char *invalid[] = {
        "", "RAUC_MF_VERSION='1.0.0'\n", "RAUC_MF_COMPATIBLE=''\n",
        "RAUC_MF_COMPATIBLE=unquoted\n", "RAUC_MF_COMPATIBLE=\"double\"\n",
        "RAUC_MF_COMPATIBLE='unterminated\n", "RAUC_MF_COMPATIBLE='x' garbage\n",
        "RAUC_MF_COMPATIBLE='x'; touch sentinel\n",
        "RAUC_MF_COMPATIBLE='x'\nRAUC_MF_COMPATIBLE='x'\n",
        "RAUC_MF_COMPATIBLE='x'\\'", "RAUC_MF_COMPATIBLE='a\rb'\n",
        "RAUC_MF_COMPATIBLE='EdgeGuard-ATK-DLRK3588-RK3588'\n"
        "RAUC_MF_VERSION=unquoted\nRAUC_MF_BUILD='candidate'\n",
        "RAUC_MF_COMPATIBLE='EdgeGuard-ATK-DLRK3588-RK3588'\n"
        "RAUC_MF_VERSION='1.2.0'\nRAUC_MF_VERSION='1.2.0'\nRAUC_MF_BUILD='candidate'\n",
        "RAUC_MF_COMPATIBLE='EdgeGuard-ATK-DLRK3588-RK3588'\n"
        "RAUC_MF_VERSION='1.2.0'\nRAUC_MF_BUILD='candidate'\nRAUC_MF_BUILD='candidate'\n"
    };
    for (gsize i = 0; i < G_N_ELEMENTS(invalid); ++i) {
        fake.info = invalid[i];
        g_assert_false(ota_rauc_verify(&adapter, "/bundle", &release, &attempt, &error));
        g_assert_cmpint(error.code, ==, OTA_ERROR_RAUC_VERIFY_FAILED);
    }
    fake.info = "RAUC_MF_COMPATIBLE='wrong'\nRAUC_MF_VERSION='1.2.0'\nRAUC_MF_BUILD='candidate'\n";
    g_assert_false(ota_rauc_verify(&adapter, "/bundle", &release, &attempt, &error));
    g_assert_cmpint(error.code, ==, OTA_ERROR_RAUC_COMPAT_MISMATCH);
    fake.info = "RAUC_MF_COMPATIBLE='x'\\''y'\nRAUC_MF_VERSION='1.2.0'\n"
                "RAUC_MF_BUILD='candidate'\nFUTURE=$(touch sentinel)\n";
    g_strlcpy(release.rauc_compatible, "x'y", sizeof(release.rauc_compatible));
    g_assert_true(ota_rauc_verify(&adapter, "/bundle", &release, &attempt, &error));
    fake.info = "RAUC_MF_COMPATIBLE='$(touch sentinel)'\nRAUC_MF_VERSION='1.2.0'\n"
                "RAUC_MF_BUILD='candidate'\n";
    g_strlcpy(release.rauc_compatible, "$(touch sentinel)", sizeof(release.rauc_compatible));
    g_assert_true(ota_rauc_verify(&adapter, "/bundle", &release, &attempt, &error));
    fake.info = "RAUC_MF_COMPATIBLE='$(touch sentinel)'\nRAUC_MF_VERSION='9.9.9'\n"
                "RAUC_MF_BUILD='candidate'\n";
    g_assert_false(ota_rauc_verify(&adapter, "/bundle", &release, &attempt, &error));
    g_assert_cmpint(error.code, ==, OTA_ERROR_RAUC_IDENTITY_MISMATCH);
    fake.info = "RAUC_MF_COMPATIBLE='$(touch sentinel)'\nRAUC_MF_VERSION='1.2.0'\n"
                "RAUC_MF_BUILD='other'\n";
    g_assert_false(ota_rauc_verify(&adapter, "/bundle", &release, &attempt, &error));
    g_assert_cmpint(error.code, ==, OTA_ERROR_RAUC_IDENTITY_MISMATCH);
    release.rauc_compatible[0] = 0;
    guint before = fake.calls->len;
    g_assert_false(ota_rauc_verify(&adapter, "/bundle", &release, &attempt, &error));
    g_assert_cmpint(error.code, ==, OTA_ERROR_LOCAL_RELEASE_INVALID);
    g_assert_cmpuint(fake.calls->len, ==, before);
    g_ptr_array_unref(fake.calls);
}

static void guarded_marks(void)
{
    Fake fake;
    OtaRaucAdapter adapter = adapter_for(&fake);
    OtaError error = {0};
    g_assert_true(ota_rauc_mark_good(&adapter, OTA_SLOT_A, &error));
    const char *current[] = {"/usr/bin/edgeguard-rk-abctl", "get-current", NULL};
    const char *good[] = {"/usr/bin/rauc", "status", "mark-good", NULL};
    const char *bad[] = {"/usr/bin/rauc", "status", "mark-bad", NULL};
    assert_call(&fake, 0, current); assert_call(&fake, 1, good);
    fake.slot = "b\n";
    g_assert_true(ota_rauc_mark_bad(&adapter, OTA_SLOT_B, &error));
    assert_call(&fake, 2, current); assert_call(&fake, 3, bad);
    const char *ambiguous[] = {"", "a\nb\n", " a\n", "a\n\n", "A", "unknown", "a\r\n", "b\n"};
    for (gsize i = 0; i < G_N_ELEMENTS(ambiguous); ++i) {
        fake.slot = ambiguous[i];
        guint before = fake.calls->len;
        g_assert_false(ota_rauc_mark_good(&adapter, OTA_SLOT_A, &error));
        g_assert_false(ota_rauc_mark_bad(&adapter, OTA_SLOT_A, &error));
        g_assert_cmpuint(fake.calls->len, ==, before + 2); /* identity calls only */
        g_assert_cmpint(error.code, ==, OTA_ERROR_IDENTITY_AMBIGUOUS);
    }
    fake.slot = "a"; fake.fail = "mark-good";
    g_assert_false(ota_rauc_mark_good(&adapter, OTA_SLOT_A, &error));
    g_assert_cmpint(error.code, ==, OTA_ERROR_RAUC_MARK_GOOD_FAILED);
    fake.fail = "mark-bad";
    g_assert_false(ota_rauc_mark_bad(&adapter, OTA_SLOT_A, &error));
    g_assert_cmpint(error.code, ==, OTA_ERROR_RAUC_MARK_BAD_FAILED);
    fake.fail = "get-current";
    g_assert_false(ota_rauc_mark_bad(&adapter, OTA_SLOT_A, &error));
    guint before = fake.calls->len;
    g_assert_false(ota_rauc_mark_good(&adapter, OTA_SLOT_UNKNOWN, &error));
    g_assert_cmpuint(fake.calls->len, ==, before);
    g_ptr_array_unref(fake.calls);
}

static void health_checks(void)
{
    Fake fake;
    OtaRaucAdapter adapter = adapter_for(&fake);
    OtaError error = {0};
    char *directory = g_dir_make_tmp("ota-health-XXXXXX", NULL);
    g_assert_nonnull(directory);
    char *file = g_build_filename(directory, "required", NULL);
    g_assert_true(g_file_set_contents(file, "test", -1, NULL));
    g_assert_cmpint(g_chmod(file, 0700), ==, 0);
    adapter.binary = file; adapter.abctl = file;
    OtaHealth health;
    ota_health_init(&health, &adapter);
    health.userdata = directory; health.backend = file; health.system_conf = file; health.keyring = file;
    health.timeout_sec = 2;
    g_assert_true(ota_health_check(&health, OTA_SLOT_A, &error));
    g_assert_cmpuint(fake.timeout, ==, 2000);
    health.hook = file;
    g_assert_true(ota_health_check(&health, OTA_SLOT_A, &error));
    const char *hook[] = {file, NULL};
    assert_call(&fake, fake.calls->len - 1, hook);
    fake.fail = "hook";
    g_assert_false(ota_health_check(&health, OTA_SLOT_A, &error));
    g_assert_cmpint(error.code, ==, OTA_ERROR_HEALTH_FAILED);
    fake.fail = "status";
    g_assert_false(ota_health_check(&health, OTA_SLOT_A, &error));
    fake.fail = NULL;
    health.hook = "relative hook";
    g_assert_false(ota_health_check(&health, OTA_SLOT_A, &error));
    health.hook = "";
    health.userdata = file; /* not a directory */
    g_assert_false(ota_health_check(&health, OTA_SLOT_A, &error));
    health.userdata = directory;
    health.keyring = directory; /* not a regular readable file */
    g_assert_false(ota_health_check(&health, OTA_SLOT_A, &error));
    health.keyring = file;
    g_assert_cmpint(g_chmod(file, 0600), ==, 0);
    g_assert_false(ota_health_check(&health, OTA_SLOT_A, &error));
    g_assert_cmpint(g_chmod(file, 0700), ==, 0);
    fake.slot = "b\n";
    g_assert_false(ota_health_check(&health, OTA_SLOT_A, &error));
    /* No health path may mark slots or reboot. */
    for (guint i = 0; i < fake.calls->len; ++i) {
        char **call = g_ptr_array_index(fake.calls, i);
        if (call[1]) g_assert_null(call[2]);
    }
    g_assert_cmpint(g_unlink(file), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0); /* probe files cleaned up */
    g_free(file); g_free(directory); g_ptr_array_unref(fake.calls);
}

static const char *fixture_binary;

static void sidecar_set(const char *binary, const char *suffix, const char *value)
{
    char *path = g_strconcat(binary, suffix, NULL);
    g_assert_true(g_file_set_contents(path, value, -1, NULL)); g_free(path);
}

static void real_runner(void)
{
    char *directory = g_dir_make_tmp("ota-command-XXXXXX", NULL);
    char *binary = g_build_filename(directory, "fake command;literal", NULL);
    char *bytes; gsize size;
    g_assert_true(g_file_get_contents(fixture_binary, &bytes, &size, NULL));
    g_assert_true(g_file_set_contents(binary, bytes, size, NULL)); g_free(bytes);
    g_assert_cmpint(g_chmod(binary, 0700), ==, 0);
    OtaRaucAdapter adapter;
    ota_rauc_adapter_init(&adapter); adapter.binary = binary; adapter.abctl = binary;
    OtaRelease release = release_identity();
    OtaPersistentState attempt = attempt_identity();
    OtaError error = {0};
    g_assert_true(ota_rauc_verify(&adapter, "/tmp/$(touch sentinel).raucb", &release,
                                  &attempt, &error));
    g_assert_true(ota_rauc_mark_good(&adapter, OTA_SLOT_A, &error));
    sidecar_set(binary, ".mode", "install-fail");
    g_assert_false(ota_rauc_install(&adapter, "/bundle", &error));
    g_assert_cmpint(error.code, ==, OTA_ERROR_RAUC_INSTALL_FAILED);
    char *log_path = g_strconcat(binary, ".log", NULL), *log;
    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    char *installed = strstr(log, "7:install\n");
    g_assert_nonnull(installed);
    g_assert_null(strstr(installed + 1, "7:install\n"));
    g_free(log); g_free(log_path);
    const char *modes[] = {"fail", "signal", "sleep", "nul", "flood"};
    const char *argv[] = {binary, NULL};
    for (gsize i = 0; i < G_N_ELEMENTS(modes); ++i) {
        sidecar_set(binary, ".mode", modes[i]);
        GError *cause = NULL; char *output = NULL;
        gint64 before = g_get_monotonic_time();
        g_assert_false(ota_command_run(NULL, argv, 1000, &output, &cause));
        g_assert_nonnull(cause); g_assert_null(output);
        g_assert_cmpint(g_get_monotonic_time() - before, <, 5000000);
        g_clear_error(&cause);
    }
    const char *missing[] = {"/nonexistent/edgeguard-fixture", NULL};
    GError *cause = NULL; char *output = NULL;
    g_assert_false(ota_command_run(NULL, missing, 1000, &output, &cause)); g_clear_error(&cause);
    GDir *dir = g_dir_open(directory, 0, NULL);
    const char *name;
    while ((name = g_dir_read_name(dir))) {
        char *path = g_build_filename(directory, name, NULL);
        g_assert_cmpint(g_unlink(path), ==, 0); g_free(path);
    }
    g_dir_close(dir); g_assert_cmpint(g_rmdir(directory), ==, 0);
    g_free(binary); g_free(directory);
}

int main(int argc, char **argv)
{
    g_assert_cmpint(argc, ==, 2);
    fixture_binary = argv[1]; argc = 1;
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/thread3/argv-failures", argv_and_failures);
    g_test_add_func("/thread3/info-data-parser", info_parser);
    g_test_add_func("/thread3/guarded-marks", guarded_marks);
    g_test_add_func("/thread3/health", health_checks);
    g_test_add_func("/thread3/real-runner-with-fake-command", real_runner);
    return g_test_run();
}
