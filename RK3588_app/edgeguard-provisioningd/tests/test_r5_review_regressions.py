"""Source-contract regressions for the R5 review fixes.

These checks intentionally make no host, SDK-build, or target claim.
"""
from pathlib import Path


PROVISIONING = Path(__file__).resolve().parents[1]
REPO = PROVISIONING.parents[1]
AGENT = REPO / "RK3588_app/edgeguard-remote-ota-agent"


def source(root, relative):
    return (root / relative).read_text(encoding="utf-8")


def test_connman_owner_reappearance_has_continuous_capped_reconciliation():
    main = source(PROVISIONING, "src/main.c")
    operations = source(PROVISIONING, "src/operations.c")
    assert 'g_bus_watch_name_on_connection(' in main
    assert '"net.connman"' in main
    assert "CONNMAN_RECONCILE_MAX_ATTEMPTS" not in main
    assert "egp_connman_retry_delay_ms" in main
    assert "egp_operations_reconcile" in main
    assert "egp_operations_connman_lost" in main
    assert "if (retryable)" in main
    assert "error.retryable" in main
    assert "reconcile_canonical" in operations
    assert "g_cancellable_cancel" in operations
    reconcile = operations[operations.index("static void reconcile_canonical"):
                           operations.index("static void worker")]
    assert "stable_idle" not in reconcile
    mutation_gate = operations[operations.index("static gboolean authorize_and_idle"):
                               operations.index("static void set_success")]
    assert "stable_idle" in mutation_gate


def test_forget_requires_fixed_profile_and_unique_owned_service():
    connman = source(PROVISIONING, "src/connman.c")
    assert '"GetNameOwner"' in connman
    assert "immutable && favorite && autoconnect" in connman
    assert "owned_profile_present" in connman
    assert "service_attested" in connman
    assert "call_sync(connman, owner, observed.path" in connman
    assert "immutable || favorite || autoconnect" not in connman
    revoke = connman[connman.index("gboolean egp_connman_revoke"):]
    assert revoke.index("observe_service") < revoke.index("egp_connman_remove_profile")
    assert '"Remove"' not in revoke and '"Disconnect"' not in revoke


def test_runtime_and_operation_result_are_encrypted_read_only():
    bluez = source(PROVISIONING, "src/bluez.c")
    docs = source(REPO, "docs/R5/03_GATT_SPEC.md")
    assert 'static const char *const result[] = { "encrypt-read", NULL };' in bluez
    assert 'static const char *const status[] = { "encrypt-read", NULL };' in bluez
    assert "Only the active BlueZ owner may invoke this object" in bluez
    for removed_surface in ("StartNotify", "StopNotify", "Notifying",
                            '"notify"', '"Value"', "egp_bluez_publish"):
        assert removed_surface not in bluez
    for characteristic in ("RuntimeStatus", "OperationResult"):
        flags = docs.split(f"| {characteristic} |", 1)[1].splitlines()[0]
        assert "notify" not in flags
        assert "encrypt-read" in flags


def test_window_epoch_change_drops_stale_transactions_and_results():
    main = source(PROVISIONING, "src/main.c")
    window_changed = main.split("static void window_changed", 1)[1].split(
        "static void physical_presence", 1
    )[0]
    assert "if (!open)" not in window_changed
    assert "egp_protocol_drop_all" in window_changed
    assert "active_valid = FALSE" in window_changed
    assert "memset(&daemon->active_request" in window_changed
    assert "memset(daemon->result_peer" in window_changed
    assert "memset(daemon->last_result" in window_changed


def test_commit_binds_window_and_connection_security_epochs():
    security = source(PROVISIONING, "src/security.c")
    bluez = source(PROVISIONING, "src/bluez.c")
    operations = source(PROVISIONING, "src/operations.c")
    main = source(PROVISIONING, "src/main.c")
    assert "egp_security_authorize_commit" in security
    assert "expected_window_epoch" in security
    assert "expected_connection_epoch" in security
    assert "bump_peer_epoch" in bluez
    for property_name in ('"Connected"', '"Paired"', '"Bonded"'):
        assert property_name in bluez
    assert "Address" in bluez and "AddressType" in bluez
    assert "peer->encrypted_transport = encrypted_gatt_gate" in bluez
    assert "peer_currently_eligible(peer, error)" in security
    assert "work->request.window_epoch" in operations
    assert "output.request.connection_epoch = peer.connection_epoch" in main
    commit_gate = operations[operations.index("static gboolean authorize_and_idle"):
                             operations.index("static void set_success")]
    assert commit_gate.index("stable_idle") < commit_gate.index("authorize_commit")


def test_gatt_transaction_to_ipc_uuid_is_scoped_and_agent_accepts_it():
    status = source(PROVISIONING, "src/status.c")
    protocol = source(PROVISIONING, "src/protocol.c")
    protocol_test = source(PROVISIONING, "tests/test_protocol.c")
    control = source(AGENT, "src/control.c")
    assert "g_uuid_string_random" in status
    assert "egp_protocol_scoped_request_id" in status
    for scope in ("daemon_session_id", "window_epoch", "peer_path",
                  "connection_epoch", "characteristic", "opcode",
                  "transaction_id"):
        assert scope in protocol
    for relation in ("g_assert_cmpstr(first, ==, retry)",
                     "changed.window_epoch++", "changed.connection_epoch++",
                     "dev_BB",
                     "changed.transaction_id[0]", "session_b"):
        assert relation in protocol_test
    assert "ota_uuid_valid(request_id, FALSE)" in control
    assert "ota_uuid_valid(request_id, TRUE)" not in control


def test_endpoint_bound_and_result_schema_are_consistent():
    model = source(PROVISIONING, "include/edgeguard_provisioning/model.h")
    status = source(PROVISIONING, "src/status.c")
    operations = source(PROVISIONING, "src/operations.c")
    config = source(AGENT, "include/edgeguard_ota/config.h")
    runtime = source(AGENT, "src/runtime_endpoint.c")
    endpoint_doc = source(REPO, "docs/R5/07_ENDPOINT_CONFIG_SPEC.md")
    assert "EGP_ENDPOINT_MAX_BYTES 512u" in model
    assert "OTA_ENDPOINT_MAX_BYTES 512u" in config
    assert "G_STATIC_ASSERT(OTA_ENDPOINT_MAX_BYTES == EGP_ENDPOINT_MAX_BYTES)" in runtime
    assert "strlen(effective_endpoint) <= EGP_ENDPOINT_MAX_BYTES" in status
    assert 'ADD_STRING(builder, "effective_endpoint_source", status->endpoint_source)' in status
    assert 'ADD_STRING(builder, "runtime_config_error", "ENDPOINT_CONFIG_INVALID")' in status
    assert 'ADD_STRING("base_url", endpoint)' in operations
    assert 'json_builder_set_member_name(builder, "generation")' in operations
    assert "maximum serialized base URL length is 512 bytes" in endpoint_doc


def test_r5_references_real_frozen_r4_package():
    for number in range(1, 7):
        assert list((REPO / "docs/R4").glob(f"{number:02d}_*.md"))
    readme = source(REPO, "docs/R5/00_README.md")
    threads = source(REPO, "docs/R5/12_CODEX_THREADS.md")
    assert "no `docs/R4/` directory" not in readme
    assert "no `docs/R4/` directory" not in threads
    assert "01_FINAL_BASELINE.md" in readme and "06_EVIDENCE_REFERENCES.md" in readme


def test_no_new_forbidden_control_paths():
    changed_surfaces = "\n".join(
        source(PROVISIONING, relative)
        for relative in ("src/main.c", "src/operations.c", "src/connman.c",
                         "src/bluez.c", "src/security.c", "src/status.c")
    )
    for forbidden in ("connmanctl", "wpa_cli", "rauc install", "reboot(",
                      "/dev/mmcblk"):
        assert forbidden not in changed_surfaces
