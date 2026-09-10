#include "edgeguard_provisioning/bluez.h"

#include <string.h>

#define BLUEZ_BUS "org.bluez"
#define DBUS_PROPERTIES "org.freedesktop.DBus.Properties"
#define BLUEZ_ADAPTER "org.bluez.Adapter1"
#define BLUEZ_DEVICE "org.bluez.Device1"
#define BLUEZ_GATT_MANAGER "org.bluez.GattManager1"
#define BLUEZ_ADV_MANAGER "org.bluez.LEAdvertisingManager1"
#define BLUEZ_AGENT_MANAGER "org.bluez.AgentManager1"

typedef enum {
    EXPORT_MANAGER,
    EXPORT_SERVICE,
    EXPORT_CHARACTERISTIC,
    EXPORT_ADVERTISEMENT,
    EXPORT_AGENT
} ExportType;

typedef struct {
    struct _EgpBluez *bluez;
    ExportType type;
    EgpBluezCharacteristic characteristic;
    const char *path;
    guint registration_id;
} Export;

struct _EgpBluez {
    GDBusConnection *connection;
    EgpBluezHandlers handlers;
    GDBusNodeInfo *manager_info;
    GDBusNodeInfo *service_info;
    GDBusNodeInfo *characteristic_info;
    GDBusNodeInfo *advertisement_info;
    GDBusNodeInfo *agent_info;
    Export exports[9];
    guint export_count;
    guint name_watch;
    guint properties_subscription;
    guint retry_source;
    GMutex peer_lock;
    GHashTable *peer_epochs;
    guint64 next_peer_epoch;
    gboolean bluez_present;
    gboolean window_open;
    gboolean advertisement_registered;
    gboolean application_registered;
    gboolean agent_registered;
    char unique_owner[64];
    char adapter_path[EGP_DBUS_PATH_CAP];
};

static const char manager_xml[] =
    "<node><interface name='org.freedesktop.DBus.ObjectManager'>"
    "<method name='GetManagedObjects'><arg name='objects' type='a{oa{sa{sv}}}' direction='out'/></method>"
    "</interface></node>";

static const char service_xml[] =
    "<node><interface name='org.bluez.GattService1'>"
    "<property name='UUID' type='s' access='read'/>"
    "<property name='Primary' type='b' access='read'/>"
    "<property name='Includes' type='ao' access='read'/>"
    "</interface></node>";

static const char characteristic_xml[] =
    "<node><interface name='org.bluez.GattCharacteristic1'>"
    "<method name='ReadValue'><arg name='options' type='a{sv}' direction='in'/><arg name='value' type='ay' direction='out'/></method>"
    "<method name='WriteValue'><arg name='value' type='ay' direction='in'/><arg name='options' type='a{sv}' direction='in'/></method>"
    "<property name='UUID' type='s' access='read'/><property name='Service' type='o' access='read'/>"
    "<property name='Flags' type='as' access='read'/></interface></node>";

static const char advertisement_xml[] =
    "<node><interface name='org.bluez.LEAdvertisement1'>"
    "<method name='Release'/><property name='Type' type='s' access='read'/>"
    "<property name='ServiceUUIDs' type='as' access='read'/><property name='LocalName' type='s' access='read'/>"
    "</interface></node>";

static const char agent_xml[] =
    "<node><interface name='org.bluez.Agent1'>"
    "<method name='Release'/><method name='RequestPinCode'><arg type='o' direction='in'/><arg type='s' direction='out'/></method>"
    "<method name='DisplayPinCode'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
    "<method name='RequestPasskey'><arg type='o' direction='in'/><arg type='u' direction='out'/></method>"
    "<method name='DisplayPasskey'><arg type='o' direction='in'/><arg type='u' direction='in'/><arg type='q' direction='in'/></method>"
    "<method name='RequestConfirmation'><arg type='o' direction='in'/><arg type='u' direction='in'/></method>"
    "<method name='RequestAuthorization'><arg type='o' direction='in'/></method>"
    "<method name='AuthorizeService'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
    "<method name='Cancel'/></interface></node>";

static const char *characteristic_path(EgpBluezCharacteristic characteristic)
{
    static const char *const paths[] = {
        EGP_DBUS_DEVICE_INFO, EGP_DBUS_RUNTIME_STATUS,
        EGP_DBUS_PROVISIONING_REQUEST, EGP_DBUS_OPERATION_RESULT,
        EGP_DBUS_CONTROL_REQUEST
    };
    return paths[characteristic];
}

static const char *characteristic_uuid(EgpBluezCharacteristic characteristic)
{
    static const char *const uuids[] = {
        EGP_DEVICE_INFO_UUID, EGP_RUNTIME_STATUS_UUID,
        EGP_PROVISIONING_REQUEST_UUID, EGP_OPERATION_RESULT_UUID,
        EGP_CONTROL_REQUEST_UUID
    };
    return uuids[characteristic];
}

static GVariant *string_array(const char *const *strings)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
    for (guint i = 0; strings[i]; ++i)
        g_variant_builder_add(&builder, "s", strings[i]);
    return g_variant_builder_end(&builder);
}

static GVariant *characteristic_flags(EgpBluezCharacteristic characteristic)
{
    static const char *const device_info[] = { "read", NULL };
    static const char *const status[] = { "encrypt-read", NULL };
    static const char *const result[] = { "encrypt-read", NULL };
    static const char *const request[] = { "encrypt-write", "authorize", NULL };
    const char *const *flags = request;
    if (characteristic == EGP_BLUEZ_DEVICE_INFO)
        flags = device_info;
    else if (characteristic == EGP_BLUEZ_RUNTIME_STATUS)
        flags = status;
    else if (characteristic == EGP_BLUEZ_OPERATION_RESULT)
        flags = result;
    return string_array(flags);
}

static GVariant *bytes_variant(GBytes *bytes)
{
    gsize length = 0;
    const guint8 *data = bytes ? g_bytes_get_data(bytes, &length) : NULL;
    return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data, length, 1);
}

static GVariant *properties_for(Export *export)
{
    GVariantBuilder properties;
    g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
    if (export->type == EXPORT_SERVICE) {
        GVariantBuilder includes;
        g_variant_builder_add(&properties, "{sv}", "UUID",
                              g_variant_new_string(EGP_SERVICE_UUID));
        g_variant_builder_add(&properties, "{sv}", "Primary",
                              g_variant_new_boolean(TRUE));
        g_variant_builder_init(&includes, G_VARIANT_TYPE("ao"));
        g_variant_builder_add(&properties, "{sv}", "Includes",
                              g_variant_builder_end(&includes));
    } else if (export->type == EXPORT_CHARACTERISTIC) {
        g_variant_builder_add(&properties, "{sv}", "UUID",
                              g_variant_new_string(characteristic_uuid(export->characteristic)));
        g_variant_builder_add(&properties, "{sv}", "Service",
                              g_variant_new_object_path(EGP_DBUS_SERVICE));
        g_variant_builder_add(&properties, "{sv}", "Flags",
                              characteristic_flags(export->characteristic));
    } else if (export->type == EXPORT_ADVERTISEMENT) {
        const char *const service_uuids[] = { EGP_SERVICE_UUID, NULL };
        g_variant_builder_add(&properties, "{sv}", "Type",
                              g_variant_new_string("peripheral"));
        g_variant_builder_add(&properties, "{sv}", "ServiceUUIDs",
                              string_array(service_uuids));
        g_variant_builder_add(&properties, "{sv}", "LocalName",
                              g_variant_new_string(EGP_ADVERTISEMENT_NAME));
    }
    return g_variant_builder_end(&properties);
}

static const char *interface_for(Export *export)
{
    switch (export->type) {
    case EXPORT_SERVICE: return "org.bluez.GattService1";
    case EXPORT_CHARACTERISTIC: return "org.bluez.GattCharacteristic1";
    case EXPORT_ADVERTISEMENT: return "org.bluez.LEAdvertisement1";
    case EXPORT_AGENT: return "org.bluez.Agent1";
    case EXPORT_MANAGER: return "org.freedesktop.DBus.ObjectManager";
    default: return "";
    }
}

static void return_error(GDBusMethodInvocation *invocation, const EgpError *error)
{
    const char *name = "org.bluez.Error.Failed";
    if (error->code == EGP_ERROR_BLE_UNAUTHORIZED ||
        error->code == EGP_ERROR_BLE_NOT_PAIRED ||
        error->code == EGP_ERROR_BLE_WINDOW_CLOSED)
        name = "org.bluez.Error.NotAuthorized";
    else if (error->code == EGP_ERROR_BLE_PAYLOAD_TOO_LARGE)
        name = "org.bluez.Error.InvalidValueLength";
    else if (error->code == EGP_ERROR_BLE_PAYLOAD_INVALID ||
             error->code == EGP_ERROR_BLE_FRAGMENT_CONFLICT ||
             error->code == EGP_ERROR_BLE_REPLAY_REJECTED)
        name = "org.bluez.Error.InvalidArguments";
    else if (error->code == EGP_ERROR_PROVISIONING_BUSY ||
             error->code == EGP_ERROR_AGENT_BUSY)
        name = "org.bluez.Error.InProgress";
    g_dbus_method_invocation_return_dbus_error(invocation, name,
                                                error->message[0] ? error->message :
                                                "Request rejected");
}

static gboolean get_peer_option(GVariant *options, gboolean required,
                                char **peer, guint16 *mtu_out, EgpError *error)
{
    *peer = NULL;
    guint16 offset = 0;
    guint16 mtu = 0;
    gboolean prepare = FALSE;
    g_variant_lookup(options, "offset", "q", &offset);
    gboolean has_mtu = g_variant_lookup(options, "mtu", "q", &mtu);
    g_variant_lookup(options, "prepare-authorize", "b", &prepare);
    GVariant *device = g_variant_lookup_value(options, "device",
                                              G_VARIANT_TYPE_OBJECT_PATH);
    if (device) {
        *peer = g_strdup(g_variant_get_string(device, NULL));
        g_variant_unref(device);
    }
    if (mtu_out)
        *mtu_out = has_mtu ? mtu : 0;
    if (offset || prepare || (has_mtu && (mtu < 23 || mtu > 517)) ||
        (required && !*peer)) {
        egp_error_set(error, EGP_ERROR_BLE_PAYLOAD_INVALID, TRUE,
                      EGP_PERSISTENT_CHANGE_NONE,
                      "Unsupported GATT options or missing device identity");
        return FALSE;
    }
    return TRUE;
}

static void manager_call(Export *export, GDBusMethodInvocation *invocation)
{
    EgpBluez *bluez = export->bluez;
    GVariantBuilder objects;
    g_variant_builder_init(&objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
    for (guint i = 0; i < bluez->export_count; ++i) {
        Export *item = &bluez->exports[i];
        if (item->type != EXPORT_SERVICE && item->type != EXPORT_CHARACTERISTIC)
            continue;
        GVariantBuilder interfaces;
        g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
        g_variant_builder_add(&interfaces, "{s@a{sv}}", interface_for(item),
                              properties_for(item));
        g_variant_builder_add(&objects, "{o@a{sa{sv}}}", item->path,
                              g_variant_builder_end(&interfaces));
    }
    g_dbus_method_invocation_return_value(invocation,
        g_variant_new("(@a{oa{sa{sv}}})", g_variant_builder_end(&objects)));
}

static void characteristic_call(Export *export, const char *method,
                                GVariant *parameters,
                                GDBusMethodInvocation *invocation)
{
    EgpBluez *bluez = export->bluez;
    EgpError error = {0};
    if (!strcmp(method, "ReadValue")) {
        GVariant *options = NULL;
        g_variant_get(parameters, "(@a{sv})", &options);
        char *peer = NULL;
        gboolean ok = get_peer_option(options,
                                     export->characteristic != EGP_BLUEZ_DEVICE_INFO,
                                     &peer, NULL, &error);
        GBytes *value = NULL;
        if (ok && bluez->handlers.read_value)
            ok = bluez->handlers.read_value(bluez->handlers.user_data,
                                            export->characteristic, peer,
                                            &value, &error);
        g_variant_unref(options);
        if (!ok) {
            g_free(peer);
            if (value)
                g_bytes_unref(value);
            return_error(invocation, &error);
            return;
        }
        GVariant *bytes = bytes_variant(value);
        if (value)
            g_bytes_unref(value);
        g_free(peer);
        g_dbus_method_invocation_return_value(invocation,
                                               g_variant_new("(@ay)", bytes));
        return;
    }
    if (!strcmp(method, "WriteValue")) {
        GVariant *bytes = NULL, *options = NULL;
        g_variant_get(parameters, "(@ay@a{sv})", &bytes, &options);
        char *peer = NULL;
        guint16 mtu = 0;
        gboolean ok = get_peer_option(options, TRUE, &peer, &mtu, &error);
        gsize length = 0;
        const guint8 *value = g_variant_get_fixed_array(bytes, &length, 1);
        if (ok && mtu && length > (gsize)(mtu - 3u)) {
            egp_error_set(&error, EGP_ERROR_BLE_PAYLOAD_TOO_LARGE, TRUE,
                          EGP_PERSISTENT_CHANGE_NONE,
                          "GATT write exceeds the negotiated MTU");
            ok = FALSE;
        }
        if (ok && bluez->handlers.write_value)
            ok = bluez->handlers.write_value(bluez->handlers.user_data,
                                             export->characteristic, peer,
                                             value, length, &error);
        g_variant_unref(bytes);
        g_variant_unref(options);
        if (!ok) {
            g_free(peer);
            return_error(invocation, &error);
            return;
        }
        g_free(peer);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    g_dbus_method_invocation_return_dbus_error(invocation,
                                               "org.bluez.Error.NotSupported",
                                               "Method is not supported");
}

static gboolean agent_peer(Export *export, const char *peer,
                           GDBusMethodInvocation *invocation)
{
    EgpError error = {0};
    if (export->bluez->handlers.allow_pairing &&
        export->bluez->handlers.allow_pairing(export->bluez->handlers.user_data,
                                              peer, &error))
        return TRUE;
    return_error(invocation, &error);
    return FALSE;
}

static void agent_call(Export *export, const char *method, GVariant *parameters,
                       GDBusMethodInvocation *invocation)
{
    if (!strcmp(method, "Release") || !strcmp(method, "Cancel")) {
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (!strcmp(method, "RequestPinCode") || !strcmp(method, "RequestPasskey")) {
        g_dbus_method_invocation_return_dbus_error(invocation,
                                                   "org.bluez.Error.Rejected",
                                                   "NoInputNoOutput capability");
        return;
    }
    if (!strcmp(method, "DisplayPinCode") || !strcmp(method, "DisplayPasskey")) {
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    const char *peer = NULL;
    if (!strcmp(method, "RequestConfirmation")) {
        guint32 passkey;
        g_variant_get(parameters, "(&ou)", &peer, &passkey);
        (void)passkey;
    } else if (!strcmp(method, "RequestAuthorization")) {
        g_variant_get(parameters, "(&o)", &peer);
    } else if (!strcmp(method, "AuthorizeService")) {
        const char *uuid = NULL;
        g_variant_get(parameters, "(&o&s)", &peer, &uuid);
        if (g_ascii_strcasecmp(uuid, EGP_SERVICE_UUID)) {
            g_dbus_method_invocation_return_dbus_error(invocation,
                                                       "org.bluez.Error.Rejected",
                                                       "Service is not authorized");
            return;
        }
    }
    if (peer && agent_peer(export, peer, invocation))
        g_dbus_method_invocation_return_value(invocation, NULL);
    else if (!peer)
        g_dbus_method_invocation_return_dbus_error(invocation,
                                                   "org.bluez.Error.Rejected",
                                                   "Agent method is unsupported");
}

static void method_call(GDBusConnection *connection, const char *sender,
                        const char *object_path, const char *interface_name,
                        const char *method_name, GVariant *parameters,
                        GDBusMethodInvocation *invocation, gpointer user_data)
{
    (void)connection; (void)object_path; (void)interface_name;
    Export *export = user_data;
    if ((export->type == EXPORT_CHARACTERISTIC || export->type == EXPORT_AGENT ||
         export->type == EXPORT_ADVERTISEMENT) &&
        (!export->bluez->unique_owner[0] || !sender ||
         strcmp(sender, export->bluez->unique_owner))) {
        g_dbus_method_invocation_return_dbus_error(invocation,
                                                   "org.bluez.Error.NotAuthorized",
                                                   "Only the active BlueZ owner may invoke this object");
        return;
    }
    if (export->type == EXPORT_MANAGER)
        manager_call(export, invocation);
    else if (export->type == EXPORT_CHARACTERISTIC)
        characteristic_call(export, method_name, parameters, invocation);
    else if (export->type == EXPORT_AGENT)
        agent_call(export, method_name, parameters, invocation);
    else if (export->type == EXPORT_ADVERTISEMENT && !strcmp(method_name, "Release")) {
        export->bluez->advertisement_registered = FALSE;
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else {
        g_dbus_method_invocation_return_dbus_error(invocation,
                                                   "org.bluez.Error.NotSupported",
                                                   "Method is not supported");
    }
}

static GVariant *get_property(GDBusConnection *connection, const char *sender,
                              const char *object_path, const char *interface_name,
                              const char *property_name, GError **error,
                              gpointer user_data)
{
    (void)connection; (void)sender; (void)object_path; (void)interface_name;
    Export *export = user_data;
    if (export->type == EXPORT_SERVICE) {
        if (!strcmp(property_name, "UUID")) return g_variant_new_string(EGP_SERVICE_UUID);
        if (!strcmp(property_name, "Primary")) return g_variant_new_boolean(TRUE);
        if (!strcmp(property_name, "Includes")) {
            GVariantBuilder builder;
            g_variant_builder_init(&builder, G_VARIANT_TYPE("ao"));
            return g_variant_builder_end(&builder);
        }
    } else if (export->type == EXPORT_CHARACTERISTIC) {
        if (!strcmp(property_name, "UUID"))
            return g_variant_new_string(characteristic_uuid(export->characteristic));
        if (!strcmp(property_name, "Service"))
            return g_variant_new_object_path(EGP_DBUS_SERVICE);
        if (!strcmp(property_name, "Flags"))
            return characteristic_flags(export->characteristic);
    } else if (export->type == EXPORT_ADVERTISEMENT) {
        if (!strcmp(property_name, "Type")) return g_variant_new_string("peripheral");
        if (!strcmp(property_name, "LocalName")) return g_variant_new_string(EGP_ADVERTISEMENT_NAME);
        if (!strcmp(property_name, "ServiceUUIDs")) {
            const char *const uuids[] = { EGP_SERVICE_UUID, NULL };
            return string_array(uuids);
        }
    }
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "Unknown property %s", property_name);
    return NULL;
}

static const GDBusInterfaceVTable interface_vtable = {
    .method_call = method_call,
    .get_property = get_property
};

static gboolean add_export(EgpBluez *bluez, ExportType type,
                           EgpBluezCharacteristic characteristic,
                           const char *path, GDBusNodeInfo *info,
                           EgpError *error)
{
    Export *export = &bluez->exports[bluez->export_count++];
    export->bluez = bluez;
    export->type = type;
    export->characteristic = characteristic;
    export->path = path;
    GError *registration_error = NULL;
    export->registration_id = g_dbus_connection_register_object(
        bluez->connection, path, info->interfaces[0], &interface_vtable,
        export, NULL, &registration_error);
    if (!export->registration_id) {
        egp_error_set(error, EGP_ERROR_GATT_REGISTRATION_FAILED, TRUE,
                      EGP_PERSISTENT_CHANGE_NONE,
                      "Local BlueZ object export failed: %s",
                      registration_error ? registration_error->message : "unknown error");
        g_clear_error(&registration_error);
        return FALSE;
    }
    return TRUE;
}

static void call_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source),
                                                     result, &error);
    if (reply)
        g_variant_unref(reply);
    if (error) {
        g_warning("BlueZ registration operation failed: %s", error->message);
        g_clear_error(&error);
    }
}

static void bluez_call(EgpBluez *bluez, const char *path, const char *interface,
                       const char *method, GVariant *parameters)
{
    g_dbus_connection_call(bluez->connection, BLUEZ_BUS, path, interface, method,
                           parameters, NULL, G_DBUS_CALL_FLAGS_NONE, 5000,
                           NULL, call_finished, NULL);
}

static gboolean bluez_call_sync(EgpBluez *bluez, const char *path,
                                const char *interface, const char *method,
                                GVariant *parameters)
{
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        bluez->connection, BLUEZ_BUS, path, interface, method, parameters, NULL,
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    gboolean already = FALSE;
    if (!reply && error && g_dbus_error_is_remote_error(error)) {
        char *name = g_dbus_error_get_remote_error(error);
        already = name && g_str_has_suffix(name, ".AlreadyExists");
        g_free(name);
    }
    if (reply)
        g_variant_unref(reply);
    if (!reply && !already)
        g_warning("BlueZ registration operation failed: %s",
                  error ? error->message : "unknown error");
    g_clear_error(&error);
    return reply != NULL || already;
}

static void set_pairable(EgpBluez *bluez, gboolean pairable)
{
    if (!bluez->adapter_path[0])
        return;
    bluez_call(bluez, bluez->adapter_path, DBUS_PROPERTIES, "Set",
               g_variant_new("(ssv)", BLUEZ_ADAPTER, "Pairable",
                             g_variant_new_boolean(pairable)));
}

static gboolean discover_adapter(EgpBluez *bluez)
{
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        bluez->connection, BLUEZ_BUS, "/", "org.freedesktop.DBus.ObjectManager",
        "GetManagedObjects", NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    if (!reply) {
        g_warning("BlueZ adapter discovery failed: %s",
                  error ? error->message : "unknown error");
        g_clear_error(&error);
        return FALSE;
    }
    GVariantIter *objects = NULL;
    g_variant_get(reply, "(a{oa{sa{sv}}})", &objects);
    const char *path;
    GVariant *interfaces;
    gboolean found = FALSE;
    while (g_variant_iter_next(objects, "{&o@a{sa{sv}}}", &path, &interfaces)) {
        GVariant *gatt = g_variant_lookup_value(interfaces, BLUEZ_GATT_MANAGER,
                                                G_VARIANT_TYPE("a{sv}"));
        GVariant *advertising = g_variant_lookup_value(interfaces, BLUEZ_ADV_MANAGER,
                                                       G_VARIANT_TYPE("a{sv}"));
        if (gatt && advertising && strlen(path) < sizeof(bluez->adapter_path)) {
            g_strlcpy(bluez->adapter_path, path, sizeof(bluez->adapter_path));
            found = TRUE;
        }
        if (gatt) g_variant_unref(gatt);
        if (advertising) g_variant_unref(advertising);
        g_variant_unref(interfaces);
        if (found)
            break;
    }
    g_variant_iter_free(objects);
    g_variant_unref(reply);
    return found;
}

static void register_bluez(EgpBluez *bluez);

static guint64 next_epoch_locked(EgpBluez *bluez)
{
    bluez->next_peer_epoch++;
    if (!bluez->next_peer_epoch)
        bluez->next_peer_epoch++;
    return bluez->next_peer_epoch;
}

static guint64 peer_epoch_locked(EgpBluez *bluez, const char *peer_path,
                                 gboolean create)
{
    guint64 *stored = g_hash_table_lookup(bluez->peer_epochs, peer_path);
    if (!stored && create) {
        stored = g_new(guint64, 1);
        *stored = next_epoch_locked(bluez);
        g_hash_table_insert(bluez->peer_epochs, g_strdup(peer_path), stored);
    }
    return stored ? *stored : 0;
}

static void bump_peer_epoch(EgpBluez *bluez, const char *peer_path)
{
    g_mutex_lock(&bluez->peer_lock);
    guint64 *stored = g_hash_table_lookup(bluez->peer_epochs, peer_path);
    if (!stored) {
        stored = g_new(guint64, 1);
        g_hash_table_insert(bluez->peer_epochs, g_strdup(peer_path), stored);
    }
    *stored = next_epoch_locked(bluez);
    g_mutex_unlock(&bluez->peer_lock);
}

static gboolean retry_registration(gpointer user_data)
{
    EgpBluez *bluez = user_data;
    if (!bluez->bluez_present) {
        bluez->retry_source = 0;
        return G_SOURCE_REMOVE;
    }
    register_bluez(bluez);
    gboolean complete = bluez->application_registered && bluez->agent_registered &&
                        (!bluez->window_open || bluez->advertisement_registered);
    if (complete)
        bluez->retry_source = 0;
    return complete ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}

static void ensure_retry(EgpBluez *bluez)
{
    if (!bluez->retry_source)
        bluez->retry_source = g_timeout_add_seconds(5, retry_registration, bluez);
}

static void register_bluez(EgpBluez *bluez)
{
    if (!bluez->adapter_path[0] && !discover_adapter(bluez)) {
        ensure_retry(bluez);
        return;
    }
    if (!bluez->application_registered)
        bluez->application_registered = bluez_call_sync(
            bluez, bluez->adapter_path, BLUEZ_GATT_MANAGER,
            "RegisterApplication", g_variant_new("(oa{sv})", EGP_DBUS_ROOT, NULL));
    if (!bluez->agent_registered) {
        bluez->agent_registered = bluez_call_sync(
            bluez, "/org/bluez", BLUEZ_AGENT_MANAGER, "RegisterAgent",
            g_variant_new("(os)", EGP_DBUS_AGENT, "NoInputNoOutput"));
        if (bluez->agent_registered)
            bluez->agent_registered = bluez_call_sync(
                bluez, "/org/bluez", BLUEZ_AGENT_MANAGER,
                "RequestDefaultAgent", g_variant_new("(o)", EGP_DBUS_AGENT));
    }
    set_pairable(bluez, bluez->window_open);
    if (bluez->window_open && !bluez->advertisement_registered)
        bluez->advertisement_registered = bluez_call_sync(
            bluez, bluez->adapter_path, BLUEZ_ADV_MANAGER,
            "RegisterAdvertisement",
            g_variant_new("(oa{sv})", EGP_DBUS_ADVERTISEMENT, NULL));
    if (!bluez->application_registered || !bluez->agent_registered ||
        (bluez->window_open && !bluez->advertisement_registered))
        ensure_retry(bluez);
}

static void name_appeared(GDBusConnection *connection, const char *name,
                          const char *owner, gpointer user_data)
{
    (void)connection; (void)name;
    EgpBluez *bluez = user_data;
    g_mutex_lock(&bluez->peer_lock);
    g_strlcpy(bluez->unique_owner, owner, sizeof(bluez->unique_owner));
    g_hash_table_remove_all(bluez->peer_epochs);
    next_epoch_locked(bluez);
    g_mutex_unlock(&bluez->peer_lock);
    bluez->bluez_present = TRUE;
    register_bluez(bluez);
}

static void name_vanished(GDBusConnection *connection, const char *name,
                          gpointer user_data)
{
    (void)connection; (void)name;
    EgpBluez *bluez = user_data;
    g_mutex_lock(&bluez->peer_lock);
    memset(bluez->unique_owner, 0, sizeof(bluez->unique_owner));
    g_hash_table_remove_all(bluez->peer_epochs);
    next_epoch_locked(bluez);
    g_mutex_unlock(&bluez->peer_lock);
    bluez->bluez_present = FALSE;
    bluez->application_registered = FALSE;
    bluez->advertisement_registered = FALSE;
    bluez->agent_registered = FALSE;
    if (bluez->retry_source) {
        g_source_remove(bluez->retry_source);
        bluez->retry_source = 0;
    }
    memset(bluez->adapter_path, 0, sizeof(bluez->adapter_path));
    if (bluez->handlers.bluez_lost)
        bluez->handlers.bluez_lost(bluez->handlers.user_data);
}

static void properties_changed(GDBusConnection *connection, const char *sender,
                               const char *object_path, const char *interface,
                               const char *signal, GVariant *parameters,
                               gpointer user_data)
{
    (void)connection; (void)sender; (void)interface; (void)signal;
    EgpBluez *bluez = user_data;
    const char *changed_interface = NULL;
    GVariant *changed = NULL;
    GVariant *invalidated = NULL;
    g_variant_get(parameters, "(&s@a{sv}@as)", &changed_interface,
                  &changed, &invalidated);
    gboolean connected = TRUE;
    gboolean device_change = !strcmp(changed_interface, BLUEZ_DEVICE);
    gboolean has_connected = device_change &&
                             g_variant_lookup(changed, "Connected", "b", &connected);
    gboolean security_change = has_connected;
    const char *const epoch_properties[] = {
        "Paired", "Bonded", "Address", "AddressType", "ServicesResolved"
    };
    for (guint i = 0; device_change && i < G_N_ELEMENTS(epoch_properties); ++i) {
        GVariant *value = g_variant_lookup_value(changed, epoch_properties[i], NULL);
        if (value) {
            security_change = TRUE;
            g_variant_unref(value);
        }
    }
    GVariantIter invalidated_iter;
    const char *invalidated_name = NULL;
    g_variant_iter_init(&invalidated_iter, invalidated);
    while (device_change && g_variant_iter_loop(&invalidated_iter, "&s",
                                                 &invalidated_name)) {
        if (!strcmp(invalidated_name, "Connected") ||
            !strcmp(invalidated_name, "Paired") ||
            !strcmp(invalidated_name, "Bonded") ||
            !strcmp(invalidated_name, "Address") ||
            !strcmp(invalidated_name, "AddressType") ||
            !strcmp(invalidated_name, "ServicesResolved")) {
            security_change = TRUE;
            break;
        }
    }
    if (security_change)
        bump_peer_epoch(bluez, object_path);
    if (has_connected && !connected && bluez->handlers.peer_disconnected)
        bluez->handlers.peer_disconnected(bluez->handlers.user_data, object_path);
    g_variant_unref(changed);
    g_variant_unref(invalidated);
}

static GDBusNodeInfo *parse_xml(const char *xml, EgpError *error)
{
    GError *parse_error = NULL;
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(xml, &parse_error);
    if (!info)
        egp_error_set(error, EGP_ERROR_GATT_REGISTRATION_FAILED, TRUE,
                      EGP_PERSISTENT_CHANGE_NONE, "D-Bus introspection is invalid: %s",
                      parse_error ? parse_error->message : "unknown error");
    g_clear_error(&parse_error);
    return info;
}

EgpBluez *egp_bluez_new(GDBusConnection *connection,
                        const EgpBluezHandlers *handlers, EgpError *error)
{
    if (!connection || !handlers)
        return NULL;
    EgpBluez *bluez = g_new0(EgpBluez, 1);
    g_mutex_init(&bluez->peer_lock);
    bluez->peer_epochs = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, g_free);
    bluez->connection = g_object_ref(connection);
    bluez->handlers = *handlers;
    bluez->manager_info = parse_xml(manager_xml, error);
    bluez->service_info = parse_xml(service_xml, error);
    bluez->characteristic_info = parse_xml(characteristic_xml, error);
    bluez->advertisement_info = parse_xml(advertisement_xml, error);
    bluez->agent_info = parse_xml(agent_xml, error);
    if (!bluez->manager_info || !bluez->service_info || !bluez->characteristic_info ||
        !bluez->advertisement_info || !bluez->agent_info)
        goto failed;
    if (!add_export(bluez, EXPORT_MANAGER, 0, EGP_DBUS_ROOT,
                    bluez->manager_info, error) ||
        !add_export(bluez, EXPORT_SERVICE, 0, EGP_DBUS_SERVICE,
                    bluez->service_info, error))
        goto failed;
    for (guint i = 0; i < 5; ++i)
        if (!add_export(bluez, EXPORT_CHARACTERISTIC, (EgpBluezCharacteristic)i,
                        characteristic_path((EgpBluezCharacteristic)i),
                        bluez->characteristic_info, error))
            goto failed;
    if (!add_export(bluez, EXPORT_ADVERTISEMENT, 0, EGP_DBUS_ADVERTISEMENT,
                    bluez->advertisement_info, error) ||
        !add_export(bluez, EXPORT_AGENT, 0, EGP_DBUS_AGENT,
                    bluez->agent_info, error))
        goto failed;
    bluez->name_watch = g_bus_watch_name_on_connection(
        connection, BLUEZ_BUS, G_BUS_NAME_WATCHER_FLAGS_NONE,
        name_appeared, name_vanished, bluez, NULL);
    bluez->properties_subscription = g_dbus_connection_signal_subscribe(
        connection, BLUEZ_BUS, DBUS_PROPERTIES, "PropertiesChanged", NULL,
        BLUEZ_DEVICE, G_DBUS_SIGNAL_FLAGS_MATCH_ARG0_NAMESPACE,
        properties_changed, bluez, NULL);
    egp_error_clear(error);
    return bluez;
failed:
    egp_bluez_free(bluez);
    return NULL;
}

void egp_bluez_set_window(EgpBluez *bluez, gboolean open)
{
    if (!bluez)
        return;
    bluez->window_open = open;
    if (!bluez->bluez_present || !bluez->adapter_path[0])
        return;
    set_pairable(bluez, open);
    if (open && !bluez->advertisement_registered) {
        bluez->advertisement_registered = bluez_call_sync(
            bluez, bluez->adapter_path, BLUEZ_ADV_MANAGER,
            "RegisterAdvertisement",
            g_variant_new("(oa{sv})", EGP_DBUS_ADVERTISEMENT, NULL));
        if (!bluez->advertisement_registered)
            ensure_retry(bluez);
    } else if (!open && bluez->advertisement_registered) {
        bluez_call(bluez, bluez->adapter_path, BLUEZ_ADV_MANAGER,
                   "UnregisterAdvertisement", g_variant_new("(o)", EGP_DBUS_ADVERTISEMENT));
        bluez->advertisement_registered = FALSE;
    }
}

gboolean egp_bluez_peer_security(EgpBluez *bluez, const char *peer_path,
                                 gboolean encrypted_gatt_gate,
                                 EgpPeerSecurity *peer, EgpError *error)
{
    if (!bluez || !peer_path || !g_variant_is_object_path(peer_path) || !peer)
        return FALSE;
    memset(peer, 0, sizeof(*peer));
    char destination[sizeof(bluez->unique_owner)] = {0};
    guint64 epoch_before = 0;
    g_mutex_lock(&bluez->peer_lock);
    g_strlcpy(destination, bluez->unique_owner, sizeof(destination));
    epoch_before = peer_epoch_locked(bluez, peer_path, TRUE);
    g_mutex_unlock(&bluez->peer_lock);
    if (!destination[0] || !epoch_before) {
        egp_error_set(error, EGP_ERROR_BLUEZ_UNAVAILABLE, TRUE,
                      EGP_PERSISTENT_CHANGE_NONE,
                      "Active BlueZ owner is unavailable");
        return FALSE;
    }
    GError *call_error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        bluez->connection, destination, peer_path, DBUS_PROPERTIES, "GetAll",
        g_variant_new("(s)", BLUEZ_DEVICE), G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &call_error);
    if (!reply) {
        g_clear_error(&call_error);
        egp_error_set(error, EGP_ERROR_BLE_UNAUTHORIZED, TRUE,
                      EGP_PERSISTENT_CHANGE_NONE,
                      "BlueZ peer properties are unavailable");
        return FALSE;
    }
    GVariant *properties = NULL;
    g_variant_get(reply, "(@a{sv})", &properties);
    const char *address = NULL, *address_type = NULL;
    g_variant_lookup(properties, "Connected", "b", &peer->connected);
    g_variant_lookup(properties, "Paired", "b", &peer->paired);
    g_variant_lookup(properties, "Bonded", "b", &peer->bonded);
    gboolean has_address = g_variant_lookup(properties, "Address", "&s", &address) &&
                           address && *address;
    gboolean has_type = g_variant_lookup(properties, "AddressType", "&s", &address_type) &&
                        address_type && (!strcmp(address_type, "public") ||
                                         !strcmp(address_type, "random"));
    peer->stable_identity = has_address && has_type && peer->paired && peer->bonded;
    /* BlueZ enforces the exported encrypt-* flag before a GATT callback. The
     * caller marks only that initial callback fact; this adapter does not
     * independently query controller encryption state. */
    peer->encrypted_transport = encrypted_gatt_gate;
    g_mutex_lock(&bluez->peer_lock);
    guint64 epoch_after = peer_epoch_locked(bluez, peer_path, FALSE);
    gboolean owner_stable = !strcmp(destination, bluez->unique_owner);
    g_mutex_unlock(&bluez->peer_lock);
    peer->connection_epoch = owner_stable && epoch_before == epoch_after
                                 ? epoch_after : 0;
    g_variant_unref(properties);
    g_variant_unref(reply);
    if (!peer->stable_identity || !peer->connection_epoch) {
        egp_error_set(error, EGP_ERROR_BLE_NOT_PAIRED, TRUE,
                      EGP_PERSISTENT_CHANGE_NONE,
                      "Peer lacks a stable paired bond identity");
        return FALSE;
    }
    egp_error_clear(error);
    return TRUE;
}

typedef struct {
    GDBusConnection *connection;
    char owner[EGP_DBUS_PATH_CAP];
} DisconnectRequest;

static void disconnect_listing(GObject *source, GAsyncResult *result, gpointer user_data)
{
    DisconnectRequest *request = user_data;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source),
                                                     result, &error);
    if (reply) {
        GVariantIter *objects = NULL;
        g_variant_get(reply, "(a{oa{sa{sv}}})", &objects);
        const char *path;
        GVariant *interfaces;
        while (g_variant_iter_next(objects, "{&o@a{sa{sv}}}", &path, &interfaces)) {
            GVariant *device = g_variant_lookup_value(interfaces, BLUEZ_DEVICE,
                                                      G_VARIANT_TYPE("a{sv}"));
            gboolean connected = FALSE;
            if (device)
                g_variant_lookup(device, "Connected", "b", &connected);
            if (device && connected && strcmp(path, request->owner))
                g_dbus_connection_call(request->connection, BLUEZ_BUS, path,
                                       BLUEZ_DEVICE, "Disconnect", NULL, NULL,
                                       G_DBUS_CALL_FLAGS_NONE, 3000, NULL,
                                       call_finished, NULL);
            if (device) g_variant_unref(device);
            g_variant_unref(interfaces);
        }
        g_variant_iter_free(objects);
        g_variant_unref(reply);
    }
    g_clear_error(&error);
    g_object_unref(request->connection);
    g_free(request);
}

void egp_bluez_disconnect_non_owner(EgpBluez *bluez, const char *owner_path)
{
    if (!bluez || !owner_path || !g_variant_is_object_path(owner_path))
        return;
    set_pairable(bluez, FALSE);
    DisconnectRequest *request = g_new0(DisconnectRequest, 1);
    request->connection = g_object_ref(bluez->connection);
    g_strlcpy(request->owner, owner_path, sizeof(request->owner));
    g_dbus_connection_call(bluez->connection, BLUEZ_BUS, "/",
                           "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
                           NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
                           G_DBUS_CALL_FLAGS_NONE, 3000, NULL,
                           disconnect_listing, request);
}

void egp_bluez_free(EgpBluez *bluez)
{
    if (!bluez)
        return;
    if (bluez->bluez_present && bluez->adapter_path[0]) {
        set_pairable(bluez, FALSE);
        if (bluez->advertisement_registered)
            bluez_call(bluez, bluez->adapter_path, BLUEZ_ADV_MANAGER,
                       "UnregisterAdvertisement", g_variant_new("(o)", EGP_DBUS_ADVERTISEMENT));
        if (bluez->application_registered)
            bluez_call(bluez, bluez->adapter_path, BLUEZ_GATT_MANAGER,
                       "UnregisterApplication", g_variant_new("(o)", EGP_DBUS_ROOT));
        if (bluez->agent_registered)
            bluez_call(bluez, "/org/bluez", BLUEZ_AGENT_MANAGER,
                       "UnregisterAgent", g_variant_new("(o)", EGP_DBUS_AGENT));
    }
    if (bluez->name_watch)
        g_bus_unwatch_name(bluez->name_watch);
    if (bluez->properties_subscription)
        g_dbus_connection_signal_unsubscribe(bluez->connection,
                                             bluez->properties_subscription);
    if (bluez->retry_source)
        g_source_remove(bluez->retry_source);
    for (guint i = 0; i < bluez->export_count; ++i)
        if (bluez->exports[i].registration_id)
            g_dbus_connection_unregister_object(bluez->connection,
                                                bluez->exports[i].registration_id);
    if (bluez->manager_info) g_dbus_node_info_unref(bluez->manager_info);
    if (bluez->service_info) g_dbus_node_info_unref(bluez->service_info);
    if (bluez->characteristic_info) g_dbus_node_info_unref(bluez->characteristic_info);
    if (bluez->advertisement_info) g_dbus_node_info_unref(bluez->advertisement_info);
    if (bluez->agent_info) g_dbus_node_info_unref(bluez->agent_info);
    if (bluez->peer_epochs) g_hash_table_unref(bluez->peer_epochs);
    g_mutex_clear(&bluez->peer_lock);
    if (bluez->connection) g_object_unref(bluez->connection);
    g_free(bluez);
}
