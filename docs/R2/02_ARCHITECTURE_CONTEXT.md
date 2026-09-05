# R2 Architecture Context

## Project

EdgeGuard Remote OTA

## Stage

R2 — Remote OTA Server MVP

## Verified inherited baseline

Do not re-prove these R0/R1 facts inside Codex.

### Target

- Board: ATK-DLRK3588
- SoC: RK3588
- Architecture: AArch64
- Storage: eMMC

### BSP

- Buildroot: vendor 2021.11
- Kernel: 5.10.209
- U-Boot: 2017.09

### R1 Wi-Fi/network

- RTL8733BU
- target interface: `wlan0`
- ConnMan 1.40 controls networking
- wpa_supplicant 2.10
- D-Bus control
- nl80211 backend
- ConnMan-managed DHCP
- ConnMan local DNS proxy
- RK3588 -> Ubuntu VM HTTP GET verified
- 32 MiB payload transfer SHA256 match verified

R2 must not introduce:

- a custom wpa_supplicant manager
- a second DHCP client

## Current VM network topology

```text
ens33
  purpose: VM Internet access
  observed addresses: 192.168.152.130/24, 192.168.152.131/24

ens34
  purpose: development-board wired/NFS path
  observed address: 192.168.50.1/24

ens38
  purpose: Wi-Fi-side R2 server path
  observed address: 10.119.65.50/24
```

`ens38` and RK3588 are on the same Wi-Fi network.

For R2 TARGET validation, the expected data path is:

```text
RK3588 wlan0
    |
    | Wi-Fi
    v
experimental Wi-Fi LAN
    |
    v
Ubuntu VM ens38
    |
    v
FastAPI OTA Server
```

`ens34`/NFS is not acceptable evidence for this path.

## R2 server architecture

```text
FastAPI application
    |
    +-- Release metadata service
    |      |
    |      +-- GET /manifest.json
    |
    +-- Artifact service
    |      |
    |      +-- GET /update.raucb
    |
    +-- Device report endpoint
           |
           +-- POST /device/report
```

Persistent fleet storage is intentionally omitted in R2.

## Artifact storage

```text
artifacts/
└── versions/
    └── 1.1.0/
        └── update.raucb
```

The artifact may be dummy data. It does not need to be a valid RAUC bundle.

Future concepts that are acknowledged but not implemented:

- multiple release versions
- retention
- rollback candidate storage
- release channels
- device targeting
- signed RAUC bundles
- object storage/CDN

## Security boundary

R2 is an experimental-LAN HTTP MVP.

R2 provides:

- SHA256 transport/integrity metadata
- strict schema validation
- bounded range parsing
- logging

R2 does not provide:

- artifact authenticity
- authentication
- authorization
- production TLS/PKI

Future RAUC signature verification remains a separate authenticity mechanism.

`SHA256 != RAUC signature`

## Buildroot/kernel/U-Boot policy

No changes by default.

Only raise a dependency gap if the future target validation proves a required HTTP client, checksum utility, or certificate/runtime capability is missing.

Codex has no RK3588 hardware and must not modify BSP files to speculate about target needs.
