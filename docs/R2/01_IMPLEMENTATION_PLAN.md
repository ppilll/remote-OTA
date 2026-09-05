# R2 Implementation Plan

## 1. Scope

Implement the Remote OTA Server MVP only.

### In scope

- FastAPI application
- release manifest generation
- dummy artifact serving
- SHA256 and byte-size metadata
- single-range HTTP downloads
- device report validation
- structured logs
- host tests
- systemd deployment template/guide
- target-validation helper commands/documentation

### Out of scope

- Remote OTA Agent
- RAUC install or bundle verification
- A/B slots
- bootloader
- rollback
- health check
- BLE
- cloud
- database cluster
- multi-device fleet management
- authentication/token issuance
- production TLS/PKI
- MQTT
- Kubernetes/CDN

## 2. Repository target layout

```text
edgeguard-ota-server/
├── app/
│   ├── __init__.py
│   ├── main.py
│   ├── config.py
│   ├── models.py
│   ├── manifest_service.py
│   ├── artifact_service.py
│   └── logging_config.py
├── artifacts/
│   └── versions/
│       └── 1.1.0/
│           └── update.raucb
├── tests/
│   ├── test_manifest.py
│   ├── test_artifact.py
│   ├── test_report.py
│   ├── test_errors.py
│   └── test_concurrency.py
├── scripts/
│   ├── create_dummy_artifact.py
│   ├── host_smoke_test.sh
│   └── target_validation_commands.md
├── deploy/
│   ├── edgeguard-ota.service
│   └── README.md
├── requirements.txt
├── README.md
└── CODEX_REPORT/
```

Exact file names may vary slightly if justified, but module ownership and scope must remain clear.

## 3. Release model

R2 has one configured active release:

- compatible: `atk-dlrk3588`
- version: `1.1.0`
- artifact path: `artifacts/versions/1.1.0/update.raucb`
- public artifact URL in manifest: `/update.raucb`

The URL is deliberately relative. The VM address is DHCP-derived and must not be embedded in release metadata.

## 4. Manifest lifecycle

At application startup:

1. Resolve the configured active artifact.
2. Verify the path is a regular readable file.
3. Calculate SHA256.
4. Calculate byte size.
5. Cache immutable release metadata for the process lifetime.

R2 assumption: the active artifact is immutable while the service is running.

If the release is replaced, restart the service so metadata is recalculated.

Before returning the manifest, verify the artifact still exists and its size is unchanged. If release state is inconsistent, return service-unavailable behavior instead of advertising stale metadata.

## 5. Artifact serving

The endpoint is fixed:

`GET /update.raucb`

The internal storage remains versioned.

R2 must support:

- normal full download: `200 OK`
- one `bytes=` range: `206 Partial Content`
- correct `Content-Range`
- correct `Content-Length`
- `Accept-Ranges: bytes`
- unsatisfiable range: `416 Range Not Satisfiable`

R2 must **not** rely on the legacy Starlette `FileResponse` multi-range parser because the Python 3.8-compatible Starlette line contains a known Range-header DoS issue.

Implement a deliberately small single-range parser:

- accept zero or one range only
- reject comma-separated multi-range requests
- enforce numeric bounds
- never allocate memory proportional to arbitrary range-header complexity
- stream file chunks rather than loading the artifact into memory

Multi-range is a non-goal for R2.

## 6. Device report

`POST /device/report`

The endpoint validates input and writes a structured report event to the application log/journal. No database is required.

Successful validation returns:

`204 No Content`

R2 report storage is observability/logging, not fleet state management.

## 7. Logging

Emit structured logs to stdout/stderr for journald capture.

Minimum fields when applicable:

- timestamp
- level
- client IP
- device_id
- method
- path
- response code
- artifact version
- artifact access
- error reason

Do not log:

- Wi-Fi passwords
- authorization secrets
- future bearer tokens
- private keys
- complete arbitrary request bodies

## 8. Dependency policy

Python 3.8.10 is a hard constraint.

Use versions compatible with Python 3.8 and pin them in `requirements.txt`.

See `06_DEPENDENCY_POLICY.md`.

## 9. Implementation sequence

### Step 1
Create dependency pins and project skeleton.

### Step 2
Implement data models and configuration.

### Step 3
Implement startup artifact metadata calculation.

### Step 4
Implement `GET /manifest.json`.

### Step 5
Implement custom safe single-range artifact response.

### Step 6
Implement `POST /device/report`.

### Step 7
Implement structured request/error logging.

### Step 8
Run host unit/API tests.

### Step 9
Create deployment/systemd assets and smoke scripts.

### Step 10
Run final host integration test and produce `CODEX_REPORT/`.

## 10. Acceptance gate for Codex

Codex work is complete only if:

- dependency installation works under Python 3.8.10
- all automated host tests pass
- a dummy artifact can be served and hashed
- manifest hash/size match the actual artifact
- valid single-range requests work
- invalid/multi-range requests fail safely
- device reports validate and are logged
- no out-of-scope OTA/device functionality was added
- evidence is labeled HOST/CODEX only
