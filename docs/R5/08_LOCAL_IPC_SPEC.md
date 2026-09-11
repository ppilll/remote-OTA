# R5 Local IPC Specification

## Decision

Agent control uses a Unix-domain stream socket:

```text
/run/edgeguard-remote-ota/control.sock
```

Read-only OTA status uses the existing `/userdata/edgeguard-remote-ota/agent-state.json` plus read-only release/slot sources. The provisioning daemon does not write Agent state. D-Bus was not selected for Agent control because the Agent has no D-Bus lifecycle requirement; signals/restart were rejected because they are untyped and unsafe around active OTA work.

## Socket ownership and lifecycle

- created by the OTA Agent, root:root mode `0600` under a root-owned runtime directory;
- created with restrictive umask, stale socket checked as a socket and safely replaced only by its owner;
- listening file descriptor is non-inheritable;
- peer credentials are checked with `SO_PEERCRED`; only UID 0 is accepted in v1;
- removed on clean shutdown without following symlinks;
- at most four connected clients, one request per connection, 1024-byte frame limit, and 2-second read/write deadline;
- unavailable socket means `AGENT_UNAVAILABLE`, never fallback to shell, signal, or restart.

## Framing

Each request and response is a 4-byte unsigned big-endian JSON length followed by that many UTF-8 bytes. Length must be `1..1024`; trailing data, duplicate JSON members, control characters outside JSON encoding, and unknown fields are rejected.

Request v1:

```json
{"version":1,"request_id":"b0cb1b28-94cb-4c03-8e4c-f52fc1294199","command":"CHECK_UPDATE_NOW"}
```

Response examples:

```json
{"version":1,"request_id":"b0cb1b28-94cb-4c03-8e4c-f52fc1294199","status":"ACCEPTED","error":"NONE"}
```

```json
{"version":1,"request_id":"b0cb1b28-94cb-4c03-8e4c-f52fc1294199","status":"REJECTED","error":"AGENT_BUSY"}
```

`request_id` is canonical UUID text and provides idempotence during a bounded in-memory cache. It is a domain-separated, collision-resistant SHA-256-derived identity, formatted from 128 digest bits without forcing UUIDv4 bits; for the bounded request population, collision probability is negligible. The canonical input framing includes a fresh per-daemon-process session UUID, provisioning window epoch, peer connection/security epoch, BlueZ peer object path, writable characteristic, command opcode, and all 128 bits of the GATT transaction ID. The same logical request retry in one daemon/window/connection scope produces the same value; a new window or connection/security epoch, different peer, different characteristic/command, different txid, or daemon restart produces a separately scoped value. The fresh daemon session UUID prevents a post-restart window counter from aliasing an entry in the Agent's existing ten-minute cache. No request carries a URL, bundle, slot, shell string, credential, or arbitrary argument, and the IPC command surface is unchanged.

## Command semantics

`CHECK_UPDATE_NOW` is the only v1 command. The socket handler validates and queues an in-process event. If the Agent is idle in its poll wait, the event wakes that wait; the main loop then refreshes the endpoint and takes the existing `IDLE -> CHECK_NETWORK -> CHECK_UPDATE` path. The handler itself never changes durable state, downloads, calls RAUC, reboots, or touches A/B.

If the Agent is not in an eligible `IDLE` wait, return `AGENT_BUSY`. Concurrent duplicate accepted request IDs return the original result; different requests do not queue an unbounded backlog. A single pending check flag is sufficient.

The wait implementation should multiplex stop and control wakeups using a bounded file-descriptor event mechanism. Do not poll the socket with an extra sleep loop.

## Status reading

The provisioning daemon opens `agent-state.json` read-only with size and schema bounds and maps the frozen state names without reinterpretation. It never fabricates install success from ConnMan, BlueZ, or RAUC observations. If a field is unavailable, RuntimeStatus says unavailable.

Slot status is obtained from the established read-only `edgeguard-rk-abctl status/get-current` interface or an already-frozen adapter without write verbs. Command execution, if retained for this existing read-only control surface, must be fixed-argument with no BLE text interpolation.

## Failure isolation

Malformed or unauthorized IPC affects only that connection. Provisioning continues to serve safe status when the Agent is busy or unavailable. Agent socket failure must not alter the durable OTA state machine or make provisioning invoke an alternate OTA path.
