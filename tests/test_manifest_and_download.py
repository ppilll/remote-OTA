"""Manifest, integrity, full-download, missing-artifact, and concurrency tests."""

import hashlib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Dict

from fastapi.testclient import TestClient


EXPECTED_MANIFEST_FIELDS = {
    "schema_version",
    "device_compatible",
    "version",
    "build_id",
    "artifact_url",
    "sha256",
    "size",
    "mandatory",
}


def test_manifest_schema_and_real_artifact_metadata(test_context: Dict[str, object]) -> None:
    client = test_context["client"]
    artifact_path = test_context["artifact_path"]
    assert isinstance(client, TestClient)
    assert isinstance(artifact_path, Path)

    response = client.get("/manifest.json")
    assert response.status_code == 200
    manifest = response.json()

    assert set(manifest) == EXPECTED_MANIFEST_FIELDS
    assert manifest["schema_version"] == 1
    assert manifest["device_compatible"] == "atk-dlrk3588"
    assert manifest["version"] == "1.1.0"
    assert manifest["build_id"] == "r2-dummy-1.1.0"
    assert manifest["artifact_url"] == "/update.raucb"
    assert len(manifest["sha256"]) == 64
    assert manifest["sha256"] == manifest["sha256"].lower()
    assert all(character in "0123456789abcdef" for character in manifest["sha256"])
    assert isinstance(manifest["size"], int) and manifest["size"] > 0
    assert isinstance(manifest["mandatory"], bool)

    source = artifact_path.read_bytes()
    assert manifest["size"] == len(source) == artifact_path.stat().st_size
    assert manifest["sha256"] == hashlib.sha256(source).hexdigest()


def test_full_download_matches_source_and_manifest(test_context: Dict[str, object]) -> None:
    client = test_context["client"]
    artifact_bytes = test_context["artifact_bytes"]
    assert isinstance(client, TestClient)
    assert isinstance(artifact_bytes, bytes)

    manifest = client.get("/manifest.json").json()
    response = client.get("/update.raucb")

    assert response.status_code == 200
    assert response.headers["content-type"] == "application/octet-stream"
    assert response.headers["content-length"] == str(len(artifact_bytes))
    assert response.headers["accept-ranges"] == "bytes"
    assert response.content == artifact_bytes
    assert hashlib.sha256(response.content).hexdigest() == manifest["sha256"]


def test_missing_artifact_has_contract_errors(test_context: Dict[str, object]) -> None:
    client = test_context["client"]
    artifact_path = test_context["artifact_path"]
    artifact_bytes = test_context["artifact_bytes"]
    assert isinstance(client, TestClient)
    assert isinstance(artifact_path, Path)
    assert isinstance(artifact_bytes, bytes)

    artifact_path.unlink()
    try:
        manifest_response = client.get("/manifest.json")
        artifact_response = client.get("/update.raucb")
        assert manifest_response.status_code == 503
        assert manifest_response.json() == {
            "detail": "active release is unavailable or inconsistent"
        }
        assert artifact_response.status_code == 404
        assert artifact_response.json() == {"detail": "artifact not found"}
    finally:
        artifact_path.write_bytes(artifact_bytes)


def test_at_least_ten_concurrent_requests_are_consistent(
    test_context: Dict[str, object]
) -> None:
    client = test_context["client"]
    artifact_bytes = test_context["artifact_bytes"]
    assert isinstance(client, TestClient)
    assert isinstance(artifact_bytes, bytes)
    expected_digest = hashlib.sha256(artifact_bytes).hexdigest()

    def request_and_validate(index: int) -> str:
        if index % 2 == 0:
            response = client.get("/manifest.json")
            assert response.status_code == 200
            assert response.json()["sha256"] == expected_digest
            return "manifest"
        response = client.get("/update.raucb")
        assert response.status_code == 200
        assert response.content == artifact_bytes
        assert hashlib.sha256(response.content).hexdigest() == expected_digest
        return "artifact"

    request_count = 20
    with ThreadPoolExecutor(max_workers=10) as executor:
        results = list(executor.map(request_and_validate, range(request_count)))

    assert len(results) == request_count
    assert results.count("manifest") == 10
    assert results.count("artifact") == 10
