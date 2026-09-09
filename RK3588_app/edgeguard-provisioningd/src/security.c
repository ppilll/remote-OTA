#include "edgeguard_provisioning/security.h"

#include <string.h>

struct _EgpSecurity {
    GMutex lock;
    gboolean open;
    gint64 deadline_us;
    char owner[EGP_DBUS_PATH_CAP];
    guint expiry_source;
    EgpWindowChanged changed;
    gpointer user_data;
};

static gboolean deny(EgpError *error, EgpErrorCode code, const char *message)
{
    egp_error_set(error, code, TRUE, EGP_PERSISTENT_CHANGE_NONE, "%s", message);
    return FALSE;
}

static gboolean expiry_tick(gpointer user_data)
{
    EgpSecurity *security = user_data;
    gboolean notify = FALSE;
    g_mutex_lock(&security->lock);
    if (!security->open) {
        security->expiry_source = 0;
        g_mutex_unlock(&security->lock);
        return G_SOURCE_REMOVE;
    }
    if (g_get_monotonic_time() >= security->deadline_us) {
        security->open = FALSE;
        security->deadline_us = 0;
        memset(security->owner, 0, sizeof(security->owner));
        security->expiry_source = 0;
        notify = TRUE;
    }
    g_mutex_unlock(&security->lock);
    if (notify && security->changed)
        security->changed(security->user_data, FALSE);
    return notify ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}

EgpSecurity *egp_security_new(EgpWindowChanged changed, gpointer user_data)
{
    EgpSecurity *security = g_new0(EgpSecurity, 1);
    g_mutex_init(&security->lock);
    security->changed = changed;
    security->user_data = user_data;
    return security;
}

void egp_security_free(EgpSecurity *security)
{
    if (!security)
        return;
    if (security->expiry_source)
        g_source_remove(security->expiry_source);
    g_mutex_clear(&security->lock);
    memset(security, 0, sizeof(*security));
    g_free(security);
}

void egp_security_open_window(EgpSecurity *security, gint64 now_us)
{
    if (!security || now_us <= 0)
        return;
    g_mutex_lock(&security->lock);
    security->open = TRUE;
    security->deadline_us = now_us + EGP_WINDOW_DURATION_US;
    memset(security->owner, 0, sizeof(security->owner));
    if (!security->expiry_source)
        security->expiry_source = g_timeout_add(250, expiry_tick, security);
    g_mutex_unlock(&security->lock);
    if (security->changed)
        security->changed(security->user_data, TRUE);
}

void egp_security_close_window(EgpSecurity *security)
{
    if (!security)
        return;
    gboolean notify;
    g_mutex_lock(&security->lock);
    notify = security->open || security->owner[0];
    security->open = FALSE;
    security->deadline_us = 0;
    memset(security->owner, 0, sizeof(security->owner));
    guint source = security->expiry_source;
    security->expiry_source = 0;
    g_mutex_unlock(&security->lock);
    if (source)
        g_source_remove(source);
    if (notify && security->changed)
        security->changed(security->user_data, FALSE);
}

void egp_security_bluez_lost(EgpSecurity *security)
{
    if (!security)
        return;
    g_mutex_lock(&security->lock);
    memset(security->owner, 0, sizeof(security->owner));
    g_mutex_unlock(&security->lock);
}

gboolean egp_security_window_open(EgpSecurity *security, gint64 now_us)
{
    if (!security)
        return FALSE;
    g_mutex_lock(&security->lock);
    gboolean open = security->open && now_us > 0 && now_us < security->deadline_us;
    g_mutex_unlock(&security->lock);
    if (!open && now_us > 0)
        egp_security_close_window(security);
    return open;
}

const char *egp_security_owner(EgpSecurity *security)
{
    return security && security->owner[0] ? security->owner : NULL;
}

gboolean egp_security_allow_pairing(EgpSecurity *security,
                                    const char *peer_path, gint64 now_us,
                                    EgpError *error)
{
    if (!security || !peer_path || !g_variant_is_object_path(peer_path))
        return deny(error, EGP_ERROR_BLE_UNAUTHORIZED,
                    "Pairing peer identity is invalid");
    g_mutex_lock(&security->lock);
    gboolean open = security->open && now_us > 0 && now_us < security->deadline_us;
    gboolean owner_ok = !security->owner[0] || !strcmp(security->owner, peer_path);
    g_mutex_unlock(&security->lock);
    if (!open)
        return deny(error, EGP_ERROR_BLE_WINDOW_CLOSED,
                    "Physical-presence window is closed");
    if (!owner_ok)
        return deny(error, EGP_ERROR_BLE_UNAUTHORIZED,
                    "Another peer owns the current provisioning window");
    egp_error_clear(error);
    return TRUE;
}

static gboolean peer_eligible(const EgpPeerSecurity *peer, EgpError *error)
{
    if (!peer || !peer->encrypted_transport)
        return deny(error, EGP_ERROR_BLE_UNAUTHORIZED,
                    "Encrypted GATT transport is required");
    if (!peer->paired || !peer->bonded)
        return deny(error, EGP_ERROR_BLE_NOT_PAIRED,
                    "A paired and bonded peer is required");
    if (!peer->connected || !peer->stable_identity)
        return deny(error, EGP_ERROR_BLE_UNAUTHORIZED,
                    "Stable connected peer identity is required");
    return TRUE;
}

gboolean egp_security_authorize_sensitive(EgpSecurity *security,
                                          const char *peer_path,
                                          const EgpPeerSecurity *peer,
                                          gboolean claim_if_unowned,
                                          gint64 now_us, EgpError *error)
{
    if (!security || !peer_path || !g_variant_is_object_path(peer_path) ||
        !peer_eligible(peer, error))
        return FALSE;
    g_mutex_lock(&security->lock);
    gboolean open = security->open && now_us > 0 && now_us < security->deadline_us;
    gboolean authorized = FALSE;
    if (open) {
        if (!security->owner[0] && claim_if_unowned) {
            g_strlcpy(security->owner, peer_path, sizeof(security->owner));
            authorized = TRUE;
        } else {
            authorized = !strcmp(security->owner, peer_path);
        }
    }
    g_mutex_unlock(&security->lock);
    if (!open)
        return deny(error, EGP_ERROR_BLE_WINDOW_CLOSED,
                    "Physical-presence window is closed");
    if (!authorized)
        return deny(error, EGP_ERROR_BLE_UNAUTHORIZED,
                    "Peer does not own the current provisioning window");
    egp_error_clear(error);
    return TRUE;
}

gboolean egp_security_can_read_runtime(EgpSecurity *security,
                                       const EgpPeerSecurity *peer,
                                       EgpError *error)
{
    (void)security;
    if (!peer_eligible(peer, error))
        return FALSE;
    egp_error_clear(error);
    return TRUE;
}

gboolean egp_security_can_read_result(EgpSecurity *security,
                                      const char *peer_path,
                                      const EgpPeerSecurity *peer,
                                      EgpError *error)
{
    return egp_security_authorize_sensitive(security, peer_path, peer, FALSE,
                                            g_get_monotonic_time(), error);
}
