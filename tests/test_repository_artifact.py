"""Verify the configured repository artifact independently of app hashing code."""

import hashlib

from host_app.config import SETTINGS
from host_app.manifest_service import calculate_manifest


def test_repository_artifact_exists_and_matches_generated_manifest() -> None:
    path = SETTINGS.artifact_path
    assert path.is_file(), "run scripts/generate_dummy_artifact.py first"
    source = path.read_bytes()
    assert len(source) > 0

    manifest = calculate_manifest(SETTINGS)
    assert manifest.size == path.stat().st_size == len(source)
    assert manifest.sha256 == hashlib.sha256(source).hexdigest()
