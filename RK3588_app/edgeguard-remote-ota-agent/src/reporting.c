#include "edgeguard_ota/reporting.h"
#include "edgeguard_ota/manifest.h"
#include "edgeguard_ota/version.h"
#include <json-glib/json-glib.h>
#include <string.h>

static const char *state_name(OtaState state)
{
    static const char *names[OTA_STATE_COUNT]={
        [OTA_STATE_IDLE]="IDLE", [OTA_STATE_CHECK_NETWORK]="CHECK_NETWORK",
        [OTA_STATE_CHECK_UPDATE]="CHECK_UPDATE", [OTA_STATE_PRECHECK]="PRECHECK",
        [OTA_STATE_DOWNLOADING]="DOWNLOADING", [OTA_STATE_VERIFY_DOWNLOAD]="VERIFY_DOWNLOAD",
        [OTA_STATE_RAUC_VERIFY]="RAUC_VERIFY", [OTA_STATE_INSTALLING]="INSTALLING",
        [OTA_STATE_REBOOT_PENDING]="REBOOT_PENDING", [OTA_STATE_BOOT_NEW_SLOT]="BOOT_NEW_SLOT",
        [OTA_STATE_HEALTH_CHECK]="HEALTH_CHECK", [OTA_STATE_MARK_GOOD]="MARK_GOOD",
        [OTA_STATE_REPORT_SUCCESS]="REPORT_SUCCESS", [OTA_STATE_ROLLBACK]="ROLLBACK",
        [OTA_STATE_ERROR]="ERROR"
    };
    return (unsigned)state<OTA_STATE_COUNT ? names[state] : NULL;
}
const char *ota_report_status(OtaState s, OtaReportingMode mode)
{
    if (!state_name(s) || (mode!=OTA_REPORT_LEGACY && mode!=OTA_REPORT_EXTENDED)) return NULL;
    switch (s) {
    case OTA_STATE_IDLE: return "idle";
    case OTA_STATE_CHECK_NETWORK: case OTA_STATE_CHECK_UPDATE: case OTA_STATE_PRECHECK: return "checking";
    case OTA_STATE_DOWNLOADING: return "downloading";
    case OTA_STATE_VERIFY_DOWNLOAD: case OTA_STATE_RAUC_VERIFY: return "downloaded";
    case OTA_STATE_ERROR: return "error";
    case OTA_STATE_ROLLBACK: return mode==OTA_REPORT_EXTENDED ? "rollback" : "error";
    case OTA_STATE_REPORT_SUCCESS: return mode==OTA_REPORT_EXTENDED ? "success" : "idle";
    case OTA_STATE_REBOOT_PENDING: return mode==OTA_REPORT_EXTENDED ? "reboot_pending" : "downloaded";
    default: return mode==OTA_REPORT_EXTENDED ? "installing" : "downloaded";
    }
}
static gboolean bounded(const char *s,gsize cap,gboolean required)
{
    if (!s || !memchr(s,0,cap) || (required && !*s) || !g_utf8_validate(s,-1,NULL)) return FALSE;
    for (const unsigned char *p=(const unsigned char *)s;*p;++p) if (*p<0x20 || *p==0x7f) return FALSE;
    return TRUE;
}
static gboolean valid_report(const OtaReport *r)
{
    OtaVersion version;
    return r && bounded(r->device_id,sizeof(r->device_id),TRUE) &&
        bounded(r->current_version,sizeof(r->current_version),TRUE) && ota_version_parse(r->current_version,&version,NULL) &&
        bounded(r->timestamp,sizeof(r->timestamp),TRUE) && bounded(r->attempt_id,sizeof(r->attempt_id),FALSE) &&
        bounded(r->target_version,sizeof(r->target_version),FALSE) && bounded(r->build_id,sizeof(r->build_id),FALSE) &&
        r->uptime_ms<=INT64_MAX && state_name(r->agent_state) && ota_error_code_name(r->error_code);
}
gboolean ota_report_build(const char *device_id, const OtaRelease *local,
                          const OtaPersistentState *state, const OtaTimeSource *time,
                          OtaReport *out, OtaError *error)
{
    OtaReport r={0};
    if (!device_id || strlen(device_id)>=sizeof(r.device_id) || !state || !out ||
        !ota_release_validate(local,error)) goto failed;
    if (!bounded(state->attempt_id,sizeof(state->attempt_id),FALSE) ||
        !bounded(state->target_version,sizeof(state->target_version),FALSE) ||
        !bounded(state->build_id,sizeof(state->build_id),FALSE)) goto failed;
    g_strlcpy(r.device_id,device_id,sizeof(r.device_id));
    g_strlcpy(r.current_version,local->version,sizeof(r.current_version));
    g_strlcpy(r.attempt_id,state->attempt_id,sizeof(r.attempt_id));
    g_strlcpy(r.target_version,state->target_version,sizeof(r.target_version));
    g_strlcpy(r.build_id,state->build_id,sizeof(r.build_id));
    r.agent_state=state->state; r.error_code=state->last_error.code;
    if (!ota_time_telemetry(time,&r,error)) return FALSE;
    /* R2 requires a timestamp even when realtime cannot be read. The epoch is a
     * telemetry placeholder, explicitly untrusted in the extended wire model. */
    if (!*r.timestamp) g_strlcpy(r.timestamp,"1970-01-01T00:00:00Z",sizeof(r.timestamp));
    r.timestamp_valid=FALSE;
    if (!valid_report(&r)) goto failed;
    *out=r; return TRUE;
 failed:
    ota_error_set(error,OTA_ERROR_REPORT_FAILED,"Cannot build report from invalid identity or state"); return FALSE;
}
gboolean ota_report_serialize(const OtaReport *r, OtaReportingMode mode, char **json, OtaError *error)
{
    if (json) *json=NULL;
    if (!json || !valid_report(r) || !ota_report_status(r->agent_state,mode)) {
        ota_error_set(error,OTA_ERROR_REPORT_FAILED,"Invalid report or reporting mode"); return FALSE;
    }
    JsonBuilder *b=json_builder_new(); json_builder_begin_object(b);
#define TEXT(key,value) do { json_builder_set_member_name(b,key); json_builder_add_string_value(b,value); } while (0)
    TEXT("device_id",r->device_id); TEXT("current_version",r->current_version);
    TEXT("status",ota_report_status(r->agent_state,mode)); TEXT("timestamp",r->timestamp);
    if (mode==OTA_REPORT_EXTENDED) {
        TEXT("attempt_id",r->attempt_id); TEXT("target_version",r->target_version); TEXT("build_id",r->build_id);
        TEXT("agent_state",state_name(r->agent_state)); TEXT("error_code",ota_error_code_name(r->error_code));
        json_builder_set_member_name(b,"timestamp_valid"); json_builder_add_boolean_value(b,FALSE);
        json_builder_set_member_name(b,"uptime_ms"); json_builder_add_int_value(b,(gint64)r->uptime_ms);
    }
#undef TEXT
    json_builder_end_object(b);
    JsonNode *node=json_builder_get_root(b); JsonGenerator *g=json_generator_new();
    json_generator_set_root(g,node); *json=json_generator_to_data(g,NULL);
    g_object_unref(g); json_node_free(node); g_object_unref(b);
    return TRUE;
}
static size_t discard_response(char *data,size_t size,size_t count,void *user)
{
    (void)data; gsize *received=user;
    if (size && count>SIZE_MAX/size) return 0;
    size_t n=size*count;
    if (n>OTA_MANIFEST_MAX_BYTES-*received) return 0;
    *received+=n; return n;
}
gboolean ota_report_send(const OtaConfig *c,const OtaReport *r,gboolean *retryable,OtaError *error)
{
    if (retryable) *retryable=FALSE;
    char *json=NULL;
    if (!c) { ota_error_set(error,OTA_ERROR_REPORT_FAILED,"Missing report configuration"); return FALSE; }
    if (!ota_report_serialize(r,c->reporting_mode,&json,error)) return FALSE;
    CURL *curl=ota_http_open(c,c->report_path,OTA_ERROR_REPORT_FAILED,error);
    if (!curl) { g_free(json); return FALSE; }
    struct curl_slist *headers=curl_slist_append(NULL,"Content-Type: application/json");
    CURLcode result=CURLE_FAILED_INIT; long status=0; gsize received=0;
    if (!headers) goto done;
#define SET(opt,value) do { if (curl_easy_setopt(curl,opt,value)!=CURLE_OK) goto done; } while (0)
    SET(CURLOPT_POST,1L); SET(CURLOPT_POSTFIELDS,json);
    SET(CURLOPT_POSTFIELDSIZE_LARGE,(curl_off_t)strlen(json)); SET(CURLOPT_HTTPHEADER,headers);
    SET(CURLOPT_WRITEFUNCTION,discard_response); SET(CURLOPT_WRITEDATA,&received);
    result=curl_easy_perform(curl);
    curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
#undef SET
 done:
    curl_slist_free_all(headers); curl_easy_cleanup(curl); g_free(json);
    if (retryable) *retryable=ota_http_retryable(result,status);
    if (result!=CURLE_OK || status<200 || status>=300) {
        ota_error_set(error,OTA_ERROR_REPORT_FAILED,"Report HTTP %ld, transport %d",status,(int)result); return FALSE;
    }
    return TRUE;
}
