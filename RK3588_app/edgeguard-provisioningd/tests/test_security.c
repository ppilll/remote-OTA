#include "edgeguard_provisioning/security.h"

#include <string.h>

typedef struct { guint opened; guint closed; } Changes;

static void changed(gpointer user_data, gboolean open)
{
    Changes *changes = user_data;
    if (open) ++changes->opened;
    else ++changes->closed;
}

static EgpPeerSecurity eligible_peer(void)
{
    EgpPeerSecurity peer = {
        .connected = TRUE,
        .paired = TRUE,
        .bonded = TRUE,
        .stable_identity = TRUE,
        .encrypted_transport = TRUE
    };
    return peer;
}

static void closed_by_default(void)
{
    Changes changes = {0};
    EgpSecurity *security = egp_security_new(changed, &changes);
    EgpPeerSecurity peer = eligible_peer();
    EgpError error = {0};
    g_assert_false(egp_security_authorize_sensitive(
        security, "/org/bluez/hci0/dev_AA", &peer, TRUE, 1000, &error));
    g_assert_cmpint(error.code, ==, EGP_ERROR_BLE_WINDOW_CLOSED);
    egp_security_free(security);
}

static void single_owner(void)
{
    Changes changes = {0};
    EgpSecurity *security = egp_security_new(changed, &changes);
    EgpPeerSecurity peer = eligible_peer();
    EgpError error = {0};
    gint64 now = g_get_monotonic_time();
    egp_security_open_window(security, now);
    g_assert_true(egp_security_authorize_sensitive(
        security, "/org/bluez/hci0/dev_AA", &peer, TRUE, now + 1, &error));
    g_assert_cmpstr(egp_security_owner(security), ==,
                    "/org/bluez/hci0/dev_AA");
    g_assert_false(egp_security_authorize_sensitive(
        security, "/org/bluez/hci0/dev_BB", &peer, TRUE, now + 2, &error));
    g_assert_cmpint(error.code, ==, EGP_ERROR_BLE_UNAUTHORIZED);
    egp_security_close_window(security);
    g_assert_null(egp_security_owner(security));
    egp_security_free(security);
}

static void fail_closed_properties(void)
{
    EgpSecurity *security = egp_security_new(NULL, NULL);
    EgpPeerSecurity peer = eligible_peer();
    EgpError error = {0};
    gint64 now = g_get_monotonic_time();
    egp_security_open_window(security, now);
    peer.bonded = FALSE;
    g_assert_false(egp_security_authorize_sensitive(
        security, "/org/bluez/hci0/dev_AA", &peer, TRUE, now + 1, &error));
    g_assert_cmpint(error.code, ==, EGP_ERROR_BLE_NOT_PAIRED);
    peer = eligible_peer();
    peer.encrypted_transport = FALSE;
    g_assert_false(egp_security_authorize_sensitive(
        security, "/org/bluez/hci0/dev_AA", &peer, TRUE, now + 2, &error));
    g_assert_cmpint(error.code, ==, EGP_ERROR_BLE_UNAUTHORIZED);
    egp_security_free(security);
}

static void bluez_loss_revokes_owner(void)
{
    EgpSecurity *security = egp_security_new(NULL, NULL);
    EgpPeerSecurity peer = eligible_peer();
    EgpError error = {0};
    gint64 now = g_get_monotonic_time();
    egp_security_open_window(security, now);
    g_assert_true(egp_security_authorize_sensitive(
        security, "/org/bluez/hci0/dev_AA", &peer, TRUE, now + 1, &error));
    egp_security_bluez_lost(security);
    g_assert_null(egp_security_owner(security));
    g_assert_true(egp_security_window_open(security, now + 2));
    egp_security_free(security);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/security/closed-default", closed_by_default);
    g_test_add_func("/security/single-owner", single_owner);
    g_test_add_func("/security/fail-closed-properties", fail_closed_properties);
    g_test_add_func("/security/bluez-loss", bluez_loss_revokes_owner);
    return g_test_run();
}
