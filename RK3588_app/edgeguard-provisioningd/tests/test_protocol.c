#include "edgeguard_provisioning/protocol.h"

#include <string.h>

static GByteArray *frame(guint8 opcode, guint8 flags, const guint8 id[16],
                         guint16 total, guint16 offset,
                         const char *fragment, guint16 fragment_length)
{
    guint8 header[EGP_FRAME_HEADER_BYTES] = {0};
    memcpy(header, "EGP1", 4);
    header[4] = EGP_PROTOCOL_VERSION;
    header[5] = opcode;
    header[6] = flags;
    memcpy(header + 8, id, 16);
    header[24] = (guint8)(total >> 8);
    header[25] = (guint8)total;
    header[26] = (guint8)(offset >> 8);
    header[27] = (guint8)offset;
    header[28] = (guint8)(fragment_length >> 8);
    header[29] = (guint8)fragment_length;
    GByteArray *value = g_byte_array_new();
    g_byte_array_append(value, header, sizeof(header));
    g_byte_array_append(value, (const guint8 *)fragment, fragment_length);
    return value;
}

static void single_fragment(void)
{
    EgpProtocol *protocol = egp_protocol_new();
    const guint8 id[16] = {1};
    const char json[] = "{}";
    GByteArray *value = frame(EGP_OPCODE_FORGET_WIFI, 3, id,
                              strlen(json), 0, json, strlen(json));
    EgpProtocolOutput output;
    EgpError error = {0};
    g_assert_true(egp_protocol_accept(protocol, "/org/bluez/hci0/dev_AA",
                                      EGP_WRITABLE_PROVISIONING,
                                      value->data, value->len, 1000,
                                      &output, &error));
    g_assert_cmpint(output.disposition, ==, EGP_PROTOCOL_COMPLETE);
    g_assert_cmpstr(output.request.json, ==, json);
    g_assert_cmpint(output.request.opcode, ==, EGP_OPCODE_FORGET_WIFI);
    g_byte_array_unref(value);
    egp_protocol_free(protocol);
}

static void fragmented_and_duplicate(void)
{
    EgpProtocol *protocol = egp_protocol_new();
    const guint8 id[16] = {2};
    const char json[] = "{\"base_url\":\"http://host:8000\"}";
    guint16 total = strlen(json), split = 12;
    GByteArray *first = frame(EGP_OPCODE_SET_ENDPOINT, 1, id, total, 0,
                              json, split);
    GByteArray *second = frame(EGP_OPCODE_SET_ENDPOINT, 2, id, total, split,
                               json + split, total - split);
    EgpProtocolOutput output;
    EgpError error = {0};
    g_assert_true(egp_protocol_accept(protocol, "/org/bluez/hci0/dev_BB",
                                      EGP_WRITABLE_PROVISIONING,
                                      first->data, first->len, 1000,
                                      &output, &error));
    g_assert_cmpint(output.disposition, ==, EGP_PROTOCOL_INCOMPLETE);
    g_assert_true(egp_protocol_accept(protocol, "/org/bluez/hci0/dev_BB",
                                      EGP_WRITABLE_PROVISIONING,
                                      first->data, first->len, 1100,
                                      &output, &error));
    g_assert_true(egp_protocol_accept(protocol, "/org/bluez/hci0/dev_BB",
                                      EGP_WRITABLE_PROVISIONING,
                                      second->data, second->len, 1200,
                                      &output, &error));
    g_assert_cmpint(output.disposition, ==, EGP_PROTOCOL_COMPLETE);
    g_assert_cmpstr(output.request.json, ==, json);
    g_byte_array_unref(first);
    g_byte_array_unref(second);
    egp_protocol_free(protocol);
}

static void overlap_conflict(void)
{
    EgpProtocol *protocol = egp_protocol_new();
    const guint8 id[16] = {3};
    GByteArray *first = frame(EGP_OPCODE_SET_ENDPOINT, 1, id, 4, 0, "{\"x", 3);
    GByteArray *conflict = frame(EGP_OPCODE_SET_ENDPOINT, 0, id, 4, 2, "y", 1);
    EgpProtocolOutput output;
    EgpError error = {0};
    g_assert_true(egp_protocol_accept(protocol, "/org/bluez/hci0/dev_CC",
                                      EGP_WRITABLE_PROVISIONING,
                                      first->data, first->len, 1000,
                                      &output, &error));
    g_assert_false(egp_protocol_accept(protocol, "/org/bluez/hci0/dev_CC",
                                       EGP_WRITABLE_PROVISIONING,
                                       conflict->data, conflict->len, 1100,
                                       &output, &error));
    g_assert_cmpint(error.code, ==, EGP_ERROR_BLE_FRAGMENT_CONFLICT);
    g_byte_array_unref(first);
    g_byte_array_unref(conflict);
    egp_protocol_free(protocol);
}

static void replay_and_conflicting_reuse(void)
{
    EgpProtocol *protocol = egp_protocol_new();
    const guint8 id[16] = {4};
    GByteArray *value = frame(EGP_OPCODE_FORGET_WIFI, 3, id, 2, 0, "{}", 2);
    EgpProtocolOutput output;
    EgpError error = {0};
    g_assert_true(egp_protocol_accept(protocol, "/org/bluez/hci0/dev_DD",
                                      EGP_WRITABLE_PROVISIONING,
                                      value->data, value->len, 1000,
                                      &output, &error));
    g_assert_true(egp_protocol_cache_result(protocol, &output.request,
                                            "{\"status\":\"SUCCEEDED\"}",
                                            1100, &error));
    g_assert_true(egp_protocol_accept(protocol, "/org/bluez/hci0/dev_DD",
                                      EGP_WRITABLE_PROVISIONING,
                                      value->data, value->len, 1200,
                                      &output, &error));
    g_assert_cmpint(output.disposition, ==, EGP_PROTOCOL_REPLAY);
    g_assert_nonnull(strstr(output.cached_result, "SUCCEEDED"));
    g_byte_array_unref(value);

    value = frame(EGP_OPCODE_FORGET_WIFI, 3, id, 3, 0, "{ }", 3);
    g_assert_false(egp_protocol_accept(protocol, "/org/bluez/hci0/dev_DD",
                                       EGP_WRITABLE_PROVISIONING,
                                       value->data, value->len, 1300,
                                       &output, &error));
    g_assert_cmpint(error.code, ==, EGP_ERROR_BLE_REPLAY_REJECTED);
    g_byte_array_unref(value);
    egp_protocol_free(protocol);
}

static void timeout(void)
{
    EgpProtocol *protocol = egp_protocol_new();
    const guint8 id[16] = {5};
    GByteArray *first = frame(EGP_OPCODE_SET_ENDPOINT, 1, id, 4, 0, "{", 1);
    GByteArray *next = frame(EGP_OPCODE_SET_ENDPOINT, 2, id, 4, 1, "xxx", 3);
    EgpProtocolOutput output;
    EgpError error = {0};
    g_assert_true(egp_protocol_accept(protocol, "/org/bluez/hci0/dev_EE",
                                      EGP_WRITABLE_PROVISIONING,
                                      first->data, first->len, 1000,
                                      &output, &error));
    g_assert_false(egp_protocol_accept(protocol, "/org/bluez/hci0/dev_EE",
                                       EGP_WRITABLE_PROVISIONING,
                                       next->data, next->len,
                                       1000 + EGP_PROTOCOL_IDLE_TIMEOUT_US,
                                       &output, &error));
    g_assert_cmpint(error.code, ==, EGP_ERROR_BLE_TRANSACTION_TIMEOUT);
    g_byte_array_unref(first);
    g_byte_array_unref(next);
    egp_protocol_free(protocol);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/protocol/single", single_fragment);
    g_test_add_func("/protocol/fragmented-duplicate", fragmented_and_duplicate);
    g_test_add_func("/protocol/overlap-conflict", overlap_conflict);
    g_test_add_func("/protocol/replay", replay_and_conflicting_reuse);
    g_test_add_func("/protocol/timeout", timeout);
    return g_test_run();
}
