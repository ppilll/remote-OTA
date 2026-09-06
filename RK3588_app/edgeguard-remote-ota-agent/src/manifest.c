#include "edgeguard_ota/manifest.h"
#include "edgeguard_ota/version.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <json-glib/json-glib.h>

/* JSON-GLib owns the data model. This lexical guard rejects permissive syntax,
 * duplicate decoded keys and lossy integer conversions before loading it. */
typedef struct { const char *p, *end; unsigned depth; } Lex;
static void ws(Lex *l) { while (l->p < l->end && strchr(" \t\r\n", *l->p)) ++l->p; }
static gboolean hex4(Lex *l, gunichar *u)
{
    *u = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (l->p == l->end || !g_ascii_isxdigit(*l->p)) return FALSE;
        *u = *u * 16 + g_ascii_xdigit_value(*l->p++);
    }
    return TRUE;
}
static gboolean string_token(Lex *l, GString *decoded, gboolean *nul)
{
    if (l->p == l->end || *l->p++ != '"') return FALSE;
    *nul = FALSE;
    while (l->p < l->end) {
        unsigned char c = (unsigned char)*l->p++;
        if (c == '"') return TRUE;
        if (c < 0x20) return FALSE;
        if (c != '\\') { if (decoded) g_string_append_c(decoded, c); continue; }
        if (l->p == l->end) return FALSE;
        c = (unsigned char)*l->p++;
        if (c == 'u') {
            gunichar u;
            if (!hex4(l, &u)) return FALSE;
            if (u >= 0xd800 && u <= 0xdbff) {
                gunichar low;
                if (l->end - l->p < 6 || l->p[0] != '\\' || l->p[1] != 'u') return FALSE;
                l->p += 2;
                if (!hex4(l, &low) || low < 0xdc00 || low > 0xdfff) return FALSE;
                u = 0x10000 + ((u - 0xd800) << 10) + low - 0xdc00;
            } else if (u >= 0xdc00 && u <= 0xdfff) return FALSE;
            if (!u) *nul = TRUE;
            if (decoded) g_string_append_unichar(decoded, u);
        } else {
            const char *esc = "\"\\/bfnrt", *v = strchr(esc, c);
            const char chars[] = {'"', '\\', '/', '\b', '\f', '\n', '\r', '\t'};
            if (!v) return FALSE;
            if (decoded) g_string_append_c(decoded, chars[v - esc]);
        }
    }
    return FALSE;
}
static gboolean known_string(const char *k)
{
    return k && (!strcmp(k,"device_compatible") || !strcmp(k,"rauc_compatible") ||
        !strcmp(k,"version") || !strcmp(k,"build_id") || !strcmp(k,"artifact_url") || !strcmp(k,"sha256"));
}
static gboolean value_token(Lex *l, const char *root_key);
static gboolean container_token(Lex *l, gboolean object)
{
    if (++l->depth > 64) return FALSE;
    ++l->p; ws(l);
    char end = object ? '}' : ']';
    GHashTable *keys = object ? g_hash_table_new_full(g_str_hash,g_str_equal,g_free,NULL) : NULL;
    gboolean ok = FALSE;
    if (l->p < l->end && *l->p == end) { ++l->p; ok = TRUE; goto done; }
    for (;;) {
        GString *key = NULL;
        if (object) {
            gboolean nul;
            key = g_string_new(NULL);
            if (!string_token(l,key,&nul) || nul || g_hash_table_contains(keys,key->str)) {
                g_string_free(key,TRUE); goto done;
            }
            g_hash_table_add(keys,g_strdup(key->str));
            ws(l);
            if (l->p == l->end || *l->p++ != ':') { g_string_free(key,TRUE); goto done; }
        }
        gboolean valid = value_token(l, object && l->depth == 1 ? key->str : NULL);
        if (key) g_string_free(key,TRUE);
        if (!valid) goto done;
        ws(l);
        if (l->p == l->end) goto done;
        if (*l->p == end) { ++l->p; ok = TRUE; goto done; }
        if (*l->p++ != ',') goto done;
        ws(l);
    }
 done:
    if (keys) g_hash_table_unref(keys);
    --l->depth;
    return ok;
}
static gboolean value_token(Lex *l, const char *root_key)
{
    ws(l);
    if (l->p == l->end) return FALSE;
    if (*l->p == '{' || *l->p == '[') return container_token(l,*l->p == '{');
    if (*l->p == '"') {
        gboolean nul;
        return string_token(l,NULL,&nul) && !(nul && known_string(root_key));
    }
    const char *words[] = {"true","false","null"};
    for (unsigned i=0;i<3;++i) {
        gsize n=strlen(words[i]);
        if ((gsize)(l->end-l->p)>=n && !memcmp(l->p,words[i],n)) { l->p+=n; return TRUE; }
    }
    const char *start = l->p;
    if (*l->p == '-') ++l->p;
    if (l->p == l->end || !g_ascii_isdigit(*l->p)) return FALSE;
    if (*l->p == '0') ++l->p;
    else while (l->p < l->end && g_ascii_isdigit(*l->p)) ++l->p;
    if (l->p < l->end && *l->p == '.') {
        ++l->p;
        if (l->p == l->end || !g_ascii_isdigit(*l->p)) return FALSE;
        while (l->p < l->end && g_ascii_isdigit(*l->p)) ++l->p;
    }
    if (l->p < l->end && (*l->p == 'e' || *l->p == 'E')) {
        ++l->p;
        if (l->p < l->end && (*l->p == '+' || *l->p == '-')) ++l->p;
        if (l->p == l->end || !g_ascii_isdigit(*l->p)) return FALSE;
        while (l->p < l->end && g_ascii_isdigit(*l->p)) ++l->p;
    }
    if (root_key && (!strcmp(root_key,"size") || !strcmp(root_key,"schema_version"))) {
        uint64_t n = 0;
        for (const char *p=start;p<l->p;++p) {
            if (!g_ascii_isdigit(*p) || n > ((uint64_t)INT64_MAX-(unsigned)(*p-'0'))/10) return FALSE;
            n = n*10+(unsigned)(*p-'0');
        }
    }
    return TRUE;
}
static JsonParser *load_json(const char *data, gsize length)
{
    if (!data || !length || length > OTA_MANIFEST_MAX_BYTES || memchr(data,0,length) ||
        !g_utf8_validate(data,length,NULL)) return NULL;
    Lex l = {data,data+length,0};
    ws(&l);
    if (l.p == l.end || *l.p != '{' || !value_token(&l,NULL)) return NULL;
    ws(&l);
    if (l.p != l.end) return NULL;
    JsonParser *parser=json_parser_new();
    if (!json_parser_load_from_data(parser,data,length,NULL) ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) { g_object_unref(parser); return NULL; }
    return parser;
}
static gboolean typed(JsonObject *o, const char *key, GType type)
{
    JsonNode *n=json_object_get_member(o,key);
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n)==type;
}
static gboolean text_field(JsonObject *o, const char *key, char *out, gsize cap)
{
    if (!typed(o,key,G_TYPE_STRING)) return FALSE;
    const char *s=json_object_get_string_member(o,key);
    if (!s || !*s || strlen(s)>=cap) return FALSE;
    g_strlcpy(out,s,cap);
    return TRUE;
}
static gboolean clean_text(const char *s, gsize cap)
{
    if (!s || !memchr(s,0,cap) || !*s || !g_utf8_validate(s,-1,NULL)) return FALSE;
    for (const unsigned char *p=(const unsigned char *)s;*p;++p)
        if (*p<0x20 || *p==0x7f) return FALSE;
    return TRUE;
}
gboolean ota_artifact_path_valid(const char *path)
{
    if (!path || !*path || strlen(path)>=OTA_PATH_CAP || path[0]!='/' || !path[1] || path[1]=='/' ||
        strstr(path,"..") || strpbrk(path,"\\?#:") || !g_utf8_validate(path,-1,NULL)) return FALSE;
    /* Decode once to rule out encoded traversal, separators, control bytes and
     * double encoding. URL syntax remains a path; no authority or query allowed. */
    GString *decoded=g_string_new(NULL);
    gboolean ok=TRUE;
    for (const unsigned char *p=(const unsigned char *)path;*p;++p) {
        unsigned c=*p;
        if (c=='%') {
            if (!p[1] || !p[2] || !g_ascii_isxdigit(p[1]) || !g_ascii_isxdigit(p[2])) { ok=FALSE; break; }
            c=g_ascii_xdigit_value(p[1])*16+g_ascii_xdigit_value(p[2]); p+=2;
            if (strchr("/%\\?#:",(int)c)) { ok=FALSE; break; }
        } else if (c==' ') { ok=FALSE; break; }
        if (c<0x20 || c==0x7f) { ok=FALSE; break; }
        g_string_append_c(decoded,(char)c);
    }
    if (strstr(decoded->str,"..") || !g_utf8_validate(decoded->str,decoded->len,NULL)) ok=FALSE;
    g_string_free(decoded,TRUE);
    return ok;
}
gboolean ota_manifest_validate(const OtaManifest *m, OtaError *error)
{
    OtaVersion v;
    if (!m || m->schema_version!=1 || !clean_text(m->device_compatible,sizeof(m->device_compatible)) ||
        !clean_text(m->build_id,sizeof(m->build_id)) || !clean_text(m->version,sizeof(m->version)) ||
        !clean_text(m->artifact_url,sizeof(m->artifact_url)) || !ota_artifact_path_valid(m->artifact_url) ||
        !memchr(m->sha256,0,sizeof(m->sha256)) || strlen(m->sha256)!=64 || !m->size || m->size>INT64_MAX ||
        (m->mandatory!=TRUE && m->mandatory!=FALSE)) goto invalid;
    for (unsigned i=0;i<64;++i) if (!g_ascii_isdigit(m->sha256[i]) && (m->sha256[i]<'a'||m->sha256[i]>'f')) goto invalid;
    if (!ota_version_parse(m->version,&v,error)) return FALSE;
    return TRUE;
 invalid:
    ota_error_set(error,OTA_ERROR_MANIFEST_INVALID,"Invalid manifest field value"); return FALSE;
}
gboolean ota_release_validate(const OtaRelease *r, OtaError *error)
{
    OtaVersion v;
    if (r && r->schema_version==1 && clean_text(r->device_compatible,sizeof(r->device_compatible)) &&
        clean_text(r->rauc_compatible,sizeof(r->rauc_compatible)) && clean_text(r->build_id,sizeof(r->build_id)) &&
        clean_text(r->version,sizeof(r->version)) && ota_version_parse(r->version,&v,NULL)) return TRUE;
    ota_error_set(error,OTA_ERROR_LOCAL_RELEASE_INVALID,"Invalid local release identity"); return FALSE;
}
gboolean ota_manifest_parse(const char *data, gsize length, OtaManifest *out, OtaError *error)
{
    JsonParser *p=load_json(data,length);
    OtaManifest m={0}; gboolean ok=FALSE;
    if (!p || !out) goto done;
    JsonObject *o=json_node_get_object(json_parser_get_root(p));
    if (!typed(o,"schema_version",G_TYPE_INT64) || json_object_get_int_member(o,"schema_version")!=1 ||
        !typed(o,"size",G_TYPE_INT64) || json_object_get_int_member(o,"size")<=0 || !typed(o,"mandatory",G_TYPE_BOOLEAN)) goto done;
    m.schema_version=1; m.size=(uint64_t)json_object_get_int_member(o,"size"); m.mandatory=json_object_get_boolean_member(o,"mandatory");
#define FIELD(key) if (!text_field(o,#key,m.key,sizeof(m.key))) goto done
    FIELD(device_compatible); FIELD(version); FIELD(build_id); FIELD(artifact_url); FIELD(sha256);
#undef FIELD
    if (!ota_manifest_validate(&m,error)) { if (p) g_object_unref(p); return FALSE; }
    *out=m; ok=TRUE;
 done:
    if (p) g_object_unref(p);
    if (!ok) ota_error_set(error,OTA_ERROR_MANIFEST_INVALID,"Invalid manifest JSON, missing field or wrong field type");
    return ok;
}
gboolean ota_release_parse(const char *data, gsize length, OtaRelease *out, OtaError *error)
{
    JsonParser *p=load_json(data,length); OtaRelease r={0}; gboolean ok=FALSE;
    if (!p || !out) goto done;
    JsonObject *o=json_node_get_object(json_parser_get_root(p));
    if (!typed(o,"schema_version",G_TYPE_INT64) || json_object_get_int_member(o,"schema_version")!=1) goto done;
    r.schema_version=1;
#define FIELD(key) if (!text_field(o,#key,r.key,sizeof(r.key))) goto done
    FIELD(device_compatible); FIELD(rauc_compatible); FIELD(version); FIELD(build_id);
#undef FIELD
    if (!ota_release_validate(&r,error)) goto done;
    *out=r; ok=TRUE;
 done:
    if (p) g_object_unref(p);
    if (!ok) ota_error_set(error,OTA_ERROR_LOCAL_RELEASE_INVALID,"Invalid or missing local release JSON field");
    return ok;
}
gboolean ota_release_load(const char *path, OtaRelease *out, OtaError *error)
{
    int fd=path ? open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK) : -1;
    struct stat st; gboolean ok=FALSE;
    char *data=g_malloc(OTA_MANIFEST_MAX_BYTES+1); gsize used=0;
    if (fd<0 || fstat(fd,&st)<0 || !S_ISREG(st.st_mode) || st.st_size<=0 || st.st_size>OTA_MANIFEST_MAX_BYTES) goto done;
    while (used<=OTA_MANIFEST_MAX_BYTES) {
        ssize_t n=read(fd,data+used,OTA_MANIFEST_MAX_BYTES+1-used);
        if (n<0) { if (errno==EINTR) continue; goto done; }
        if (!n) { ok=ota_release_parse(data,used,out,error); break; }
        used+=(gsize)n;
    }
 done:
    if (fd>=0) close(fd);
    g_free(data);
    if (!ok) ota_error_set(error,OTA_ERROR_LOCAL_RELEASE_INVALID,"Cannot load a valid bounded local release identity");
    return ok;
}

CURL *ota_http_open(const OtaConfig *c, const char *path, OtaErrorCode code, OtaError *error)
{
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        CURLcode result=curl_global_init(CURL_GLOBAL_DEFAULT);
        g_once_init_leave(&initialized,result==CURLE_OK ? 1 : 2);
    }
    if (!c || !ota_artifact_path_valid(path) || !memchr(c->base_url,0,sizeof(c->base_url)) ||
        (!g_str_has_prefix(c->base_url,"http://") && !g_str_has_prefix(c->base_url,"https://")) ||
        !c->connect_timeout_sec || !c->request_timeout_sec || c->connect_timeout_sec>INT32_MAX ||
        c->request_timeout_sec>INT32_MAX || initialized!=1) goto failed;
    CURL *curl=curl_easy_init();
    if (!curl) goto failed;
    char *url=g_strconcat(c->base_url,path,NULL);
#define SET(opt,value) do { if (curl_easy_setopt(curl,opt,value)!=CURLE_OK) goto cleanup; } while (0)
    SET(CURLOPT_URL,url);
#if LIBCURL_VERSION_NUM >= 0x075500
    SET(CURLOPT_PROTOCOLS_STR,"http,https");
#else
    SET(CURLOPT_PROTOCOLS,(long)(CURLPROTO_HTTP|CURLPROTO_HTTPS));
#endif
    SET(CURLOPT_FOLLOWLOCATION,0L);
    SET(CURLOPT_PROXY,"");
    SET(CURLOPT_NOSIGNAL,1L);
    SET(CURLOPT_CONNECTTIMEOUT,(long)c->connect_timeout_sec);
    SET(CURLOPT_TIMEOUT,(long)c->request_timeout_sec);
    SET(CURLOPT_ACCEPT_ENCODING,"identity");
    SET(CURLOPT_HTTP_CONTENT_DECODING,0L);
    SET(CURLOPT_USERAGENT,"edgeguard-remote-ota-agent/R3");
#undef SET
    g_free(url); return curl;
 cleanup:
    g_free(url); curl_easy_cleanup(curl);
 failed:
    ota_error_set(error,code,"Cannot initialize bounded HTTP request"); return NULL;
}
gboolean ota_http_retryable(CURLcode result, long status)
{
    return status==503 || status==502 || status==504 || status==408 || status==429 ||
        result==CURLE_OPERATION_TIMEDOUT || result==CURLE_RECV_ERROR || result==CURLE_SEND_ERROR ||
        result==CURLE_PARTIAL_FILE || result==CURLE_COULDNT_CONNECT || result==CURLE_COULDNT_RESOLVE_HOST;
}
typedef struct { GByteArray *bytes; gsize limit; gboolean overflow; } ManifestBody;
static size_t manifest_body(char *data,size_t size,size_t count,void *user)
{
    ManifestBody *b=user;
    if (size && count>SIZE_MAX/size) { b->overflow=TRUE; return 0; }
    size_t n=size*count;
    if (n>b->limit-b->bytes->len) { b->overflow=TRUE; return 0; }
    g_byte_array_append(b->bytes,(guint8 *)data,n); return n;
}
gboolean ota_manifest_fetch(const OtaConfig *c, OtaManifest *out, gboolean *available,
                            gboolean *retryable, OtaError *error)
{
    if (available) *available=FALSE;
    if (retryable) *retryable=FALSE;
    if (!c || !out || !available || !c->max_manifest_bytes || c->max_manifest_bytes>OTA_MANIFEST_MAX_BYTES) {
        ota_error_set(error,OTA_ERROR_MANIFEST_INVALID,"Invalid manifest output or size limit"); return FALSE;
    }
    CURL *curl=ota_http_open(c,c->manifest_path,OTA_ERROR_MANIFEST_HTTP,error);
    if (!curl) return FALSE;
    ManifestBody b={g_byte_array_new(),c->max_manifest_bytes,FALSE};
    CURLcode result=CURLE_FAILED_INIT; long status=0; gboolean ok=FALSE;
    if (curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,manifest_body)==CURLE_OK &&
        curl_easy_setopt(curl,CURLOPT_WRITEDATA,&b)==CURLE_OK) result=curl_easy_perform(curl);
    curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
    if (retryable) *retryable=ota_http_retryable(result,status);
    if (result!=CURLE_OK || (status!=200 && status!=204)) {
        ota_error_set(error,b.overflow && status==200 ? OTA_ERROR_MANIFEST_INVALID : OTA_ERROR_MANIFEST_HTTP,
                      "Manifest HTTP %ld, transport %d%s",status,(int)result,b.overflow ? ", body exceeds limit" : "");
    } else if (status==204) ok=TRUE;
    else if (ota_manifest_parse((char *)b.bytes->data,b.bytes->len,out,error)) { *available=TRUE; ok=TRUE; }
    g_byte_array_unref(b.bytes); curl_easy_cleanup(curl); return ok;
}
