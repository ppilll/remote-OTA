#ifndef EDGEGUARD_OTA_CONFIG_H
#define EDGEGUARD_OTA_CONFIG_H
#include "model.h"

typedef struct {
    char base_url[OTA_PATH_CAP], manifest_path[OTA_PATH_CAP], report_path[OTA_PATH_CAP];
    uint32_t poll_interval_sec, connect_timeout_sec, request_timeout_sec;
    char state_dir[OTA_PATH_CAP], release_file[OTA_PATH_CAP];
    uint32_t max_manifest_bytes;
    char part_file[OTA_PATH_CAP], bundle_file[OTA_PATH_CAP];
    uint64_t reserve_bytes;
    char rauc_binary[OTA_PATH_CAP], device_id_file[OTA_PATH_CAP];
    char health_hook[OTA_PATH_CAP];
    uint32_t health_timeout_sec;
    OtaReportingMode reporting_mode;
} OtaConfig;

/* Only health.hook and reporting.mode are optional (empty / legacy). */
gboolean ota_config_load(const char *path, OtaConfig *out, OtaError *error);
#endif
