#define _GNU_SOURCE
#include "edgeguard_ota/rauc_adapter.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define OUTPUT_LIMIT (256 * 1024)

gboolean ota_command_run(void *user, const char *const argv[], uint32_t timeout_ms,
                         char **output, GError **error)
{
    (void)user;
    if (output) *output = NULL;
    if (!output || !argv || !argv[0] || argv[0][0] != '/' || !timeout_ms) {
        g_set_error_literal(error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED, "Invalid argv runner input");
        return FALSE;
    }
    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC) < 0) {
        g_set_error(error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED, "pipe: %s", g_strerror(errno));
        return FALSE;
    }
    /* Keep pipe descriptors away from stdin/stdout/stderr, even if a caller has
     * closed them. Only the read end is nonblocking; the child writes normally. */
    for (int i = 0; i < 2; ++i) {
        if (pipes[i] <= STDERR_FILENO) {
            int replacement = fcntl(pipes[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
            if (replacement < 0) {
                close(pipes[0]); close(pipes[1]);
                g_set_error_literal(error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED, "Cannot duplicate pipe");
                return FALSE;
            }
            close(pipes[i]); pipes[i] = replacement;
        }
    }
    if (fcntl(pipes[0], F_SETFL, O_NONBLOCK) < 0) {
        close(pipes[0]); close(pipes[1]);
        g_set_error_literal(error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED, "Cannot configure pipe");
        return FALSE;
    }
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int rc = posix_spawn_file_actions_init(&actions);
    gboolean actions_ready = rc == 0, attributes_ready = FALSE;
    if (!rc) { rc = posix_spawnattr_init(&attributes); attributes_ready = rc == 0; }
    if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    if (!rc) rc = posix_spawn_file_actions_addclose(&actions, pipes[0]);
    if (!rc) rc = posix_spawn_file_actions_addclose(&actions, pipes[1]);
    if (!rc) rc = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    sigset_t empty, defaults;
    sigemptyset(&empty); sigemptyset(&defaults);
    sigaddset(&defaults, SIGTERM); sigaddset(&defaults, SIGINT); sigaddset(&defaults, SIGPIPE);
    if (!rc) rc = posix_spawnattr_setsigmask(&attributes, &empty);
    if (!rc) rc = posix_spawnattr_setsigdefault(&attributes, &defaults);
    if (!rc) rc = posix_spawnattr_setpgroup(&attributes, 0);
    if (!rc) rc = posix_spawnattr_setflags(&attributes,
        POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    char *const environment[] = {"PATH=/usr/bin:/bin", "LANG=C", "LC_ALL=C", NULL};
    pid_t pid = -1;
    if (!rc) rc = posix_spawn(&pid, argv[0], &actions, &attributes,
                               (char *const *)argv, environment);
    if (actions_ready) posix_spawn_file_actions_destroy(&actions);
    if (attributes_ready) posix_spawnattr_destroy(&attributes);
    close(pipes[1]);
    if (rc) {
        close(pipes[0]);
        g_set_error(error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED, "exec %s: %s", argv[0], g_strerror(rc));
        return FALSE;
    }

    GString *captured = g_string_new(NULL);
    gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
    gboolean eof = FALSE, reaped = FALSE, invalid_output = FALSE, ok = FALSE;
    int status = 0;
    const char *failure = NULL;
    while (!reaped || !eof) {
        /* A finite batch prevents a flooding child from starving timeout/waitpid. */
        for (int batch = 0; batch < 16 && !eof; ++batch) {
            char buffer[4096];
            ssize_t n = read(pipes[0], buffer, sizeof(buffer));
            if (n > 0) {
                if (memchr(buffer, 0, (gsize)n) || captured->len + (gsize)n > OUTPUT_LIMIT)
                    invalid_output = TRUE;
                if (!invalid_output) g_string_append_len(captured, buffer, n);
            } else if (!n) eof = TRUE;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            else if (errno != EINTR) { failure = "stdout read failed"; break; }
        }
        if (failure) break;
        if (!reaped) {
            pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid) reaped = TRUE;
            else if (waited < 0 && errno != EINTR) { failure = "waitpid failed"; break; }
        }
        if (reaped && eof) break;
        gint64 remaining = deadline - g_get_monotonic_time();
        if (remaining <= 0) { failure = "command timed out; operation outcome may be uncertain"; break; }
        struct pollfd descriptor = {pipes[0], POLLIN | POLLHUP, 0};
        int delay = (int)MIN((remaining + 999) / 1000, 20);
        if (poll(eof ? NULL : &descriptor, eof ? 0 : 1, delay) < 0 && errno != EINTR) {
            failure = "poll failed"; break;
        }
    }
    if (failure) {
        /* Kill this command's group only. Never signal or restart the RAUC service.
         * A CLI timeout does not establish whether the service stopped installing. */
        kill(-pid, SIGKILL);
        if (!reaped) while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        g_set_error(error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED, "%s: %s", argv[0], failure);
    } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        g_set_error(error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED,
                    "%s: exit=%d signal=%d", argv[0],
                    WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                    WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    } else if (invalid_output) {
        g_set_error_literal(error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED, "Invalid or oversized command output");
    } else {
        *output = g_string_free(captured, FALSE); captured = NULL; ok = TRUE;
    }
    close(pipes[0]);
    if (captured) g_string_free(captured, TRUE);
    return ok;
}

void ota_rauc_adapter_init(OtaRaucAdapter *adapter)
{
    *adapter = (OtaRaucAdapter){ota_command_run, NULL, "/usr/bin/rauc",
        "/usr/bin/edgeguard-rk-abctl", 30000, 3600000};
}

static gboolean invoke(const OtaRaucAdapter *adapter, const char *const argv[],
                       uint32_t timeout, OtaErrorCode code, char **output, OtaError *error)
{
    GError *cause = NULL;
    *output = NULL;
    if (!adapter || !adapter->run || !argv[0] || argv[0][0] != '/' || !timeout ||
        !adapter->run(adapter->user, argv, timeout, output, &cause)) {
        ota_error_set(error, code, "%s", cause ? cause->message : "Invalid command adapter");
        g_clear_error(&cause); g_clear_pointer(output, g_free);
        return FALSE;
    }
    if (!*output) {
        ota_error_set(error, code, "Command runner returned no output buffer");
        return FALSE;
    }
    return TRUE;
}

static gboolean bundle_path(const char *bundle, OtaErrorCode code, OtaError *error)
{
    /* An absolute path cannot become a RAUC option; metacharacters stay data. */
    if (!bundle || bundle[0] != '/' || !bundle[1]) {
        ota_error_set(error, code, "Bundle must be an absolute file path"); return FALSE;
    }
    return TRUE;
}

gboolean ota_rauc_current_slot(const OtaRaucAdapter *adapter, OtaSlot *slot, OtaError *error)
{
    const char *argv[] = {adapter ? adapter->abctl : NULL, "get-current", NULL};
    char *output;
    if (!invoke(adapter, argv, adapter ? adapter->query_timeout_ms : 0,
                OTA_ERROR_IDENTITY_AMBIGUOUS, &output, error)) return FALSE;
    OtaSlot current = OTA_SLOT_UNKNOWN;
    if (!strcmp(output, "a") || !strcmp(output, "a\n")) current = OTA_SLOT_A;
    if (!strcmp(output, "b") || !strcmp(output, "b\n")) current = OTA_SLOT_B;
    g_free(output);
    if (!slot || current == OTA_SLOT_UNKNOWN) {
        ota_error_set(error, OTA_ERROR_IDENTITY_AMBIGUOUS, "get-current did not return exactly a or b");
        return FALSE;
    }
    *slot = current; return TRUE;
}

/* RAUC 1.5.1 emits g_shell_quote strings. Decode only single-quoted segments
 * plus the apostrophe escape between segments. No expansion or execution. */
static char *quoted_value(const char *text)
{
    const char *p = text;
    GString *decoded = g_string_new(NULL);
    gboolean valid = TRUE;
    while (*p) {
        if (*p++ != '\'') { valid = FALSE; break; }
        while (*p && *p != '\'') {
            if ((unsigned char)*p < 0x20 || *p == 0x7f) { valid = FALSE; break; }
            g_string_append_c(decoded, *p++);
        }
        if (!valid || *p != '\'') { valid = FALSE; break; }
        ++p;
        if (p[0] == '\\' && p[1] == '\'') {
            g_string_append_c(decoded, '\''); p += 2;
            if (*p != '\'') { valid = FALSE; break; }
        } else if (*p) { valid = FALSE; break; }
    }
    if (!decoded->len || decoded->len >= OTA_TEXT_CAP ||
        !g_utf8_validate(decoded->str, -1, NULL)) valid = FALSE;
    if (!valid) { g_string_free(decoded, TRUE); return NULL; }
    return g_string_free(decoded, FALSE);
}

typedef struct { char *compatible, *version, *build; } BundleIdentity;

static gboolean identity_value(char **destination, const char *line,
                               const char *key)
{
    gsize length = strlen(key);
    if (strncmp(line, key, length) || line[length] != '=') return TRUE;
    if (*destination) return FALSE;
    *destination = quoted_value(line + length + 1);
    return *destination != NULL;
}

static gboolean bundle_identity_parse(const char *output, BundleIdentity *identity)
{
    char **lines = g_strsplit(output, "\n", -1);
    gboolean valid = TRUE;
    for (gsize i = 0; lines[i] && valid; ++i) {
        valid = identity_value(&identity->compatible, lines[i], "RAUC_MF_COMPATIBLE") &&
                identity_value(&identity->version, lines[i], "RAUC_MF_VERSION") &&
                identity_value(&identity->build, lines[i], "RAUC_MF_BUILD");
    }
    g_strfreev(lines);
    return valid && identity->compatible && identity->version && identity->build;
}

static void bundle_identity_clear(BundleIdentity *identity)
{
    g_free(identity->compatible);
    g_free(identity->version);
    g_free(identity->build);
}

gboolean ota_rauc_verify(const OtaRaucAdapter *adapter, const char *bundle,
                         const OtaRelease *release,
                         const OtaPersistentState *attempt, OtaError *error)
{
    if (!release || !release->rauc_compatible[0] ||
        !memchr(release->rauc_compatible, 0, sizeof(release->rauc_compatible))) {
        ota_error_set(error, OTA_ERROR_LOCAL_RELEASE_INVALID,
                      "Missing local RAUC compatible");
        return FALSE;
    }
    if (!attempt || !attempt->attempt_id[0] || !attempt->target_version[0] ||
        !attempt->build_id[0] ||
        !memchr(attempt->target_version, 0, sizeof(attempt->target_version)) ||
        !memchr(attempt->build_id, 0, sizeof(attempt->build_id))) {
        ota_error_set(error, OTA_ERROR_RAUC_IDENTITY_MISMATCH,
                      "Missing persisted candidate identity");
        return FALSE;
    }
    if (!bundle_path(bundle, OTA_ERROR_RAUC_VERIFY_FAILED, error)) return FALSE;
    const char *argv[] = {adapter ? adapter->binary : NULL, "info", "--output-format=shell", bundle, NULL};
    char *output;
    if (!invoke(adapter, argv, adapter ? adapter->query_timeout_ms : 0,
                OTA_ERROR_RAUC_VERIFY_FAILED, &output, error)) return FALSE;
    BundleIdentity identity = {0};
    gboolean parsed = bundle_identity_parse(output, &identity);
    g_free(output);
    if (!parsed) {
        bundle_identity_clear(&identity);
        ota_error_set(error, OTA_ERROR_RAUC_VERIFY_FAILED,
                      "Missing, duplicate or malformed required RAUC identity assignment");
        return FALSE;
    }
    gboolean ok = !strcmp(release->rauc_compatible, identity.compatible);
    if (!ok) ota_error_set(error, OTA_ERROR_RAUC_COMPAT_MISMATCH,
                           "Bundle and local RAUC compatible differ");
    if (ok && (strcmp(identity.version, attempt->target_version) ||
               strcmp(identity.build, attempt->build_id))) {
        ota_error_set(error, OTA_ERROR_RAUC_IDENTITY_MISMATCH,
                      "Signed bundle version/build differs from persisted attempt");
        ok = FALSE;
    }
    bundle_identity_clear(&identity);
    return ok;
}

gboolean ota_rauc_install(const OtaRaucAdapter *adapter, const char *bundle, OtaError *error)
{
    if (!bundle_path(bundle, OTA_ERROR_RAUC_INSTALL_FAILED, error)) return FALSE;
    const char *argv[] = {adapter ? adapter->binary : NULL, "install", bundle, NULL};
    char *output;
    gboolean ok = invoke(adapter, argv, adapter ? adapter->install_timeout_ms : 0,
                         OTA_ERROR_RAUC_INSTALL_FAILED, &output, error);
    g_free(output); return ok;
}

gboolean ota_rauc_status(const OtaRaucAdapter *adapter, OtaError *error)
{
    const char *argv[] = {adapter ? adapter->binary : NULL, "status", NULL};
    char *output;
    gboolean ok = invoke(adapter, argv, adapter ? adapter->query_timeout_ms : 0,
                         OTA_ERROR_HEALTH_FAILED, &output, error);
    g_free(output); return ok;
}

static gboolean mark(const OtaRaucAdapter *adapter, OtaSlot expected,
                     const char *verb, OtaErrorCode code, OtaError *error)
{
    OtaSlot current;
    if (expected != OTA_SLOT_A && expected != OTA_SLOT_B) {
        ota_error_set(error, OTA_ERROR_IDENTITY_AMBIGUOUS, "Invalid expected candidate"); return FALSE;
    }
    if (!ota_rauc_current_slot(adapter, &current, error)) return FALSE;
    if (current != expected) {
        ota_error_set(error, OTA_ERROR_IDENTITY_AMBIGUOUS, "Current slot is not expected candidate"); return FALSE;
    }
    const char *argv[] = {adapter->binary, "status", verb, NULL};
    char *output;
    gboolean ok = invoke(adapter, argv, adapter->query_timeout_ms, code, &output, error);
    g_free(output); return ok;
}

gboolean ota_rauc_mark_good(const OtaRaucAdapter *adapter, OtaSlot expected, OtaError *error)
{ return mark(adapter, expected, "mark-good", OTA_ERROR_RAUC_MARK_GOOD_FAILED, error); }

gboolean ota_rauc_mark_bad(const OtaRaucAdapter *adapter, OtaSlot expected, OtaError *error)
{ return mark(adapter, expected, "mark-bad", OTA_ERROR_RAUC_MARK_BAD_FAILED, error); }
