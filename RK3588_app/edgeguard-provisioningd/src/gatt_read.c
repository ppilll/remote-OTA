#include "edgeguard_provisioning/gatt_read.h"

#include <string.h>

static gboolean invalid_options(EgpError *error, const char *message)
{
    egp_error_set(error, EGP_ERROR_BLE_PAYLOAD_INVALID, TRUE,
                  EGP_PERSISTENT_CHANGE_NONE, "%s", message);
    return FALSE;
}

static gboolean uint16_option(GVariant *options, const char *name,
                              guint16 *value, gboolean *present)
{
    GVariant *option = g_variant_lookup_value(options, name, NULL);
    *present = option != NULL;
    if (!option)
        return TRUE;
    gboolean valid = g_variant_is_of_type(option, G_VARIANT_TYPE_UINT16);
    if (valid)
        *value = g_variant_get_uint16(option);
    g_variant_unref(option);
    return valid;
}

static gboolean boolean_option(GVariant *options, const char *name,
                               gboolean *value, gboolean *present)
{
    GVariant *option = g_variant_lookup_value(options, name, NULL);
    *present = option != NULL;
    if (!option)
        return TRUE;
    gboolean valid = g_variant_is_of_type(option, G_VARIANT_TYPE_BOOLEAN);
    if (valid)
        *value = g_variant_get_boolean(option);
    g_variant_unref(option);
    return valid;
}

static gboolean device_option(GVariant *options, gboolean required,
                              char output[EGP_DBUS_PATH_CAP])
{
    GVariant *device = g_variant_lookup_value(options, "device", NULL);
    if (!device)
        return !required;
    gboolean valid = g_variant_is_of_type(device, G_VARIANT_TYPE_OBJECT_PATH);
    const char *path = valid ? g_variant_get_string(device, NULL) : NULL;
    valid = valid && path && strlen(path) < EGP_DBUS_PATH_CAP;
    if (valid)
        g_strlcpy(output, path, EGP_DBUS_PATH_CAP);
    g_variant_unref(device);
    return valid;
}

gboolean egp_gatt_parse_read_options(GVariant *options, gboolean require_peer,
                                     EgpGattOptions *parsed, EgpError *error)
{
    if (!options || !parsed ||
        !g_variant_is_of_type(options, G_VARIANT_TYPE("a{sv}")))
        return invalid_options(error, "ReadValue options are invalid");
    memset(parsed, 0, sizeof(*parsed));
    gboolean offset_present = FALSE, mtu_present = FALSE, prepare_present = FALSE;
    gboolean prepare = FALSE;
    if (!uint16_option(options, "offset", &parsed->offset, &offset_present) ||
        !uint16_option(options, "mtu", &parsed->mtu, &mtu_present) ||
        !boolean_option(options, "prepare-authorize", &prepare,
                        &prepare_present) ||
        !device_option(options, require_peer, parsed->peer_path) ||
        prepare || (mtu_present && (parsed->mtu < EGP_GATT_DEFAULT_MTU ||
                                    parsed->mtu > EGP_GATT_MAX_MTU)))
        return invalid_options(error, "Unsupported ReadValue options or missing device identity");
    (void)offset_present;
    (void)prepare_present;
    egp_error_clear(error);
    return TRUE;
}

gboolean egp_gatt_parse_write_options(GVariant *options,
                                      EgpGattOptions *parsed,
                                      EgpError *error)
{
    if (!options || !parsed ||
        !g_variant_is_of_type(options, G_VARIANT_TYPE("a{sv}")))
        return invalid_options(error, "WriteValue options are invalid");
    memset(parsed, 0, sizeof(*parsed));
    gboolean offset_present = FALSE, mtu_present = FALSE, prepare_present = FALSE;
    gboolean prepare = FALSE;
    if (!uint16_option(options, "offset", &parsed->offset, &offset_present) ||
        !uint16_option(options, "mtu", &parsed->mtu, &mtu_present) ||
        !boolean_option(options, "prepare-authorize", &prepare,
                        &prepare_present) ||
        !device_option(options, TRUE, parsed->peer_path) || parsed->offset ||
        prepare || (mtu_present && (parsed->mtu < EGP_GATT_DEFAULT_MTU ||
                                    parsed->mtu > EGP_GATT_MAX_MTU)))
        return invalid_options(error, "Unsupported WriteValue options or missing device identity");
    (void)offset_present;
    (void)prepare_present;
    egp_error_clear(error);
    return TRUE;
}

void egp_gatt_read_snapshot_clear(EgpGattReadSnapshot *snapshot)
{
    if (snapshot)
        memset(snapshot, 0, sizeof(*snapshot));
}

gboolean egp_gatt_read_snapshot_expired(const EgpGattReadSnapshot *snapshot,
                                        gint64 now_us)
{
    return snapshot && snapshot->valid &&
           (now_us <= 0 || now_us >= snapshot->expires_at_us);
}

gboolean egp_gatt_read_snapshot_begin(EgpGattReadSnapshot *snapshot,
                                      const char *bluez_owner,
                                      const char *peer_path,
                                      guint characteristic,
                                      guint64 security_epoch,
                                      const guint8 *value, gsize length,
                                      gint64 now_us, EgpError *error)
{
    if (!snapshot || !bluez_owner || !bluez_owner[0] ||
        strlen(bluez_owner) >= EGP_BLUEZ_OWNER_CAP || !peer_path ||
        !g_variant_is_object_path(peer_path) || !security_epoch || now_us <= 0 ||
        length > EGP_GATT_VALUE_MAX_BYTES || (length && !value)) {
        egp_gatt_read_snapshot_clear(snapshot);
        egp_error_set(error,
                      length > EGP_GATT_VALUE_MAX_BYTES ?
                          EGP_ERROR_BLE_PAYLOAD_TOO_LARGE : EGP_ERROR_INTERNAL_ERROR,
                      TRUE, EGP_PERSISTENT_CHANGE_NONE,
                      "GATT read snapshot is invalid or exceeds 512 bytes");
        return FALSE;
    }
    egp_gatt_read_snapshot_clear(snapshot);
    snapshot->valid = TRUE;
    snapshot->characteristic = characteristic;
    snapshot->security_epoch = security_epoch;
    snapshot->expires_at_us = now_us + EGP_GATT_READ_SNAPSHOT_TTL_US;
    snapshot->length = length;
    g_strlcpy(snapshot->bluez_owner, bluez_owner,
              sizeof(snapshot->bluez_owner));
    g_strlcpy(snapshot->peer_path, peer_path, sizeof(snapshot->peer_path));
    if (length)
        memcpy(snapshot->value, value, length);
    egp_error_clear(error);
    return TRUE;
}

static gboolean snapshot_matches(const EgpGattReadSnapshot *snapshot,
                                 const char *bluez_owner,
                                 const char *peer_path,
                                 guint characteristic,
                                 guint64 security_epoch)
{
    return snapshot && snapshot->valid && bluez_owner && peer_path &&
           security_epoch && snapshot->characteristic == characteristic &&
           snapshot->security_epoch == security_epoch &&
           !strcmp(snapshot->bluez_owner, bluez_owner) &&
           !strcmp(snapshot->peer_path, peer_path);
}

gboolean egp_gatt_read_snapshot_slice(EgpGattReadSnapshot *snapshot,
                                      const char *bluez_owner,
                                      const char *peer_path,
                                      guint characteristic,
                                      guint64 security_epoch,
                                      guint16 offset, guint16 mtu,
                                      gint64 now_us, GBytes **value,
                                      EgpError *error)
{
    if (value)
        *value = NULL;
    if (mtu && (mtu < EGP_GATT_DEFAULT_MTU || mtu > EGP_GATT_MAX_MTU)) {
        egp_error_set(error, EGP_ERROR_BLE_PAYLOAD_INVALID, TRUE,
                      EGP_PERSISTENT_CHANGE_NONE,
                      "ReadValue MTU is invalid");
        return FALSE;
    }
    if (!value || egp_gatt_read_snapshot_expired(snapshot, now_us) ||
        !snapshot_matches(snapshot, bluez_owner, peer_path, characteristic,
                          security_epoch)) {
        egp_gatt_read_snapshot_clear(snapshot);
        egp_error_set(error, EGP_ERROR_BLE_INVALID_OFFSET, TRUE,
                      EGP_PERSISTENT_CHANGE_NONE,
                      "Read snapshot is unavailable or stale; restart at offset zero");
        return FALSE;
    }
    if ((gsize)offset > snapshot->length) {
        egp_error_set(error, EGP_ERROR_BLE_INVALID_OFFSET, TRUE,
                      EGP_PERSISTENT_CHANGE_NONE,
                      "ReadValue offset exceeds the value length");
        return FALSE;
    }
    gsize response_max = (gsize)(mtu ? mtu : EGP_GATT_DEFAULT_MTU) - 1u;
    gsize remaining = snapshot->length - offset;
    gsize count = MIN(remaining, response_max);
    *value = g_bytes_new(count ? snapshot->value + offset : NULL, count);
    if (count < response_max)
        egp_gatt_read_snapshot_clear(snapshot);
    egp_error_clear(error);
    return TRUE;
}
