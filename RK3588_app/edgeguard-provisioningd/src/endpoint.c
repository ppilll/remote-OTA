#include "edgeguard_provisioning/endpoint.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>

const char *egp_error_code_name(EgpErrorCode code)
{
    switch (code) {
#define EGP_ERROR_CASE(name) case EGP_ERROR_##name: return #name;
        EGP_ERROR_CODES(EGP_ERROR_CASE)
#undef EGP_ERROR_CASE
    default:
        return NULL;
    }
}

const char *egp_provisioning_state_name(EgpProvisioningState state)
{
    static const char *names[] = {
        "UNPROVISIONED", "STORED", "APPLYING", "ASSOCIATING", "CONNECTED",
        "FAILED_CONFIG", "FAILED_AUTH", "FAILED_NOT_FOUND", "FAILED_TIMEOUT",
        "FAILED_DEPENDENCY"
    };
    G_STATIC_ASSERT(G_N_ELEMENTS(names) == EGP_STATE_FAILED_DEPENDENCY + 1);
    return state >= EGP_STATE_UNPROVISIONED && state <= EGP_STATE_FAILED_DEPENDENCY
               ? names[state] : NULL;
}

void egp_error_clear(EgpError *error)
{
    if (error)
        memset(error, 0, sizeof(*error));
}

void egp_error_set(EgpError *error, EgpErrorCode code, gboolean retryable,
                   EgpPersistentChange change, const char *format, ...)
{
    if (!error)
        return;
    memset(error, 0, sizeof(*error));
    error->code = code;
    error->retryable = retryable;
    error->persistent_change = change;
    va_list args;
    va_start(args, format);
    g_vsnprintf(error->message, sizeof(error->message), format, args);
    va_end(args);
}

static gboolean endpoint_fail(EgpError *error)
{
    egp_error_set(error, EGP_ERROR_ENDPOINT_INVALID, TRUE,
                  EGP_PERSISTENT_CHANGE_NONE, "Endpoint is not a valid R5 base URL");
    return FALSE;
}

static gboolean has_forbidden_character(const char *text)
{
    const char *p = text;
    while (*p) {
        gunichar ch = g_utf8_get_char_validated(p, -1);
        if (ch == (gunichar)-1 || ch == (gunichar)-2 || ch == 0 ||
            g_unichar_iscntrl(ch) || g_unichar_isspace(ch))
            return TRUE;
        p = g_utf8_next_char(p);
    }
    return FALSE;
}

static gboolean dns_name_valid(const char *host)
{
    gsize length = strlen(host);
    if (!length || length > 253 || host[0] == '.' || host[length - 1] == '.')
        return FALSE;

    const char *label = host;
    for (const char *p = host;; ++p) {
        if (*p != '.' && *p != '\0') {
            if (!g_ascii_isalnum(*p) && *p != '-')
                return FALSE;
            continue;
        }
        gsize label_length = (gsize)(p - label);
        if (!label_length || label_length > 63 || label[0] == '-' || p[-1] == '-')
            return FALSE;
        if (!*p)
            break;
        label = p + 1;
    }
    return TRUE;
}

static gboolean raw_ipv6_was_bracketed(const char *url)
{
    const char *authority = strstr(url, "://");
    return authority && authority[3] == '[';
}

gboolean egp_endpoint_canonicalize(const char *input,
                                   char output[EGP_ENDPOINT_MAX_BYTES + 1u],
                                   EgpError *error)
{
    if (!input || !output)
        return endpoint_fail(error);

    gsize input_length = strlen(input);
    if (!input_length || input_length > EGP_ENDPOINT_MAX_BYTES ||
        !g_utf8_validate(input, input_length, NULL) ||
        has_forbidden_character(input) || strchr(input, '%') || strchr(input, '\\'))
        return endpoint_fail(error);

    /* Requiring the authority delimiter rejects relative and opaque URIs before
     * parser normalization can make either form look hierarchical. */
    const char *delimiter = strstr(input, "://");
    if (!delimiter || delimiter == input)
        return endpoint_fail(error);
    const char *authority_end = strpbrk(delimiter + 3, "/?#");
    if (!authority_end)
        authority_end = input + input_length;
    if (authority_end == delimiter + 3 || authority_end[-1] == ':')
        return endpoint_fail(error);

    GError *parse_error = NULL;
    GUri *uri = g_uri_parse(input, G_URI_FLAGS_NONE, &parse_error);
    if (!uri) {
        g_clear_error(&parse_error);
        return endpoint_fail(error);
    }

    gboolean ok = FALSE;
    const char *scheme = g_uri_get_scheme(uri);
    const char *host = g_uri_get_host(uri);
    const char *path = g_uri_get_path(uri);
    gint port = g_uri_get_port(uri);
    gboolean http = scheme && !g_ascii_strcasecmp(scheme, "http");
    gboolean https = scheme && !g_ascii_strcasecmp(scheme, "https");

    if ((!http && !https) || !host || !*host || g_uri_get_userinfo(uri) ||
        g_uri_get_query(uri) || g_uri_get_fragment(uri) || port == 0 || port > 65535 ||
        (path && *path && strcmp(path, "/")))
        goto done;

    gboolean is_ip = g_hostname_is_ip_address(host);
    gboolean is_ipv6 = is_ip && strchr(host, ':') != NULL;
    if (strchr(host, '%') || (is_ipv6 && !raw_ipv6_was_bracketed(input)))
        goto done;

    if (!is_ip) {
        gboolean numeric_dotted = TRUE;
        for (const char *p = host; *p; ++p) {
            if (!g_ascii_isdigit(*p) && *p != '.') {
                numeric_dotted = FALSE;
                break;
            }
        }
        if (numeric_dotted || !dns_name_valid(host))
            goto done;
    }

    GInetAddress *inet_address = is_ip ? g_inet_address_new_from_string(host) : NULL;
    char *canonical_host = inet_address ? g_inet_address_to_string(inet_address)
                                        : g_ascii_strdown(host, -1);
    g_clear_object(&inet_address);
    gboolean default_port = port < 0 || (http && port == 80) || (https && port == 443);
    char *canonical = NULL;
    if (is_ipv6 && !default_port)
        canonical = g_strdup_printf("%s://[%s]:%d", http ? "http" : "https",
                                    canonical_host, port);
    else if (is_ipv6)
        canonical = g_strdup_printf("%s://[%s]", http ? "http" : "https",
                                    canonical_host);
    else if (!default_port)
        canonical = g_strdup_printf("%s://%s:%d", http ? "http" : "https",
                                    canonical_host, port);
    else
        canonical = g_strdup_printf("%s://%s", http ? "http" : "https",
                                    canonical_host);
    g_free(canonical_host);

    if (strlen(canonical) <= EGP_ENDPOINT_MAX_BYTES) {
        g_strlcpy(output, canonical, EGP_ENDPOINT_MAX_BYTES + 1u);
        egp_error_clear(error);
        ok = TRUE;
    }
    g_free(canonical);

done:
    g_uri_unref(uri);
    if (!ok)
        endpoint_fail(error);
    return ok;
}

typedef struct {
    GHashTable *members;
    gboolean duplicate;
} RuntimeJsonGuard;

static void runtime_member_seen(JsonParser *parser, JsonObject *object,
                                const char *name, gpointer user_data)
{
    (void)parser;
    RuntimeJsonGuard *guard = user_data;
    char *key = g_strdup_printf("%p:%s", (void *)object, name);
    if (g_hash_table_contains(guard->members, key)) {
        guard->duplicate = TRUE;
        g_free(key);
    } else {
        g_hash_table_add(guard->members, key);
    }
}

static gboolean runtime_integer(JsonObject *object, const char *name, gint64 *out)
{
    JsonNode *node = json_object_get_member(object, name);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_INT64)
        return FALSE;
    *out = json_node_get_int(node);
    return TRUE;
}

static gboolean runtime_string(JsonObject *object, const char *name,
                               char *out, gsize capacity)
{
    JsonNode *node = json_object_get_member(object, name);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING)
        return FALSE;
    const char *value = json_node_get_string(node);
    if (!value || strlen(value) >= capacity)
        return FALSE;
    g_strlcpy(out, value, capacity);
    return TRUE;
}

gboolean egp_runtime_parse_json(const char *data, gsize length,
                                EgpRuntimeConfig *out, EgpError *error)
{
    if (!data || !out || !length || length > EGP_STORE_MAX_BYTES ||
        memchr(data, 0, length) || !g_utf8_validate(data, length, NULL) ||
        g_strstr_len(data, length, "\\u0000"))
        goto invalid;

    memset(out, 0, sizeof(*out));
    JsonParser *parser = json_parser_new();
    RuntimeJsonGuard guard = {
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL), FALSE
    };
    g_signal_connect(parser, "object-member", G_CALLBACK(runtime_member_seen), &guard);
    gboolean parsed = json_parser_load_from_data(parser, data, length, NULL) &&
                      !guard.duplicate;
    JsonNode *root = parsed ? json_parser_get_root(parser) : NULL;
    JsonObject *object = root && JSON_NODE_HOLDS_OBJECT(root)
                             ? json_node_get_object(root) : NULL;
    gint64 schema = 0, generation = 0;
    char raw[EGP_ENDPOINT_MAX_BYTES + 1u] = {0};
    gboolean schema_integer = object && runtime_integer(object, "schema_version", &schema);
    if (schema_integer && schema != EGP_SCHEMA_VERSION) {
        egp_error_set(error, EGP_ERROR_PERSISTENCE_SCHEMA_UNSUPPORTED, FALSE,
                      EGP_PERSISTENT_CHANGE_NONE, "Runtime store schema is unsupported");
        g_hash_table_unref(guard.members);
        g_object_unref(parser);
        return FALSE;
    }
    gboolean ok = object && schema_integer && schema == EGP_SCHEMA_VERSION &&
                  json_object_get_size(object) == 3 &&
                  runtime_integer(object, "generation", &generation) && generation > 0 &&
                  runtime_string(object, "ota_server_base_url", raw, sizeof(raw));
    EgpRuntimeConfig runtime = {0};
    if (ok) {
        runtime.generation = (guint64)generation;
        ok = egp_endpoint_canonicalize(raw, runtime.ota_server_base_url, error) &&
             !strcmp(raw, runtime.ota_server_base_url);
    }
    g_hash_table_unref(guard.members);
    g_object_unref(parser);
    if (!ok)
        goto invalid;
    *out = runtime;
    egp_error_clear(error);
    return TRUE;

invalid:
    if (out)
        memset(out, 0, sizeof(*out));
    egp_error_set(error, EGP_ERROR_ENDPOINT_CONFIG_INVALID, TRUE,
                  EGP_PERSISTENT_CHANGE_NONE, "Canonical runtime endpoint is invalid");
    return FALSE;
}
