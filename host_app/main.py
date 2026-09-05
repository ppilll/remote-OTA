"""FastAPI entry point for the EdgeGuard R2 OTA server."""

import logging
from contextlib import asynccontextmanager
from pathlib import Path
from typing import AsyncIterator, Optional

from fastapi import FastAPI, Request, Response
from fastapi.encoders import jsonable_encoder
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse, StreamingResponse
from starlette.exceptions import HTTPException as StarletteHTTPException

from app.artifact_service import (
    MalformedRangeError,
    UnsatisfiableRangeError,
    select_range,
    stream_file_region,
)
from app.config import SETTINGS, Settings
from app.logging_config import configure_logging
from app.manifest_service import (
    ReleaseStateError,
    calculate_manifest,
    verify_cached_manifest,
)
from app.models import DeviceReport, Manifest


LOGGER = configure_logging()


def _client_ip(request: Request) -> Optional[str]:
    return request.client.host if request.client is not None else None


def _error_response(status_code: int, detail: str, headers: Optional[dict] = None) -> JSONResponse:
    return JSONResponse(status_code=status_code, content={"detail": detail}, headers=headers)


def create_app(settings: Settings = SETTINGS) -> FastAPI:
    @asynccontextmanager
    async def lifespan(application: FastAPI) -> AsyncIterator[None]:
        manifest = calculate_manifest(settings)
        application.state.manifest = manifest
        LOGGER.info(
            "release_metadata_loaded",
            extra={
                "artifact_version": manifest.version,
                "artifact_size": manifest.size,
                "artifact_sha256": manifest.sha256,
            },
        )
        yield

    application = FastAPI(title="EdgeGuard R2 OTA Server", version="2.0", lifespan=lifespan)
    application.state.settings = settings
    application.state.manifest = None

    @application.middleware("http")
    async def structured_request_log(request: Request, call_next):
        try:
            response = await call_next(request)
        except Exception:
            LOGGER.exception(
                "request_failed",
                extra={
                    "client_ip": _client_ip(request),
                    "method": request.method,
                    "path": request.url.path,
                    "response_code": 500,
                    "error_reason": "unexpected_server_error",
                },
            )
            raise

        LOGGER.info(
            "request_completed",
            extra={
                "client_ip": _client_ip(request),
                "method": request.method,
                "path": request.url.path,
                "response_code": response.status_code,
            },
        )
        return response

    @application.exception_handler(RequestValidationError)
    async def validation_exception_handler(request: Request, exc: RequestValidationError):
        errors = exc.errors()
        malformed_json = any(error.get("type") == "json_invalid" for error in errors)
        status_code = 400 if malformed_json else 422
        reason = "malformed_json" if malformed_json else "schema_validation_failed"
        LOGGER.warning(
            "request_validation_failed",
            extra={
                "client_ip": _client_ip(request),
                "method": request.method,
                "path": request.url.path,
                "response_code": status_code,
                "error_reason": reason,
            },
        )
        return JSONResponse(status_code=status_code, content={"detail": jsonable_encoder(errors)})

    @application.exception_handler(StarletteHTTPException)
    async def http_exception_handler(request: Request, exc: StarletteHTTPException):
        LOGGER.warning(
            "http_error",
            extra={
                "client_ip": _client_ip(request),
                "method": request.method,
                "path": request.url.path,
                "response_code": exc.status_code,
                "error_reason": str(exc.detail),
            },
        )
        return JSONResponse(
            status_code=exc.status_code,
            content={"detail": exc.detail},
            headers=exc.headers,
        )

    @application.get(
        "/manifest.json",
        response_model=Manifest,
        responses={204: {"description": "No active release"}, 503: {"description": "Release inconsistent"}},
    )
    async def get_manifest(request: Request):
        manifest: Optional[Manifest] = request.app.state.manifest
        if manifest is None:
            return Response(status_code=204)
        try:
            verify_cached_manifest(settings.artifact_path, manifest)
        except ReleaseStateError as exc:
            LOGGER.error(
                "release_state_inconsistent",
                extra={
                    "artifact_version": settings.version,
                    "error_reason": str(exc),
                },
            )
            return _error_response(503, "active release is unavailable or inconsistent")
        return manifest

    @application.get("/update.raucb", response_class=StreamingResponse)
    async def get_artifact(request: Request):
        path: Path = settings.artifact_path
        try:
            stat_result = path.stat()
            if not path.is_file():
                LOGGER.warning(
                    "artifact_not_found",
                    extra={
                        "artifact_version": settings.version,
                        "artifact_access": "failed",
                        "error_reason": "artifact_not_found",
                    },
                )
                return _error_response(404, "artifact not found")
            # Detect ordinary permission/read failures before response headers are sent.
            with path.open("rb"):
                pass
            file_size = stat_result.st_size
        except FileNotFoundError:
            LOGGER.warning(
                "artifact_not_found",
                extra={
                    "artifact_version": settings.version,
                    "artifact_access": "failed",
                    "error_reason": "artifact_not_found",
                },
            )
            return _error_response(404, "artifact not found")
        except OSError:
            LOGGER.exception(
                "artifact_stat_failed",
                extra={
                    "artifact_version": settings.version,
                    "artifact_access": "failed",
                    "error_reason": "artifact_stat_error",
                },
            )
            return _error_response(500, "artifact is not readable")

        range_headers = request.headers.getlist("range")
        if len(range_headers) > 1:
            LOGGER.warning(
                "artifact_range_rejected",
                extra={
                    "artifact_version": settings.version,
                    "artifact_access": "rejected",
                    "error_reason": "multiple Range headers are not supported",
                },
            )
            return _error_response(
                400,
                "multiple Range headers are not supported",
                headers={"Accept-Ranges": "bytes"},
            )

        range_header = range_headers[0] if range_headers else None
        try:
            byte_range = select_range(
                range_header,
                file_size,
                settings.max_range_header_length,
                settings.max_range_number_digits,
            )
        except MalformedRangeError as exc:
            LOGGER.warning(
                "artifact_range_rejected",
                extra={
                    "artifact_version": settings.version,
                    "artifact_access": "rejected",
                    "error_reason": str(exc),
                },
            )
            return _error_response(400, str(exc), headers={"Accept-Ranges": "bytes"})
        except UnsatisfiableRangeError as exc:
            LOGGER.warning(
                "artifact_range_unsatisfiable",
                extra={
                    "artifact_version": settings.version,
                    "artifact_access": "rejected",
                    "error_reason": str(exc),
                },
            )
            return _error_response(
                416,
                str(exc),
                headers={
                    "Accept-Ranges": "bytes",
                    "Content-Range": "bytes */{0}".format(file_size),
                },
            )

        if byte_range is None:
            start = 0
            length = file_size
            status_code = 200
            headers = {
                "Accept-Ranges": "bytes",
                "Content-Length": str(file_size),
            }
        else:
            start = byte_range.start
            length = byte_range.length
            status_code = 206
            headers = {
                "Accept-Ranges": "bytes",
                "Content-Length": str(length),
                "Content-Range": "bytes {0}-{1}/{2}".format(
                    byte_range.start, byte_range.end, file_size
                ),
            }

        LOGGER.info(
            "artifact_download_started",
            extra={
                "client_ip": _client_ip(request),
                "artifact_version": settings.version,
                "artifact_access": "partial" if byte_range is not None else "full",
                "response_code": status_code,
            },
        )
        return StreamingResponse(
            stream_file_region(path, start, length, settings.stream_chunk_size),
            status_code=status_code,
            media_type="application/octet-stream",
            headers=headers,
        )

    @application.post("/device/report", status_code=204, response_class=Response)
    async def post_device_report(report: DeviceReport, request: Request) -> Response:
        LOGGER.info(
            "device_report_accepted",
            extra={
                "client_ip": _client_ip(request),
                "device_id": report.device_id,
                "current_version": report.current_version,
                "device_status": report.status.value,
                "device_timestamp": report.timestamp.isoformat(),
                "method": request.method,
                "path": request.url.path,
                "response_code": 204,
            },
        )
        return Response(status_code=204)

    return application


app = create_app()
