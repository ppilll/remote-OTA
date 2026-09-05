# R3 Input Contract from R2

## Server origin
Do not hard-code `10.119.65.50`. It was a runtime test address.

## Manifest
```text
GET /manifest.json -> 200
```

Expected schema version: `1`.

## Compatibility
Check:
```text
manifest.device_compatible
```
Validated value:
```text
atk-dlrk3588
```

## Version
The manifest owns `version`; the device owns `current_version`.

R3 must freeze:
- version ordering
- same-version behavior
- downgrade policy
- malformed version behavior
- build_id semantics

Do not use lexical string comparison.

## Artifact URL
Validated:
```text
/update.raucb
```
Resolve relative to the server origin.

## Size
Use manifest size for sanity/progress/storage checks. Do not assume the R2 dummy size for future releases.

## SHA256
After download:
```text
calculate SHA256 -> compare manifest.sha256
```
Mismatch is a failure.

SHA256 does not replace RAUC signature verification.

## Range
R2 supports bounded single byte ranges.
Multi-range must not be assumed.

R3 can start with full download; resume is a separate design choice.

## Reporting
Endpoint:
```text
POST /device/report
```

Required:
```text
device_id
current_version
status
timestamp
```

R2 statuses:
```text
idle checking downloading downloaded error
```

New status values require an intentional API/server change.

## Networking
ConnMan remains network owner.
Do not add a second DHCP client or custom wpa_supplicant manager.

## R3 must define failure handling
- connection refused
- timeout
- connection reset
- HTTP 4xx/5xx
- truncated file
- SHA mismatch
- Wi-Fi loss
- retry/backoff

## Installation boundary
R2 dummy `.raucb` is not evidence of RAUC installability.

R3 may rely on the R2 server/API transport facts, but must independently validate its Agent on the RK3588.
