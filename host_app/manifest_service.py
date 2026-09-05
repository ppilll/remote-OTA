"""Artifact inspection and immutable manifest metadata creation."""

import hashlib
from pathlib import Path

from app.config import Settings
from app.models import Manifest


HASH_CHUNK_SIZE = 1024 * 1024


class ArtifactConfigurationError(RuntimeError):
    """The configured active artifact cannot be used at startup."""


class ReleaseStateError(RuntimeError):
    """The active artifact changed after its metadata was cached."""


def calculate_manifest(settings: Settings) -> Manifest:
    path = settings.artifact_path
    try:
        if not path.is_file():
            raise ArtifactConfigurationError(
                "configured artifact is not a regular file: {0}".format(path)
            )

        digest = hashlib.sha256()
        size = 0
        with path.open("rb") as artifact:
            while True:
                chunk = artifact.read(HASH_CHUNK_SIZE)
                if not chunk:
                    break
                digest.update(chunk)
                size += len(chunk)
    except ArtifactConfigurationError:
        raise
    except OSError as exc:
        raise ArtifactConfigurationError(
            "configured artifact is not readable: {0}".format(path)
        ) from exc

    return Manifest(
        schema_version=settings.schema_version,
        device_compatible=settings.device_compatible,
        version=settings.version,
        build_id=settings.build_id,
        artifact_url=settings.artifact_url,
        sha256=digest.hexdigest(),
        size=size,
        mandatory=settings.mandatory,
    )


def verify_cached_manifest(path: Path, manifest: Manifest) -> None:
    try:
        stat_result = path.stat()
    except OSError as exc:
        raise ReleaseStateError("artifact is unavailable") from exc

    if not path.is_file():
        raise ReleaseStateError("artifact is not a regular file")
    if stat_result.st_size != manifest.size:
        raise ReleaseStateError("artifact size changed after startup")

