"""Frozen request and response schemas for the R2 API."""

from datetime import datetime
from enum import Enum

from pydantic import BaseModel, ConfigDict, Field, field_validator


class Manifest(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)

    schema_version: int
    device_compatible: str
    version: str
    build_id: str
    artifact_url: str
    sha256: str = Field(pattern=r"^[0-9a-f]{64}$")
    size: int = Field(ge=0)
    mandatory: bool


class DeviceStatus(str, Enum):
    idle = "idle"
    checking = "checking"
    downloading = "downloading"
    downloaded = "downloaded"
    error = "error"


class DeviceReport(BaseModel):
    model_config = ConfigDict(extra="forbid")

    device_id: str
    current_version: str
    status: DeviceStatus
    timestamp: datetime

    @field_validator("device_id", "current_version")
    @classmethod
    def required_non_empty_string(cls, value: str) -> str:
        if not value.strip():
            raise ValueError("must not be empty")
        return value

    @field_validator("timestamp")
    @classmethod
    def timestamp_must_have_timezone(cls, value: datetime) -> datetime:
        if value.tzinfo is None or value.utcoffset() is None:
            raise ValueError("timezone is required")
        return value

