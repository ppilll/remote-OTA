# R2 Manifest Schema Handoff

| Field | Type | Meaning |
|---|---|---|
| schema_version | integer | EdgeGuard manifest schema revision |
| device_compatible | string | target compatibility identifier |
| version | string | available release version |
| build_id | string | opaque build/release ID |
| artifact_url | string | artifact URL, currently relative |
| sha256 | string | expected artifact SHA256 |
| size | integer | expected artifact byte count |
| mandatory | boolean | release-policy hint |

Validated compatible:
```text
atk-dlrk3588
```

R3 must define version ordering before implementing upgrade decisions. Do not use naive lexical comparison.

The manifest must not be confused with a RAUC `manifest.raucm`.
