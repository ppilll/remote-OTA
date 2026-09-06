#include "edgeguard_ota/state_machine.h"
#include "edgeguard_ota/identity.h"
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <unistd.h>

static const char *uuid_a = "12345678-1234-4123-8123-123456789abc";
static const char *uuid_b = "87654321-4321-4321-9321-cba987654321";
typedef struct { char *dir, *path; } Fixture;
static void setup(Fixture *f, gconstpointer unused)
{
    (void)unused;
    f->dir = g_dir_make_tmp("ota-core-XXXXXX", NULL);
    g_assert_nonnull(f->dir);
    f->path = g_build_filename(f->dir, "agent-state.json", NULL);
}
static void teardown(Fixture *f, gconstpointer unused)
{
    (void)unused;
    GDir *dir = g_dir_open(f->dir, 0, NULL);
    const char *name;
    while ((name = g_dir_read_name(dir))) {
        char *path = g_build_filename(f->dir, name, NULL);
        g_assert_cmpint(g_unlink(path), ==, 0);
        g_free(path);
    }
    g_dir_close(dir);
    g_assert_cmpint(g_rmdir(f->dir), ==, 0);
    g_free(f->path);
    g_free(f->dir);
}
static OtaPersistentState sample(OtaState state)
{
    OtaPersistentState s;
    ota_persistent_state_init(&s);
    s.state = state;
    if (state >= OTA_STATE_PRECHECK) {
        g_strlcpy(s.attempt_id, uuid_a, sizeof(s.attempt_id));
        g_strlcpy(s.target_version, "1.2.0", sizeof(s.target_version));
        g_strlcpy(s.build_id, "build-123", sizeof(s.build_id));
        g_strlcpy(s.artifact_url, "/update.raucb", sizeof(s.artifact_url));
        s.expected_size = 123456789;
        memset(s.expected_sha256, 'a', 64);
    }
    if (state >= OTA_STATE_INSTALLING) {
        s.previous_slot = OTA_SLOT_A;
        s.expected_candidate_slot = OTA_SLOT_B;
        g_strlcpy(s.boot_id_before, uuid_a, sizeof(s.boot_id_before));
        s.reboot_context = OTA_REBOOT_INSTALL;
    }
    if (state == OTA_STATE_ERROR) ota_error_set(&s.last_error, OTA_ERROR_RAUC_INSTALL_FAILED, "install failed");
    return s;
}
static char *read_file(const char *path)
{
    char *data = NULL;
    g_assert_true(g_file_get_contents(path, &data, NULL, NULL));
    return data;
}
static void roundtrip(Fixture *f, gconstpointer unused)
{
    (void)unused;
    OtaError error = {0};
    for (int i = 0; i < OTA_STATE_COUNT; ++i) {
        OtaPersistentState s = sample((OtaState)i), loaded;
        gboolean missing = TRUE;
        g_assert_true(ota_persistence_save(f->path, &s, NULL, &error));
        g_assert_true(ota_persistence_load(f->path, &loaded, &missing, &error));
        g_assert_false(missing);
        g_assert_cmpint(loaded.state, ==, s.state);
        g_assert_cmpstr(loaded.attempt_id, ==, s.attempt_id);
        g_assert_cmpstr(loaded.target_version, ==, s.target_version);
        g_assert_cmpstr(loaded.build_id, ==, s.build_id);
        g_assert_cmpstr(loaded.artifact_url, ==, s.artifact_url);
        g_assert_cmpuint(loaded.expected_size, ==, s.expected_size);
        g_assert_cmpstr(loaded.expected_sha256, ==, s.expected_sha256);
        g_assert_cmpint(loaded.previous_slot, ==, s.previous_slot);
        g_assert_cmpint(loaded.expected_candidate_slot, ==, s.expected_candidate_slot);
        g_assert_cmpstr(loaded.boot_id_before, ==, s.boot_id_before);
        g_assert_cmpint(loaded.reboot_context, ==, s.reboot_context);
        g_assert_cmpint(loaded.last_error.code, ==, s.last_error.code);
        g_assert_cmpstr(loaded.last_error.message, ==, s.last_error.message);
    }
}
static void malformed(Fixture *f, gconstpointer unused)
{
    (void)unused;
    const char *bad[] = {"", "{", "null", "[]", "{}", "{\"state\":\"IDLE\"}"};
    OtaError error = {0};
    for (gsize i = 0; i < G_N_ELEMENTS(bad); ++i) {
        g_assert_true(g_file_set_contents(f->path, bad[i], -1, NULL));
        OtaStateMachine machine;
        g_assert_false(ota_state_machine_open(&machine, f->path, NULL, &error));
        g_assert_true(machine.blocked);
        char *after = read_file(f->path);
        g_assert_cmpstr(after, ==, bad[i]);
        g_free(after);
    }
    OtaPersistentState good = sample(OTA_STATE_REBOOT_PENDING);
    g_assert_true(ota_persistence_save(f->path, &good, NULL, &error));
    char *original = read_file(f->path);
    const char *fields[] = {"schema_version", "expected_size", "state", "attempt_id",
        "target_version", "build_id", "artifact_url", "expected_sha256", "previous_slot",
        "expected_candidate_slot", "boot_id_before", "reboot_context", "last_error"};
    for (gsize i = 0; i < G_N_ELEMENTS(fields); ++i) {
        for (unsigned variant = 0; variant < 2; ++variant) {
            JsonParser *parser = json_parser_new();
            g_assert_true(json_parser_load_from_data(parser, original, -1, NULL));
            JsonObject *object = json_node_get_object(json_parser_get_root(parser));
            if (variant) json_object_set_boolean_member(object, fields[i], TRUE);
            else json_object_remove_member(object, fields[i]);
            JsonGenerator *gen = json_generator_new();
            json_generator_set_root(gen, json_parser_get_root(parser));
            char *data = json_generator_to_data(gen, NULL);
            g_assert_true(g_file_set_contents(f->path, data, -1, NULL));
            OtaPersistentState out = sample(OTA_STATE_IDLE);
            gboolean missing;
            g_assert_false(ota_persistence_load(f->path, &out, &missing, &error));
            g_assert_cmpint(out.state, ==, OTA_STATE_IDLE);
            g_free(data); g_object_unref(gen); g_object_unref(parser);
        }
    }
    char *duplicate = g_strconcat("{\"state\":\"IDLE\",", original + 1, NULL);
    g_assert_true(g_file_set_contents(f->path, duplicate, -1, NULL));
    OtaPersistentState out;
    gboolean missing;
    g_assert_false(ota_persistence_load(f->path, &out, &missing, &error));
    g_free(duplicate);
    JsonParser *parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, original, -1, NULL));
    json_object_remove_member(json_node_get_object(json_parser_get_root(parser)), "expected_size");
    JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, json_parser_get_root(parser));
    char *without_size = json_generator_to_data(generator, NULL);
    const char *bad_numbers[] = {"-1", "0", "01", "1.0", "1e3", "9223372036854775808",
                                 "1844674407370955161600000"};
    for (gsize i = 0; i < G_N_ELEMENTS(bad_numbers); ++i) {
        char *data = g_strdup_printf("{\"expected_size\":%s,%s", bad_numbers[i], without_size + 1);
        g_assert_true(g_file_set_contents(f->path, data, -1, NULL));
        g_assert_false(ota_persistence_load(f->path, &out, &missing, &error));
        g_free(data);
    }
    g_free(without_size); g_object_unref(generator); g_object_unref(parser);
    g_free(original);
    good.expected_candidate_slot = good.previous_slot;
    g_assert_false(ota_persistence_save(f->path, &good, NULL, &error));
    g_assert_cmpint(g_unlink(f->path), ==, 0);
    g_assert_cmpint(symlink("absent", f->path), ==, 0);
    g_assert_false(ota_persistence_load(f->path, &out, &missing, &error));
    g_assert_false(missing);
    g_assert_false(ota_atomic_write(f->path, "x", 1, NULL, &error));
}

typedef struct { int fail_at, count; OtaIoStep order[4]; int reboots; } Fault;
static gboolean inject(void *user, OtaIoStep step)
{
    Fault *f = user;
    g_assert_cmpint(f->count, <, 4);
    f->order[f->count++] = step;
    return f->fail_at != (int)step;
}
static gboolean fake_reboot(void *user, OtaError *error)
{
    (void)error;
    Fault *f = user;
    g_assert_cmpint(f->count, ==, 4);
    for (int i = 0; i < 4; ++i) g_assert_cmpint(f->order[i], ==, i);
    ++f->reboots;
    return TRUE;
}
static void atomic_failures(Fixture *f, gconstpointer unused)
{
    (void)unused;
    OtaError error = {0};
    for (int failure = 0; failure < 4; ++failure) {
        OtaPersistentState old = sample(OTA_STATE_IDLE);
        g_assert_true(ota_persistence_save(f->path, &old, NULL, &error));
        OtaStateMachine machine;
        g_assert_true(ota_state_machine_open(&machine, f->path, NULL, &error));
        Fault fault = {.fail_at = failure};
        machine.persistence = (OtaPersistenceOps){inject, &fault};
        OtaPersistentState next = sample(OTA_STATE_CHECK_NETWORK);
        g_assert_false(ota_state_machine_transition(&machine, &next, &error));
        g_assert_true(machine.blocked);
        g_assert_cmpint(machine.current.state, ==, OTA_STATE_IDLE);
        OtaRebootOps reboot = {fake_reboot, &fault};
        g_assert_false(ota_state_machine_reboot(&machine, &reboot, &error));
        g_assert_cmpint(fault.reboots, ==, 0);
        OtaPersistentState loaded;
        gboolean missing;
        g_assert_true(ota_persistence_load(f->path, &loaded, &missing, &error));
        g_assert_cmpint(loaded.state, ==,
                       failure == OTA_IO_PARENT_FSYNC ? OTA_STATE_CHECK_NETWORK : OTA_STATE_IDLE);
    }
    OtaPersistentState pending = sample(OTA_STATE_REBOOT_PENDING);
    for (int failure = -1; failure < 4; ++failure) {
        Fault fault = {.fail_at = failure};
        OtaPersistenceOps persistence = {inject, &fault};
        OtaRebootOps reboot = {fake_reboot, &fault};
        g_assert_cmpint(ota_reboot_controlled(f->path, &pending, &persistence, &reboot, &error),
                       ==, failure == -1);
        g_assert_cmpint(fault.reboots, ==, failure == -1 ? 1 : 0);
    }
}

static void transitions(Fixture *f, gconstpointer unused)
{
    (void)unused;
    OtaError error = {0};
    OtaStateMachine machine;
    g_assert_true(ota_state_machine_open(&machine, f->path, NULL, &error));
    char *before = read_file(f->path);
    OtaPersistentState bad = sample(OTA_STATE_INSTALLING);
    g_assert_false(ota_state_machine_transition(&machine, &bad, &error));
    g_assert_cmpint(error.code, ==, OTA_ERROR_ILLEGAL_TRANSITION);
    g_assert_cmpint(machine.current.state, ==, OTA_STATE_IDLE);
    char *after = read_file(f->path);
    g_assert_cmpstr(before, ==, after);
    g_free(before); g_free(after);
    for (int state = OTA_STATE_CHECK_NETWORK; state <= OTA_STATE_REPORT_SUCCESS; ++state) {
        OtaPersistentState next = sample((OtaState)state);
        g_assert_true(ota_state_machine_transition(&machine, &next, &error));
    }
    OtaPersistentState idle = sample(OTA_STATE_IDLE);
    g_assert_true(ota_state_machine_transition(&machine, &idle, &error));
    g_assert_true(ota_transition_allowed(OTA_STATE_CHECK_UPDATE, OTA_STATE_IDLE));
    g_assert_true(ota_transition_allowed(OTA_STATE_PRECHECK, OTA_STATE_IDLE));
    g_assert_true(ota_transition_allowed(OTA_STATE_INSTALLING, OTA_STATE_ERROR));
    for (int state = 0; state < OTA_STATE_COUNT; ++state) {
        g_assert_false(ota_transition_allowed((OtaState)state, (OtaState)state));
        if (state != OTA_STATE_ERROR)
            g_assert_false(ota_transition_allowed(OTA_STATE_ERROR, (OtaState)state));
    }
    g_assert_false(ota_transition_allowed(OTA_STATE_ROLLBACK, OTA_STATE_INSTALLING));
    g_assert_false(ota_transition_allowed(OTA_STATE_REBOOT_PENDING, OTA_STATE_INSTALLING));
    g_assert_false(ota_transition_allowed((OtaState)-1, OTA_STATE_ERROR));
    g_assert_false(ota_transition_allowed(OTA_STATE_IDLE, OTA_STATE_COUNT));
    OtaPersistentState precheck = sample(OTA_STATE_PRECHECK);
    g_assert_true(ota_persistence_save(f->path, &precheck, NULL, &error));
    g_assert_true(ota_state_machine_open(&machine, f->path, NULL, &error));
    OtaPersistentState changed = sample(OTA_STATE_DOWNLOADING);
    changed.expected_size++;
    g_assert_false(ota_state_machine_transition(&machine, &changed, &error));
    g_assert_cmpint(error.code, ==, OTA_ERROR_ILLEGAL_TRANSITION);
}

typedef struct { OtaSlot slot; int calls; gboolean success; } SlotSource;
static gboolean current_slot(void *user, OtaSlot *out, OtaError *error)
{
    (void)error;
    SlotSource *source = user;
    ++source->calls;
    *out = source->slot;
    return source->success;
}
static void recovery(Fixture *f, gconstpointer unused)
{
    (void)unused;
    OtaError error = {0};
    for (unsigned variant = 0; variant < 7; ++variant) {
        OtaPersistentState pending = sample(variant == 5 ? OTA_STATE_BOOT_NEW_SLOT : OTA_STATE_REBOOT_PENDING);
        g_assert_true(ota_persistence_save(f->path, &pending, NULL, &error));
        OtaStateMachine machine;
        g_assert_true(ota_state_machine_open(&machine, f->path, NULL, &error));
        SlotSource slot = {variant == 2 ? OTA_SLOT_A : variant == 3 ? OTA_SLOT_UNKNOWN : OTA_SLOT_B,
                           0, variant != 4};
        const char *boot = variant == 0 ? uuid_a : variant == 6 ? "invalid" : uuid_b;
        gboolean result = ota_state_machine_recover(&machine, boot, current_slot, &slot, &error);
        g_assert_true(result);
        OtaState expected = variant == 0 ? OTA_STATE_REBOOT_PENDING :
                            variant == 1 || variant == 5 ? OTA_STATE_HEALTH_CHECK :
                            variant == 2 ? OTA_STATE_ROLLBACK : OTA_STATE_ERROR;
        g_assert_cmpint(machine.current.state, ==, expected);
        g_assert_cmpint(slot.calls, ==, variant == 0 || variant == 6 ? 0 : 1);
        OtaPersistentState loaded;
        gboolean missing;
        g_assert_true(ota_persistence_load(f->path, &loaded, &missing, &error));
        g_assert_cmpint(loaded.state, ==, expected);
    }
    OtaPersistentState installing = sample(OTA_STATE_INSTALLING);
    g_assert_true(ota_persistence_save(f->path, &installing, NULL, &error));
    OtaStateMachine machine;
    g_assert_true(ota_state_machine_open(&machine, f->path, NULL, &error));
    SlotSource slot = {OTA_SLOT_B, 0, TRUE};
    g_assert_true(ota_state_machine_recover(&machine, uuid_b, current_slot, &slot, &error));
    g_assert_cmpint(machine.current.state, ==, OTA_STATE_ERROR);
    g_assert_cmpint(slot.calls, ==, 0);
    g_assert_false(ota_state_machine_transition(&machine, &installing, &error));
    OtaPersistentState pending = sample(OTA_STATE_REBOOT_PENDING);
    g_assert_true(ota_persistence_save(f->path, &pending, NULL, &error));
    g_assert_true(ota_state_machine_open(&machine, f->path, NULL, &error));
    Fault fault = {.fail_at = OTA_IO_FILE_FSYNC};
    machine.persistence = (OtaPersistenceOps){inject, &fault};
    slot.calls = 0;
    g_assert_false(ota_state_machine_recover(&machine, uuid_b, current_slot, &slot, &error));
    g_assert_true(machine.blocked);
    g_assert_cmpint(slot.calls, ==, 0);
}

static gboolean uuid_source(void *user, char out[OTA_UUID_CAP], OtaError *error)
{
    (void)error;
    ++*(int *)user;
    g_strlcpy(out, uuid_a, OTA_UUID_CAP);
    return TRUE;
}
static void identity(Fixture *f, gconstpointer unused)
{
    (void)unused;
    int calls = 0;
    OtaError error = {0};
    char value[OTA_UUID_CAP];
    g_assert_true(ota_identity_load_or_create(f->path, uuid_source, &calls, NULL, value, &error));
    g_assert_cmpstr(value, ==, uuid_a);
    g_assert_true(ota_identity_load_or_create(f->path, uuid_source, &calls, NULL, value, &error));
    g_assert_cmpint(calls, ==, 1);
    g_assert_true(g_file_set_contents(f->path, "invalid\n", -1, NULL));
    g_assert_false(ota_identity_load_or_create(f->path, uuid_source, &calls, NULL, value, &error));
    g_assert_cmpint(calls, ==, 1);
    char *data = read_file(f->path);
    g_assert_cmpstr(data, ==, "invalid\n");
    g_free(data);
    g_assert_false(ota_uuid_valid("12345678-1234-1123-8123-123456789abc", TRUE));
    g_assert_false(ota_uuid_valid("12345678-1234-4123-7123-123456789abc", TRUE));
    g_assert_false(ota_uuid_valid("12345678-1234-4123-8123-123456789ABC", TRUE));
    g_assert_false(ota_uuid_valid("", TRUE));
    g_assert_cmpint(g_unlink(f->path), ==, 0);
    Fault fault = {.fail_at = OTA_IO_FILE_FSYNC};
    OtaPersistenceOps persistence = {inject, &fault};
    g_strlcpy(value, "sentinel", sizeof(value));
    g_assert_false(ota_identity_load_or_create(f->path, uuid_source, &calls, &persistence, value, &error));
    g_assert_cmpstr(value, ==, "sentinel");
    g_assert_false(g_file_test(f->path, G_FILE_TEST_EXISTS));
}

static const char *valid_config =
    "[server]\nbase_url=http://192.168.1.100:8000\nmanifest_path=/manifest.json\n"
    "report_path=/device/report\npoll_interval_sec=300\nconnect_timeout_sec=5\nrequest_timeout_sec=30\n"
    "[agent]\nstate_dir=/userdata/edgeguard-remote-ota\nrelease_file=/etc/edgeguard-ota/release.json\n"
    "max_manifest_bytes=65536\n[download]\npart_file=/userdata/edgeguard-remote-ota/update.raucb.part\n"
    "bundle_file=/userdata/edgeguard-remote-ota/update.raucb\nreserve_bytes=67108864\n"
    "[rauc]\nbinary=/usr/bin/rauc\n[identity]\ndevice_id_file=/userdata/edgeguard-remote-ota/device-id\n"
    "[health]\nhook=\ntimeout_sec=30\n";
static char *replace_once(const char *text, const char *needle, const char *replacement)
{
    const char *position = strstr(text, needle);
    g_assert_nonnull(position);
    char *prefix = g_strndup(text, position - text);
    char *result = g_strconcat(prefix, replacement, position + strlen(needle), NULL);
    g_free(prefix);
    return result;
}
static void configuration(Fixture *f, gconstpointer unused)
{
    (void)unused;
    OtaError error = {0};
    OtaConfig config;
    g_assert_true(g_file_set_contents(f->path, valid_config, -1, NULL));
    g_assert_true(ota_config_load(f->path, &config, &error));
    g_assert_cmpint(config.reporting_mode, ==, OTA_REPORT_LEGACY);
    g_assert_cmpuint(config.max_manifest_bytes, ==, 65536);
    const char *changes[][2] = {
        {"base_url=", "base_ur1="}, {"[server]", "[unknown]"},
        {"poll_interval_sec=300", "poll_interval_sec=0"},
        {"poll_interval_sec=300", "poll_interval_sec=-1"},
        {"poll_interval_sec=300", "poll_interval_sec=4294967296"},
        {"connect_timeout_sec=5", "connect_timeout_sec=+5"},
        {"connect_timeout_sec=5", "connect_timeout_sec=31"},
        {"request_timeout_sec=30", "request_timeout_sec=30garbage"},
        {"reserve_bytes=67108864", "reserve_bytes=18446744073709551616"},
        {"max_manifest_bytes=65536", "max_manifest_bytes=65537"},
        {"[server]", "[server]\n[server]"},
        {"\nhook=\ntimeout_sec=30\n", "\nhook=\ntimeout_sec=30\ntimeout_sec=40\n"},
        {"hook=", "hook=relative/path"},
        {"binary=/usr/bin/rauc", "binary=/bin/sh"},
        {"manifest_path=/manifest.json", "manifest_path=//evil/manifest"},
        {"manifest_path=/manifest.json", "manifest_path=/%2e%2e/manifest"},
        {"http://192.168.1.100:8000", "http://user@host:8000"},
        {"http://192.168.1.100:8000", "http://host:99999"},
        {"http://192.168.1.100:8000", "file:///tmp"},
        {"release_file=/etc/edgeguard-ota/release.json\n", ""},
        {"bundle_file=/userdata/edgeguard-remote-ota/update.raucb", "bundle_file=/tmp/update.raucb"}
    };
    for (gsize i = 0; i < G_N_ELEMENTS(changes); ++i) {
        char *changed = replace_once(valid_config, changes[i][0], changes[i][1]);
        g_assert_true(g_file_set_contents(f->path, changed, -1, NULL));
        memset(&config, 0, sizeof(config));
        config.poll_interval_sec = 123;
        g_assert_false(ota_config_load(f->path, &config, &error));
        g_assert_cmpint(error.code, ==, OTA_ERROR_CONFIG_INVALID);
        g_assert_cmpuint(config.poll_interval_sec, ==, 123);
        g_free(changed);
    }
    const char *modes[] = {"legacy", "extended", "typo", ""};
    for (gsize i = 0; i < G_N_ELEMENTS(modes); ++i) {
        char *text = g_strconcat(valid_config, "[reporting]\nmode=", modes[i], "\n", NULL);
        g_assert_true(g_file_set_contents(f->path, text, -1, NULL));
        g_assert_cmpint(ota_config_load(f->path, &config, &error), ==, i < 2);
        g_free(text);
    }
}

typedef struct { time_t monotonic, wall; int fail; } Clock;
static int fake_clock(void *user, clockid_t clock, struct timespec *out)
{
    Clock *c = user;
    if (c->fail) return -1;
    out->tv_sec = clock == CLOCK_MONOTONIC ? c->monotonic : c->wall;
    out->tv_nsec = 0;
    return 0;
}
static void time_tests(void)
{
    Clock clock = {10, 1700000000, 0};
    OtaTimeSource source = {fake_clock, &clock};
    OtaError error = {0};
    uint64_t deadline;
    g_assert_true(ota_deadline_after(&source, 5000, &deadline, &error));
    g_assert_cmpuint(deadline, ==, 15000);
    gboolean expired;
    clock.wall = 1;
    g_assert_true(ota_deadline_expired(&source, deadline, &expired, &error));
    g_assert_false(expired);
    clock.wall = 2000000000;
    clock.monotonic = 15;
    g_assert_true(ota_deadline_expired(&source, deadline, &expired, &error));
    g_assert_true(expired);
    OtaReport report = {0};
    g_assert_true(ota_time_telemetry(&source, &report, &error));
    g_assert_false(report.timestamp_valid);
    g_assert_cmpuint(report.uptime_ms, ==, 15000);
    g_assert_cmpuint(strlen(report.timestamp), ==, 20);
    g_assert_false(ota_deadline_after(&source, UINT64_MAX, &deadline, &error));
    clock.fail = 1;
    g_assert_false(ota_monotonic_ms(&source, &deadline, &error));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
#define TEST(name, function) g_test_add(name, Fixture, NULL, setup, function, teardown)
    TEST("/core/persistence/roundtrip", roundtrip);
    TEST("/core/persistence/malformed", malformed);
    TEST("/core/persistence/faults-and-reboot", atomic_failures);
    TEST("/core/state/transitions", transitions);
    TEST("/core/state/recovery", recovery);
    TEST("/core/identity", identity);
    TEST("/core/config", configuration);
#undef TEST
    g_test_add_func("/core/time", time_tests);
    return g_test_run();
}
