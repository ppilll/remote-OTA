#include "edgeguard_provisioning/protocol.h"

#include <string.h>

#define FRAME_FLAG_START 0x01u
#define FRAME_FLAG_END 0x02u
#define COMPLETED_CACHE_LIMIT 64u

typedef struct {
    char *peer_path;
    EgpWritableCharacteristic characteristic;
    guint8 transaction_id[16];
    guint8 opcode;
    guint16 total_length;
    guint8 data[EGP_PROTOCOL_MAX_JSON];
    guint8 received[EGP_PROTOCOL_MAX_JSON];
    guint16 received_count;
    gboolean end_seen;
    gint64 created_us;
    gint64 touched_us;
} Incomplete;

typedef struct {
    char digest[65];
    char result[EGP_PROTOCOL_MAX_JSON + 1u];
    gint64 expires_us;
    gint64 completed_us;
} Completed;

struct _EgpProtocol {
    GHashTable *incomplete;
    GHashTable *completed;
    GHashTable *expired;
};

static void secure_clear(void *memory, gsize length)
{
    volatile guint8 *bytes = memory;
    while (length--)
        *bytes++ = 0;
}

static void incomplete_free(gpointer data)
{
    Incomplete *transaction = data;
    if (!transaction)
        return;
    g_free(transaction->peer_path);
    secure_clear(transaction, sizeof(*transaction));
    g_free(transaction);
}

static void completed_free(gpointer data)
{
    Completed *completed = data;
    if (!completed)
        return;
    secure_clear(completed, sizeof(*completed));
    g_free(completed);
}

static char *incomplete_key(const char *peer_path,
                            EgpWritableCharacteristic characteristic)
{
    return g_strdup_printf("%u:%s", (unsigned)characteristic, peer_path);
}

void egp_transaction_id_format(const guint8 id[16], char output[37])
{
    g_snprintf(output, 37,
               "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
               id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7],
               id[8], id[9], id[10], id[11], id[12], id[13], id[14], id[15]);
}

static char *completed_key(const char *peer_path,
                           EgpWritableCharacteristic characteristic,
                           const guint8 transaction_id[16])
{
    char id[37];
    egp_transaction_id_format(transaction_id, id);
    return g_strdup_printf("%u:%s:%s", (unsigned)characteristic, peer_path, id);
}

static gboolean fail(EgpError *error, EgpErrorCode code, gboolean retryable,
                     const char *message)
{
    egp_error_set(error, code, retryable, EGP_PERSISTENT_CHANGE_NONE,
                  "%s", message);
    return FALSE;
}

static guint16 read_be16(const guint8 *value)
{
    return (guint16)(((guint16)value[0] << 8) | value[1]);
}

static gboolean transaction_id_nonzero(const guint8 id[16])
{
    guint8 any = 0;
    for (guint i = 0; i < 16; ++i)
        any |= id[i];
    return any != 0;
}

const char *egp_opcode_name(guint8 opcode)
{
    switch (opcode) {
    case EGP_OPCODE_SET_WIFI: return "SET_WIFI";
    case EGP_OPCODE_SET_ENDPOINT: return "SET_ENDPOINT";
    case EGP_OPCODE_FORGET_WIFI: return "FORGET_WIFI";
    case EGP_OPCODE_CHECK_UPDATE_NOW: return "CHECK_UPDATE_NOW";
    default: return NULL;
    }
}

static gboolean opcode_allowed(EgpWritableCharacteristic characteristic,
                               guint8 opcode)
{
    if (characteristic == EGP_WRITABLE_PROVISIONING)
        return opcode == EGP_OPCODE_SET_WIFI ||
               opcode == EGP_OPCODE_SET_ENDPOINT ||
               opcode == EGP_OPCODE_FORGET_WIFI;
    return characteristic == EGP_WRITABLE_CONTROL &&
           opcode == EGP_OPCODE_CHECK_UPDATE_NOW;
}

EgpProtocol *egp_protocol_new(void)
{
    EgpProtocol *protocol = g_new0(EgpProtocol, 1);
    protocol->incomplete = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free, incomplete_free);
    protocol->completed = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, completed_free);
    protocol->expired = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, g_free);
    return protocol;
}

void egp_protocol_free(EgpProtocol *protocol)
{
    if (!protocol)
        return;
    g_hash_table_unref(protocol->incomplete);
    g_hash_table_unref(protocol->completed);
    g_hash_table_unref(protocol->expired);
    g_free(protocol);
}

typedef struct {
    gint64 now_us;
    const char *peer_path;
    gboolean all;
} RemoveContext;

static gboolean remove_incomplete(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    Incomplete *transaction = value;
    RemoveContext *context = user_data;
    return context->all ||
           (context->peer_path && !strcmp(context->peer_path, transaction->peer_path)) ||
           (context->now_us > 0 &&
            (context->now_us - transaction->touched_us >= EGP_PROTOCOL_IDLE_TIMEOUT_US ||
             context->now_us - transaction->created_us >= EGP_PROTOCOL_LIFETIME_US));
}

static gboolean remove_completed(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    Completed *completed = value;
    RemoveContext *context = user_data;
    return context->all ||
           (context->now_us > 0 && context->now_us >= completed->expires_us);
}

static gboolean remove_expired_marker(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    gint64 *expires_us = value;
    RemoveContext *context = user_data;
    return context->all ||
           (context->now_us > 0 && context->now_us >= *expires_us);
}

static void remember_timeout(EgpProtocol *protocol, const Incomplete *transaction,
                             gint64 now_us)
{
    while (g_hash_table_size(protocol->expired) >= COMPLETED_CACHE_LIMIT) {
        GHashTableIter iterator;
        gpointer key;
        g_hash_table_iter_init(&iterator, protocol->expired);
        if (!g_hash_table_iter_next(&iterator, &key, NULL))
            break;
        g_hash_table_iter_remove(&iterator);
    }
    char *key = completed_key(transaction->peer_path, transaction->characteristic,
                              transaction->transaction_id);
    gint64 *expires_us = g_new(gint64, 1);
    *expires_us = now_us + EGP_PROTOCOL_LIFETIME_US;
    g_hash_table_replace(protocol->expired, key, expires_us);
}

void egp_protocol_expire(EgpProtocol *protocol, gint64 now_us)
{
    if (!protocol || now_us <= 0)
        return;
    GHashTableIter iterator;
    gpointer value;
    g_hash_table_iter_init(&iterator, protocol->incomplete);
    while (g_hash_table_iter_next(&iterator, NULL, &value)) {
        Incomplete *transaction = value;
        if (now_us - transaction->touched_us >= EGP_PROTOCOL_IDLE_TIMEOUT_US ||
            now_us - transaction->created_us >= EGP_PROTOCOL_LIFETIME_US) {
            remember_timeout(protocol, transaction, now_us);
            g_hash_table_iter_remove(&iterator);
        }
    }
    RemoveContext context = { .now_us = now_us };
    g_hash_table_foreach_remove(protocol->completed, remove_completed, &context);
    g_hash_table_foreach_remove(protocol->expired, remove_expired_marker, &context);
}

void egp_protocol_drop_peer(EgpProtocol *protocol, const char *peer_path)
{
    if (!protocol || !peer_path)
        return;
    RemoveContext context = { .peer_path = peer_path };
    g_hash_table_foreach_remove(protocol->incomplete, remove_incomplete, &context);

    GHashTableIter iterator;
    gpointer key;
    g_hash_table_iter_init(&iterator, protocol->expired);
    while (g_hash_table_iter_next(&iterator, &key, NULL)) {
        const char *text = key;
        const char *first = strchr(text, ':');
        const char *last = strrchr(text, ':');
        if (first && last && last > first + 1 &&
            strlen(peer_path) == (gsize)(last - first - 1) &&
            !memcmp(first + 1, peer_path, (gsize)(last - first - 1)))
            g_hash_table_iter_remove(&iterator);
    }
}

void egp_protocol_drop_all(EgpProtocol *protocol)
{
    if (!protocol)
        return;
    g_hash_table_remove_all(protocol->incomplete);
    g_hash_table_remove_all(protocol->completed);
    g_hash_table_remove_all(protocol->expired);
}

static void digest_request(const char *peer_path,
                           EgpWritableCharacteristic characteristic,
                           guint8 opcode, guint16 length, const guint8 *data,
                           char digest[65])
{
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    guint8 characteristic_byte = (guint8)characteristic;
    guint8 length_bytes[2] = { (guint8)(length >> 8), (guint8)length };
    g_checksum_update(checksum, (const guchar *)peer_path, strlen(peer_path));
    g_checksum_update(checksum, &characteristic_byte, 1);
    g_checksum_update(checksum, &opcode, 1);
    g_checksum_update(checksum, length_bytes, sizeof(length_bytes));
    if (length)
        g_checksum_update(checksum, data, length);
    g_strlcpy(digest, g_checksum_get_string(checksum), 65);
    g_checksum_free(checksum);
}

static void prune_completed_limit(EgpProtocol *protocol)
{
    while (g_hash_table_size(protocol->completed) >= COMPLETED_CACHE_LIMIT) {
        GHashTableIter iterator;
        gpointer key, value;
        gpointer oldest_key = NULL;
        gint64 oldest = G_MAXINT64;
        g_hash_table_iter_init(&iterator, protocol->completed);
        while (g_hash_table_iter_next(&iterator, &key, &value)) {
            Completed *entry = value;
            if (entry->completed_us < oldest) {
                oldest = entry->completed_us;
                oldest_key = key;
            }
        }
        if (!oldest_key)
            return;
        g_hash_table_remove(protocol->completed, oldest_key);
    }
}

static gboolean complete_transaction(EgpProtocol *protocol, const char *key,
                                     Incomplete *transaction, gint64 now_us,
                                     EgpProtocolOutput *output, EgpError *error)
{
    if (!g_utf8_validate((const char *)transaction->data,
                         transaction->total_length, NULL) ||
        memchr(transaction->data, 0, transaction->total_length)) {
        g_hash_table_remove(protocol->incomplete, key);
        return fail(error, EGP_ERROR_BLE_PAYLOAD_INVALID, TRUE,
                    "Request JSON is not valid NUL-free UTF-8");
    }

    char digest[65];
    digest_request(transaction->peer_path, transaction->characteristic,
                   transaction->opcode, transaction->total_length,
                   transaction->data, digest);
    char *cache_key = completed_key(transaction->peer_path,
                                    transaction->characteristic,
                                    transaction->transaction_id);
    Completed *completed = g_hash_table_lookup(protocol->completed, cache_key);
    if (completed && strcmp(completed->digest, digest)) {
        g_free(cache_key);
        g_hash_table_remove(protocol->incomplete, key);
        return fail(error, EGP_ERROR_BLE_REPLAY_REJECTED, FALSE,
                    "Completed transaction identifier was reused with different content");
    }

    memset(output, 0, sizeof(*output));
    output->disposition = completed ? EGP_PROTOCOL_REPLAY : EGP_PROTOCOL_COMPLETE;
    g_strlcpy(output->request.peer_path, transaction->peer_path,
              sizeof(output->request.peer_path));
    output->request.characteristic = transaction->characteristic;
    memcpy(output->request.transaction_id, transaction->transaction_id, 16);
    output->request.opcode = transaction->opcode;
    output->request.json_length = transaction->total_length;
    memcpy(output->request.json, transaction->data, transaction->total_length);
    output->request.json[transaction->total_length] = '\0';

    if (completed) {
        g_strlcpy(output->cached_result, completed->result,
                  sizeof(output->cached_result));
        g_free(cache_key);
    } else {
        prune_completed_limit(protocol);
        completed = g_new0(Completed, 1);
        g_strlcpy(completed->digest, digest, sizeof(completed->digest));
        completed->completed_us = now_us;
        completed->expires_us = now_us + EGP_PROTOCOL_REPLAY_TTL_US;
        g_hash_table_insert(protocol->completed, cache_key, completed);
    }
    g_hash_table_remove(protocol->incomplete, key);
    egp_error_clear(error);
    return TRUE;
}

gboolean egp_protocol_accept(EgpProtocol *protocol, const char *peer_path,
                             EgpWritableCharacteristic characteristic,
                             const guint8 *value, gsize value_length,
                             gint64 now_us, EgpProtocolOutput *output,
                             EgpError *error)
{
    if (!protocol || !peer_path || strlen(peer_path) >= EGP_DBUS_PATH_CAP ||
        !g_variant_is_object_path(peer_path) ||
        !value || !output || now_us <= 0)
        return fail(error, EGP_ERROR_BLE_PAYLOAD_INVALID, TRUE,
                    "Protocol input is invalid");
    memset(output, 0, sizeof(*output));
    output->disposition = EGP_PROTOCOL_INCOMPLETE;

    if (value_length < EGP_FRAME_HEADER_BYTES)
        return fail(error, EGP_ERROR_BLE_PAYLOAD_INVALID, TRUE,
                    "Frame is shorter than the fixed header");
    if (memcmp(value, "EGP1", 4) || value[4] != EGP_PROTOCOL_VERSION)
        return fail(error, EGP_ERROR_BLE_PROTOCOL_UNSUPPORTED, FALSE,
                    "Protocol magic or version is unsupported");
    guint8 opcode = value[5];
    guint8 flags = value[6];
    if ((flags & ~(FRAME_FLAG_START | FRAME_FLAG_END)) || value[7] ||
        value[30] || value[31] || !opcode_allowed(characteristic, opcode))
        return fail(error, EGP_ERROR_BLE_PAYLOAD_INVALID, TRUE,
                    "Frame flags, reserved fields, or opcode are invalid");

    guint16 total = read_be16(value + 24);
    guint16 offset = read_be16(value + 26);
    guint16 fragment = read_be16(value + 28);
    if (total > EGP_PROTOCOL_MAX_JSON)
        return fail(error, EGP_ERROR_BLE_PAYLOAD_TOO_LARGE, TRUE,
                    "Logical request exceeds the protocol bound");
    if ((gsize)fragment != value_length - EGP_FRAME_HEADER_BYTES ||
        offset > total || fragment > total - offset ||
        !transaction_id_nonzero(value + 8) ||
        ((flags & FRAME_FLAG_START) && offset != 0) ||
        ((flags & FRAME_FLAG_END) && (guint)offset + fragment != total))
        return fail(error, EGP_ERROR_BLE_PAYLOAD_INVALID, TRUE,
                    "Frame lengths, offset, or transaction identifier are invalid");

    char *key = incomplete_key(peer_path, characteristic);
    Incomplete *transaction = g_hash_table_lookup(protocol->incomplete, key);
    if (transaction &&
        (now_us - transaction->touched_us >= EGP_PROTOCOL_IDLE_TIMEOUT_US ||
         now_us - transaction->created_us >= EGP_PROTOCOL_LIFETIME_US)) {
        gboolean same_id = !memcmp(transaction->transaction_id, value + 8, 16);
        remember_timeout(protocol, transaction, now_us);
        g_hash_table_remove(protocol->incomplete, key);
        if (same_id) {
            g_free(key);
            return fail(error, EGP_ERROR_BLE_TRANSACTION_TIMEOUT, TRUE,
                        "Incomplete transaction expired");
        }
        transaction = NULL;
    }
    egp_protocol_expire(protocol, now_us);
    char *timeout_key = completed_key(peer_path, characteristic, value + 8);
    gboolean timed_out = g_hash_table_contains(protocol->expired, timeout_key);
    g_free(timeout_key);
    if (timed_out) {
        g_free(key);
        return fail(error, EGP_ERROR_BLE_TRANSACTION_TIMEOUT, TRUE,
                    "Incomplete transaction expired");
    }
    transaction = g_hash_table_lookup(protocol->incomplete, key);
    if (!transaction) {
        if (!(flags & FRAME_FLAG_START)) {
            g_free(key);
            return fail(error, EGP_ERROR_BLE_FRAGMENT_CONFLICT, TRUE,
                        "First fragment must carry START");
        }
        if (g_hash_table_size(protocol->incomplete) >= EGP_PROTOCOL_MAX_INCOMPLETE) {
            g_free(key);
            return fail(error, EGP_ERROR_PROVISIONING_BUSY, TRUE,
                        "Global incomplete transaction limit reached");
        }
        transaction = g_new0(Incomplete, 1);
        transaction->peer_path = g_strdup(peer_path);
        transaction->characteristic = characteristic;
        memcpy(transaction->transaction_id, value + 8, 16);
        transaction->opcode = opcode;
        transaction->total_length = total;
        transaction->created_us = now_us;
        transaction->touched_us = now_us;
        g_hash_table_insert(protocol->incomplete, g_strdup(key), transaction);
    } else if (memcmp(transaction->transaction_id, value + 8, 16) ||
               transaction->opcode != opcode || transaction->total_length != total) {
        g_free(key);
        return fail(error, EGP_ERROR_PROVISIONING_BUSY, TRUE,
                    "Peer already has an incomplete transaction on this characteristic");
    }

    gboolean conflict = FALSE;
    for (guint i = 0; i < fragment; ++i) {
        guint position = (guint)offset + i;
        guint8 byte = value[EGP_FRAME_HEADER_BYTES + i];
        if (transaction->received[position]) {
            if (transaction->data[position] != byte) {
                conflict = TRUE;
                break;
            }
        } else {
            transaction->received[position] = 1;
            transaction->data[position] = byte;
            ++transaction->received_count;
        }
    }
    if (conflict) {
        g_hash_table_remove(protocol->incomplete, key);
        g_free(key);
        return fail(error, EGP_ERROR_BLE_FRAGMENT_CONFLICT, TRUE,
                    "Overlapping fragment bytes conflict");
    }
    transaction->touched_us = now_us;
    if (flags & FRAME_FLAG_END)
        transaction->end_seen = TRUE;
    if (transaction->end_seen && transaction->received_count != total) {
        g_hash_table_remove(protocol->incomplete, key);
        g_free(key);
        return fail(error, EGP_ERROR_BLE_FRAGMENT_CONFLICT, TRUE,
                    "END arrived while the logical request still has gaps");
    }
    if (!transaction->end_seen) {
        g_free(key);
        egp_error_clear(error);
        return TRUE;
    }
    gboolean ok = complete_transaction(protocol, key, transaction, now_us,
                                       output, error);
    g_free(key);
    return ok;
}

gboolean egp_protocol_cache_result(EgpProtocol *protocol,
                                   const EgpProtocolRequest *request,
                                   const char *result_json, gint64 now_us,
                                   EgpError *error)
{
    if (!protocol || !request || !result_json || now_us <= 0 ||
        strlen(result_json) > EGP_PROTOCOL_MAX_JSON ||
        !g_utf8_validate(result_json, -1, NULL))
        return fail(error, EGP_ERROR_INTERNAL_ERROR, TRUE,
                    "Result cache input is invalid");
    egp_protocol_expire(protocol, now_us);
    char *key = completed_key(request->peer_path, request->characteristic,
                              request->transaction_id);
    Completed *completed = g_hash_table_lookup(protocol->completed, key);
    g_free(key);
    if (!completed)
        return fail(error, EGP_ERROR_BLE_REPLAY_REJECTED, TRUE,
                    "Completed request cache entry no longer exists");
    char digest[65];
    digest_request(request->peer_path, request->characteristic, request->opcode,
                   request->json_length, (const guint8 *)request->json, digest);
    if (strcmp(completed->digest, digest))
        return fail(error, EGP_ERROR_BLE_REPLAY_REJECTED, FALSE,
                    "Result does not match the completed request digest");
    return egp_protocol_cache_result_id(protocol, request->peer_path,
                                        request->characteristic,
                                        request->transaction_id, result_json,
                                        now_us, error);
}

gboolean egp_protocol_cache_result_id(EgpProtocol *protocol,
                                      const char *peer_path,
                                      EgpWritableCharacteristic characteristic,
                                      const guint8 transaction_id[16],
                                      const char *result_json, gint64 now_us,
                                      EgpError *error)
{
    if (!protocol || !peer_path || !transaction_id || !result_json ||
        now_us <= 0 || strlen(result_json) > EGP_PROTOCOL_MAX_JSON ||
        !g_utf8_validate(result_json, -1, NULL))
        return fail(error, EGP_ERROR_INTERNAL_ERROR, TRUE,
                    "Result cache input is invalid");
    egp_protocol_expire(protocol, now_us);
    char *key = completed_key(peer_path, characteristic, transaction_id);
    Completed *completed = g_hash_table_lookup(protocol->completed, key);
    g_free(key);
    if (!completed)
        return fail(error, EGP_ERROR_BLE_REPLAY_REJECTED, TRUE,
                    "Completed request cache entry no longer exists");
    g_strlcpy(completed->result, result_json, sizeof(completed->result));
    completed->expires_us = now_us + EGP_PROTOCOL_REPLAY_TTL_US;
    egp_error_clear(error);
    return TRUE;
}
