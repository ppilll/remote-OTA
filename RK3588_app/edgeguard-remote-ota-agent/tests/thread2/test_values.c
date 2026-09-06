#include "edgeguard_ota/version.h"
#include "edgeguard_ota/manifest.h"
#include "edgeguard_ota/compatibility.h"
#include "edgeguard_ota/download.h"
#include "edgeguard_ota/reporting.h"
#include <json-glib/json-glib.h>
#include <string.h>

static const char manifest_json[]=
    "{\"schema_version\":1,\"device_compatible\":\"atk-dlrk3588\",\"version\":\"1.2.3\","
    "\"build_id\":\"fixture\",\"artifact_url\":\"/update.raucb\","
    "\"sha256\":\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\","
    "\"size\":3,\"mandatory\":false}";
static OtaManifest manifest(void)
{
    OtaManifest m={0}; OtaError e={0};
    g_assert_true(ota_manifest_parse(manifest_json,strlen(manifest_json),&m,&e)); return m;
}
static OtaRelease release(void)
{
    OtaRelease r={.schema_version=1,.device_compatible="atk-dlrk3588",.rauc_compatible="EdgeGuard-ATK-DLRK3588-RK3588",
                  .version="1.2.3",.build_id="fixture"}; return r;
}
static void versions(void)
{
    const char *bad[]={"","v1.2.3","1.2","1.2.3.4","1..3",".1.2","1.2.","01.2.3","1.02.3","1.2.03",
        "1.2.3-beta","1.2.3+foo","-1.2.3","+1.2.3"," 1.2.3","1.2.3 ","1.2.3\n","1.2.a","１.2.3",
        "4294967296.0.0","0.4294967296.0","0.0.4294967296","999999999999999999999999.1.1"};
    for (gsize i=0;i<G_N_ELEMENTS(bad);++i) {
        OtaVersion v={7,8,9}; OtaError e={0};
        g_assert_false(ota_version_parse(bad[i],&v,&e));
        g_assert_cmpint(e.code,==,OTA_ERROR_VERSION_MALFORMED); g_assert_cmpuint(v.major,==,7);
    }
    OtaVersion v;
    g_assert_true(ota_version_parse("0.0.0",&v,NULL));
    g_assert_true(ota_version_parse("4294967295.4294967295.4294967295",&v,NULL));
    g_assert_cmpuint(v.patch,==,UINT32_MAX);
    const char *ordered[]={"0.0.0","0.0.1","0.1.0","1.0.0","1.2.3","1.10.0","2.0.0","4294967295.0.0"};
    for (gsize i=0;i<G_N_ELEMENTS(ordered);++i) for (gsize j=0;j<G_N_ELEMENTS(ordered);++j) {
        OtaVersion a,b; g_assert_true(ota_version_parse(ordered[i],&a,NULL)); g_assert_true(ota_version_parse(ordered[j],&b,NULL));
        g_assert_cmpint(ota_version_compare(&a,&b),==,i==j?0:i>j?1:-1);
    }
}
static gboolean parse_object(JsonObject *o,OtaError *e)
{
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_set_object(n,o);
    JsonGenerator *g=json_generator_new(); json_generator_set_root(g,n);
    gsize length; char *data=json_generator_to_data(g,&length); OtaManifest m;
    gboolean ok=ota_manifest_parse(data,length,&m,e);
    g_free(data); g_object_unref(g); json_node_free(n); return ok;
}
static void manifest_types(void)
{
    const char *fields[]={"schema_version","device_compatible","version","build_id","artifact_url","sha256","size","mandatory"};
    for (gsize i=0;i<G_N_ELEMENTS(fields);++i) for (int type=-1;type<6;++type) {
        JsonParser *p=json_parser_new(); g_assert_true(json_parser_load_from_data(p,manifest_json,-1,NULL));
        JsonObject *o=json_node_get_object(json_parser_get_root(p));
        JsonNode *node=NULL;
        if (type==-1) json_object_remove_member(o,fields[i]);
        else {
            if (type==0) node=json_node_new(JSON_NODE_NULL);
            if (type==1) { node=json_node_new(JSON_NODE_VALUE); json_node_set_string(node,"3"); }
            if (type==2) { node=json_node_new(JSON_NODE_VALUE); json_node_set_int(node,3); }
            if (type==3) { node=json_node_new(JSON_NODE_VALUE); json_node_set_boolean(node,TRUE); }
            if (type==4) { node=json_node_new(JSON_NODE_ARRAY); json_node_take_array(node,json_array_new()); }
            if (type==5) { node=json_node_new(JSON_NODE_OBJECT); json_node_take_object(node,json_object_new()); }
            json_object_set_member(o,fields[i],node);
        }
        gboolean correct_type=(type==1 && i>=1 && i<=5) || (type==2 && (i==0 || i==6)) || (type==3 && i==7);
        if (!correct_type) { OtaError e={0}; g_assert_false(parse_object(o,&e)); g_assert_cmpint(e.code,==,OTA_ERROR_MANIFEST_INVALID); }
        g_object_unref(p);
    }
    JsonParser *p=json_parser_new(); g_assert_true(json_parser_load_from_data(p,manifest_json,-1,NULL));
    JsonObject *o=json_node_get_object(json_parser_get_root(p));
    JsonObject *future=json_object_new(); json_object_set_array_member(future,"list",json_array_new());
    json_object_set_object_member(o,"future_extension",future); json_object_set_null_member(o,"future_null");
    g_assert_true(parse_object(o,NULL));
    json_object_set_int_member(o,"size",0); g_assert_false(parse_object(o,NULL));
    json_object_set_int_member(o,"size",-1); g_assert_false(parse_object(o,NULL));
    json_object_set_int_member(o,"size",INT64_MAX); g_assert_true(parse_object(o,NULL));
    json_object_set_int_member(o,"schema_version",2); g_assert_false(parse_object(o,NULL));
    g_object_unref(p);
}
static char *replace_once(const char *data,const char *needle,const char *replacement)
{
    const char *where=strstr(data,needle); g_assert_nonnull(where);
    char *prefix=g_strndup(data,where-data); char *result=g_strconcat(prefix,replacement,where+strlen(needle),NULL);
    g_free(prefix); return result;
}
static void manifest_syntax(void)
{
    const char *bad_sizes[]={"1.0","1e0","true","\"3\"","null","-0","01","9223372036854775808","18446744073709551616"};
    for (gsize i=0;i<G_N_ELEMENTS(bad_sizes);++i) {
        char *field=g_strconcat("\"size\":",bad_sizes[i],NULL);
        char *data=replace_once(manifest_json,"\"size\":3",field); OtaManifest m;
        g_assert_false(ota_manifest_parse(data,strlen(data),&m,NULL)); g_free(field); g_free(data);
    }
    const char *suffix[]={" trailing"," {}",",", "\ntrue"};
    for (gsize i=0;i<G_N_ELEMENTS(suffix);++i) {
        char *data=g_strconcat(manifest_json,suffix[i],NULL); OtaManifest m;
        g_assert_false(ota_manifest_parse(data,strlen(data),&m,NULL)); g_free(data);
    }
    const char *needles[]={"\"size\":3","\"size\":3","\"build_id\":\"fixture\"","\"schema_version\":1","\"mandatory\":false"};
    const char *replacements[]={"\"size\":3,\"size\":3","\"size\":3,\"si\\u007ae\":3",
        "\"build_id\":\"fixture\\u0000hidden\"","\"schema_version\":1.0","\"mandatory\":false,"};
    for (gsize i=0;i<G_N_ELEMENTS(needles);++i) {
        char *data=replace_once(manifest_json,needles[i],replacements[i]); OtaManifest m;
        g_assert_false(ota_manifest_parse(data,strlen(data),&m,NULL)); g_free(data);
    }
    OtaManifest m;
    char *escaped=replace_once(manifest_json,"\"size\":3","\"si\\u007ae\":3");
    g_assert_true(ota_manifest_parse(escaped,strlen(escaped),&m,NULL)); g_free(escaped);
    char *data=g_malloc0(OTA_MANIFEST_MAX_BYTES+2); memcpy(data,manifest_json,strlen(manifest_json));
    memset(data+strlen(manifest_json),' ',OTA_MANIFEST_MAX_BYTES+1-strlen(manifest_json));
    g_assert_true(ota_manifest_parse(data,OTA_MANIFEST_MAX_BYTES,&m,NULL));
    g_assert_false(ota_manifest_parse(data,OTA_MANIFEST_MAX_BYTES+1,&m,NULL));
    data[5]=0; g_assert_false(ota_manifest_parse(data,strlen(manifest_json),&m,NULL)); g_free(data);
    const char *invalid[]={"[]","null","{/*x*/}","{foo:1}","{\"x\":NaN}","{\"x\":+1}","{\"x\":\"\\uD800\"}"};
    for(gsize i=0;i<G_N_ELEMENTS(invalid);++i) g_assert_false(ota_manifest_parse(invalid[i],strlen(invalid[i]),&m,NULL));
}
static void manifest_values(void)
{
    OtaManifest m=manifest();
    const char *bad[]={"","update.raucb","https://host/x","//host/x","/../x","/a..b","/a/../x","/a\\b",
        "/x\n","/x\177","/x?host=y","/x#y","/http://host","/%2e%2e/x","/%2E./x","/%2fhost/x",
        "/%5chost","/%00x","/%0dx","/%252e%252e/x","/%", "/%GG", "/a b"};
    for(gsize i=0;i<G_N_ELEMENTS(bad);++i) g_assert_false(ota_artifact_path_valid(bad[i]));
    g_assert_true(ota_artifact_path_valid("/releases/update-1.2.3.raucb"));
    g_assert_true(ota_artifact_path_valid("/releases/update%20file.raucb"));
    m.sha256[0]='A'; g_assert_false(ota_manifest_validate(&m,NULL));
    m=manifest(); m.sha256[63]=0; g_assert_false(ota_manifest_validate(&m,NULL));
    m=manifest(); memset(m.build_id,'a',sizeof(m.build_id)); g_assert_false(ota_manifest_validate(&m,NULL));
    m=manifest(); m.mandatory=2; g_assert_false(ota_manifest_validate(&m,NULL));
}
static void compatibility(void)
{
    OtaManifest m=manifest(); OtaRelease r=release(); OtaError e={0}; gboolean update=TRUE;
    g_assert_true(ota_compatibility_precheck(&r,&m,&update,&e)); g_assert_false(update);
    g_strlcpy(m.version,"1.2.4",sizeof(m.version));
    g_assert_true(ota_compatibility_precheck(&r,&m,&update,&e)); g_assert_true(update);
    for (unsigned mandatory=0;mandatory<2;++mandatory) {
        m=manifest(); m.mandatory=mandatory; g_strlcpy(m.build_id,"other",sizeof(m.build_id));
        g_assert_false(ota_compatibility_precheck(&r,&m,&update,&e)); g_assert_cmpint(e.code,==,OTA_ERROR_VERSION_COLLISION);
        g_strlcpy(m.version,"1.2.2",sizeof(m.version));
        g_assert_false(ota_compatibility_precheck(&r,&m,&update,&e)); g_assert_cmpint(e.code,==,OTA_ERROR_DOWNGRADE_REJECTED);
        g_strlcpy(m.version,"1.2.3-beta",sizeof(m.version));
        g_assert_false(ota_compatibility_precheck(&r,&m,&update,&e)); g_assert_cmpint(e.code,==,OTA_ERROR_VERSION_MALFORMED);
        m=manifest(); m.mandatory=mandatory; g_strlcpy(m.device_compatible,"other",sizeof(m.device_compatible));
        g_assert_false(ota_compatibility_precheck(&r,&m,&update,&e)); g_assert_cmpint(e.code,==,OTA_ERROR_DEVICE_COMPAT_MISMATCH);
    }
    g_assert_true(ota_compatibility_rauc(&r,r.rauc_compatible,&e));
    g_assert_false(ota_compatibility_rauc(&r,r.device_compatible,&e)); g_assert_cmpint(e.code,==,OTA_ERROR_RAUC_COMPAT_MISMATCH);
    g_assert_false(ota_compatibility_rauc(&r,NULL,&e));
    r.version[0]=0; g_assert_false(ota_compatibility_precheck(&r,&m,&update,&e));
    g_assert_cmpint(e.code,==,OTA_ERROR_LOCAL_RELEASE_INVALID);
}
static void release_json(void)
{
    const char *data="{\"schema_version\":1,\"device_compatible\":\"atk-dlrk3588\",\"rauc_compatible\":\"EdgeGuard-ATK-DLRK3588-RK3588\",\"version\":\"1.2.3\",\"build_id\":\"fixture\"}";
    OtaRelease r; g_assert_true(ota_release_parse(data,strlen(data),&r,NULL));
    const char *bad[]={"\"rauc_compatible\":null","\"rauc_compatible\":3","\"rauc_compatible\":\"\""};
    for(gsize i=0;i<G_N_ELEMENTS(bad);++i) {
        char *modified=replace_once(data,"\"rauc_compatible\":\"EdgeGuard-ATK-DLRK3588-RK3588\"",bad[i]);
        g_assert_false(ota_release_parse(modified,strlen(modified),&r,NULL)); g_free(modified);
    }
    g_assert_false(ota_release_load("/nonexistent/thread2-release.json",&r,NULL));
}
static void ranges(void)
{
    g_assert_cmpint(ota_download_response(1,3,206,"bytes 1-2/3",NULL),==,OTA_DOWNLOAD_APPEND);
    const char *bad[]={NULL,"bytes 0-2/3","bytes 2-2/3","bytes 1-2/4","bytes 1-3/3","bytes 1-1/3",
        "bytes 1-2/*","bytes */3","bytes 1-2/3junk","bytes -1-2/3","bytes 9223372036854775808-2/3","Bytes 1-2/3"};
    for(gsize i=0;i<G_N_ELEMENTS(bad);++i) {
        OtaError e={0}; g_assert_cmpint(ota_download_response(1,3,206,bad[i],&e),==,OTA_DOWNLOAD_REJECT);
        g_assert_cmpint(e.code,==,OTA_ERROR_DOWNLOAD_RANGE_MISMATCH);
    }
    g_assert_cmpint(ota_download_response(1,3,200,NULL,NULL),==,OTA_DOWNLOAD_WRITE_ZERO);
    g_assert_cmpint(ota_download_response(0,3,200,NULL,NULL),==,OTA_DOWNLOAD_WRITE_ZERO);
    g_assert_cmpint(ota_download_response(1,3,416,"bytes */3",NULL),==,OTA_DOWNLOAD_RESTART);
    g_assert_cmpint(ota_download_response(3,3,416,"bytes */3",NULL),==,OTA_DOWNLOAD_VERIFY);
    g_assert_cmpint(ota_download_response(4,3,206,"bytes 4-4/5",NULL),==,OTA_DOWNLOAD_REJECT);
    g_assert_cmpint(ota_download_response(1,3,404,NULL,NULL),==,OTA_DOWNLOAD_REJECT);
}
static int fake_clock(void *user,clockid_t clock,struct timespec *out)
{
    if (clock==CLOCK_REALTIME && user) return -1;
    *out=(struct timespec){clock==CLOCK_MONOTONIC?123:0,456000000}; return 0;
}
static void reports(void)
{
    OtaRelease r=release(); OtaPersistentState s={.state=OTA_STATE_REBOOT_PENDING,.target_version="1.2.4",.build_id="next"};
    OtaTimeSource time={fake_clock,NULL}; OtaReport report; OtaError e={0}; char *json;
    g_assert_true(ota_report_build("12345678-1234-4234-8234-123456789abc",&r,&s,&time,&report,&e));
    g_assert_cmpstr(report.current_version,==,"1.2.3"); g_assert_cmpuint(report.uptime_ms,==,123456);
    g_assert_false(report.timestamp_valid);
    for (OtaState state=OTA_STATE_IDLE;state<OTA_STATE_COUNT;++state) {
        report.agent_state=state;
        for (OtaReportingMode mode=OTA_REPORT_LEGACY;mode<=OTA_REPORT_EXTENDED;++mode) {
            g_assert_true(ota_report_serialize(&report,mode,&json,&e));
            JsonParser *p=json_parser_new(); g_assert_true(json_parser_load_from_data(p,json,-1,NULL));
            JsonObject *o=json_node_get_object(json_parser_get_root(p));
            g_assert_cmpuint(json_object_get_size(o),==,mode==OTA_REPORT_LEGACY?4:11);
            const char *status=json_object_get_string_member(o,"status");
            if (mode==OTA_REPORT_LEGACY) {
                g_assert_true(!strcmp(status,"idle") || !strcmp(status,"checking") || !strcmp(status,"downloading") ||
                              !strcmp(status,"downloaded") || !strcmp(status,"error"));
                g_assert_false(json_object_has_member(o,"attempt_id"));
            } else {
                g_assert_false(json_object_get_boolean_member(o,"timestamp_valid"));
                g_assert_cmpint(json_object_get_int_member(o,"uptime_ms"),==,123456);
            }
            g_object_unref(p); g_free(json);
        }
    }
    report.timestamp_valid=TRUE;
    g_assert_true(ota_report_serialize(&report,OTA_REPORT_EXTENDED,&json,&e));
    g_assert_nonnull(strstr(json,"\"timestamp_valid\":false")); g_free(json);
    time.user=&s;
    g_assert_true(ota_report_build("12345678-1234-4234-8234-123456789abc",&r,&s,&time,&report,&e));
    g_assert_cmpstr(report.timestamp,==,"1970-01-01T00:00:00Z");
    g_assert_false(ota_report_serialize(&report,(OtaReportingMode)99,&json,&e));
    g_assert_null(json);
}
int main(int argc,char **argv)
{
    g_test_init(&argc,&argv,NULL);
    g_test_add_func("/thread2/version",versions);
    g_test_add_func("/thread2/manifest/types",manifest_types);
    g_test_add_func("/thread2/manifest/syntax",manifest_syntax);
    g_test_add_func("/thread2/manifest/values",manifest_values);
    g_test_add_func("/thread2/release",release_json);
    g_test_add_func("/thread2/compatibility",compatibility);
    g_test_add_func("/thread2/ranges",ranges);
    g_test_add_func("/thread2/reporting",reports);
    return g_test_run();
}
