"""Positive and defensive single-range behavior tests."""

from typing import Dict

import pytest
from fastapi.testclient import TestClient


@pytest.mark.parametrize(
    ("range_header", "expected_start", "expected_end"),
    [
        ("bytes=0-1023", 0, 1023),
        ("bytes=2048-3071", 2048, 3071),
        ("bytes=8191-8191", 8191, 8191),
        ("bytes=-64", 8128, 8191),
        ("bytes=8128-", 8128, 8191),
    ],
)
def test_single_range_206(
    test_context: Dict[str, object],
    range_header: str,
    expected_start: int,
    expected_end: int,
) -> None:
    client = test_context["client"]
    artifact_bytes = test_context["artifact_bytes"]
    assert isinstance(client, TestClient)
    assert isinstance(artifact_bytes, bytes)

    response = client.get("/update.raucb", headers={"Range": range_header})
    expected = artifact_bytes[expected_start : expected_end + 1]

    assert response.status_code == 206
    assert response.headers["content-type"] == "application/octet-stream"
    assert response.headers["accept-ranges"] == "bytes"
    assert response.headers["content-range"] == "bytes {0}-{1}/{2}".format(
        expected_start, expected_end, len(artifact_bytes)
    )
    assert response.headers["content-length"] == str(len(expected))
    assert response.content == expected


@pytest.mark.parametrize(
    "range_header",
    [
        "items=0-1",
        "bytes=one-two",
        "bytes=0-1,4-5",
        "bytes=0 -1",
        "bytes=",
        "bytes=1-2-3",
        "bytes=111111111111111111111-",
        "bytes=" + ("9" * 129),
    ],
)
def test_malformed_or_unsupported_ranges_return_400(
    test_context: Dict[str, object], range_header: str
) -> None:
    client = test_context["client"]
    assert isinstance(client, TestClient)

    response = client.get("/update.raucb", headers={"Range": range_header})

    assert response.status_code == 400
    assert response.headers["accept-ranges"] == "bytes"
    assert "detail" in response.json()


@pytest.mark.parametrize(
    "range_header",
    [
        "bytes=8192-8193",
        "bytes=100-99",
        "bytes=-0",
    ],
)
def test_unsatisfiable_ranges_return_416(
    test_context: Dict[str, object], range_header: str
) -> None:
    client = test_context["client"]
    artifact_bytes = test_context["artifact_bytes"]
    assert isinstance(client, TestClient)
    assert isinstance(artifact_bytes, bytes)

    response = client.get("/update.raucb", headers={"Range": range_header})

    assert response.status_code == 416
    assert response.headers["accept-ranges"] == "bytes"
    assert response.headers["content-range"] == "bytes */{0}".format(len(artifact_bytes))
    assert "detail" in response.json()
