#include "edgeguard_ota/persistence.h"
#include "edgeguard_ota/artifact.h"
#include "edgeguard_ota/identity.h"
#include <json-glib/json-glib.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static gboolean io_error(OtaError *error, const char *operation)
{
    ota_error_set(error, OTA_ERROR_PERSISTENCE_FAILED, "%s: %s", operation, g_strerror(errno));
    return FALSE;
}

gboolean ota_storage_prepare(const char *directory, OtaError *error)
{
    char *parent = g_path_get_dirname(directory);
    int pfd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    g_free(parent);
    if (pfd < 0) return io_error(error, "open state parent");
    gboolean ok = TRUE;
    if (mkdir(directory, 0700) < 0 && errno != EEXIST) ok = io_error(error, "mkdir state directory");
    int fd = -1;
    if (ok) {
        fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) ok = io_error(error, "open state directory");
        else if (fsync(fd) < 0 || fsync(pfd) < 0) ok = io_error(error, "fsync state directory");
    }
    if (fd >= 0) close(fd);
    close(pfd);
    return ok;
}

gboolean ota_file_read(const char *path, gsize limit, char **data, gsize *length,
                       gboolean *missing, OtaError *error)
{
    *missing = FALSE;
    *data = NULL;
    *length = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT) { *missing = TRUE; return TRUE; }
        return io_error(error, "open persistent file");
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > limit || limit == G_MAXSIZE) {
        close(fd);
        ota_error_set(error, OTA_ERROR_PERSISTENCE_FAILED, "Not a bounded regular file: %s", path);
        return FALSE;
    }
    char *buffer = g_malloc(limit + 1);
    gsize used = 0;
    gboolean ok = TRUE;
    /* Read one extra byte: fstat alone cannot bound a concurrently growing file. */
    while (used <= limit) {
        ssize_t n = read(fd, buffer + used, limit + 1 - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { ok = io_error(error, "read persistent file"); break; }
        if (n == 0) break;
        used += (gsize)n;
        if (used > limit) {
            ota_error_set(error, OTA_ERROR_PERSISTENCE_FAILED, "Persistent file exceeds limit");
            ok = FALSE;
            break;
        }
    }
    if (close(fd) < 0 && ok) ok = io_error(error, "close persistent file");
    if (!ok) { g_free(buffer); return FALSE; }
    buffer[used] = 0;
    *data = buffer;
    *length = used;
    return TRUE;
}

static gboolean step(const OtaPersistenceOps *ops, OtaIoStep value)
{
    if (ops && ops->before && !ops->before(ops->user, value)) { errno = EIO; return FALSE; }
    return TRUE;
}

gboolean ota_atomic_write(const char *path, const void *data, gsize length,
                          const OtaPersistenceOps *ops, OtaError *error)
{
    char *parent = g_path_get_dirname(path);
    char *base = g_path_get_basename(path);
    int dir = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    g_free(parent);
    if (dir < 0) { g_free(base); return io_error(error, "open persistence directory"); }
    int fd = -1;
    char temporary[80] = {0};
    gboolean created = FALSE, ok = FALSE;
    struct stat st;
    if (!strcmp(base, ".") || !strcmp(base, "..")) { errno = EINVAL; goto done; }
    if (fstatat(dir, base, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(st.st_mode)) { errno = EINVAL; goto done; }
    } else if (errno != ENOENT) goto done;
    for (unsigned i = 0; i < 32; ++i) {
        g_snprintf(temporary, sizeof(temporary), ".ota-tmp-%ld-%08x-%08x",
                   (long)getpid(), g_random_int(), g_random_int());
        fd = openat(dir, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) { created = TRUE; break; }
        if (errno != EEXIST) goto done;
    }
    if (fd < 0 || !step(ops, OTA_IO_WRITE)) goto done;
    gsize offset = 0;
    while (offset < length) {
        ssize_t n = write(fd, (const char *)data + offset, length - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (n == 0) errno = EIO; goto done; }
        offset += (gsize)n;
    }
    if (!step(ops, OTA_IO_FILE_FSYNC) || fsync(fd) < 0) goto done;
    if (close(fd) < 0) { fd = -1; goto done; }
    fd = -1;
    if (!step(ops, OTA_IO_RENAME) || renameat(dir, temporary, dir, base) < 0) goto done;
    created = FALSE;
    if (!step(ops, OTA_IO_PARENT_FSYNC) || fsync(dir) < 0) goto done;
    ok = TRUE;
done:
    if (!ok) io_error(error, "atomic persistence (durability may be uncertain after rename)");
    if (fd >= 0) close(fd);
    if (created) unlinkat(dir, temporary, 0);
    close(dir);
    g_free(base);
    return ok;
}

static const char *state_names[] = {
    "IDLE", "CHECK_NETWORK", "CHECK_UPDATE", "PRECHECK", "DOWNLOADING",
    "VERIFY_DOWNLOAD", "RAUC_VERIFY", "INSTALLING", "REBOOT_PENDING",
    "BOOT_NEW_SLOT", "HEALTH_CHECK", "MARK_GOOD", "REPORT_SUCCESS", "ROLLBACK", "ERROR"
};
G_STATIC_ASSERT(G_N_ELEMENTS(state_names) == OTA_STATE_COUNT);

const char *ota_state_name(OtaState state)
{
    return state >= 0 && state < OTA_STATE_COUNT ? state_names[state] : NULL;
}

void ota_persistent_state_init(OtaPersistentState *state)
{
    memset(state, 0, sizeof(*state));
    state->schema_version = OTA_SCHEMA_VERSION;
    state->state = OTA_STATE_IDLE;
}

static gboolean bounded_text(const char *text, gsize capacity)
{
    const char *end = memchr(text, 0, capacity);
    if (!end || !g_utf8_validate(text, end - text, NULL)) return FALSE;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        if (*p < 0x20 || *p == 0x7f) return FALSE;
    return TRUE;
}

/* Structural validation of persisted version, not version ordering/policy. */
static gboolean persisted_version_valid(const char *text)
{
    for (unsigned part = 0; part < 3; ++part) {
        if (!g_ascii_isdigit(*text) || (*text == '0' && g_ascii_isdigit(text[1]))) return FALSE;
        uint64_t value = 0;
        do {
            value = value * 10 + (unsigned)(*text++ - '0');
            if (value > UINT32_MAX) return FALSE;
        } while (g_ascii_isdigit(*text));
        if (part < 2) { if (*text++ != '.') return FALSE; }
    }
    return *text == 0;
}

gboolean ota_persistent_state_validate(const OtaPersistentState *s, OtaError *error)
{
    if (s->schema_version != OTA_SCHEMA_VERSION || !ota_state_name(s->state) ||
        !ota_error_code_name(s->last_error.code)) goto invalid;
#define TEXT(field) if (!bounded_text(s->field, sizeof(s->field))) goto invalid
    TEXT(attempt_id); TEXT(target_version); TEXT(build_id); TEXT(artifact_url);
    TEXT(expected_sha256); TEXT(last_error.message); TEXT(boot_id_before);
#undef TEXT
    gboolean has_attempt = *s->attempt_id != 0;
    if (has_attempt) {
        if (!ota_uuid_valid(s->attempt_id, TRUE) || !persisted_version_valid(s->target_version) ||
            !*s->build_id || !ota_artifact_path_valid(s->artifact_url) || !s->expected_size ||
            s->expected_size > INT64_MAX || strlen(s->expected_sha256) != 64) goto invalid;
        for (const char *p = s->expected_sha256; *p; ++p)
            if (!g_ascii_isdigit(*p) && (*p < 'a' || *p > 'f')) goto invalid;
    } else if (*s->target_version || *s->build_id || *s->artifact_url ||
               s->expected_size || *s->expected_sha256) goto invalid;
    if (s->state >= OTA_STATE_PRECHECK && s->state <= OTA_STATE_ROLLBACK && !has_attempt)
        goto invalid;
    if (s->state <= OTA_STATE_CHECK_UPDATE && has_attempt) goto invalid;
    if (s->previous_slot < OTA_SLOT_UNKNOWN || s->previous_slot > OTA_SLOT_B ||
        s->expected_candidate_slot < OTA_SLOT_UNKNOWN || s->expected_candidate_slot > OTA_SLOT_B ||
        s->reboot_context < OTA_REBOOT_NONE || s->reboot_context > OTA_REBOOT_HEALTH_FAILURE)
        goto invalid;
    gboolean has_boot = s->previous_slot != OTA_SLOT_UNKNOWN;
    if (has_boot) {
        if (!has_attempt || s->expected_candidate_slot == OTA_SLOT_UNKNOWN ||
            s->previous_slot == s->expected_candidate_slot ||
            !ota_uuid_valid(s->boot_id_before, TRUE) || s->reboot_context == OTA_REBOOT_NONE)
            goto invalid;
    } else if (s->expected_candidate_slot != OTA_SLOT_UNKNOWN || *s->boot_id_before ||
               s->reboot_context != OTA_REBOOT_NONE) goto invalid;
    if (s->state >= OTA_STATE_INSTALLING && s->state <= OTA_STATE_ROLLBACK && !has_boot)
        goto invalid;
    if (s->state < OTA_STATE_INSTALLING && has_boot) goto invalid;
    if (s->reboot_context == OTA_REBOOT_HEALTH_FAILURE &&
        s->state != OTA_STATE_ROLLBACK && s->state != OTA_STATE_ERROR) goto invalid;
    if (s->state == OTA_STATE_ERROR && s->last_error.code == OTA_ERROR_NONE) goto invalid;
    if (s->state != OTA_STATE_ERROR && s->state != OTA_STATE_ROLLBACK &&
        s->last_error.code != OTA_ERROR_NONE) goto invalid;
    if (s->last_error.code == OTA_ERROR_NONE && *s->last_error.message) goto invalid;
    return TRUE;
invalid:
    ota_error_set(error, OTA_ERROR_PERSISTENCE_FAILED, "Malformed or inconsistent persistent state");
    return FALSE;
}

static const char *slot_name(OtaSlot slot)
{
    return slot == OTA_SLOT_A ? "a" : slot == OTA_SLOT_B ? "b" : "";
}
static const char *context_name(OtaRebootContext context)
{
    return context == OTA_REBOOT_INSTALL ? "install" :
           context == OTA_REBOOT_HEALTH_FAILURE ? "health_failure" : "none";
}

gboolean ota_persistence_save(const char *path, const OtaPersistentState *s,
                              const OtaPersistenceOps *ops, OtaError *error)
{
    if (!ota_persistent_state_validate(s, error)) return FALSE;
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
#define STRING(key, value) do { json_builder_set_member_name(builder, key); \
    json_builder_add_string_value(builder, value); } while (0)
#define INTEGER(key, value) do { json_builder_set_member_name(builder, key); \
    json_builder_add_int_value(builder, value); } while (0)
    INTEGER("schema_version", s->schema_version);
    STRING("state", ota_state_name(s->state));
    STRING("attempt_id", s->attempt_id);
    STRING("target_version", s->target_version);
    STRING("build_id", s->build_id);
    STRING("artifact_url", s->artifact_url);
    INTEGER("expected_size", s->expected_size);
    STRING("expected_sha256", s->expected_sha256);
    STRING("previous_slot", slot_name(s->previous_slot));
    STRING("expected_candidate_slot", slot_name(s->expected_candidate_slot));
    STRING("boot_id_before", s->boot_id_before);
    STRING("reboot_context", context_name(s->reboot_context));
    json_builder_set_member_name(builder, "last_error");
    json_builder_begin_object(builder);
    STRING("code", ota_error_code_name(s->last_error.code));
    STRING("message", s->last_error.message);
    json_builder_end_object(builder);
    json_builder_end_object(builder);
#undef STRING
#undef INTEGER
    JsonNode *root = json_builder_get_root(builder);
    JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, root);
    gsize length;
    char *data = json_generator_to_data(generator, &length);
    gboolean ok = length <= OTA_STATE_MAX_BYTES && ota_atomic_write(path, data, length, ops, error);
    if (length > OTA_STATE_MAX_BYTES)
        ota_error_set(error, OTA_ERROR_PERSISTENCE_FAILED, "Serialized state exceeds limit");
    g_free(data);
    json_node_free(root);
    g_object_unref(generator);
    g_object_unref(builder);
    return ok;
}

typedef struct { GHashTable *members; gboolean duplicate; } ParseGuard;
static void member_seen(JsonParser *parser, JsonObject *object, const char *name, gpointer user)
{
    (void)parser;
    ParseGuard *guard = user;
    char *key = g_strdup_printf("%p:%s", (void *)object, name);
    if (g_hash_table_contains(guard->members, key)) { guard->duplicate = TRUE; g_free(key); }
    else g_hash_table_add(guard->members, key);
}

static gboolean string_member(JsonObject *object, const char *name, char *out, gsize capacity)
{
    JsonNode *node = json_object_get_member(object, name);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
        return FALSE;
    const char *value = json_node_get_string(node);
    if (!value || strlen(value) >= capacity) return FALSE;
    g_strlcpy(out, value, capacity);
    return TRUE;
}
static gboolean integer_member(JsonObject *object, const char *name, gint64 *out)
{
    JsonNode *node = json_object_get_member(object, name);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64)
        return FALSE;
    *out = json_node_get_int(node);
    return TRUE;
}

/* Do not let a permissive JSON scanner normalize overflow, floating point or
 * leading-zero tokens into accepted schema/size integers. Bound nesting before
 * handing data to JSON-GLib (this schema needs just two object levels). */
static gboolean lexical_guard(const char *data)
{
    unsigned depth = 0;
    gboolean quoted = FALSE;
    for (const char *p = data; *p; ++p) {
        if (quoted) {
            if (*p == '\\') { if (!p[1]) return FALSE; ++p; }
            else if (*p == '"') quoted = FALSE;
            continue;
        }
        if (*p == '"') { quoted = TRUE; continue; }
        if (*p == '{' || *p == '[') { if (++depth > 2) return FALSE; }
        if (*p == '}' || *p == ']') { if (!depth) return FALSE; --depth; }
        if (*p == '-' || *p == '+' || *p == '.') return FALSE;
        if (g_ascii_isdigit(*p)) {
            if (*p == '0' && g_ascii_isdigit(p[1])) return FALSE;
            uint64_t number = 0;
            do {
                unsigned digit = (unsigned)(*p - '0');
                if (number > ((uint64_t)INT64_MAX - digit) / 10) return FALSE;
                number = number * 10 + digit;
                ++p;
            } while (g_ascii_isdigit(*p));
            if (*p && !strchr(",}] \t\r\n", *p)) return FALSE;
            --p;
        }
    }
    return !quoted && depth == 0;
}

gboolean ota_persistence_load(const char *path, OtaPersistentState *out,
                              gboolean *missing, OtaError *error)
{
    char *data = NULL;
    gsize length;
    if (!ota_file_read(path, OTA_STATE_MAX_BYTES, &data, &length, missing, error)) return FALSE;
    if (*missing) return TRUE; /* caller decides whether a fresh IDLE is allowed */
    OtaPersistentState s;
    ota_persistent_state_init(&s);
    JsonParser *parser = json_parser_new();
    ParseGuard guard = {g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL), FALSE};
    g_signal_connect(parser, "object-member", G_CALLBACK(member_seen), &guard);
    gboolean ok = FALSE;
    if (!length || memchr(data, 0, length) || strstr(data, "\\u0000") ||
        !g_utf8_validate(data, length, NULL) || !lexical_guard(data) ||
        !json_parser_load_from_data(parser, data, length, NULL) || guard.duplicate) goto done;
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) goto done;
    JsonObject *object = json_node_get_object(root);
    if (json_object_get_size(object) != 13) goto done;
    gint64 number;
    if (!integer_member(object, "schema_version", &number) || number != OTA_SCHEMA_VERSION) goto done;
    s.schema_version = (int)number;
    if (!integer_member(object, "expected_size", &number) || number < 0) goto done;
    s.expected_size = (uint64_t)number;
    char state[32], previous[8], candidate[8], context[32], code[64];
#define READ(name, field) if (!string_member(object, name, field, sizeof(field))) goto done
    READ("state", state); READ("attempt_id", s.attempt_id);
    READ("target_version", s.target_version); READ("build_id", s.build_id);
    READ("artifact_url", s.artifact_url); READ("expected_sha256", s.expected_sha256);
    READ("previous_slot", previous); READ("expected_candidate_slot", candidate);
    READ("boot_id_before", s.boot_id_before); READ("reboot_context", context);
#undef READ
    s.state = OTA_STATE_COUNT;
    for (unsigned i = 0; i < OTA_STATE_COUNT; ++i)
        if (!strcmp(state, state_names[i])) s.state = (OtaState)i;
    if (*previous && strcmp(previous, "a") && strcmp(previous, "b")) goto done;
    if (*candidate && strcmp(candidate, "a") && strcmp(candidate, "b")) goto done;
    s.previous_slot = !*previous ? OTA_SLOT_UNKNOWN : *previous == 'a' ? OTA_SLOT_A : OTA_SLOT_B;
    s.expected_candidate_slot = !*candidate ? OTA_SLOT_UNKNOWN : *candidate == 'a' ? OTA_SLOT_A : OTA_SLOT_B;
    if (!strcmp(context, "none")) s.reboot_context = OTA_REBOOT_NONE;
    else if (!strcmp(context, "install")) s.reboot_context = OTA_REBOOT_INSTALL;
    else if (!strcmp(context, "health_failure")) s.reboot_context = OTA_REBOOT_HEALTH_FAILURE;
    else goto done;
    JsonNode *last = json_object_get_member(object, "last_error");
    if (!last || !JSON_NODE_HOLDS_OBJECT(last)) goto done;
    JsonObject *last_object = json_node_get_object(last);
    if (json_object_get_size(last_object) != 2 ||
        !string_member(last_object, "code", code, sizeof(code)) ||
        !string_member(last_object, "message", s.last_error.message, sizeof(s.last_error.message))) goto done;
    s.last_error.code = OTA_ERROR_COUNT;
    for (unsigned i = 0; i < OTA_ERROR_COUNT; ++i)
        if (!strcmp(code, ota_error_code_name((OtaErrorCode)i))) s.last_error.code = (OtaErrorCode)i;
    if (!ota_persistent_state_validate(&s, error)) goto done;
    *out = s;
    ok = TRUE;
done:
    if (!ok) ota_error_set(error, OTA_ERROR_PERSISTENCE_FAILED, "Malformed persistent JSON: %s", path);
    g_hash_table_unref(guard.members);
    g_object_unref(parser);
    g_free(data);
    return ok;
}
