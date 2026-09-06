#include "edgeguard_ota/time_source.h"
#include <errno.h>
#include <string.h>

static int read_clock(const OtaTimeSource *source, clockid_t clock, struct timespec *out)
{
    return source && source->read ? source->read(source->user, clock, out) : clock_gettime(clock, out);
}

gboolean ota_monotonic_ms(const OtaTimeSource *source, uint64_t *out, OtaError *error)
{
    struct timespec value;
    if (read_clock(source, CLOCK_MONOTONIC, &value) < 0 || value.tv_sec < 0 ||
        value.tv_nsec < 0 || value.tv_nsec >= 1000000000L ||
        (uint64_t)value.tv_sec > (UINT64_MAX - 999) / 1000) {
        ota_error_set(error, OTA_ERROR_TIME_SOURCE_FAILED, "Invalid/unavailable monotonic clock");
        return FALSE;
    }
    *out = (uint64_t)value.tv_sec * 1000 + (uint64_t)value.tv_nsec / 1000000;
    return TRUE;
}

gboolean ota_deadline_after(const OtaTimeSource *source, uint64_t delay_ms,
                            uint64_t *deadline, OtaError *error)
{
    uint64_t now;
    if (!ota_monotonic_ms(source, &now, error)) return FALSE;
    if (delay_ms > UINT64_MAX - now) {
        ota_error_set(error, OTA_ERROR_TIME_SOURCE_FAILED, "Monotonic deadline overflow");
        return FALSE;
    }
    *deadline = now + delay_ms;
    return TRUE;
}

gboolean ota_deadline_expired(const OtaTimeSource *source, uint64_t deadline,
                              gboolean *expired, OtaError *error)
{
    uint64_t now;
    if (!ota_monotonic_ms(source, &now, error)) return FALSE;
    *expired = now >= deadline;
    return TRUE;
}

gboolean ota_time_telemetry(const OtaTimeSource *source, OtaReport *report, OtaError *error)
{
    report->timestamp_valid = FALSE;
    report->timestamp[0] = 0;
    if (!ota_monotonic_ms(source, &report->uptime_ms, error)) return FALSE;
    struct timespec wall;
    struct tm utc;
    if (read_clock(source, CLOCK_REALTIME, &wall) == 0 && gmtime_r(&wall.tv_sec, &utc))
        strftime(report->timestamp, sizeof(report->timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    /* Realtime failure does not invalidate monotonic telemetry. */
    return TRUE;
}
