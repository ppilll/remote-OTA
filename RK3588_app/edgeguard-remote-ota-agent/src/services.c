#include "edgeguard_ota/services.h"
#include "edgeguard_ota/compatibility.h"
#include "edgeguard_ota/manifest.h"
#include <string.h>

#define DEVICE_COMPATIBLE "atk-dlrk3588"
#define RAUC_COMPATIBLE "EdgeGuard-ATK-DLRK3588-RK3588"

static gboolean load_release(void *user, const char *path, OtaRelease *out,
                             OtaError *error)
{
    (void)user;
    return ota_release_load(path, out, error);
}

static gboolean current_slot(void *user, OtaSlot *out, OtaError *error)
{
    OtaAgentServiceContext *context = user;
    return ota_rauc_current_slot(&context->rauc, out, error);
}

static OtaManifest attempt_manifest(const OtaRelease *release,
                                    const OtaPersistentState *state)
{
    OtaManifest manifest = {.schema_version = OTA_SCHEMA_VERSION, .mandatory = FALSE};
    g_strlcpy(manifest.device_compatible, release->device_compatible,
              sizeof(manifest.device_compatible));
    g_strlcpy(manifest.version, state->target_version, sizeof(manifest.version));
    g_strlcpy(manifest.build_id, state->build_id, sizeof(manifest.build_id));
    g_strlcpy(manifest.artifact_url, state->artifact_url, sizeof(manifest.artifact_url));
    g_strlcpy(manifest.sha256, state->expected_sha256, sizeof(manifest.sha256));
    manifest.size = state->expected_size;
    return manifest;
}

static gboolean candidate_release_valid(const OtaRelease *release,
                                        const OtaPersistentState *state,
                                        OtaError *error)
{
    if (!ota_release_validate(release, error)) return FALSE;
    if (strcmp(release->device_compatible, DEVICE_COMPATIBLE) ||
        strcmp(release->rauc_compatible, RAUC_COMPATIBLE) ||
        strcmp(release->version, state->target_version) ||
        strcmp(release->build_id, state->build_id)) {
        ota_error_set(error, OTA_ERROR_RAUC_IDENTITY_MISMATCH,
                      "Booted release identity differs from the durable candidate attempt");
        return FALSE;
    }
    return TRUE;
}

static OtaPhaseResult transport_result(gboolean retryable)
{
    return retryable ? OTA_PHASE_RETRY : OTA_PHASE_HARD_FAILURE;
}

static OtaPhaseResult report_phase(OtaAgentServiceContext *context,
                                   const OtaConfig *config,
                                   const OtaRelease *release,
                                   const char *device_id,
                                   const OtaPersistentState *current,
                                   OtaError *error)
{
    OtaReport report;
    if (!ota_report_build(device_id, release, current, &context->time, &report, error))
        return OTA_PHASE_HARD_FAILURE;
    gboolean retryable = FALSE;
    if (!context->report_send(config, &report, &retryable, error))
        return transport_result(retryable);
    return OTA_PHASE_ADVANCE;
}

static OtaPhaseResult phase(void *user, const OtaConfig *config,
                            const OtaRelease *release, const char *device_id,
                            const OtaPersistentState *current,
                            OtaPersistentState *next, OtaError *error)
{
    OtaAgentServiceContext *context = user;
    gboolean retryable = FALSE;
    *next = *current;
    switch (current->state) {
    case OTA_STATE_CHECK_NETWORK:
        next->state = OTA_STATE_CHECK_UPDATE;
        return OTA_PHASE_ADVANCE;
    case OTA_STATE_CHECK_UPDATE: {
        OtaManifest manifest = {0};
        gboolean available = FALSE;
        if (!context->manifest_fetch(config, &manifest, &available, &retryable, error))
            return transport_result(retryable);
        if (!available) {
            next->state = OTA_STATE_IDLE;
            return OTA_PHASE_ADVANCE;
        }
        gboolean update = FALSE;
        if (!ota_compatibility_precheck(release, &manifest, &update, error))
            return OTA_PHASE_HARD_FAILURE;
        if (!update) {
            next->state = OTA_STATE_IDLE;
            return OTA_PHASE_ADVANCE;
        }
        next->state = OTA_STATE_PRECHECK;
        g_strlcpy(next->target_version, manifest.version, sizeof(next->target_version));
        g_strlcpy(next->build_id, manifest.build_id, sizeof(next->build_id));
        g_strlcpy(next->artifact_url, manifest.artifact_url, sizeof(next->artifact_url));
        next->expected_size = manifest.size;
        g_strlcpy(next->expected_sha256, manifest.sha256, sizeof(next->expected_sha256));
        return OTA_PHASE_ADVANCE;
    }
    case OTA_STATE_PRECHECK: {
        OtaManifest manifest = attempt_manifest(release, current);
        gboolean update = FALSE;
        /* Revalidate durable version/build/path/hash/size. Device compatibility
         * was checked before this snapshot was admitted from CHECK_UPDATE. */
        if (!ota_compatibility_precheck(release, &manifest, &update, error))
            return OTA_PHASE_HARD_FAILURE;
        if (!update) {
            ota_error_set(error, OTA_ERROR_ILLEGAL_TRANSITION,
                          "Persisted PRECHECK attempt is not an upgrade");
            return OTA_PHASE_HARD_FAILURE;
        }
        /* PRECHECK is reached only for a newly created durable attempt. Replaying
         * it after a crash is safe because no download has begun in this state. */
        if (!context->discard_partial(config, error)) return OTA_PHASE_HARD_FAILURE;
        next->state = OTA_STATE_DOWNLOADING;
        return OTA_PHASE_ADVANCE;
    }
    case OTA_STATE_DOWNLOADING: {
        OtaManifest manifest = attempt_manifest(release, current);
        if (!context->download_bundle(config, &manifest, &retryable, error))
            return transport_result(retryable);
        next->state = OTA_STATE_VERIFY_DOWNLOAD;
        return OTA_PHASE_ADVANCE;
    }
    case OTA_STATE_VERIFY_DOWNLOAD: {
        OtaManifest manifest = attempt_manifest(release, current);
        if (!context->download_validate(config, &manifest, error))
            return OTA_PHASE_HARD_FAILURE;
        next->state = OTA_STATE_RAUC_VERIFY;
        return OTA_PHASE_ADVANCE;
    }
    case OTA_STATE_RAUC_VERIFY:
        if (!ota_rauc_verify(&context->rauc, config->bundle_file, release,
                             current, error)) return OTA_PHASE_HARD_FAILURE;
        next->state = OTA_STATE_INSTALLING;
        return OTA_PHASE_ADVANCE;
    case OTA_STATE_INSTALLING:
        if (!ota_rauc_install(&context->rauc, config->bundle_file, error))
            return OTA_PHASE_HARD_FAILURE;
        next->state = OTA_STATE_REBOOT_PENDING;
        return OTA_PHASE_ADVANCE;
    case OTA_STATE_HEALTH_CHECK:
        if (!candidate_release_valid(release, current, error))
            return OTA_PHASE_HARD_FAILURE;
        context->health.rauc = &context->rauc;
        context->health.hook = config->health_hook;
        context->health.timeout_sec = config->health_timeout_sec;
        if (context->health_check(&context->health,
                                  current->expected_candidate_slot, error)) {
            next->state = OTA_STATE_MARK_GOOD;
            return OTA_PHASE_ADVANCE;
        } else {
            OtaError health_error = *error;
            if (!ota_rauc_mark_bad(&context->rauc,
                                   current->expected_candidate_slot, error))
                return OTA_PHASE_HARD_FAILURE;
            next->state = OTA_STATE_ROLLBACK;
            next->last_error = health_error;
            return OTA_PHASE_ADVANCE;
        }
    case OTA_STATE_MARK_GOOD:
        if (!candidate_release_valid(release, current, error) ||
            !ota_rauc_mark_good(&context->rauc,
                                current->expected_candidate_slot, error))
            return OTA_PHASE_HARD_FAILURE;
        next->state = OTA_STATE_REPORT_SUCCESS;
        return OTA_PHASE_ADVANCE;
    case OTA_STATE_REPORT_SUCCESS: {
        OtaPhaseResult result = report_phase(context, config, release, device_id,
                                             current, error);
        if (result == OTA_PHASE_ADVANCE) next->state = OTA_STATE_IDLE;
        return result;
    }
    case OTA_STATE_ERROR:
    case OTA_STATE_ROLLBACK:
        return report_phase(context, config, release, device_id, current, error);
    default:
        ota_error_set(error, OTA_ERROR_ILLEGAL_TRANSITION,
                      "No service action exists for state %s",
                      ota_state_name(current->state));
        return OTA_PHASE_HARD_FAILURE;
    }
}

void ota_agent_service_context_init(OtaAgentServiceContext *context)
{
    memset(context, 0, sizeof(*context));
    ota_rauc_adapter_init(&context->rauc);
    ota_health_init(&context->health, &context->rauc);
    context->manifest_fetch = ota_manifest_fetch;
    context->download_bundle = ota_download_bundle;
    context->download_validate = ota_download_validate_bundle;
    context->discard_partial = ota_download_discard_partial;
    context->health_check = ota_health_check;
    context->report_send = ota_report_send;
}

void ota_agent_services_bind(OtaAgentServices *services,
                             OtaAgentServiceContext *context)
{
    memset(services, 0, sizeof(*services));
    services->user = context;
    services->load_release = load_release;
    services->phase = phase;
    services->current_slot = current_slot;
    services->reboot.request = ota_reboot_production;
    services->time = context->time;
}

gboolean ota_agent_services_init(OtaAgentServices *services, OtaError *error)
{
    static OtaAgentServiceContext production;
    if (!services) {
        ota_error_set(error, OTA_ERROR_CONFIG_INVALID,
                      "Missing production service output");
        return FALSE;
    }
    ota_agent_service_context_init(&production);
    ota_agent_services_bind(services, &production);
    return TRUE;
}
