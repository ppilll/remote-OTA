#include "edgeguard_provisioning/operations.h"

#include <json-glib/json-glib.h>
#include <string.h>

#define CONNMAN_TIMEOUT_MS 30000u

typedef struct {
    EgpOperations *operations;
    EgpProtocolRequest request;
    char result[EGP_PROTOCOL_MAX_JSON + 1u];
} Work;

struct _EgpOperations {
    GMutex lock;
    gboolean busy;
    gboolean closing;
    EgpStore *store;
    EgpConnman *connman;
    EgpStatus *status;
    EgpAuthorizeCommit authorize_commit;
    EgpResultPublished result_published;
    gpointer user_data;
};

typedef struct {
    GHashTable *members;
    gboolean duplicate;
} JsonGuard;

static void member_seen(JsonParser *parser, JsonObject *object,
                        const char *name, gpointer user_data)
{
    (void)parser;
    JsonGuard *guard = user_data;
    char *key = g_strdup_printf("%p:%s", (void *)object, name);
    if (g_hash_table_contains(guard->members, key)) {
        guard->duplicate = TRUE;
        g_free(key);
    } else {
        g_hash_table_add(guard->members, key);
    }
}

static JsonParser *parse_request_object(const EgpProtocolRequest *request,
                                        JsonObject **object, EgpError *error)
{
    if (!request->json_length || request->json_length > EGP_PROTOCOL_MAX_JSON ||
        request->json[request->json_length] != '\0' ||
        !g_utf8_validate(request->json, request->json_length, NULL)) {
        egp_error_set(error, EGP_ERROR_BLE_PAYLOAD_INVALID, TRUE,
                      EGP_PERSISTENT_CHANGE_NONE, "Request JSON is invalid");
        return NULL;
    }
    JsonParser *parser = json_parser_new();
    JsonGuard guard = {
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL), FALSE
    };
    g_signal_connect(parser, "object-member", G_CALLBACK(member_seen), &guard);
    gboolean ok = json_parser_load_from_data(parser, request->json,
                                             request->json_length, NULL) &&
                  !guard.duplicate;
    JsonNode *root = ok ? json_parser_get_root(parser) : NULL;
    ok = root && JSON_NODE_HOLDS_OBJECT(root);
    g_hash_table_unref(guard.members);
    if (!ok) {
        g_object_unref(parser);
        egp_error_set(error, EGP_ERROR_BLE_PAYLOAD_INVALID, TRUE,
                      EGP_PERSISTENT_CHANGE_NONE,
                      "Request must be one JSON object without duplicate members");
        return NULL;
    }
    *object = json_node_get_object(root);
    return parser;
}

static gboolean exact_string(JsonObject *object, const char *name,
                             char *output, gsize capacity)
{
    JsonNode *node = json_object_get_member(object, name);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING)
        return FALSE;
    const char *value = json_node_get_string(node);
    if (!value || strlen(value) >= capacity)
        return FALSE;
    g_strlcpy(output, value, capacity);
    return TRUE;
}

static gboolean exact_boolean(JsonObject *object, const char *name,
                              gboolean *output)
{
    JsonNode *node = json_object_get_member(object, name);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_BOOLEAN)
        return FALSE;
    *output = json_node_get_boolean(node);
    return TRUE;
}

static gboolean parse_wifi(const EgpProtocolRequest *request,
                           EgpWifiConfig *wifi, EgpError *error)
{
    JsonObject *object = NULL;
    JsonParser *parser = parse_request_object(request, &object, error);
    if (!parser)
        return FALSE;
    memset(wifi, 0, sizeof(*wifi));
    gboolean ok = json_object_get_size(object) == 4 &&
                  exact_string(object, "ssid", wifi->ssid, sizeof(wifi->ssid)) &&
                  exact_string(object, "security", wifi->security,
                               sizeof(wifi->security)) &&
                  exact_string(object, "passphrase", wifi->passphrase,
                               sizeof(wifi->passphrase)) &&
                  exact_boolean(object, "hidden", &wifi->hidden) &&
                  egp_wifi_validate(wifi, error);
    g_object_unref(parser);
    if (!ok) {
        egp_wifi_clear(wifi);
        if (!error || error->code == EGP_ERROR_NONE)
            egp_error_set(error, EGP_ERROR_WIFI_CONFIG_INVALID, TRUE,
                          EGP_PERSISTENT_CHANGE_NONE,
                          "SET_WIFI fields are invalid");
    }
    return ok;
}

static gboolean parse_endpoint(const EgpProtocolRequest *request,
                               char endpoint[EGP_ENDPOINT_MAX_BYTES + 1u],
                               EgpError *error)
{
    JsonObject *object = NULL;
    JsonParser *parser = parse_request_object(request, &object, error);
    if (!parser)
        return FALSE;
    gboolean ok = json_object_get_size(object) == 1 &&
                  exact_string(object, "base_url", endpoint,
                               EGP_ENDPOINT_MAX_BYTES + 1u);
    g_object_unref(parser);
    if (!ok)
        egp_error_set(error, EGP_ERROR_ENDPOINT_INVALID, TRUE,
                      EGP_PERSISTENT_CHANGE_NONE,
                      "SET_ENDPOINT fields are invalid");
    return ok;
}

static gboolean parse_empty(const EgpProtocolRequest *request, EgpError *error)
{
    JsonObject *object = NULL;
    JsonParser *parser = parse_request_object(request, &object, error);
    if (!parser)
        return FALSE;
    gboolean ok = json_object_get_size(object) == 0;
    g_object_unref(parser);
    if (!ok)
        egp_error_set(error, EGP_ERROR_BLE_PAYLOAD_INVALID, TRUE,
                      EGP_PERSISTENT_CHANGE_NONE,
                      "Operation accepts no JSON members");
    return ok;
}

static const char *change_name(EgpPersistentChange change)
{
    switch (change) {
    case EGP_PERSISTENT_CHANGE_NONE: return "NONE";
    case EGP_PERSISTENT_CHANGE_COMMITTED: return "COMMITTED";
    case EGP_PERSISTENT_CHANGE_ROLLED_BACK: return "ROLLED_BACK";
    case EGP_PERSISTENT_CHANGE_UNCERTAIN: return "UNCERTAIN";
    default: return "NONE";
    }
}

static void serialize_result(const EgpProtocolRequest *request,
                             const char *status, const EgpError *error,
                             char output[EGP_PROTOCOL_MAX_JSON + 1u])
{
    char transaction_id[37];
    egp_transaction_id_format(request->transaction_id, transaction_id);
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
#define ADD_STRING(name, value) do { \
    json_builder_set_member_name(builder, (name)); \
    json_builder_add_string_value(builder, (value)); \
} while (0)
    json_builder_set_member_name(builder, "schema_version");
    json_builder_add_int_value(builder, EGP_SCHEMA_VERSION);
    ADD_STRING("transaction_id", transaction_id);
    ADD_STRING("operation", egp_opcode_name(request->opcode));
    ADD_STRING("status", status);
    ADD_STRING("error_code", egp_error_code_name(error->code));
    json_builder_set_member_name(builder, "retryable");
    json_builder_add_boolean_value(builder, error->retryable);
    ADD_STRING("persistent_change", change_name(error->persistent_change));
    ADD_STRING("message", error->message);
    json_builder_end_object(builder);
#undef ADD_STRING
    JsonNode *root = json_builder_get_root(builder);
    JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, root);
    gsize length = 0;
    char *json = json_generator_to_data(generator, &length);
    if (json && length <= EGP_PROTOCOL_MAX_JSON)
        g_strlcpy(output, json, EGP_PROTOCOL_MAX_JSON + 1u);
    else
        g_strlcpy(output,
                  "{\"schema_version\":1,\"operation\":\"UNKNOWN\",\"status\":\"FAILED\",\"error_code\":\"INTERNAL_ERROR\",\"retryable\":true,\"persistent_change\":\"NONE\",\"message\":\"Result serialization failed\"}",
                  EGP_PROTOCOL_MAX_JSON + 1u);
    g_free(json);
    json_node_free(root);
    g_object_unref(generator);
    g_object_unref(builder);
}

static gboolean stable_idle(EgpOperations *operations, EgpError *error)
{
    char first[32] = {0}, second[32] = {0};
    if (!egp_status_agent_idle(operations->status, first, error))
        return FALSE;
    g_usleep(50 * 1000u);
    if (!egp_status_agent_idle(operations->status, second, error))
        return FALSE;
    if (strcmp(first, second) || strcmp(second, "IDLE")) {
        egp_error_set(error, EGP_ERROR_PROVISIONING_BUSY, TRUE,
                      EGP_PERSISTENT_CHANGE_NONE,
                      "Agent state did not remain stable at IDLE");
        return FALSE;
    }
    return TRUE;
}

static gboolean authorize_and_idle(Work *work, EgpError *error)
{
    EgpOperations *operations = work->operations;
    if (!operations->authorize_commit ||
        !operations->authorize_commit(operations->user_data,
                                      work->request.peer_path, error))
        return FALSE;
    return stable_idle(operations, error);
}

static void set_success(EgpError *error, EgpPersistentChange change,
                        const char *message)
{
    egp_error_set(error, EGP_ERROR_NONE, FALSE, change, "%s", message);
}

static void apply_wifi(Work *work, EgpError *result)
{
    EgpOperations *operations = work->operations;
    EgpWifiConfig candidate = {0}, previous = {0};
    gboolean previous_present = FALSE;
    guint64 generation = 0;
    if (!parse_wifi(&work->request, &candidate, result) ||
        !authorize_and_idle(work, result))
        goto done;
    egp_status_set_provisioning(operations->status, EGP_STATE_APPLYING, FALSE);
    if (!egp_store_replace_wifi(operations->store, &candidate, &previous,
                                &previous_present, &generation, result))
        goto done;

    char observation[32];
    gboolean raced = !egp_status_agent_idle(operations->status, observation, result);
    EgpError apply_error = {0};
    EgpConnmanResult connman_result = {0};
    gboolean applied = egp_connman_apply(operations->connman, &candidate,
                                         CONNMAN_TIMEOUT_MS, NULL,
                                         &connman_result, &apply_error);
    egp_status_set_provisioning(operations->status, connman_result.state,
                                applied && connman_result.state == EGP_STATE_CONNECTED);
    if (applied) {
        if (raced)
            egp_error_set(result, EGP_ERROR_PROVISIONING_RACE, TRUE,
                          EGP_PERSISTENT_CHANGE_COMMITTED,
                          "Agent state changed after Wi-Fi commit; canonical reconciliation completed");
        else
            set_success(result, EGP_PERSISTENT_CHANGE_COMMITTED,
                        "Wi-Fi configuration committed and connected");
        goto done;
    }

    *result = apply_error;
    result->persistent_change = EGP_PERSISTENT_CHANGE_COMMITTED;
    if (connman_result.disposition == EGP_CONNMAN_ROLLBACK_ALLOWED) {
        EgpError restore_error = {0};
        guint64 restored_generation = 0;
        if (!egp_store_restore_wifi(operations->store, &previous,
                                    previous_present, &restored_generation,
                                    &restore_error)) {
            *result = restore_error;
            goto done;
        }
        gboolean reconciled;
        if (previous_present) {
            EgpConnmanResult ignored = {0};
            EgpError ignored_error = {0};
            reconciled = egp_connman_apply(operations->connman, &previous,
                                           CONNMAN_TIMEOUT_MS, NULL,
                                           &ignored, &ignored_error);
            egp_status_set_provisioning(operations->status, ignored.state,
                                        reconciled && ignored.state == EGP_STATE_CONNECTED);
        } else {
            EgpError ignored_error = {0};
            reconciled = egp_connman_remove_profile(operations->connman,
                                                    &ignored_error);
            egp_status_set_provisioning(operations->status,
                                        EGP_STATE_UNPROVISIONED, FALSE);
        }
        if (reconciled)
            result->persistent_change = EGP_PERSISTENT_CHANGE_ROLLED_BACK;
        else
            result->persistent_change = EGP_PERSISTENT_CHANGE_UNCERTAIN;
    }
done:
    egp_wifi_clear(&candidate);
    egp_wifi_clear(&previous);
}

static void set_endpoint(Work *work, EgpError *result)
{
    char endpoint[EGP_ENDPOINT_MAX_BYTES + 1u] = {0};
    EgpRuntimeConfig previous = {0}, committed = {0};
    gboolean previous_present = FALSE;
    if (!parse_endpoint(&work->request, endpoint, result) ||
        !authorize_and_idle(work, result))
        goto done;
    if (!egp_store_replace_runtime(work->operations->store, endpoint, &previous,
                                   &previous_present, &committed, result))
        goto done;
    char observation[32];
    if (!egp_status_agent_idle(work->operations->status, observation, result))
        egp_error_set(result, EGP_ERROR_PROVISIONING_RACE, TRUE,
                      EGP_PERSISTENT_CHANGE_COMMITTED,
                      "Agent state changed after endpoint commit");
    else
        set_success(result, EGP_PERSISTENT_CHANGE_COMMITTED,
                    "Runtime endpoint committed");
    egp_status_refresh(work->operations->status);
done:
    memset(endpoint, 0, sizeof(endpoint));
    memset(&previous, 0, sizeof(previous));
    memset(&committed, 0, sizeof(committed));
}

static void forget_wifi(Work *work, EgpError *result)
{
    EgpOperations *operations = work->operations;
    EgpWifiConfig forgotten = {0};
    gboolean had_wifi = FALSE;
    if (!parse_empty(&work->request, result) ||
        !authorize_and_idle(work, result))
        goto done;
    if (!egp_store_forget_begin(operations->store, &forgotten,
                                &had_wifi, result))
        goto done;
    char observation[32];
    gboolean raced = !egp_status_agent_idle(operations->status, observation, result);
    EgpConnmanResult connman_result = {0};
    gboolean revoked;
    if (had_wifi) {
        revoked = egp_connman_revoke(operations->connman, &forgotten,
                                     CONNMAN_TIMEOUT_MS, NULL,
                                     &connman_result, result);
    } else {
        revoked = egp_connman_remove_profile(operations->connman, result);
    }
    if (!revoked) {
        if (had_wifi)
            result->persistent_change = EGP_PERSISTENT_CHANGE_COMMITTED;
        goto done;
    }
    if (had_wifi && !egp_store_forget_finish(operations->store, result))
        goto done;
    egp_status_set_provisioning(operations->status, EGP_STATE_UNPROVISIONED, FALSE);
    if (raced)
        egp_error_set(result, EGP_ERROR_PROVISIONING_RACE, TRUE,
                      EGP_PERSISTENT_CHANGE_COMMITTED,
                      "Agent state changed after forget began; revocation completed");
    else
        set_success(result, EGP_PERSISTENT_CHANGE_COMMITTED,
                    "Provisioned Wi-Fi credential was forgotten");
done:
    egp_wifi_clear(&forgotten);
}

static void check_update(Work *work, EgpError *result)
{
    if (!parse_empty(&work->request, result) ||
        !authorize_and_idle(work, result))
        return;
    if (egp_status_check_update_now(work->operations->status,
                                    work->request.transaction_id, result))
        set_success(result, EGP_PERSISTENT_CHANGE_NONE,
                    "OTA check request accepted by the Agent");
}

static void worker(GTask *task, gpointer source_object, gpointer task_data,
                   GCancellable *cancellable)
{
    (void)source_object;
    (void)cancellable;
    Work *work = task_data;
    EgpError result = {0};
    switch (work->request.opcode) {
    case EGP_OPCODE_SET_WIFI:
        apply_wifi(work, &result);
        break;
    case EGP_OPCODE_SET_ENDPOINT:
        set_endpoint(work, &result);
        break;
    case EGP_OPCODE_FORGET_WIFI:
        forget_wifi(work, &result);
        break;
    case EGP_OPCODE_CHECK_UPDATE_NOW:
        check_update(work, &result);
        break;
    default:
        egp_error_set(&result, EGP_ERROR_BLE_PROTOCOL_UNSUPPORTED, FALSE,
                      EGP_PERSISTENT_CHANGE_NONE, "Opcode is unsupported");
        break;
    }
    serialize_result(&work->request,
                     result.code == EGP_ERROR_NONE ? "SUCCEEDED" : "FAILED",
                     &result, work->result);
    g_task_return_boolean(task, TRUE);
}

static void work_free(gpointer data)
{
    Work *work = data;
    if (!work)
        return;
    memset(&work->request, 0, sizeof(work->request));
    memset(work->result, 0, sizeof(work->result));
    g_free(work);
}

static void completed(GObject *source_object, GAsyncResult *result,
                      gpointer user_data)
{
    (void)source_object;
    EgpOperations *operations = user_data;
    GTask *task = G_TASK(result);
    Work *work = g_task_get_task_data(task);
    g_task_propagate_boolean(task, NULL);
    if (operations->result_published)
        operations->result_published(operations->user_data,
                                     work->request.peer_path, work->result);
    g_mutex_lock(&operations->lock);
    operations->busy = FALSE;
    g_mutex_unlock(&operations->lock);
}

EgpOperations *egp_operations_new(EgpStore *store, EgpConnman *connman,
                                  EgpStatus *status,
                                  EgpAuthorizeCommit authorize_commit,
                                  EgpResultPublished result_published,
                                  gpointer user_data)
{
    if (!store || !connman || !status || !authorize_commit || !result_published)
        return NULL;
    EgpOperations *operations = g_new0(EgpOperations, 1);
    g_mutex_init(&operations->lock);
    operations->store = store;
    operations->connman = connman;
    operations->status = status;
    operations->authorize_commit = authorize_commit;
    operations->result_published = result_published;
    operations->user_data = user_data;
    return operations;
}

gboolean egp_operations_submit(EgpOperations *operations,
                               const EgpProtocolRequest *request,
                               EgpError *error)
{
    if (!operations || !request)
        return FALSE;
    g_mutex_lock(&operations->lock);
    if (operations->busy || operations->closing) {
        g_mutex_unlock(&operations->lock);
        egp_error_set(error, EGP_ERROR_PROVISIONING_BUSY, TRUE,
                      EGP_PERSISTENT_CHANGE_NONE,
                      "Another provisioning operation is in progress");
        return FALSE;
    }
    operations->busy = TRUE;
    g_mutex_unlock(&operations->lock);

    Work *work = g_new0(Work, 1);
    work->operations = operations;
    work->request = *request;
    EgpError accepted = {0};
    egp_error_set(&accepted, EGP_ERROR_NONE, FALSE,
                  EGP_PERSISTENT_CHANGE_NONE, "Operation accepted");
    char accepted_json[EGP_PROTOCOL_MAX_JSON + 1u] = {0};
    serialize_result(request, "ACCEPTED", &accepted, accepted_json);
    operations->result_published(operations->user_data, request->peer_path,
                                 accepted_json);

    GTask *task = g_task_new(NULL, NULL, completed, operations);
    g_task_set_task_data(task, work, work_free);
    g_task_run_in_thread(task, worker);
    g_object_unref(task);
    egp_error_clear(error);
    return TRUE;
}

gboolean egp_operations_busy(EgpOperations *operations)
{
    if (!operations)
        return FALSE;
    g_mutex_lock(&operations->lock);
    gboolean busy = operations->busy;
    g_mutex_unlock(&operations->lock);
    return busy;
}

void egp_operations_free(EgpOperations *operations)
{
    if (!operations)
        return;
    g_mutex_lock(&operations->lock);
    operations->closing = TRUE;
    g_mutex_unlock(&operations->lock);
    while (egp_operations_busy(operations))
        g_main_context_iteration(NULL, TRUE);
    g_mutex_clear(&operations->lock);
    memset(operations, 0, sizeof(*operations));
    g_free(operations);
}
