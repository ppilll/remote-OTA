"""Device-report validation and structured-log safety tests."""

import json
from typing import Dict, List

import pytest
from fastapi.testclient import TestClient


VALID_REPORT = {
    "device_id": "rk3588-001",
    "current_version": "1.0.0",
    "status": "downloaded",
    "timestamp": "2026-09-02T04:00:00Z",
}

EXTENDED_REPORT = {
    **VALID_REPORT,
    "status": "reboot_pending",
    "attempt_id": "12345678-1234-4234-8234-123456789abc",
    "target_version": "1.1.0",
    "build_id": "rk3588-r4-1.1.0-001",
    "agent_state": "REBOOT_PENDING",
    "error_code": "NONE",
    "timestamp_valid": False,
    "uptime_ms": 123456,
}

LEGACY_STATUSES = ["idle", "checking", "downloading", "downloaded", "error"]
EXTENDED_STATUSES = ["rollback", "success", "reboot_pending", "installing"]
AGENT_STATES = [
    "IDLE",
    "CHECK_NETWORK",
    "CHECK_UPDATE",
    "PRECHECK",
    "DOWNLOADING",
    "VERIFY_DOWNLOAD",
    "RAUC_VERIFY",
    "INSTALLING",
    "REBOOT_PENDING",
    "BOOT_NEW_SLOT",
    "HEALTH_CHECK",
    "MARK_GOOD",
    "REPORT_SUCCESS",
    "ROLLBACK",
    "ERROR",
]
AGENT_ERROR_CODES = [
    "NONE",
    "CONFIG_INVALID",
    "IDENTITY_INVALID",
    "IDENTITY_AMBIGUOUS",
    "LOCAL_RELEASE_INVALID",
    "MANIFEST_HTTP",
    "MANIFEST_INVALID",
    "DEVICE_COMPAT_MISMATCH",
    "VERSION_MALFORMED",
    "VERSION_COLLISION",
    "DOWNGRADE_REJECTED",
    "DOWNLOAD_HTTP",
    "DOWNLOAD_RANGE_MISMATCH",
    "DOWNLOAD_DISK_SPACE",
    "DOWNLOAD_SIZE_MISMATCH",
    "DOWNLOAD_HASH_MISMATCH",
    "RAUC_VERIFY_FAILED",
    "RAUC_COMPAT_MISMATCH",
    "RAUC_IDENTITY_MISMATCH",
    "RAUC_INSTALL_FAILED",
    "RAUC_MARK_GOOD_FAILED",
    "RAUC_MARK_BAD_FAILED",
    "HEALTH_FAILED",
    "REBOOT_CONTEXT_INVALID",
    "REBOOT_FAILED",
    "PERSISTENCE_FAILED",
    "REPORT_FAILED",
    "ILLEGAL_TRANSITION",
    "TIME_SOURCE_FAILED",
]


def _log_objects(stream_text: str) -> List[dict]:
    return [json.loads(line) for line in stream_text.splitlines() if line.strip()]


def test_valid_report_returns_204_and_logs_structured_fields(
    test_context: Dict[str, object], json_log_stream
) -> None:
    client = test_context["client"]
    assert isinstance(client, TestClient)

    response = client.post("/device/report", json=VALID_REPORT)

    assert response.status_code == 204
    assert response.content == b""
    logs = _log_objects(json_log_stream.getvalue())
    accepted = next(item for item in logs if item["event"] == "device_report_accepted")
    completed = next(
        item
        for item in logs
        if item["event"] == "request_completed" and item["path"] == "/device/report"
    )

    assert accepted["device_id"] == "rk3588-001"
    assert accepted["device_status"] == "downloaded"
    assert accepted["method"] == "POST"
    assert accepted["path"] == "/device/report"
    assert accepted["response_code"] == 204
    assert accepted["client_ip"]
    assert accepted["timestamp"].endswith("Z")
    assert "attempt_id" not in accepted
    assert "target_version" not in accepted
    assert "build_id" not in accepted
    assert "agent_state" not in accepted
    assert "error_code" not in accepted
    assert completed["response_code"] == 204


def test_extended_report_returns_204_and_logs_correlation_fields(
    test_context: Dict[str, object], json_log_stream
) -> None:
    client = test_context["client"]
    assert isinstance(client, TestClient)

    response = client.post("/device/report", json=EXTENDED_REPORT)

    assert response.status_code == 204
    assert response.content == b""
    logs = _log_objects(json_log_stream.getvalue())
    accepted = next(item for item in logs if item["event"] == "device_report_accepted")
    assert accepted["attempt_id"] == EXTENDED_REPORT["attempt_id"]
    assert accepted["target_version"] == "1.1.0"
    assert accepted["build_id"] == "rk3588-r4-1.1.0-001"
    assert accepted["agent_state"] == "REBOOT_PENDING"
    assert accepted["error_code"] == "NONE"


def test_valid_subset_of_extended_fields_is_accepted(
    test_context: Dict[str, object]
) -> None:
    client = test_context["client"]
    assert isinstance(client, TestClient)
    response = client.post(
        "/device/report",
        json={**VALID_REPORT, "attempt_id": "", "uptime_ms": 2**63 - 1},
    )
    assert response.status_code == 204


@pytest.mark.parametrize("status", LEGACY_STATUSES + EXTENDED_STATUSES)
def test_agent_emitted_status_values_are_accepted(
    test_context: Dict[str, object], status: str
) -> None:
    client = test_context["client"]
    assert isinstance(client, TestClient)
    response = client.post("/device/report", json={**VALID_REPORT, "status": status})
    assert response.status_code == 204


@pytest.mark.parametrize("agent_state", AGENT_STATES)
def test_agent_emitted_state_values_are_accepted(
    test_context: Dict[str, object], agent_state: str
) -> None:
    client = test_context["client"]
    assert isinstance(client, TestClient)
    response = client.post(
        "/device/report", json={**VALID_REPORT, "agent_state": agent_state}
    )
    assert response.status_code == 204


@pytest.mark.parametrize("error_code", AGENT_ERROR_CODES)
def test_agent_emitted_error_codes_are_accepted(
    test_context: Dict[str, object], error_code: str
) -> None:
    client = test_context["client"]
    assert isinstance(client, TestClient)
    response = client.post(
        "/device/report", json={**VALID_REPORT, "error_code": error_code}
    )
    assert response.status_code == 204


@pytest.mark.parametrize(
    ("payload", "expected_status"),
    [
        ({"current_version": "1.0.0", "status": "idle", "timestamp": "2026-09-02T04:00:00Z"}, 422),
        ({**VALID_REPORT, "device_id": ""}, 422),
        ({**VALID_REPORT, "device_id": "   "}, 422),
        ({**VALID_REPORT, "current_version": ""}, 422),
        ({**VALID_REPORT, "status": "installed"}, 422),
        ({**VALID_REPORT, "timestamp": "not-a-timestamp"}, 422),
        ({**VALID_REPORT, "timestamp": "2026-09-02T04:00:00"}, 422),
    ],
)
def test_invalid_reports_return_422(
    test_context: Dict[str, object], payload: dict, expected_status: int
) -> None:
    client = test_context["client"]
    assert isinstance(client, TestClient)
    response = client.post("/device/report", json=payload)
    assert response.status_code == expected_status
    assert "detail" in response.json()


@pytest.mark.parametrize(
    ("field", "invalid_value"),
    [
        ("attempt_id", 7),
        ("attempt_id", "a" * 37),
        ("attempt_id", "测" * 13),
        ("target_version", 7),
        ("target_version", "v" * 256),
        ("target_version", "1.2.3\n"),
        ("build_id", 7),
        ("build_id", "b" * 256),
        ("agent_state", 7),
        ("agent_state", "UNKNOWN"),
        ("error_code", 7),
        ("error_code", "UNKNOWN"),
        ("timestamp_valid", 0),
        ("timestamp_valid", "false"),
        ("uptime_ms", True),
        ("uptime_ms", "123"),
        ("uptime_ms", -1),
        ("uptime_ms", 2**63),
    ],
)
def test_malformed_extended_fields_return_422(
    test_context: Dict[str, object], field: str, invalid_value
) -> None:
    client = test_context["client"]
    assert isinstance(client, TestClient)
    response = client.post(
        "/device/report", json={**VALID_REPORT, field: invalid_value}
    )
    assert response.status_code == 422
    assert "detail" in response.json()


@pytest.mark.parametrize(
    "field",
    [
        "attempt_id",
        "target_version",
        "build_id",
        "agent_state",
        "error_code",
        "timestamp_valid",
        "uptime_ms",
    ],
)
def test_supplied_extended_fields_reject_null(
    test_context: Dict[str, object], field: str
) -> None:
    client = test_context["client"]
    assert isinstance(client, TestClient)
    response = client.post("/device/report", json={**VALID_REPORT, field: None})
    assert response.status_code == 422


def test_malformed_json_returns_400(test_context: Dict[str, object]) -> None:
    client = test_context["client"]
    assert isinstance(client, TestClient)
    response = client.post(
        "/device/report",
        content=b'{"device_id":',
        headers={"Content-Type": "application/json"},
    )
    assert response.status_code == 400
    assert "detail" in response.json()


def test_logs_include_artifact_access_and_errors_without_request_secrets(
    test_context: Dict[str, object], json_log_stream
) -> None:
    client = test_context["client"]
    assert isinstance(client, TestClient)
    secret_marker = "DO-NOT-LOG-THIS-TOKEN"

    download_response = client.get("/update.raucb", headers={"Range": "bytes=0-15"})
    invalid_response = client.post(
        "/device/report",
        json={**VALID_REPORT, "token": secret_marker},
    )

    assert download_response.status_code == 206
    assert invalid_response.status_code == 422
    log_text = json_log_stream.getvalue()
    logs = _log_objects(log_text)
    download = next(item for item in logs if item["event"] == "artifact_download_started")
    validation = next(item for item in logs if item["event"] == "request_validation_failed")

    assert download["artifact_access"] == "partial"
    assert download["response_code"] == 206
    assert download["client_ip"]
    assert validation["error_reason"] == "schema_validation_failed"
    assert validation["response_code"] == 422
    assert secret_marker not in log_text
