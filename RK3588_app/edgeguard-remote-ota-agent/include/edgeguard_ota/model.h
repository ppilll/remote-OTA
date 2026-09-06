#ifndef EDGEGUARD_OTA_MODEL_H
#define EDGEGUARD_OTA_MODEL_H

#include <glib.h>
#include <stdint.h>
#include <stdarg.h>

/* Thread 1 owns these shared contracts. Strings are bounded, owned values. */
#define OTA_SCHEMA_VERSION 1
#define OTA_UUID_CAP 37
#define OTA_TEXT_CAP 256
#define OTA_PATH_CAP 4096
#define OTA_ERROR_CAP 512
#define OTA_SHA256_CAP 65
#define OTA_STATE_MAX_BYTES (64 * 1024)

typedef enum {
    OTA_STATE_IDLE, OTA_STATE_CHECK_NETWORK, OTA_STATE_CHECK_UPDATE,
    OTA_STATE_PRECHECK, OTA_STATE_DOWNLOADING, OTA_STATE_VERIFY_DOWNLOAD,
    OTA_STATE_RAUC_VERIFY, OTA_STATE_INSTALLING, OTA_STATE_REBOOT_PENDING,
    OTA_STATE_BOOT_NEW_SLOT, OTA_STATE_HEALTH_CHECK, OTA_STATE_MARK_GOOD,
    OTA_STATE_REPORT_SUCCESS, OTA_STATE_ROLLBACK, OTA_STATE_ERROR,
    OTA_STATE_COUNT
} OtaState;

#define OTA_ERROR_CODES(X) \
    X(NONE) X(CONFIG_INVALID) X(IDENTITY_INVALID) X(IDENTITY_AMBIGUOUS) \
    X(LOCAL_RELEASE_INVALID) X(MANIFEST_HTTP) X(MANIFEST_INVALID) \
    X(DEVICE_COMPAT_MISMATCH) X(VERSION_MALFORMED) X(VERSION_COLLISION) \
    X(DOWNGRADE_REJECTED) X(DOWNLOAD_HTTP) X(DOWNLOAD_RANGE_MISMATCH) \
    X(DOWNLOAD_DISK_SPACE) X(DOWNLOAD_SIZE_MISMATCH) X(DOWNLOAD_HASH_MISMATCH) \
    X(RAUC_VERIFY_FAILED) X(RAUC_COMPAT_MISMATCH) X(RAUC_IDENTITY_MISMATCH) X(RAUC_INSTALL_FAILED) \
    X(RAUC_MARK_GOOD_FAILED) X(RAUC_MARK_BAD_FAILED) X(HEALTH_FAILED) \
    X(REBOOT_CONTEXT_INVALID) X(REBOOT_FAILED) X(PERSISTENCE_FAILED) \
    X(REPORT_FAILED) X(ILLEGAL_TRANSITION) X(TIME_SOURCE_FAILED)

typedef enum {
#define OTA_ERROR_ENUM(name) OTA_ERROR_##name,
    OTA_ERROR_CODES(OTA_ERROR_ENUM)
#undef OTA_ERROR_ENUM
    OTA_ERROR_COUNT
} OtaErrorCode;

typedef struct { OtaErrorCode code; char message[OTA_ERROR_CAP]; } OtaError;

static inline void ota_error_set(OtaError *error, OtaErrorCode code,
                                 const char *format, ...) G_GNUC_PRINTF(3, 4);
static inline void ota_error_set(OtaError *error, OtaErrorCode code,
                                 const char *format, ...)
{
    if (!error) return;
    error->code = code;
    va_list args;
    va_start(args, format);
    g_vsnprintf(error->message, sizeof(error->message), format, args);
    va_end(args);
}

static inline const char *ota_error_code_name(OtaErrorCode code)
{
    switch (code) {
#define OTA_ERROR_CASE(name) case OTA_ERROR_##name: return #name;
        OTA_ERROR_CODES(OTA_ERROR_CASE)
#undef OTA_ERROR_CASE
    default: return NULL;
    }
}

typedef enum { OTA_SLOT_UNKNOWN, OTA_SLOT_A, OTA_SLOT_B } OtaSlot;
typedef enum {
    OTA_REBOOT_NONE, OTA_REBOOT_INSTALL, OTA_REBOOT_HEALTH_FAILURE
} OtaRebootContext;
typedef enum { OTA_REPORT_LEGACY, OTA_REPORT_EXTENDED } OtaReportingMode;

typedef struct { uint32_t major, minor, patch; } OtaVersion;
typedef struct {
    int schema_version;
    char device_compatible[OTA_TEXT_CAP];
    char version[OTA_TEXT_CAP];
    char build_id[OTA_TEXT_CAP];
    char artifact_url[OTA_PATH_CAP];
    char sha256[OTA_SHA256_CAP];
    uint64_t size; /* JSON wire representation must fit signed 64 bits. */
    gboolean mandatory;
} OtaManifest;

typedef struct {
    int schema_version;
    char device_compatible[OTA_TEXT_CAP];
    char rauc_compatible[OTA_TEXT_CAP];
    char version[OTA_TEXT_CAP];
    char build_id[OTA_TEXT_CAP];
} OtaRelease;

typedef struct {
    int schema_version;
    OtaState state;
    char attempt_id[OTA_UUID_CAP];
    char target_version[OTA_TEXT_CAP];
    char build_id[OTA_TEXT_CAP];
    char artifact_url[OTA_PATH_CAP];
    uint64_t expected_size;
    char expected_sha256[OTA_SHA256_CAP];
    OtaError last_error;
    OtaSlot previous_slot;
    OtaSlot expected_candidate_slot;
    char boot_id_before[OTA_UUID_CAP];
    OtaRebootContext reboot_context;
} OtaPersistentState;

typedef struct {
    char device_id[OTA_UUID_CAP];
    char current_version[OTA_TEXT_CAP];
    char timestamp[32]; /* best effort UTC, never decision authority */
    gboolean timestamp_valid; /* false until trusted time source is frozen */
    uint64_t uptime_ms;
    char attempt_id[OTA_UUID_CAP];
    char target_version[OTA_TEXT_CAP];
    char build_id[OTA_TEXT_CAP];
    OtaState agent_state;
    OtaErrorCode error_code;
} OtaReport;

#endif
