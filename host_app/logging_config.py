"""Small JSON logging setup suitable for stdout/journald capture."""

import json
import logging
import sys
from datetime import datetime, timezone
from typing import Any, Dict


STANDARD_LOG_RECORD_FIELDS = frozenset(
    logging.LogRecord("", 0, "", 0, "", (), None).__dict__.keys()
)


class JsonFormatter(logging.Formatter):
    """Serialize standard log data and explicitly supplied context as JSON."""

    def format(self, record: logging.LogRecord) -> str:
        payload: Dict[str, Any] = {
            "timestamp": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
            "level": record.levelname,
            "event": record.getMessage(),
        }
        for key, value in record.__dict__.items():
            if key not in STANDARD_LOG_RECORD_FIELDS and not key.startswith("_"):
                payload[key] = value
        if record.exc_info:
            payload["exception"] = self.formatException(record.exc_info)
        return json.dumps(payload, ensure_ascii=True, separators=(",", ":"), default=str)


def configure_logging() -> logging.Logger:
    logger = logging.getLogger("edgeguard_ota")
    logger.setLevel(logging.INFO)
    logger.propagate = False

    if not any(getattr(handler, "_edgeguard_json_handler", False) for handler in logger.handlers):
        handler = logging.StreamHandler(sys.stdout)
        handler.setFormatter(JsonFormatter())
        handler._edgeguard_json_handler = True  # type: ignore[attr-defined]
        logger.addHandler(handler)

    return logger

