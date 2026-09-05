"""Static configuration for the single R2 release."""

from dataclasses import dataclass
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parent.parent


@dataclass(frozen=True)
class Settings:
    schema_version: int = 1
    device_compatible: str = "atk-dlrk3588"
    version: str = "1.1.0"
    build_id: str = "r2-dummy-1.1.0"
    artifact_url: str = "/update.raucb"
    artifact_path: Path = PROJECT_ROOT / "artifacts" / "versions" / "1.1.0" / "update.raucb"
    mandatory: bool = False
    stream_chunk_size: int = 64 * 1024
    max_range_header_length: int = 128
    max_range_number_digits: int = 20


SETTINGS = Settings()

