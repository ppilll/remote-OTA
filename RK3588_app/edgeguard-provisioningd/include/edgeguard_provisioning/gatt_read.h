#ifndef EDGEGUARD_PROVISIONING_GATT_READ_H
#define EDGEGUARD_PROVISIONING_GATT_READ_H

#include "model.h"

G_BEGIN_DECLS

#define EGP_GATT_DEFAULT_MTU 23u
#define EGP_GATT_MAX_MTU 517u
#define EGP_GATT_READ_SNAPSHOT_TTL_US (30 * G_USEC_PER_SEC)
#define EGP_BLUEZ_OWNER_CAP 64u

typedef struct {
    char peer_path[EGP_DBUS_PATH_CAP];
    guint16 offset;
    guint16 mtu;
} EgpGattOptions;

/* Read and write parsing intentionally remain separate: reads accept a valid
 * ATT offset, while writes use only EGP1 fragmentation and reject offset and
 * prepare-write semantics. */
gboolean egp_gatt_parse_read_options(GVariant *options, gboolean require_peer,
                                     EgpGattOptions *parsed, EgpError *error);
gboolean egp_gatt_parse_write_options(GVariant *options,
                                      EgpGattOptions *parsed,
                                      EgpError *error);

typedef struct {
    gboolean valid;
    guint characteristic;
    guint64 security_epoch;
    gint64 expires_at_us;
    gsize length;
    char bluez_owner[EGP_BLUEZ_OWNER_CAP];
    char peer_path[EGP_DBUS_PATH_CAP];
    guint8 value[EGP_GATT_VALUE_MAX_BYTES];
} EgpGattReadSnapshot;

void egp_gatt_read_snapshot_clear(EgpGattReadSnapshot *snapshot);
gboolean egp_gatt_read_snapshot_begin(EgpGattReadSnapshot *snapshot,
                                      const char *bluez_owner,
                                      const char *peer_path,
                                      guint characteristic,
                                      guint64 security_epoch,
                                      const guint8 *value, gsize length,
                                      gint64 now_us, EgpError *error);
gboolean egp_gatt_read_snapshot_slice(EgpGattReadSnapshot *snapshot,
                                      const char *bluez_owner,
                                      const char *peer_path,
                                      guint characteristic,
                                      guint64 security_epoch,
                                      guint16 offset, guint16 mtu,
                                      gint64 now_us, GBytes **value,
                                      EgpError *error);
gboolean egp_gatt_read_snapshot_expired(const EgpGattReadSnapshot *snapshot,
                                        gint64 now_us);

G_END_DECLS

#endif
