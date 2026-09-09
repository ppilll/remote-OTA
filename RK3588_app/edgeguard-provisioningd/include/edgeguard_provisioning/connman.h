#ifndef EDGEGUARD_PROVISIONING_CONNMAN_H
#define EDGEGUARD_PROVISIONING_CONNMAN_H

#include "model.h"
#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _EgpConnman EgpConnman;

typedef enum {
    EGP_CONNMAN_CONNECTED,
    EGP_CONNMAN_ROLLBACK_ALLOWED,
    EGP_CONNMAN_RETAIN_CANONICAL
} EgpConnmanDisposition;

typedef struct {
    EgpConnmanDisposition disposition;
    EgpProvisioningState state;
    EgpErrorCode code;
    char service_path[EGP_DBUS_PATH_CAP];
} EgpConnmanResult;

/* connection is referenced by the adapter. NULL obtains the system bus. The
 * production profile path is fixed; EGP_ALLOW_TEST_PATHS permits a fixture. */
gboolean egp_connman_new(GDBusConnection *connection, const char *profile_path,
                         EgpConnman **out, EgpError *error);
void egp_connman_free(EgpConnman *connman);

/* Blocking, bounded operations. Call them only from the daemon's serialized
 * mutation worker, never directly from a GDBus method callback/main-loop
 * dispatch. The adapter derives service identity from validated wifi; callers
 * cannot supply a D-Bus object path. */
gboolean egp_connman_apply(EgpConnman *connman, const EgpWifiConfig *wifi,
                           guint timeout_ms, GCancellable *cancellable,
                           EgpConnmanResult *result, EgpError *error);
gboolean egp_connman_revoke(EgpConnman *connman, const EgpWifiConfig *wifi,
                            guint timeout_ms, GCancellable *cancellable,
                            EgpConnmanResult *result, EgpError *error);

/* Startup helpers modify only the owned derived path. write_profile is atomic;
 * remove_profile fsyncs the parent. */
gboolean egp_connman_write_profile(EgpConnman *connman, const EgpWifiConfig *wifi,
                                   EgpError *error);
gboolean egp_connman_remove_profile(EgpConnman *connman, EgpError *error);

/* Pure bounded backoff: 1 s exponential base, 60 s ceiling, +/-20% jitter.
 * random_value is caller-supplied so tests can be deterministic. */
guint egp_connman_retry_delay_ms(guint attempt, guint32 random_value);

G_END_DECLS

#endif
