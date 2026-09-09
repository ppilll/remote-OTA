# Thread 2 interface freeze

Thread 3 must consume these interfaces without redesigning them.

## D-Bus object tree

```text
/com/edgeguard/provisioning
  org.freedesktop.DBus.ObjectManager
  /service0
    org.bluez.GattService1
    /char0  DeviceInfo
    /char1  RuntimeStatus
    /char2  ProvisioningRequest
    /char3  OperationResult
    /char4  ControlRequest
  /advertisement0
    org.bluez.LEAdvertisement1
  /agent0
    org.bluez.Agent1 (NoInputNoOutput)
```

The root is passed to `GattManager1.RegisterApplication`. The advertisement is
registered independently through `LEAdvertisingManager1` only while the
physical-presence window is open. The agent is registered through
`AgentManager1` with `NoInputNoOutput`. A BlueZ name-owner change rebuilds these
registrations without opening a closed window.

## UUID and property registry

| Object | UUID | Flags/properties |
|---|---|---|
| service0 | `8a1d4e59-84e0-56d3-8d97-2080e5e77791` | `Primary=true` |
| char0 DeviceInfo | `6a174d82-0b81-5fa2-bca8-1cef78011280` | `read` |
| char1 RuntimeStatus | `ed5101e3-0964-5afa-b52c-c653c2c1e3ca` | `encrypt-read`, `notify` |
| char2 ProvisioningRequest | `9f6e55ea-003a-5254-95dd-5806ad2fb97d` | `encrypt-write`, `authorize` |
| char3 OperationResult | `6b26b3c6-d089-5b53-9fea-5e5524ce169c` | `encrypt-read`, `notify` |
| char4 ControlRequest | `413bd486-a58e-5947-b570-9cffba5eec5c` | `encrypt-write`, `authorize` |

The advertisement contains only `Type=peripheral`, local name
`EdgeGuard Setup`, and the service UUID. Read/notify values are bounded
NUL-free UTF-8 JSON with schema version 1 and at most 1024 bytes.

## Authorization callback contract

`EgpAuthorizeCommit` is called from the serialized operation worker immediately
before commit/control dispatch. It must re-read the BlueZ `Device1` object and
return true only when all of the following remain true:

```text
the call arrived through an encrypt-* GATT characteristic
AND Connected=true
AND Paired=true
AND Bonded=true
AND Address and AddressType provide a stable BlueZ identity
AND the monotonic 120-second physical-presence window is open
AND the same BlueZ object path owns the current window
```

The first eligible sensitive fragment claims an otherwise-unowned window.
Pairability is then disabled and other connected peers are disconnected.
Window close, input loss, daemon shutdown, or BlueZ owner loss clears volatile
authorization and plaintext fragment state. A historical bond is never
permanent application authorization. `Trusted` is not an authorization input.

The input mapping is deployment data: `main` accepts the paired options
`--key-code N --hold-ms N`. Omitting both leaves input disabled and the window
closed; missing, ambiguous, or invalid `adc-keys` discovery also fails closed.
No `/dev/input/eventN` is frozen. `--one-shot` closes the window after a
successful sensitive operation.

## Protocol and operation contract

The 32-byte `EGP1` header, opcodes, bounds, reassembly rules, and replay cache
are public in `protocol.h`. Reassembly is keyed by BlueZ peer object path,
writable characteristic, and the opaque 16-byte transaction ID. Buffers are
cleared on completion, conflict, timeout, disconnect/authorization loss,
window close, and shutdown. Completed entries retain only a request digest and
redacted result for ten minutes.

All complete operations run on one worker. `SET_WIFI`, `SET_ENDPOINT`, and
`FORGET_WIFI` require two stable read-only `IDLE` observations before commit and
another observation before external reconciliation. The worker calls only the
frozen Thread 1 store/endpoint/ConnMan interfaces. `CHECK_UPDATE_NOW` uses the
fixed root-only socket contract below and never falls back to shell, signal,
restart, RAUC, or boot-control mutation.

## Status and Thread 3 source expectations

Thread 2 reads these sources only:

| Source | Use | Failure behavior |
|---|---|---|
| `/userdata/edgeguard-remote-ota/device-id` | DeviceInfo identity | `device_id_available=false` |
| `/etc/edgeguard-ota/release.json` | release/build | `release_available=false` |
| `/userdata/edgeguard-remote-ota/agent-state.json` | exact frozen state, attempt ID, last error | `agent_state_available=false`; mutations fail closed |
| Thread 1 `runtime.json` API, then immutable `/etc/edgeguard-ota/agent.conf` fallback | effective endpoint | `effective_endpoint_available=false` |
| `/usr/bin/edgeguard-rk-abctl get-current` | read-only slot | `current_slot_available=false` |
| `/run/edgeguard-remote-ota/control.sock` | `CHECK_UPDATE_NOW` client | `AGENT_UNAVAILABLE`/typed rejection |

Thread 3 owns the socket server and Agent wakeup. It must keep the frozen
4-byte big-endian length plus JSON protocol, root peer-credential check,
one-request-per-connection bound, and `CHECK_UPDATE_NOW` as the sole command.
It also owns endpoint refresh in the Agent. It must not make the provisioning
daemon a writer of Agent state or an OTA state authority.

## Frozen source list for Thread 3

Build the daemon from exactly these production sources unless an integration
defect is reported rather than silently redesigned:

```text
src/main.c
src/store.c
src/endpoint.c
src/connman.c
src/protocol.c
src/security.c
src/status.c
src/operations.c
src/input.c
src/bluez.c
```

Public headers are all files under `include/edgeguard_provisioning/`. Required
libraries are GLib, GIO/GDBus, gio-unix, JSON-GLib, and POSIX/Linux input APIs.
Thread 3 supplies Buildroot/SysV wiring and the validated deployment keycode and
hold duration; it does not change the object tree, UUIDs, security flags,
authorization predicate, framing, or operation schemas.
