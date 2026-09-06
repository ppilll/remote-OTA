#include "edgeguard_ota/download.h"
#include "edgeguard_ota/manifest.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static gboolean decimal(const char **p, uint64_t *out)
{
    uint64_t n=0;
    if (!g_ascii_isdigit(**p)) return FALSE;
    do {
        unsigned digit=(unsigned)(*(*p)++-'0');
        if (n>((uint64_t)INT64_MAX-digit)/10) return FALSE;
        n=n*10+digit;
    } while (g_ascii_isdigit(**p));
    *out=n; return TRUE;
}
static gboolean exact_range(const char *s, uint64_t partial, uint64_t expected)
{
    uint64_t start,end,total;
    if (!s || !g_str_has_prefix(s,"bytes ")) return FALSE;
    s+=6;
    return decimal(&s,&start) && *s++=='-' && decimal(&s,&end) && *s++=='/' &&
           decimal(&s,&total) && !*s && start==partial && start<=end && end==expected-1 && total==expected;
}
OtaDownloadAction ota_download_response(uint64_t partial, uint64_t expected, long status,
                                        const char *range, OtaError *error)
{
    if (!expected || expected>INT64_MAX || partial>expected) goto mismatch;
    if (status==416) return partial==expected ? OTA_DOWNLOAD_VERIFY : OTA_DOWNLOAD_RESTART;
    if (status==200) {
        if (range) goto mismatch;
        return OTA_DOWNLOAD_WRITE_ZERO;
    }
    if (status==206) {
        if (partial && partial<expected && exact_range(range,partial,expected)) return OTA_DOWNLOAD_APPEND;
        goto mismatch;
    }
    ota_error_set(error,OTA_ERROR_DOWNLOAD_HTTP,"Bundle HTTP %ld",status);
    return OTA_DOWNLOAD_REJECT;
 mismatch:
    ota_error_set(error,OTA_ERROR_DOWNLOAD_RANGE_MISMATCH,"Response does not describe the exact requested tail");
    return OTA_DOWNLOAD_REJECT;
}
typedef struct {
    int fd;
    uint64_t partial,expected,received,limit;
    long status;
    gsize header_bytes;
    gboolean headers_done,range_seen,length_seen,encoding_seen;
    uint64_t length;
    char range[128];
    OtaDownloadAction action;
    OtaError error;
} Transfer;
static gboolean disk_error(Transfer *t, const char *action)
{
    ota_error_set(&t->error,errno==ENOSPC || errno==EDQUOT ? OTA_ERROR_DOWNLOAD_DISK_SPACE : OTA_ERROR_PERSISTENCE_FAILED,
                  "%s: %s",action,g_strerror(errno));
    return FALSE;
}
static gboolean reset_file(Transfer *t)
{
    if (ftruncate(t->fd,0)<0 || lseek(t->fd,0,SEEK_SET)<0 || fsync(t->fd)<0) return disk_error(t,"Reset partial file");
    return TRUE;
}
static gboolean space_available(Transfer *t, uint64_t reserve)
{
    struct statvfs st;
    if (fstatvfs(t->fd,&st)<0) return disk_error(t,"Read available space");
    uint64_t unit=st.f_frsize ? st.f_frsize : st.f_bsize;
    uint64_t bytes=unit && (uint64_t)st.f_bavail>UINT64_MAX/unit ? UINT64_MAX : (uint64_t)st.f_bavail*unit;
    uint64_t needed=t->expected-t->partial;
    if (reserve>UINT64_MAX-needed || bytes<needed+reserve) {
        ota_error_set(&t->error,OTA_ERROR_DOWNLOAD_DISK_SPACE,"Insufficient space for bundle and configured reserve"); return FALSE;
    }
    return TRUE;
}
static size_t headers(char *data, size_t size, size_t count, void *user)
{
    Transfer *t=user;
    if (size && count>SIZE_MAX/size) return 0;
    size_t n=size*count;
    if (n>65536-t->header_bytes || memchr(data,0,n)) {
        ota_error_set(&t->error,OTA_ERROR_DOWNLOAD_HTTP,"Invalid or oversized HTTP headers"); return 0;
    }
    t->header_bytes+=n;
    char *line=g_strndup(data,n); g_strchomp(line);
    if (g_str_has_prefix(line,"HTTP/")) {
        if (t->headers_done) { ota_error_set(&t->error,OTA_ERROR_DOWNLOAD_HTTP,"Unexpected second final response"); goto bad; }
        char *p=strchr(line,' ');
        if (!p || strlen(p)<4 || !g_ascii_isdigit(p[1]) || !g_ascii_isdigit(p[2]) || !g_ascii_isdigit(p[3]) ||
            (p[4] && p[4]!=' ')) { ota_error_set(&t->error,OTA_ERROR_DOWNLOAD_HTTP,"Malformed HTTP status line"); goto bad; }
        t->status=(p[1]-'0')*100+(p[2]-'0')*10+p[3]-'0';
        t->range_seen=t->length_seen=t->encoding_seen=FALSE;
        t->range[0]=0;
    } else if (t->headers_done) {
        /* Chunked trailers cannot authorize writes or modify the accepted range. */
    } else if (!*line) {
        if (t->status>=100 && t->status<200 && t->status!=101) { g_free(line); return n; }
        t->action=ota_download_response(t->partial,t->expected,t->status,t->range_seen?t->range:NULL,&t->error);
        if (t->action==OTA_DOWNLOAD_REJECT) goto bad;
        t->headers_done=TRUE;
        if (t->action==OTA_DOWNLOAD_APPEND || t->action==OTA_DOWNLOAD_WRITE_ZERO) {
            t->limit=t->action==OTA_DOWNLOAD_APPEND ? t->expected-t->partial : t->expected;
            if (t->length_seen && t->length!=t->limit) {
                ota_error_set(&t->error,OTA_ERROR_DOWNLOAD_SIZE_MISMATCH,"Content-Length differs from expected response size"); goto bad;
            }
            /* Gate FIRST; a full 200 response is only ever written from byte zero. */
            if (t->action==OTA_DOWNLOAD_WRITE_ZERO) {
                if (!reset_file(t)) goto bad;
            } else if (lseek(t->fd,(off_t)t->partial,SEEK_SET)<0) { disk_error(t,"Seek partial file"); goto bad; }
        }
    } else {
        char *colon=strchr(line,':');
        if (!colon) { ota_error_set(&t->error,OTA_ERROR_DOWNLOAD_HTTP,"Malformed HTTP header"); goto bad; }
        *colon=0; char *value=g_strstrip(colon+1);
        if (!g_ascii_strcasecmp(line,"Content-Range")) {
            if (t->range_seen || strlen(value)>=sizeof(t->range)) goto range_bad;
            t->range_seen=TRUE; g_strlcpy(t->range,value,sizeof(t->range));
        } else if (!g_ascii_strcasecmp(line,"Content-Length")) {
            const char *p=value;
            if (t->length_seen || !decimal(&p,&t->length) || *p) {
                ota_error_set(&t->error,OTA_ERROR_DOWNLOAD_SIZE_MISMATCH,"Invalid or duplicate Content-Length"); goto bad;
            }
            t->length_seen=TRUE;
        } else if (!g_ascii_strcasecmp(line,"Content-Encoding")) {
            if (t->encoding_seen || g_ascii_strcasecmp(value,"identity")) {
                ota_error_set(&t->error,OTA_ERROR_DOWNLOAD_HTTP,"Encoded bundle response is not permitted"); goto bad;
            }
            t->encoding_seen=TRUE;
        }
    }
    g_free(line); return n;
 range_bad:
    ota_error_set(&t->error,OTA_ERROR_DOWNLOAD_RANGE_MISMATCH,"Duplicate or oversized Content-Range");
 bad:
    g_free(line); return 0;
}
static size_t body(char *data, size_t size, size_t count, void *user)
{
    Transfer *t=user;
    if (size && count>SIZE_MAX/size) return 0;
    size_t n=size*count;
    if (!t->headers_done || (t->action!=OTA_DOWNLOAD_APPEND && t->action!=OTA_DOWNLOAD_WRITE_ZERO))
        return 0; /* Including 416 bodies: never write, recover after perform. */
    if (n>t->limit-t->received) {
        ota_error_set(&t->error,OTA_ERROR_DOWNLOAD_SIZE_MISMATCH,"Response exceeds expected bundle size"); return 0;
    }
    size_t used=0;
    while (used<n) {
        ssize_t wrote=write(t->fd,data+used,n-used);
        if (wrote<0 && errno==EINTR) continue;
        if (wrote<=0) { if (!wrote) errno=EIO; disk_error(t,"Write partial file"); return 0; }
        used+=(size_t)wrote; t->received+=(uint64_t)wrote;
    }
    return n;
}
static gboolean verify(Transfer *t, const char *hash)
{
    struct stat st;
    if (fstat(t->fd,&st)<0) return disk_error(t,"Stat downloaded bundle");
    if (st.st_size<0 || (uint64_t)st.st_size!=t->expected) {
        ota_error_set(&t->error,OTA_ERROR_DOWNLOAD_SIZE_MISMATCH,"Downloaded file size does not match manifest"); return FALSE;
    }
    if (lseek(t->fd,0,SEEK_SET)<0) return disk_error(t,"Seek downloaded bundle");
    GChecksum *sum=g_checksum_new(G_CHECKSUM_SHA256);
    char buffer[65536]; uint64_t bytes=0; gboolean ok=FALSE;
    for (;;) {
        ssize_t n=read(t->fd,buffer,sizeof(buffer));
        if (n<0) { if (errno==EINTR) continue; disk_error(t,"Read downloaded bundle"); goto done; }
        if (!n) break;
        if ((uint64_t)n>t->expected-bytes) {
            ota_error_set(&t->error,OTA_ERROR_DOWNLOAD_SIZE_MISMATCH,"Bundle grew during verification"); goto done;
        }
        bytes+=(uint64_t)n; g_checksum_update(sum,(guchar *)buffer,n);
    }
    if (bytes!=t->expected) {
        ota_error_set(&t->error,OTA_ERROR_DOWNLOAD_SIZE_MISMATCH,"Bundle changed during verification"); goto done;
    }
    if (strcmp(g_checksum_get_string(sum),hash)) {
        /* Discard invalid COMPLETE data, including a complete corrupt .part on entry. */
        if (!reset_file(t)) goto done;
        ota_error_set(&t->error,OTA_ERROR_DOWNLOAD_HASH_MISMATCH,"SHA256 mismatch; invalid partial data discarded"); goto done;
    }
    ok=TRUE;
 done:
    g_checksum_free(sum); return ok;
}

static gboolean validate_bundle_path(const char *path, const OtaManifest *m,
                                     gboolean *missing, OtaError *error)
{
    *missing = FALSE;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT) { *missing = TRUE; return FALSE; }
        ota_error_set(error, OTA_ERROR_PERSISTENCE_FAILED,
                      "Open completed bundle: %s", g_strerror(errno));
        return FALSE;
    }
    struct stat st;
    gboolean ok = FALSE;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
        ota_error_set(error, OTA_ERROR_PERSISTENCE_FAILED,
                      "Completed bundle is not a private regular file");
        goto done;
    }
    if (st.st_size < 0 || (uint64_t)st.st_size != m->size) {
        ota_error_set(error, OTA_ERROR_DOWNLOAD_SIZE_MISMATCH,
                      "Completed bundle size differs from persisted attempt");
        goto done;
    }
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    char buffer[65536];
    uint64_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            ota_error_set(error, OTA_ERROR_PERSISTENCE_FAILED,
                          "Read completed bundle: %s", g_strerror(errno));
            break;
        }
        if (!n) {
            if (total == m->size && !strcmp(g_checksum_get_string(sum), m->sha256))
                ok = TRUE;
            else ota_error_set(error, OTA_ERROR_DOWNLOAD_HASH_MISMATCH,
                               "Completed bundle hash differs from persisted attempt");
            break;
        }
        total += (uint64_t)n;
        if (total > m->size) {
            ota_error_set(error, OTA_ERROR_DOWNLOAD_SIZE_MISMATCH,
                          "Completed bundle grew during validation");
            break;
        }
        g_checksum_update(sum, (const guchar *)buffer, (gsize)n);
    }
    g_checksum_free(sum);
done:
    close(fd);
    return ok;
}

gboolean ota_download_validate_bundle(const OtaConfig *c, const OtaManifest *m,
                                      OtaError *error)
{
    if (!c || !m || !ota_manifest_validate(m, error) || c->bundle_file[0] != '/') {
        if (error && error->code == OTA_ERROR_NONE)
            ota_error_set(error, OTA_ERROR_CONFIG_INVALID, "Invalid completed-bundle validation input");
        return FALSE;
    }
    gboolean missing = FALSE;
    if (validate_bundle_path(c->bundle_file, m, &missing, error)) return TRUE;
    if (missing) ota_error_set(error, OTA_ERROR_DOWNLOAD_SIZE_MISMATCH,
                               "Completed bundle is absent");
    return FALSE;
}

gboolean ota_download_discard_partial(const OtaConfig *c, OtaError *error)
{
    if (!c || c->part_file[0] != '/') {
        ota_error_set(error, OTA_ERROR_CONFIG_INVALID, "Invalid partial path");
        return FALSE;
    }
    struct stat st;
    if (lstat(c->part_file, &st) < 0) {
        if (errno == ENOENT) return TRUE;
        ota_error_set(error, OTA_ERROR_PERSISTENCE_FAILED,
                      "Inspect stale partial: %s", g_strerror(errno));
        return FALSE;
    }
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1) {
        ota_error_set(error, OTA_ERROR_PERSISTENCE_FAILED,
                      "Stale partial is not a private regular file");
        return FALSE;
    }
    char *parent = g_path_get_dirname(c->part_file);
    int dir = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    g_free(parent);
    if (dir < 0 || unlink(c->part_file) < 0 || fsync(dir) < 0) {
        if (dir >= 0) close(dir);
        ota_error_set(error, OTA_ERROR_PERSISTENCE_FAILED,
                      "Durably discard stale partial: %s", g_strerror(errno));
        return FALSE;
    }
    close(dir);
    return TRUE;
}

static gboolean publish(Transfer *t, const char *part, const char *bundle)
{
    char *parent=g_path_get_dirname(bundle);
    int dir=open(parent,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW); g_free(parent);
    if (dir<0) return disk_error(t,"Open bundle directory");
    gboolean ok=FALSE;
    struct stat st;
    if (lstat(part,&st)<0) goto failed;
    struct stat held;
    if (fstat(t->fd,&held)<0) goto failed;
    if (!S_ISREG(st.st_mode) || st.st_dev!=held.st_dev || st.st_ino!=held.st_ino) { errno=ESTALE; goto failed; }
    if (fsync(t->fd)<0 || rename(part,bundle)<0 || fsync(dir)<0) goto failed;
    ok=TRUE; goto done;
 failed:
    disk_error(t,"Durably publish verified bundle");
 done:
    close(dir); return ok;
}
gboolean ota_download_bundle(const OtaConfig *c, const OtaManifest *m, gboolean *retryable, OtaError *error)
{
    if (retryable) *retryable=FALSE;
    if (!ota_manifest_validate(m,error)) return FALSE;
    if (!c || !memchr(c->part_file,0,sizeof(c->part_file)) || !memchr(c->bundle_file,0,sizeof(c->bundle_file)) ||
        c->part_file[0]!='/' || c->bundle_file[0]!='/' || !strcmp(c->part_file,c->bundle_file)) {
        ota_error_set(error,OTA_ERROR_CONFIG_INVALID,"Invalid download paths"); return FALSE;
    }
    char *a=g_path_get_dirname(c->part_file), *b=g_path_get_dirname(c->bundle_file);
    gboolean same_parent=!strcmp(a,b); g_free(a); g_free(b);
    if (!same_parent) { ota_error_set(error,OTA_ERROR_CONFIG_INVALID,"Download files must share one directory"); return FALSE; }
    gboolean final_missing = FALSE;
    OtaError final_error = {0};
    if (validate_bundle_path(c->bundle_file, m, &final_missing, &final_error)) {
        if (!ota_download_discard_partial(c, error)) return FALSE;
        return TRUE;
    }
    if (!final_missing && final_error.code != OTA_ERROR_DOWNLOAD_SIZE_MISMATCH &&
        final_error.code != OTA_ERROR_DOWNLOAD_HASH_MISMATCH) {
        if (error) *error = final_error;
        return FALSE;
    }
    Transfer t={.fd=-1,.expected=m->size}; gboolean ok=FALSE;
    t.fd=open(c->part_file,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);
    struct stat st;
    if (t.fd<0 || fstat(t.fd,&st)<0) { disk_error(&t,"Open partial file"); goto done; }
    if (!S_ISREG(st.st_mode) || st.st_nlink!=1 || st.st_size<0) {
        ota_error_set(&t.error,OTA_ERROR_PERSISTENCE_FAILED,"Partial path is not a private regular file"); goto done;
    }
    if (flock(t.fd,LOCK_EX|LOCK_NB)<0) { disk_error(&t,"Lock partial file"); goto done; }
    t.partial=(uint64_t)st.st_size;
    if (t.partial>t.expected) { if (!reset_file(&t)) goto done; t.partial=0; }
    if (t.partial==t.expected) goto complete;
    if (!space_available(&t,c->reserve_bytes)) goto done;
    for (unsigned attempt=0;attempt<2;++attempt) {
        CURL *curl=ota_http_open(c,m->artifact_url,OTA_ERROR_DOWNLOAD_HTTP,&t.error);
        if (!curl) goto done;
        char range[32]; g_snprintf(range,sizeof(range),"%" PRIu64 "-",t.partial);
        CURLcode result=CURLE_FAILED_INIT;
#define SET(opt,value) do { if (curl_easy_setopt(curl,opt,value)!=CURLE_OK) goto performed; } while (0)
        SET(CURLOPT_HEADERFUNCTION,headers); SET(CURLOPT_HEADERDATA,&t);
        SET(CURLOPT_WRITEFUNCTION,body); SET(CURLOPT_WRITEDATA,&t);
        if (t.partial) SET(CURLOPT_RANGE,range);
        result=curl_easy_perform(curl);
#undef SET
 performed:
        curl_easy_cleanup(curl);
        if (t.error.code!=OTA_ERROR_NONE) {
            if (retryable && t.error.code==OTA_ERROR_DOWNLOAD_HTTP) *retryable=ota_http_retryable(result,t.status);
            goto done;
        }
        if (t.headers_done && t.status==416) {
            if (t.action==OTA_DOWNLOAD_VERIFY) goto complete;
            if (!reset_file(&t)) goto done;
            if (attempt==1) { ota_error_set(&t.error,OTA_ERROR_DOWNLOAD_HTTP,"Repeated HTTP 416 after restart"); goto done; }
            int fd=t.fd; t=(Transfer){.fd=fd,.expected=m->size};
            if (!space_available(&t,c->reserve_bytes)) goto done;
            continue;
        }
        if (result!=CURLE_OK) {
            if (retryable) *retryable=ota_http_retryable(result,t.status);
            ota_error_set(&t.error,OTA_ERROR_DOWNLOAD_HTTP,"Bundle HTTP %ld, transport %d; partial retained",t.status,(int)result);
            goto done;
        }
        if (!t.headers_done || t.received!=t.limit) {
            ota_error_set(&t.error,OTA_ERROR_DOWNLOAD_SIZE_MISMATCH,"Incomplete response body; partial retained"); goto done;
        }
        goto complete;
    }
    goto done;
 complete:
    if (verify(&t,m->sha256)) ok=publish(&t,c->part_file,c->bundle_file);
 done:
    if (!ok && t.fd>=0 && t.received && fsync(t.fd)<0) {
        disk_error(&t,"Flush retained partial");
        if (retryable) *retryable=FALSE;
    }
    if (t.fd>=0) close(t.fd);
    if (!ok && error) *error=t.error;
    return ok;
}
