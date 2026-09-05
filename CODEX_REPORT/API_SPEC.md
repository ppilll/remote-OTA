# R2 API Specification — As Validated

## Transport
HTTP, experimental LAN, TCP port 8000.

Do not hard-code the observed DHCP VM address into future Agent logic.

## GET /manifest.json
Success:
```text
200 OK
Content-Type: application/json
```

Schema:
```json
{
  "schema_version": 1,
  "device_compatible": "atk-dlrk3588",
  "version": "1.1.0",
  "build_id": "r2-dummy-1.1.0",
  "artifact_url": "/update.raucb",
  "sha256": "<64 lowercase hex>",
  "size": 33554432,
  "mandatory": false
}
```

`current_version` is device state and is not part of the server release manifest.

## GET /update.raucb
Full:
```text
200 OK
Accept-Ranges: bytes
Content-Type: application/octet-stream
```

Validated single range:
```text
Range: bytes=0-1023

206 Partial Content
Content-Length: 1024
Content-Range: bytes 0-1023/33554432
```

Multi-range must not be assumed.

After full download, the client must calculate SHA256 and compare it with manifest metadata.

## POST /device/report
Request:
```json
{
  "device_id": "rk3588-001",
  "current_version": "1.0.0",
  "status": "downloaded",
  "timestamp": "2026-09-03T09:00:00Z"
}
```

R2 statuses:
```text
idle
checking
downloading
downloaded
error
```

Success:
```text
204 No Content
```

## Error behavior
Relevant R2 behavior:
```text
400 malformed/unsupported request
404 artifact unavailable
416 unsatisfiable range
422 report schema failure
500 server/read failure
503 inconsistent active release state
```

Important handoff note: older design text mentioned a conceptual `/manifest.json` 204 "no active release" path. The actual R2 server is built around one configured active release at startup. Later Agent logic must not depend on manifest 204 unless the server is explicitly changed and tested.
