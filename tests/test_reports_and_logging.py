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
    assert completed["response_code"] == 204


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
