# EdgeGuard Remote OTA — R2 Codex Execution Package

## Stage

R2 — Remote OTA Server MVP

## Objective

Implement and host-verify the server side only:

- `GET /manifest.json`
- `GET /update.raucb`
- `POST /device/report`
- SHA256/size metadata generation
- single-byte-range artifact download support
- structured server logging
- host tests
- VM deployment assets/documentation

This package does **not** authorize any OTA Agent, RAUC installation, A/B switching, bootloader, rollback, health-check, BLE, cloud, MQTT, Kubernetes, CDN, or production authentication work.

## Fixed environment

- Ubuntu VM: Ubuntu 20.04
- Python: 3.8.10
- init/service manager: systemd 245
- Internet-facing VM NIC: `ens33`
- development/NFS NIC: `ens34`
- R2 Wi-Fi/LAN validation NIC: `ens38`
- observed `ens38` IPv4 during planning: `10.119.65.50/24`
- RK3588 and `ens38` are connected to the same Wi-Fi network
- during Wi-Fi target validation, the `ens34` development/NFS path must not be used as a substitute path
- artifact layout:
  `artifacts/versions/1.1.0/update.raucb`
- device reports require `device_id`
- framework: FastAPI

The observed `10.119.65.50` address is DHCP-derived. Do not hard-code it into application logic or the manifest. Bind the service to `0.0.0.0:8000`; obtain the current `ens38` address before target validation.

## Required Codex output

Codex should create a server repository containing, at minimum:

```text
app/
tests/
scripts/
deploy/
artifacts/
requirements.txt
README.md
```

The implementation must include host tests and deployment instructions.

## Required completion report

At the end of Codex work, return:

```text
CODEX_REPORT/
  SUMMARY.md
  FILES_CHANGED.md
  HOST_TEST_RESULTS.md
  DEPENDENCY_RESOLUTION.md
  OPEN_ISSUES.md
```

Claims must use the correct evidence level.

Codex may claim:

- `CODEX VERIFIED`
- `HOST VERIFIED`

Codex must not claim:

- `TARGET VERIFIED`
- `HARDWARE VERIFIED`
- RK3588 OTA success
- RAUC install success

## Recommended execution

Use **2 implementation threads**, with controlled sequencing:

1. Thread 1 — server implementation and dependency baseline.
2. Thread 2 — tests, scripts, deployment assets, and documentation.

Thread 2 may begin after Thread 1 has frozen the endpoint models and project layout. Avoid concurrent edits to the same files. A final integration pass is performed in the parent/main Codex session; it is not a third implementation thread.

See `05_CODEX_THREADS.md`.
