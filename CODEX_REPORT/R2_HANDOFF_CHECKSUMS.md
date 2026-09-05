# R2 Validated Values

Release:
```text
schema_version: 1
device_compatible: atk-dlrk3588
version: 1.1.0
build_id: r2-dummy-1.1.0
artifact_url: /update.raucb
mandatory: false
```

Dummy artifact:
```text
size: 33554432
sha256: 4489164df7478188c3fd968f2c8d77a6e2a0522ae051972f588495cf9e118823
```

Target Range sample:
```text
bytes=0-1023 -> 206
Content-Length: 1024
Content-Range: bytes 0-1023/33554432
```

Validated target route:
```text
10.119.65.50 dev wlan0 src 10.119.65.12 uid 0
```

These are R2 test-session values, not future release constants.
