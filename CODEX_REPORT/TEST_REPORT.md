# R2 Test Report

## CODEX
Result: `CODEX VERIFIED`

Automated server tests cover manifest, full/range download, bad ranges, missing/read-error artifact, report validation, malformed JSON, logging, and concurrency.

## HOST
Result: `HOST VERIFIED`

Ubuntu VM deployment was confirmed passed by the human test owner. Raw final VM transcript is not included in this handoff package.

## TARGET
Result: `TARGET VERIFIED`

### Route
```text
10.119.65.50 dev wlan0 src 10.119.65.12 uid 0
```

### Manifest
```text
HTTP/1.1 200 OK
```

### Range
```text
HTTP/1.1 206 Partial Content
Content-Length: 1024
Content-Range: bytes 0-1023/33554432
```

```text
1024 /tmp/r2-range.bin
```

### Full artifact
```text
33554432 /tmp/update.raucb
```

### SHA256
Manifest and RK3588:
```text
4489164df7478188c3fd968f2c8d77a6e2a0522ae051972f588495cf9e118823
```

### Device report
Result: `PASS — human test owner confirmed`
Raw POST/journal transcript omitted.

## Interpretation
Transfer integrity is verified. Artifact authenticity and OTA installation are not.
