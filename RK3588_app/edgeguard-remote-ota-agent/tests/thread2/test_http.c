#include "edgeguard_ota/download.h"
#include "edgeguard_ota/manifest.h"
#include "edgeguard_ota/reporting.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

/* Raw loopback scripts exercise the REAL libcurl callbacks and file operations.
 * No external service, shell, interpreter, RAUC command or real reboot is used. */
typedef struct {
    const char *response;
    const char *range; /* NULL requires no Range header */
    gboolean stall,reset,post;
} Step;
typedef struct {
    int fd; unsigned port; const Step *steps; unsigned count,served;
    GThread *thread; GMutex mutex; GCond cond; gboolean stop;
} Server;
static void send_all(int fd,const char *data)
{
    gsize left=strlen(data);
    while (left) {
        ssize_t n=send(fd,data,left,MSG_NOSIGNAL);
        if (n<0 && errno==EINTR) continue;
        g_assert_cmpint(n,>,0); data+=n; left-=n;
    }
}
static const char *header_value(char **lines,const char *name)
{
    gsize n=strlen(name);
    for (gsize i=1;lines[i];++i)
        if (!g_ascii_strncasecmp(lines[i],name,n) && lines[i][n]==':') return g_strstrip(lines[i]+n+1);
    return NULL;
}
static gpointer serve(gpointer user)
{
    Server *s=user;
    for(unsigned i=0;i<s->count;++i) {
        struct pollfd wait={s->fd,POLLIN,0}; g_assert_cmpint(poll(&wait,1,5000),==,1);
        int fd=accept(s->fd,NULL,NULL); g_assert_cmpint(fd,>=,0);
        GString *request=g_string_new(NULL); char bytes[2048]; char *end;
        do {
            struct pollfd input={fd,POLLIN,0}; g_assert_cmpint(poll(&input,1,5000),==,1);
            ssize_t n=recv(fd,bytes,sizeof(bytes),0); g_assert_cmpint(n,>,0);
            g_string_append_len(request,bytes,n); g_assert_cmpuint(request->len,<,65536);
            end=strstr(request->str,"\r\n\r\n");
        } while (!end);
        gsize header_length=(gsize)(end-request->str)+4;
        char *header=g_strndup(request->str,header_length); char **lines=g_strsplit(header,"\r\n",-1);
        const Step *step=&s->steps[i];
        const char *range=header_value(lines,"Range"); g_assert_cmpstr(range,==,step->range);
        g_assert_true(g_str_has_prefix(lines[0],step->post ? "POST /device/report " : "GET /"));
        if(step->post) {
            g_assert_cmpstr(header_value(lines,"Content-Type"),==,"application/json");
            const char *length=header_value(lines,"Content-Length"); g_assert_nonnull(length);
            guint64 count=g_ascii_strtoull(length,NULL,10); g_assert_cmpuint(count,<,65536);
            while(request->len<header_length+count) {
                struct pollfd input={fd,POLLIN,0}; g_assert_cmpint(poll(&input,1,5000),==,1);
                ssize_t n=recv(fd,bytes,sizeof(bytes),0); g_assert_cmpint(n,>,0); g_string_append_len(request,bytes,n);
            }
            JsonParser *p=json_parser_new();
            g_assert_true(json_parser_load_from_data(p,request->str+header_length,count,NULL));
            JsonObject *o=json_node_get_object(json_parser_get_root(p));
            g_assert_cmpuint(json_object_get_size(o),==,4);
            g_assert_cmpstr(json_object_get_string_member(o,"current_version"),==,"1.0.0");
            g_object_unref(p);
        }
        g_strfreev(lines); g_free(header); g_string_free(request,TRUE);
        send_all(fd,step->response); ++s->served;
        if(step->stall) {
            g_mutex_lock(&s->mutex);
            gint64 deadline=g_get_monotonic_time()+5*G_TIME_SPAN_SECOND;
            while(!s->stop && g_cond_wait_until(&s->cond,&s->mutex,deadline)) {}
            g_mutex_unlock(&s->mutex);
        }
        if(step->reset) { struct linger linger={1,0}; g_assert_cmpint(setsockopt(fd,SOL_SOCKET,SO_LINGER,&linger,sizeof(linger)),==,0); }
        close(fd);
    }
    return NULL;
}
static void start(Server *s,const Step *steps,unsigned count,OtaConfig *config)
{
    memset(s,0,sizeof(*s)); s->steps=steps; s->count=count;
    g_mutex_init(&s->mutex); g_cond_init(&s->cond);
    s->fd=socket(AF_INET,SOCK_STREAM,0); g_assert_cmpint(s->fd,>=,0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK),.sin_port=0};
    g_assert_cmpint(bind(s->fd,(struct sockaddr *)&address,sizeof(address)),==,0);
    socklen_t size=sizeof(address); g_assert_cmpint(getsockname(s->fd,(struct sockaddr *)&address,&size),==,0);
    s->port=ntohs(address.sin_port); g_assert_cmpint(listen(s->fd,4),==,0);
    g_snprintf(config->base_url,sizeof(config->base_url),"http://127.0.0.1:%u",s->port);
    config->connect_timeout_sec=1; config->request_timeout_sec=1; config->max_manifest_bytes=65536;
    g_strlcpy(config->manifest_path,"/manifest.json",sizeof(config->manifest_path));
    g_strlcpy(config->report_path,"/device/report",sizeof(config->report_path));
    s->thread=g_thread_new("thread2-http-fixture",serve,s);
}
static void finish(Server *s)
{
    g_mutex_lock(&s->mutex); s->stop=TRUE; g_cond_signal(&s->cond); g_mutex_unlock(&s->mutex);
    g_thread_join(s->thread); g_assert_cmpuint(s->served,==,s->count); close(s->fd);
    g_cond_clear(&s->cond); g_mutex_clear(&s->mutex);
}
static OtaManifest manifest(void)
{
    OtaManifest m={.schema_version=1,.device_compatible="atk-dlrk3588",.version="1.2.3",.build_id="test",
        .artifact_url="/update.raucb",.size=3,
        .sha256="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}; return m;
}
static char *temp_config(OtaConfig *c,const char *partial)
{
    char *dir=g_dir_make_tmp("ota-thread2-XXXXXX",NULL); g_assert_nonnull(dir);
    memset(c,0,sizeof(*c));
    g_snprintf(c->part_file,sizeof(c->part_file),"%s/update.raucb.part",dir);
    g_snprintf(c->bundle_file,sizeof(c->bundle_file),"%s/update.raucb",dir);
    g_strlcpy(c->base_url,"http://127.0.0.1:9",sizeof(c->base_url));
    c->connect_timeout_sec=c->request_timeout_sec=1;
    if(partial) g_assert_true(g_file_set_contents(c->part_file,partial,-1,NULL));
    /* Existing final artifact must survive EVERY unsuccessful download. */
    g_assert_true(g_file_set_contents(c->bundle_file,"previous",-1,NULL));
    return dir;
}
static void assert_file(const char *path,const char *expected)
{
    char *data; gsize size; g_assert_true(g_file_get_contents(path,&data,&size,NULL));
    g_assert_cmpuint(size,==,strlen(expected)); g_assert_cmpmem(data,size,expected,strlen(expected)); g_free(data);
}
static void cleanup(char *dir,OtaConfig *c)
{
    g_unlink(c->part_file); g_unlink(c->bundle_file); g_assert_cmpint(g_rmdir(dir),==,0); g_free(dir);
}
static void run_download(const char *partial,const Step *steps,unsigned count,gboolean success,
                         OtaErrorCode code,gboolean retry,const char *retained)
{
    OtaConfig c; char *dir=temp_config(&c,partial); Server server; start(&server,steps,count,&c);
    OtaManifest m=manifest(); OtaError error={0}; gboolean retryable=FALSE;
    g_assert_cmpint(ota_download_bundle(&c,&m,&retryable,&error),==,success);
    finish(&server);
    g_assert_cmpint(retryable,==,retry);
    if(success) { assert_file(c.bundle_file,"abc"); g_assert_false(g_file_test(c.part_file,G_FILE_TEST_EXISTS)); }
    else {
        g_assert_cmpint(error.code,==,code); assert_file(c.bundle_file,"previous");
        if(retained && strcmp(retained,"*")) assert_file(c.part_file,retained);
        else {
            char *data; gsize size; g_assert_true(g_file_get_contents(c.part_file,&data,&size,NULL));
            g_assert_cmpuint(size,>=,retained?0:1); g_assert_cmpuint(size,<=,retained?3:2);
            g_assert_cmpmem(data,size,"abc",size); g_free(data);
        }
    }
    cleanup(dir,&c);
}
static void downloads(void)
{
    const Step full[]={ {.response="HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: close\r\n\r\nabc"} };
    run_download(NULL,full,1,TRUE,OTA_ERROR_NONE,FALSE,NULL);
    run_download("",full,1,TRUE,OTA_ERROR_NONE,FALSE,NULL);
    const Step resume[]={ {.response="HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 1-2/3\r\nContent-Length: 2\r\n\r\nbc",.range="bytes=1-"} };
    run_download("a",resume,1,TRUE,OTA_ERROR_NONE,FALSE,NULL);
    const Step ignore[]={ {.response=full[0].response,.range="bytes=1-"} };
    run_download("X",ignore,1,TRUE,OTA_ERROR_NONE,FALSE,NULL); /* proves truncate, not append */
    run_download("oversized",full,1,TRUE,OTA_ERROR_NONE,FALSE,NULL);
    const Step wrong[]={ {.response="HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-2/3\r\nContent-Length: 3\r\n\r\nabc",.range="bytes=1-"} };
    run_download("a",wrong,1,FALSE,OTA_ERROR_DOWNLOAD_RANGE_MISMATCH,FALSE,"a");
    const Step missing[]={ {.response="HTTP/1.1 206 Partial Content\r\nContent-Length: 2\r\n\r\nbc",.range="bytes=1-"} };
    run_download("a",missing,1,FALSE,OTA_ERROR_DOWNLOAD_RANGE_MISMATCH,FALSE,"a");
    const Step duplicate[]={ {.response="HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 1-2/3\r\nContent-Range: bytes 1-2/3\r\n\r\nbc",.range="bytes=1-"} };
    run_download("a",duplicate,1,FALSE,OTA_ERROR_DOWNLOAD_RANGE_MISMATCH,FALSE,"a");
    const Step unsatisfied[]={
        {.response="HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */3\r\nContent-Length: 3\r\n\r\nerr",.range="bytes=1-"},
        {.response=full[0].response} };
    run_download("a",unsatisfied,2,TRUE,OTA_ERROR_NONE,FALSE,NULL);
    const Step repeated[]={ unsatisfied[0], {.response=unsatisfied[0].response} };
    run_download("a",repeated,2,FALSE,OTA_ERROR_DOWNLOAD_HTTP,FALSE,"");
    const Step notfound[]={ {.response="HTTP/1.1 404 Not Found\r\nContent-Length: 3\r\n\r\nerr",.range="bytes=1-"} };
    run_download("a",notfound,1,FALSE,OTA_ERROR_DOWNLOAD_HTTP,FALSE,"a");
    const Step busy[]={ {.response="HTTP/1.1 503 Unavailable\r\nContent-Length: 3\r\n\r\nerr",.range="bytes=1-"} };
    run_download("a",busy,1,FALSE,OTA_ERROR_DOWNLOAD_HTTP,TRUE,"a");
    const Step timeout[]={ {.response="HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 1-2/3\r\nContent-Length: 2\r\n\r\n",.range="bytes=1-",.stall=TRUE} };
    run_download("a",timeout,1,FALSE,OTA_ERROR_DOWNLOAD_HTTP,TRUE,"a");
    const Step reset[]={ {.response="HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 1-2/3\r\nContent-Length: 2\r\n\r\nb",.range="bytes=1-",.reset=TRUE} };
    run_download("a",reset,1,FALSE,OTA_ERROR_DOWNLOAD_HTTP,TRUE,NULL);
    const Step interrupted[]={ {.response="HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 1-2/3\r\nContent-Length: 2\r\n\r\nb",.range="bytes=1-"} };
    run_download("a",interrupted,1,FALSE,OTA_ERROR_DOWNLOAD_HTTP,TRUE,"ab");
    const Step shortbody[]={ {.response="HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nab"} };
    run_download(NULL,shortbody,1,FALSE,OTA_ERROR_DOWNLOAD_SIZE_MISMATCH,FALSE,"ab");
    const Step wronglength[]={ {.response="HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nabcd",.range="bytes=1-"} };
    run_download("a",wronglength,1,FALSE,OTA_ERROR_DOWNLOAD_SIZE_MISMATCH,FALSE,"a");
    const Step excess[]={ {.response="HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nabcd\r\n0\r\n\r\n"} };
    run_download(NULL,excess,1,FALSE,OTA_ERROR_DOWNLOAD_SIZE_MISMATCH,FALSE,"*");
    const Step corrupt[]={ {.response="HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nxyz"} };
    run_download(NULL,corrupt,1,FALSE,OTA_ERROR_DOWNLOAD_HASH_MISMATCH,FALSE,"");
    const Step corrupt_tail[]={ {.response="HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 1-2/3\r\nContent-Length: 2\r\n\r\nbc",.range="bytes=1-"} };
    run_download("X",corrupt_tail,1,FALSE,OTA_ERROR_DOWNLOAD_HASH_MISMATCH,FALSE,"");
    const Step encoded[]={ {.response="HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 3\r\n\r\nabc",.range="bytes=1-"} };
    run_download("a",encoded,1,FALSE,OTA_ERROR_DOWNLOAD_HTTP,FALSE,"a");
    const Step interim[]={ {.response="HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 206 Partial Content\r\nContent-Range: bytes 1-2/3\r\nContent-Length: 2\r\n\r\nbc",.range="bytes=1-"} };
    run_download("a",interim,1,TRUE,OTA_ERROR_NONE,FALSE,NULL);
    const Step redirect[]={ {.response="HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:9/foreign\r\nContent-Length: 0\r\n\r\n",.range="bytes=1-"} };
    run_download("a",redirect,1,FALSE,OTA_ERROR_DOWNLOAD_HTTP,FALSE,"a");
}
static void resume_across_calls(void)
{
    const Step steps[]={
        {.response="HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 1-2/3\r\nContent-Length: 2\r\n\r\nb",.range="bytes=1-"},
        {.response="HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 2-2/3\r\nContent-Length: 1\r\n\r\nc",.range="bytes=2-"}
    };
    OtaConfig c; char *dir=temp_config(&c,"a"); Server s; start(&s,steps,2,&c);
    OtaManifest m=manifest(); OtaError e={0}; gboolean retry=FALSE;
    g_assert_false(ota_download_bundle(&c,&m,&retry,&e));
    g_assert_true(retry); g_assert_cmpint(e.code,==,OTA_ERROR_DOWNLOAD_HTTP);
    assert_file(c.part_file,"ab"); assert_file(c.bundle_file,"previous");
    e=(OtaError){0};
    g_assert_true(ota_download_bundle(&c,&m,&retry,&e));
    g_assert_false(retry); assert_file(c.bundle_file,"abc");
    g_assert_false(g_file_test(c.part_file,G_FILE_TEST_EXISTS));
    finish(&s); cleanup(dir,&c);
}
static void local_partials(void)
{
    for(unsigned corrupt=0;corrupt<2;++corrupt) {
        OtaConfig c; char *dir=temp_config(&c,corrupt?"xyz":"abc"); OtaManifest m=manifest(); OtaError e={0}; gboolean retry;
        g_assert_cmpint(ota_download_bundle(&c,&m,&retry,&e),==,!corrupt);
        g_assert_false(retry);
        if(corrupt) { g_assert_cmpint(e.code,==,OTA_ERROR_DOWNLOAD_HASH_MISMATCH); assert_file(c.part_file,""); assert_file(c.bundle_file,"previous"); }
        else { assert_file(c.bundle_file,"abc"); g_assert_false(g_file_test(c.part_file,G_FILE_TEST_EXISTS)); }
        cleanup(dir,&c);
    }
    OtaConfig c; char *dir=temp_config(&c,"a"); OtaManifest m=manifest(); OtaError e={0};
    c.reserve_bytes=INT64_MAX;
    g_assert_false(ota_download_bundle(&c,&m,NULL,&e)); g_assert_cmpint(e.code,==,OTA_ERROR_DOWNLOAD_DISK_SPACE);
    assert_file(c.part_file,"a"); cleanup(dir,&c);
    dir=temp_config(&c,NULL); g_assert_cmpint(symlink(c.bundle_file,c.part_file),==,0);
    g_assert_false(ota_download_bundle(&c,&m,NULL,&e)); assert_file(c.bundle_file,"previous"); cleanup(dir,&c);
    dir=temp_config(&c,NULL); g_assert_cmpint(mkfifo(c.part_file,0600),==,0);
    g_assert_false(ota_download_bundle(&c,&m,NULL,&e)); assert_file(c.bundle_file,"previous"); cleanup(dir,&c);
}
static void completed_bundle_reuse(void)
{
    OtaConfig c;
    char *dir = temp_config(&c, NULL);
    OtaManifest m = manifest();
    OtaError e = {0};
    gboolean retry = TRUE;
    g_assert_true(g_file_set_contents(c.bundle_file, "abc", 3, NULL));
    g_assert_true(ota_download_bundle(&c, &m, &retry, &e));
    g_assert_false(retry);
    assert_file(c.bundle_file, "abc");
    g_assert_false(g_file_test(c.part_file, G_FILE_TEST_EXISTS));
    cleanup(dir, &c);
}
static void manifest_http(void)
{
    const char *json="{\"schema_version\":1,\"device_compatible\":\"atk-dlrk3588\",\"version\":\"1.2.3\",\"build_id\":\"test\",\"artifact_url\":\"/update.raucb\",\"size\":3,\"mandatory\":false,\"sha256\":\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"}";
    char *response=g_strdup_printf("HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n%s",strlen(json),json);
    const struct {const char *response; gboolean success,available,retry; OtaErrorCode code; unsigned limit;} cases[]={
        {response,TRUE,TRUE,FALSE,OTA_ERROR_NONE,65536},
        {"HTTP/1.1 204 No Content\r\n\r\n",TRUE,FALSE,FALSE,OTA_ERROR_NONE,65536},
        {"HTTP/1.1 503 Unavailable\r\nContent-Length: 0\r\n\r\n",FALSE,FALSE,TRUE,OTA_ERROR_MANIFEST_HTTP,65536},
        {"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n",FALSE,FALSE,FALSE,OTA_ERROR_MANIFEST_HTTP,65536},
        {"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}",FALSE,FALSE,FALSE,OTA_ERROR_MANIFEST_INVALID,65536},
        {response,FALSE,FALSE,FALSE,OTA_ERROR_MANIFEST_INVALID,16}
    };
    for(gsize i=0;i<G_N_ELEMENTS(cases);++i) {
        Step step={.response=cases[i].response}; Server s; OtaConfig c={0}; start(&s,&step,1,&c); c.max_manifest_bytes=cases[i].limit;
        OtaManifest m={.version="unchanged"}; OtaError e={0}; gboolean available=TRUE,retry=FALSE;
        g_assert_cmpint(ota_manifest_fetch(&c,&m,&available,&retry,&e),==,cases[i].success); finish(&s);
        g_assert_cmpint(available,==,cases[i].available); g_assert_cmpint(retry,==,cases[i].retry);
        if(!cases[i].success) g_assert_cmpint(e.code,==,cases[i].code);
        g_assert_cmpstr(m.version,==,available?"1.2.3":"unchanged");
    }
    g_free(response);
}
static void report_http(void)
{
    OtaReport r={.device_id="12345678-1234-4234-8234-123456789abc",.current_version="1.0.0",
        .timestamp="1970-01-01T00:00:00Z",.agent_state=OTA_STATE_INSTALLING};
    const char *responses[]={"HTTP/1.1 204 No Content\r\n\r\n","HTTP/1.1 503 Unavailable\r\nContent-Length: 0\r\n\r\n"};
    for(unsigned i=0;i<2;++i) {
        Step step={.response=responses[i],.post=TRUE}; Server s; OtaConfig c={0}; start(&s,&step,1,&c);
        OtaError e={0}; gboolean retry=FALSE;
        g_assert_cmpint(ota_report_send(&c,&r,&retry,&e),==,!i); finish(&s);
        g_assert_cmpint(retry,==,i!=0); if(i) g_assert_cmpint(e.code,==,OTA_ERROR_REPORT_FAILED);
    }
}
int main(int argc,char **argv)
{
    g_test_init(&argc,&argv,NULL);
    g_test_add_func("/thread2/http/download",downloads);
    g_test_add_func("/thread2/http/resume-across-calls",resume_across_calls);
    g_test_add_func("/thread2/http/local-partials",local_partials);
    g_test_add_func("/thread2/http/completed-bundle-reuse",completed_bundle_reuse);
    g_test_add_func("/thread2/http/manifest",manifest_http);
    g_test_add_func("/thread2/http/report",report_http);
    return g_test_run();
}
