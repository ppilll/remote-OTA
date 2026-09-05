# R2 RK3588 Target Validation Report

# TARGET VERIFIED

Target:
```text
ATK-DLRK3588
RK3588 / AArch64
wlan0
curl 7.79.1
```

Route:
```text
10.119.65.50 dev wlan0 src 10.119.65.12 uid 0
```

Manifest:
```text
HTTP/1.1 200 OK
```

Range:
```text
HTTP/1.1 206 Partial Content
Content-Length: 1024
Content-Range: bytes 0-1023/33554432
```

Range file:
```text
1024 bytes
```

Full artifact:
```text
33554432 bytes
```

SHA256:
```text
4489164df7478188c3fd968f2c8d77a6e2a0522ae051972f588495cf9e118823
```

Matches manifest exactly.

Report upload: human test owner confirms PASS; raw report transcript not bundled.

Proves:
- target reaches server via `wlan0`
- manifest works on target
- single Range works on target
- full transfer works
- target SHA256 matches manifest metadata

Does not prove RAUC/A-B/boot/rollback/health/TLS/auth.
