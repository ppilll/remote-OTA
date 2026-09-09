# R5 Provisioning Store Specification

## Authority and layout

`/userdata/edgeguard-provisioning/` is the only canonical provisioning authority. It is root-owned mode `0700`. R5 uses:

```text
/userdata/edgeguard-provisioning/
├── wifi.json       # secret, 0600
└── runtime.json    # endpoint override, 0600
```

Temporary files are created in the same directory, mode `0600`, never followed through symlinks, and removed after recovery. OTA Agent state, `/etc`, `/var/lib/connman`, logs, crash messages, and evidence packages are not canonical stores.

## Wi-Fi schema v1

```json
{
  "schema_version": 1,
  "generation": 1,
  "ssid": "laboratory-ap",
  "security": "psk",
  "passphrase": "REDACTED-IN-DOCUMENTATION",
  "hidden": false
}
```

Required fields are exact; unknown fields are rejected in v1. `generation` is an unsigned 64-bit integer represented within JSON's exact accepted range and monotonically increases on successful replacement. Validation follows `03_GATT_SPEC.md`. The file contains no Agent state, release identity, BLE bond, transaction cache, or ConnMan service path.

## Runtime schema v1

```json
{
  "schema_version": 1,
  "generation": 1,
  "ota_server_base_url": "http://192.168.77.1:8000"
}
```

The endpoint is not a credential in R5, but the file remains private to simplify directory ownership and avoid future permission migration. Validation is defined in `07_ENDPOINT_CONFIG_SPEC.md`.

## Atomic write contract

For each replacement:

1. parse and validate the full candidate in memory;
2. open a same-directory unique temporary file with `O_CREAT|O_EXCL|O_NOFOLLOW`, mode `0600`;
3. write all bytes, handling short writes;
4. `fsync` the temporary file;
5. verify owner, mode, regular-file type, bounded size, and serialized content;
6. atomically `rename` over the destination;
7. `fsync` the parent directory;
8. publish the new in-memory generation and clear the secret buffer.

Failure before rename leaves the prior file authoritative. Failure after rename but before directory sync is reported as persistence uncertainty and must not trigger an automatic second apply. Never truncate the live file in place.

## Read and recovery contract

- open with `O_NOFOLLOW`; require a regular root-owned file with no group/other permissions;
- bound the complete file to 64 KiB before parsing, although valid v1 files are much smaller;
- reject duplicate JSON members, wrong types, unsupported schema, invalid UTF-8, and out-of-range fields;
- a corrupt `wifi.json` means `FAILED_CONFIG` and no generated credential; never guess or reuse an unvalidated fragment;
- a corrupt `runtime.json` is ignored in favor of the immutable endpoint default, with a redacted typed error;
- schema migrations are explicit functions from a recognized older schema to v1 and use the same atomic write path; unknown future schemas are never rewritten.

## Credential replacement

R5 uses commit-then-apply with rollback to the previous canonical bytes only when the new ConnMan application deterministically fails before connectivity acceptance:

1. validate candidate;
2. retain the old validated object in protected process memory;
3. atomically commit the new `wifi.json` generation;
4. atomically regenerate the derived ConnMan file;
5. request/observe ConnMan association;
6. on success, discard the old object;
7. on deterministic apply failure, atomically restore the previous canonical generation as a new generation and regenerate it;
8. on uncertain apply outcome, report uncertainty and do not loop replacements.

An interrupted process recovers from the single canonical file at restart; no partially written credential is considered valid.

## Forget

`FORGET_WIFI` is an authorized atomic operation. First rename `wifi.json` to a same-directory tombstone, sync the directory, revoke the derived ConnMan file and connection, then remove the tombstone and sync again. On restart, a tombstone means complete the forget; never restore its credential automatically. Forget does not delete Agent state, runtime endpoint, release data, RAUC data, or A/B metadata.

## Locking

The daemon is the sole writer and holds a process lock within the store. In-process transaction serialization permits one committing mutation at a time. Readers consume immutable validated snapshots. External file edits are unsupported and detected on the next bounded reload.
