#include "edgeguard_provisioning/gatt_read.h"

#include <string.h>

#define PEER "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"
#define OWNER ":1.42"

static GVariant *options(guint16 offset, gboolean prepare)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&builder, "{sv}", "device",
                          g_variant_new_object_path(PEER));
    g_variant_builder_add(&builder, "{sv}", "offset",
                          g_variant_new_uint16(offset));
    g_variant_builder_add(&builder, "{sv}", "mtu",
                          g_variant_new_uint16(23));
    if (prepare)
        g_variant_builder_add(&builder, "{sv}", "prepare-authorize",
                              g_variant_new_boolean(TRUE));
    return g_variant_ref_sink(g_variant_builder_end(&builder));
}

static void read_and_write_options_are_distinct(void)
{
    EgpGattOptions parsed = {0};
    EgpError error = {0};
    GVariant *read = options(7, FALSE);
    g_assert_true(egp_gatt_parse_read_options(read, TRUE, &parsed, &error));
    g_assert_cmpuint(parsed.offset, ==, 7);
    g_assert_cmpstr(parsed.peer_path, ==, PEER);
    g_assert_false(egp_gatt_parse_write_options(read, &parsed, &error));
    g_assert_cmpint(error.code, ==, EGP_ERROR_BLE_PAYLOAD_INVALID);
    g_variant_unref(read);

    GVariant *prepare = options(0, TRUE);
    g_assert_false(egp_gatt_parse_write_options(prepare, &parsed, &error));
    g_assert_cmpint(error.code, ==, EGP_ERROR_BLE_PAYLOAD_INVALID);
    g_variant_unref(prepare);
}

static void offset_boundaries_and_snapshot_consistency(void)
{
    guint8 source[44];
    memset(source, 'A', 22);
    memset(source + 22, 'B', 22);
    EgpGattReadSnapshot snapshot = {0};
    EgpError error = {0};
    gint64 now = 1000;
    g_assert_true(egp_gatt_read_snapshot_begin(
        &snapshot, OWNER, PEER, 1, 9, source, sizeof(source), now, &error));
    memset(source, 'X', sizeof(source));

    GBytes *value = NULL;
    g_assert_true(egp_gatt_read_snapshot_slice(
        &snapshot, OWNER, PEER, 1, 9, 0, 23, now + 1, &value, &error));
    gsize length = 0;
    const guint8 *bytes = g_bytes_get_data(value, &length);
    g_assert_cmpuint(length, ==, 22);
    g_assert_cmpmem(bytes, length, "AAAAAAAAAAAAAAAAAAAAAA", 22);
    g_bytes_unref(value);

    g_assert_true(egp_gatt_read_snapshot_slice(
        &snapshot, OWNER, PEER, 1, 9, 22, 23, now + 2, &value, &error));
    bytes = g_bytes_get_data(value, &length);
    g_assert_cmpuint(length, ==, 22);
    g_assert_cmpmem(bytes, length, "BBBBBBBBBBBBBBBBBBBBBB", 22);
    g_bytes_unref(value);

    g_assert_true(egp_gatt_read_snapshot_slice(
        &snapshot, OWNER, PEER, 1, 9, 44, 23, now + 3, &value, &error));
    g_bytes_get_data(value, &length);
    g_assert_cmpuint(length, ==, 0);
    g_assert_false(snapshot.valid);
    g_bytes_unref(value);

    g_assert_true(egp_gatt_read_snapshot_begin(
        &snapshot, OWNER, PEER, 1, 9, source, sizeof(source), now, &error));
    g_assert_false(egp_gatt_read_snapshot_slice(
        &snapshot, OWNER, PEER, 1, 9, 45, 23, now + 4, &value, &error));
    g_assert_cmpint(error.code, ==, EGP_ERROR_BLE_INVALID_OFFSET);
}

static void snapshot_context_and_lifetime_invalidation(void)
{
    const guint8 source[] = "immutable";
    EgpGattReadSnapshot snapshot = {0};
    EgpError error = {0};
    GBytes *value = NULL;
    gint64 now = 1000;
    g_assert_true(egp_gatt_read_snapshot_begin(
        &snapshot, OWNER, PEER, 3, 12, source, sizeof(source) - 1u,
        now, &error));
    g_assert_false(egp_gatt_read_snapshot_slice(
        &snapshot, OWNER, PEER, 3, 13, 1, 23, now + 1, &value, &error));
    g_assert_cmpint(error.code, ==, EGP_ERROR_BLE_INVALID_OFFSET);
    g_assert_false(snapshot.valid);

    g_assert_true(egp_gatt_read_snapshot_begin(
        &snapshot, OWNER, PEER, 3, 12, source, sizeof(source) - 1u,
        now, &error));
    g_assert_false(egp_gatt_read_snapshot_slice(
        &snapshot, ":1.99", PEER, 3, 12, 1, 23, now + 1,
        &value, &error));

    g_assert_true(egp_gatt_read_snapshot_begin(
        &snapshot, OWNER, PEER, 3, 12, source, sizeof(source) - 1u,
        now, &error));
    g_assert_false(egp_gatt_read_snapshot_slice(
        &snapshot, OWNER, "/org/bluez/hci0/dev_11", 3, 12, 1, 23,
        now + 1, &value, &error));

    g_assert_true(egp_gatt_read_snapshot_begin(
        &snapshot, OWNER, PEER, 3, 12, source, sizeof(source) - 1u,
        now, &error));
    g_assert_false(egp_gatt_read_snapshot_slice(
        &snapshot, OWNER, PEER, 1, 12, 1, 23, now + 1,
        &value, &error));

    g_assert_true(egp_gatt_read_snapshot_begin(
        &snapshot, OWNER, PEER, 3, 12, source, sizeof(source) - 1u,
        now, &error));
    g_assert_false(egp_gatt_read_snapshot_slice(
        &snapshot, OWNER, PEER, 3, 12, 1, 23,
        now + EGP_GATT_READ_SNAPSHOT_TTL_US, &value, &error));
    g_assert_cmpint(error.code, ==, EGP_ERROR_BLE_INVALID_OFFSET);

    g_assert_true(egp_gatt_read_snapshot_begin(
        &snapshot, OWNER, PEER, 3, 12, source, sizeof(source) - 1u,
        now, &error));
    egp_gatt_read_snapshot_clear(&snapshot);
    g_assert_false(snapshot.valid);
}

static void snapshot_has_hard_bound(void)
{
    guint8 oversized[EGP_GATT_VALUE_MAX_BYTES + 1u] = {0};
    EgpGattReadSnapshot snapshot = {0};
    EgpError error = {0};
    g_assert_false(egp_gatt_read_snapshot_begin(
        &snapshot, OWNER, PEER, 0, 1, oversized, sizeof(oversized), 1,
        &error));
    g_assert_cmpint(error.code, ==, EGP_ERROR_BLE_PAYLOAD_TOO_LARGE);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/gatt/options/read-write-distinct",
                    read_and_write_options_are_distinct);
    g_test_add_func("/gatt/read/offset-boundaries",
                    offset_boundaries_and_snapshot_consistency);
    g_test_add_func("/gatt/read/context-lifetime",
                    snapshot_context_and_lifetime_invalidation);
    g_test_add_func("/gatt/read/hard-bound", snapshot_has_hard_bound);
    return g_test_run();
}
