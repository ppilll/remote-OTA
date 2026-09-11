#include "edgeguard_provisioning/endpoint.h"
#include "edgeguard_provisioning/operations.h"
#include "edgeguard_provisioning/status.h"

#include <string.h>

static void fill(char *output, gsize length, char byte)
{
    memset(output, byte, length);
    output[length] = '\0';
}

static void endpoint_max_and_max_plus_one(void)
{
    char maximum[EGP_ENDPOINT_MAX_BYTES + 1u] = {0};
    char oversized[EGP_ENDPOINT_MAX_BYTES + 2u] = {0};
    char canonical[EGP_ENDPOINT_MAX_BYTES + 1u] = {0};
    EgpError error = {0};
    g_strlcpy(maximum, "http://", sizeof(maximum));
    fill(maximum + 7, EGP_ENDPOINT_MAX_BYTES - 7u, 'a');
    g_strlcpy(oversized, "http://", sizeof(oversized));
    fill(oversized + 7, EGP_ENDPOINT_MAX_BYTES - 6u, 'a');
    g_assert_cmpuint(strlen(maximum), ==, EGP_ENDPOINT_MAX_BYTES);
    g_assert_cmpuint(strlen(oversized), ==, EGP_ENDPOINT_MAX_BYTES + 1u);
    g_assert_true(egp_endpoint_canonicalize(maximum, canonical, &error));
    g_assert_cmpstr(canonical, ==, maximum);
    g_assert_false(egp_endpoint_canonicalize(oversized, canonical, &error));
    g_assert_cmpint(error.code, ==, EGP_ERROR_ENDPOINT_INVALID);
}

static void three_real_serializers_fit_att_bound(void)
{
    char json[EGP_GATT_VALUE_MAX_BYTES + 1u] = {0};
    gsize length = 0;
    EgpError error = {0};

    EgpDeviceInfoValue device = {
        .device_available = TRUE,
        .release_available = TRUE,
        .provisioning_state = EGP_STATE_FAILED_DEPENDENCY
    };
    g_strlcpy(device.device_id, "12345678-1234-4123-8123-123456789abc",
              sizeof(device.device_id));
    fill(device.release, EGP_RELEASE_FIELD_MAX_BYTES, '\\');
    fill(device.build_id, EGP_RELEASE_FIELD_MAX_BYTES, '\\');
    g_assert_true(egp_status_serialize_device_info(
        &device, json, &length, &error));
    g_assert_cmpuint(length, ==, 468);
    g_assert_cmpuint(length, <=, EGP_GATT_VALUE_MAX_BYTES);
    g_test_message("DeviceInfo worst-case serialized size: %" G_GSIZE_FORMAT,
                   length);

    EgpRuntimeStatusValue runtime = {
        .provisioning_state = EGP_STATE_FAILED_DEPENDENCY,
        .wifi_connected = TRUE,
        .effective_endpoint_available = TRUE,
        .runtime_config_invalid = TRUE,
        .agent_state_available = TRUE,
        .current_slot_available = TRUE
    };
    g_strlcpy(runtime.effective_endpoint, "https://",
              sizeof(runtime.effective_endpoint));
    fill(runtime.effective_endpoint + 8, EGP_ENDPOINT_MAX_BYTES - 8u, 'a');
    g_strlcpy(runtime.effective_endpoint_source, "immutable_default",
              sizeof(runtime.effective_endpoint_source));
    g_strlcpy(runtime.agent_state, "VERIFY_DOWNLOAD",
              sizeof(runtime.agent_state));
    g_strlcpy(runtime.attempt_id, "12345678-1234-4123-8123-123456789abc",
              sizeof(runtime.attempt_id));
    g_strlcpy(runtime.last_ota_error, "DOWNLOAD_RANGE_MISMATCH",
              sizeof(runtime.last_ota_error));
    g_strlcpy(runtime.current_slot, "a", sizeof(runtime.current_slot));
    g_assert_true(egp_status_serialize_runtime(
        &runtime, json, &length, &error));
    g_assert_cmpuint(length, ==, 509);
    g_assert_cmpuint(length, <=, EGP_GATT_VALUE_MAX_BYTES);
    g_test_message("RuntimeStatus worst-case serialized size: %" G_GSIZE_FORMAT,
                   length);

    EgpProtocolRequest request = { .opcode = EGP_OPCODE_SET_ENDPOINT };
    memset(request.transaction_id, 0xff, sizeof(request.transaction_id));
    EgpError operation_error = {
        .code = EGP_ERROR_PERSISTENCE_SCHEMA_UNSUPPORTED,
        .retryable = TRUE,
        .persistent_change = EGP_PERSISTENT_CHANGE_ROLLED_BACK
    };
    fill(operation_error.message, sizeof(operation_error.message) - 1u, '\x01');
    char endpoint[EGP_ENDPOINT_MAX_BYTES + 1u] = {0};
    g_strlcpy(endpoint, "https://", sizeof(endpoint));
    fill(endpoint + 8, EGP_ENDPOINT_MAX_BYTES - 8u, 'a');
    length = egp_operations_result_json(
        &request, "IN_PROGRESS", &operation_error, G_MAXINT64, endpoint, json);
    g_assert_cmpuint(length, ==, 368);
    g_assert_cmpuint(length, <=, EGP_GATT_VALUE_MAX_BYTES);
    g_assert_nonnull(strstr(json, "\"message\":\"Result detail omitted\""));
    g_test_message("OperationResult worst-case serialized size: %" G_GSIZE_FORMAT,
                   length);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/endpoint/max-bound", endpoint_max_and_max_plus_one);
    g_test_add_func("/gatt/read/serializer-bounds",
                    three_real_serializers_fit_att_bound);
    return g_test_run();
}
