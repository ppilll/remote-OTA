# R5 GATT Specification

## Version and UUID registry

Protocol version is integer `1`. These UUIDs are permanent ABI values and must never be regenerated at build or runtime.

| Object | UUID | Properties | BlueZ security flags |
|---|---|---|---|
| EdgeGuard Provisioning Service | `8a1d4e59-84e0-56d3-8d97-2080e5e77791` | primary service | — |
| DeviceInfo | `6a174d82-0b81-5fa2-bca8-1cef78011280` | read | `read` |
| RuntimeStatus | `ed5101e3-0964-5afa-b52c-c653c2c1e3ca` | read | `encrypt-read` |
| ProvisioningRequest | `9f6e55ea-003a-5254-95dd-5806ad2fb97d` | write | `encrypt-write`, `authorize` |
| OperationResult | `6b26b3c6-d089-5b53-9fea-5e5524ce169c` | read | `encrypt-read` |
| ControlRequest | `413bd486-a58e-5947-b570-9cffba5eec5c` | write | `encrypt-write`, `authorize` |

The application exports `org.freedesktop.DBus.ObjectManager`, one `org.bluez.GattService1`, and the five `org.bluez.GattCharacteristic1` objects. It registers the root with `GattManager1.RegisterApplication`.

## Read payloads

Read values are canonical UTF-8 JSON, maximum 1024 bytes, no NUL, no secret fields, and `schema_version: 1`.

DeviceInfo fields:

```json
{"schema_version":1,"protocol_version":1,"device_id":"...","release":"1.2.5","build_id":"rk3588-r4-1.2.5-001","provisioning_state":"UNPROVISIONED"}
```

RuntimeStatus fields are a bounded subset of:

```json
{"schema_version":1,"provisioning_state":"CONNECTED","wifi_connected":true,"effective_endpoint":"http://host:8000","agent_state":"IDLE","attempt_id":"","last_ota_error":"NONE","current_slot":"a"}
```

Missing or unreadable sources are represented with an explicit `*_available:false` field or a typed error; values are never guessed. RuntimeStatus is a paired, bonded, connected, encrypted-read-gated read/poll characteristic and is not an OTA state authority. It has no notify flag, `StartNotify`/`StopNotify`, `Notifying` state, public `Value` property, or `PropertiesChanged(Value)` publication.

OperationResult belongs to the single provisioning-authorized window owner and includes `transaction_id`, `operation`, `status` (`ACCEPTED`, `IN_PROGRESS`, `SUCCEEDED`, `FAILED`), and stable `error_code`. A peer must not receive another peer's result. OperationResult is re-frozen as owner-only encrypted read/poll: it has no notify flag, `StartNotify`/`StopNotify`, `Notifying` state, public `Value` property, or `PropertiesChanged(Value)` publication. Its UUID is unchanged.

## Write framing

ProvisioningRequest and ControlRequest use a deterministic 32-byte big-endian header followed by a UTF-8 JSON fragment:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII magic `EGP1` |
| 4 | 1 | protocol version (`1`) |
| 5 | 1 | opcode |
| 6 | 1 | flags: bit 0 `START`, bit 1 `END`; other bits zero |
| 7 | 1 | reserved, zero |
| 8 | 16 | opaque transaction ID; all-zero is invalid |
| 24 | 2 | total JSON length, 0..1024 |
| 26 | 2 | fragment offset |
| 28 | 2 | fragment length; equals bytes after header |
| 30 | 2 | reserved, zero |

The maximum logical JSON payload is 1024 bytes. Each GATT write must fit the link/BlueZ write limit. The sender fragments as needed; the daemon does not assume one `WriteValue` contains a complete request.

Reassembly is keyed by `(peer object path, characteristic, transaction_id)`. Only one incomplete transaction per peer per writable characteristic and eight incomplete transactions globally are permitted. Timeout is 30 seconds from the most recent accepted fragment, with a hard lifetime of 60 seconds. Disconnect, window close, authorization loss, timeout, or conflicting overlap discards the incomplete plaintext buffer.

Exact duplicate fragments are idempotent. Overlap containing different bytes, gaps at `END`, inconsistent opcode/length, out-of-range offsets, invalid UTF-8, duplicate completed transaction with different content, or excess limits fails deterministically. Recently completed transaction IDs are cached for 10 minutes per peer with only request digest and non-secret result.

## Opcodes and JSON schemas

ProvisioningRequest:

- `0x01 SET_WIFI`: `{"ssid":"...","security":"psk","passphrase":"...","hidden":false}`
- `0x02 SET_ENDPOINT`: `{"base_url":"http://host:port"}` (base URL maximum 512 bytes)
- `0x03 FORGET_WIFI`: `{}`

ControlRequest:

- `0x10 CHECK_UPDATE_NOW`: `{}`

SSID is 1..32 bytes as transmitted, no NUL/control characters. R5 v1 accepts only `security:"psk"`; passphrase is 8..63 printable ASCII characters or exactly 64 hexadecimal characters. `hidden` is boolean. Endpoint rules are in `07_ENDPOINT_CONFIG_SPEC.md`.

No opcode exists for bundle transfer, direct URL download, RAUC, reboot, boot-slot mutation, arbitrary shell, OTA-state deletion, factory reset, or credential readback.

## Transaction rules

Authorization and concurrency are checked again at commit, not only on the first fragment. Commit authorization proves that the initial sensitive request passed the encrypted characteristic gate and that the same active BlueZ unique owner, window/authorization epoch, peer connection/security epoch, stable peer identity, and current `Connected`/`Paired`/`Bonded` conditions remain. It does not independently re-measure link encryption at commit. A complete request is validated before any persistent mutation. `SET_WIFI` commits one complete credential object; SSID and passphrase are never independently applied. An invalid request leaves the previous working configuration unchanged.

Writes return only acceptance of the ATT write. Final application results are delivered through OperationResult read. A peer may read the latest result after reconnect when still authorized in the same provisioning window and the cached result exists.

## BlueZ method behavior

Reject unsupported offsets/options, prepare-write patterns not implemented by this protocol, invalid devices, and unauthorized calls using stable D-Bus errors mapped to `10_ERROR_MODEL.md`. Inspect the `device`, `offset`, `mtu`, and `prepare-authorize` options supplied by BlueZ. Never trust an address or peer ID supplied inside JSON.
