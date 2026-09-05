"""Defined artifact read-failure behavior."""

import json
from pathlib import Path
from typing import Dict

from fastapi.testclient import TestClient


def test_artifact_read_failure_returns_defined_500_without_traceback(
    test_context: Dict[str, object], json_log_stream, monkeypatch
) -> None:
    client = test_context["client"]
    artifact_path = test_context["artifact_path"]
    assert isinstance(client, TestClient)
    assert isinstance(artifact_path, Path)
    original_open = Path.open

    def deny_artifact_read(path: Path, *args, **kwargs):
        if path == artifact_path:
            raise PermissionError("injected host-test read denial")
        return original_open(path, *args, **kwargs)

    monkeypatch.setattr(Path, "open", deny_artifact_read)
    response = client.get("/update.raucb")

    assert response.status_code == 500
    assert response.json() == {"detail": "artifact is not readable"}
    assert "traceback" not in response.text.lower()
    logs = [
        json.loads(line)
        for line in json_log_stream.getvalue().splitlines()
        if line.strip()
    ]
    failure = next(item for item in logs if item["event"] == "artifact_stat_failed")
    assert failure["artifact_access"] == "failed"
    assert failure["error_reason"] == "artifact_stat_error"
    assert "PermissionError" in failure["exception"]
