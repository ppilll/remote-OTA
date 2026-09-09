#ifndef EDGEGUARD_PROVISIONING_SECURITY_H
#define EDGEGUARD_PROVISIONING_SECURITY_H

#include "model.h"

G_BEGIN_DECLS

#define EGP_WINDOW_DURATION_US (120 * G_USEC_PER_SEC)

typedef struct {
    gboolean connected;
    gboolean paired;
    gboolean bonded;
    gboolean stable_identity;
    /* True only when the call arrived through a BlueZ characteristic whose
     * exported flag requires link encryption. */
    gboolean encrypted_transport;
} EgpPeerSecurity;

typedef void (*EgpWindowChanged)(gpointer user_data, gboolean open);
typedef struct _EgpSecurity EgpSecurity;

EgpSecurity *egp_security_new(EgpWindowChanged changed, gpointer user_data);
void egp_security_free(EgpSecurity *security);
void egp_security_open_window(EgpSecurity *security, gint64 now_us);
void egp_security_close_window(EgpSecurity *security);
void egp_security_bluez_lost(EgpSecurity *security);
gboolean egp_security_window_open(EgpSecurity *security, gint64 now_us);
const char *egp_security_owner(EgpSecurity *security);

/* Pairing is allowed only while the physical-presence window is open and no
 * different owner exists. This callback never permanently authorizes a bond. */
gboolean egp_security_allow_pairing(EgpSecurity *security,
                                    const char *peer_path, gint64 now_us,
                                    EgpError *error);

/* claim_if_unowned is true for the first sensitive fragment. The first fully
 * eligible peer becomes the sole owner for the current window. Call again at
 * commit with claim_if_unowned=false to enforce the same peer and predicates. */
gboolean egp_security_authorize_sensitive(EgpSecurity *security,
                                          const char *peer_path,
                                          const EgpPeerSecurity *peer,
                                          gboolean claim_if_unowned,
                                          gint64 now_us, EgpError *error);
gboolean egp_security_can_read_runtime(EgpSecurity *security,
                                       const EgpPeerSecurity *peer,
                                       EgpError *error);
gboolean egp_security_can_read_result(EgpSecurity *security,
                                      const char *peer_path,
                                      const EgpPeerSecurity *peer,
                                      EgpError *error);

G_END_DECLS

#endif
