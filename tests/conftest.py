"""Shared isolated application fixtures for R2 host tests."""

import io
import logging
from pathlib import Path
from typing import Dict, Iterator

import pytest
from fastapi.testclient import TestClient

from host_app.config import Settings
from host_app.main import LOGGER, create_app


@pytest.fixture
def artifact_bytes() -> bytes:
    return bytes(range(256)) * 32


@pytest.fixture
def test_context(tmp_path: Path, artifact_bytes: bytes) -> Iterator[Dict[str, object]]:
    artifact_path = tmp_path / "artifacts" / "versions" / "1.1.0" / "update.raucb"
    artifact_path.parent.mkdir(parents=True)
    artifact_path.write_bytes(artifact_bytes)
    settings = Settings(artifact_path=artifact_path, stream_chunk_size=257)
    application = create_app(settings)
    with TestClient(application) as client:
        yield {
            "app": application,
            "client": client,
            "settings": settings,
            "artifact_path": artifact_path,
            "artifact_bytes": artifact_bytes,
        }


@pytest.fixture
def json_log_stream() -> Iterator[io.StringIO]:
    stream = io.StringIO()
    handler = logging.StreamHandler(stream)
    handler.setFormatter(LOGGER.handlers[0].formatter)
    LOGGER.addHandler(handler)
    try:
        yield stream
    finally:
        LOGGER.removeHandler(handler)
        handler.close()
