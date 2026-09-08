"""Request and response schemas for the backward-compatible R2/R4 API."""

from datetime import datetime
from enum import Enum
from typing import Optional

from typing_extensions import Annotated

from pydantic import (
    BaseModel,
    ConfigDict,
    Field,
    StrictBool,
    StrictInt,
    StrictStr,
    ValidationInfo,
    field_validator,
)


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
    rollback = "rollback"
    success = "success"
    reboot_pending = "reboot_pending"
    installing = "installing"


class AgentState(str, Enum):
    IDLE = "IDLE"
    CHECK_NETWORK = "CHECK_NETWORK"
    CHECK_UPDATE = "CHECK_UPDATE"
    PRECHECK = "PRECHECK"
    DOWNLOADING = "DOWNLOADING"
    VERIFY_DOWNLOAD = "VERIFY_DOWNLOAD"
    RAUC_VERIFY = "RAUC_VERIFY"
    INSTALLING = "INSTALLING"
    REBOOT_PENDING = "REBOOT_PENDING"
    BOOT_NEW_SLOT = "BOOT_NEW_SLOT"
    HEALTH_CHECK = "HEALTH_CHECK"
    MARK_GOOD = "MARK_GOOD"
    REPORT_SUCCESS = "REPORT_SUCCESS"
    ROLLBACK = "ROLLBACK"
    ERROR = "ERROR"


class AgentErrorCode(str, Enum):
    NONE = "NONE"
    CONFIG_INVALID = "CONFIG_INVALID"
    IDENTITY_INVALID = "IDENTITY_INVALID"
    IDENTITY_AMBIGUOUS = "IDENTITY_AMBIGUOUS"
    LOCAL_RELEASE_INVALID = "LOCAL_RELEASE_INVALID"
    MANIFEST_HTTP = "MANIFEST_HTTP"
    MANIFEST_INVALID = "MANIFEST_INVALID"
    DEVICE_COMPAT_MISMATCH = "DEVICE_COMPAT_MISMATCH"
    VERSION_MALFORMED = "VERSION_MALFORMED"
    VERSION_COLLISION = "VERSION_COLLISION"
    DOWNGRADE_REJECTED = "DOWNGRADE_REJECTED"
    DOWNLOAD_HTTP = "DOWNLOAD_HTTP"
    DOWNLOAD_RANGE_MISMATCH = "DOWNLOAD_RANGE_MISMATCH"
    DOWNLOAD_DISK_SPACE = "DOWNLOAD_DISK_SPACE"
    DOWNLOAD_SIZE_MISMATCH = "DOWNLOAD_SIZE_MISMATCH"
    DOWNLOAD_HASH_MISMATCH = "DOWNLOAD_HASH_MISMATCH"
    RAUC_VERIFY_FAILED = "RAUC_VERIFY_FAILED"
    RAUC_COMPAT_MISMATCH = "RAUC_COMPAT_MISMATCH"
    RAUC_IDENTITY_MISMATCH = "RAUC_IDENTITY_MISMATCH"
    RAUC_INSTALL_FAILED = "RAUC_INSTALL_FAILED"
    RAUC_MARK_GOOD_FAILED = "RAUC_MARK_GOOD_FAILED"
    RAUC_MARK_BAD_FAILED = "RAUC_MARK_BAD_FAILED"
    HEALTH_FAILED = "HEALTH_FAILED"
    REBOOT_CONTEXT_INVALID = "REBOOT_CONTEXT_INVALID"
    REBOOT_FAILED = "REBOOT_FAILED"
    PERSISTENCE_FAILED = "PERSISTENCE_FAILED"
    REPORT_FAILED = "REPORT_FAILED"
    ILLEGAL_TRANSITION = "ILLEGAL_TRANSITION"
    TIME_SOURCE_FAILED = "TIME_SOURCE_FAILED"


UptimeMilliseconds = Annotated[StrictInt, Field(ge=0, le=2**63 - 1)]


class DeviceReport(BaseModel):
    model_config = ConfigDict(extra="forbid")

    device_id: str
    current_version: str
    status: DeviceStatus
    timestamp: datetime
    attempt_id: Optional[StrictStr] = None
    target_version: Optional[StrictStr] = None
    build_id: Optional[StrictStr] = None
    agent_state: Optional[AgentState] = None
    error_code: Optional[AgentErrorCode] = None
    timestamp_valid: Optional[StrictBool] = None
    uptime_ms: Optional[UptimeMilliseconds] = None

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

    @field_validator(
        "attempt_id",
        "target_version",
        "build_id",
        "agent_state",
        "error_code",
        "timestamp_valid",
        "uptime_ms",
        mode="before",
    )
    @classmethod
    def supplied_optional_field_must_not_be_null(cls, value):
        if value is None:
            raise ValueError("must be omitted rather than null")
        return value

    @field_validator("attempt_id", "target_version", "build_id")
    @classmethod
    def extended_text_matches_agent_bounds(
        cls, value: Optional[str], info: ValidationInfo
    ) -> Optional[str]:
        if value is None:
            return value
        capacity = 37 if info.field_name == "attempt_id" else 256
        try:
            encoded = value.encode("utf-8")
        except UnicodeEncodeError as exc:
            raise ValueError("must be valid UTF-8") from exc
        if len(encoded) >= capacity:
            raise ValueError("exceeds Agent wire-field byte capacity")
        if any(byte < 0x20 or byte == 0x7F for byte in encoded):
            raise ValueError("must not contain ASCII control characters")
        return value
