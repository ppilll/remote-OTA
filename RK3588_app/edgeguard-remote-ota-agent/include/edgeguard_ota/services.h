#ifndef EDGEGUARD_OTA_SERVICES_H
#define EDGEGUARD_OTA_SERVICES_H

#include "download.h"
#include "health.h"
#include "reporting.h"
#include "state_machine.h"

/* Production defaults plus narrow dependency seams for the merged host suite. */
typedef struct {
    OtaRaucAdapter rauc;
    OtaHealth health;
    OtaTimeSource time;
    gboolean (*manifest_fetch)(const OtaConfig *, OtaManifest *, gboolean *,
                               gboolean *, OtaError *);
    gboolean (*download_bundle)(const OtaConfig *, const OtaManifest *,
                                gboolean *, OtaError *);
    gboolean (*download_validate)(const OtaConfig *, const OtaManifest *, OtaError *);
    gboolean (*discard_partial)(const OtaConfig *, OtaError *);
    gboolean (*health_check)(const OtaHealth *, OtaSlot, OtaError *);
    gboolean (*report_send)(const OtaConfig *, const OtaReport *, gboolean *, OtaError *);
} OtaAgentServiceContext;

void ota_agent_service_context_init(OtaAgentServiceContext *context);
void ota_agent_services_bind(OtaAgentServices *services,
                             OtaAgentServiceContext *context);

#endif
