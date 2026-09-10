#ifndef EDGEGUARD_PROVISIONING_BLUEZ_H
#define EDGEGUARD_PROVISIONING_BLUEZ_H

#include "model.h"
#include "protocol.h"
#include "security.h"

G_BEGIN_DECLS

#define EGP_DBUS_ROOT "/com/edgeguard/provisioning"
#define EGP_DBUS_SERVICE EGP_DBUS_ROOT "/service0"
#define EGP_DBUS_DEVICE_INFO EGP_DBUS_SERVICE "/char0"
#define EGP_DBUS_RUNTIME_STATUS EGP_DBUS_SERVICE "/char1"
#define EGP_DBUS_PROVISIONING_REQUEST EGP_DBUS_SERVICE "/char2"
#define EGP_DBUS_OPERATION_RESULT EGP_DBUS_SERVICE "/char3"
#define EGP_DBUS_CONTROL_REQUEST EGP_DBUS_SERVICE "/char4"
#define EGP_DBUS_ADVERTISEMENT EGP_DBUS_ROOT "/advertisement0"
#define EGP_DBUS_AGENT EGP_DBUS_ROOT "/agent0"
#define EGP_ADVERTISEMENT_NAME "EdgeGuard Setup"

typedef enum {
    EGP_BLUEZ_DEVICE_INFO,
    EGP_BLUEZ_RUNTIME_STATUS,
    EGP_BLUEZ_PROVISIONING_REQUEST,
    EGP_BLUEZ_OPERATION_RESULT,
    EGP_BLUEZ_CONTROL_REQUEST
} EgpBluezCharacteristic;

typedef struct {
    gpointer user_data;
    gboolean (*read_value)(gpointer user_data, EgpBluezCharacteristic characteristic,
                           const char *peer_path, GBytes **value, EgpError *error);
    gboolean (*write_value)(gpointer user_data, EgpBluezCharacteristic characteristic,
                            const char *peer_path, const guint8 *value, gsize length,
                            EgpError *error);
    gboolean (*allow_pairing)(gpointer user_data, const char *peer_path,
                              EgpError *error);
    void (*peer_disconnected)(gpointer user_data, const char *peer_path);
    void (*bluez_lost)(gpointer user_data);
} EgpBluezHandlers;

typedef struct _EgpBluez EgpBluez;

EgpBluez *egp_bluez_new(GDBusConnection *connection,
                        const EgpBluezHandlers *handlers, EgpError *error);
void egp_bluez_free(EgpBluez *bluez);
void egp_bluez_set_window(EgpBluez *bluez, gboolean open);
gboolean egp_bluez_peer_security(EgpBluez *bluez, const char *peer_path,
                                 EgpPeerSecurity *peer, EgpError *error);
void egp_bluez_disconnect_non_owner(EgpBluez *bluez, const char *owner_path);
void egp_bluez_publish(EgpBluez *bluez, EgpBluezCharacteristic characteristic,
                       const char *json);

G_END_DECLS

#endif
