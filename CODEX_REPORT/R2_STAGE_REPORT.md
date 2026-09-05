# R2 Stage Report

## Verdict
# PASS

## Objective
Provide update metadata, artifact download, and device status reporting from an Ubuntu VM server and prove the contract over the real RK3588 Wi-Fi path.

## Server API
```text
GET  /manifest.json
GET  /update.raucb
POST /device/report
```

## Target path
```text
RK3588 wlan0 -> Wi-Fi -> Ubuntu VM ens38 -> FastAPI :8000
```

Observed RK3588 route:
```text
10.119.65.50 dev wlan0 src 10.119.65.12 uid 0
    cache
```

## Manifest result
Observed:
```text
HTTP/1.1 200 OK
```

```json
{
  "schema_version": 1,
  "device_compatible": "atk-dlrk3588",
  "version": "1.1.0",
  "build_id": "r2-dummy-1.1.0",
  "artifact_url": "/update.raucb",
  "sha256": "4489164df7478188c3fd968f2c8d77a6e2a0522ae051972f588495cf9e118823",
  "size": 33554432,
  "mandatory": false
}
```

## Range result
Request:
```text
Range: bytes=0-1023
```

Observed:
```text
HTTP/1.1 206 Partial Content
Accept-Ranges: bytes
Content-Length: 1024
Content-Range: bytes 0-1023/33554432
```

Target:
```text
1024 /tmp/r2-range.bin
```

Result: **TARGET VERIFIED**

## Full artifact result
Target size:
```text
33554432 /tmp/update.raucb
```

Target SHA256:
```text
4489164df7478188c3fd968f2c8d77a6e2a0522ae051972f588495cf9e118823
```

This exactly matches manifest SHA256.

Result: **TARGET VERIFIED**

## Device report
`POST /device/report` is implemented. The human test owner confirmed the R2 report validation passed. Raw POST/journal transcript was not included in the final handoff input.

## VM deployment
Ubuntu 20.04 / Python 3.8.10 / systemd deployment was confirmed passed by the human test owner. Detailed terminal output was omitted from the final handoff input.

## BSP impact
No R2 modification required to:
- Buildroot
- kernel
- U-Boot
- ConnMan ownership
- wpa_supplicant ownership
- DHCP architecture

## Evidence levels
- CODEX VERIFIED
- HOST VERIFIED
- TARGET VERIFIED

No broader OTA/A-B `HARDWARE VERIFIED` claim is made.
