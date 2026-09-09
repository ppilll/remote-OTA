# R5 Error Model

## Stable result envelope

Every operation result has a stable machine code, a non-secret short message, retryability, and whether persistent state changed. Internal errno/D-Bus details may be logged only after redaction and are not protocol ABI.

```json
{"schema_version":1,"transaction_id":"...","operation":"SET_WIFI","status":"FAILED","error_code":"WIFI_AUTH_FAILED","retryable":true,"persistent_change":"ROLLED_BACK"}
```

## Error codes

| Code | Class | Retryable | Persistent change |
|---|---|---:|---|
| `NONE` | success | — | committed as reported |
| `BLE_UNAUTHORIZED` | security | after new authorization | none |
| `BLE_NOT_PAIRED` | security | after pairing | none |
| `BLE_WINDOW_CLOSED` | security | after physical presence | none |
| `BLE_PROTOCOL_UNSUPPORTED` | protocol | no | none |
| `BLE_PAYLOAD_INVALID` | protocol | corrected request | none |
| `BLE_PAYLOAD_TOO_LARGE` | protocol | smaller request | none |
| `BLE_FRAGMENT_CONFLICT` | protocol | new transaction | none |
| `BLE_TRANSACTION_TIMEOUT` | protocol | new transaction | none |
| `BLE_REPLAY_REJECTED` | security/protocol | new authorized transaction | none |
| `PROVISIONING_BUSY` | concurrency | yes | none |
| `PROVISIONING_RACE` | concurrency | inspect result | explicit in result |
| `WIFI_CONFIG_INVALID` | validation | corrected request | none |
| `WIFI_AUTH_FAILED` | network | corrected credential | previous restored or unchanged |
| `WIFI_AP_NOT_FOUND` | network | yes | previous restored or unchanged |
| `WIFI_CONNECT_TIMEOUT` | network | yes | previous restored or uncertain |
| `PERSISTENCE_READ_FAILED` | storage | after repair | none |
| `PERSISTENCE_WRITE_FAILED` | storage | after repair | unchanged or uncertain |
| `PERSISTENCE_SCHEMA_UNSUPPORTED` | storage | migration/software required | none |
| `ENDPOINT_INVALID` | validation | corrected request | none |
| `ENDPOINT_CONFIG_INVALID` | Agent fallback | after repair | immutable default used |
| `CONNMAN_UNAVAILABLE` | dependency | yes | canonical store retained |
| `CONNMAN_APPLY_FAILED` | dependency | yes | previous restored or unchanged |
| `BLUEZ_UNAVAILABLE` | dependency | yes | none |
| `GATT_REGISTRATION_FAILED` | dependency | yes | none |
| `ADVERTISEMENT_FAILED` | dependency | yes | none |
| `AGENT_BUSY` | OTA concurrency | later | none |
| `AGENT_UNAVAILABLE` | dependency | later | none |
| `OTA_CONTROL_REJECTED` | control | depends | none |
| `FACTORY_RESET_BLOCKED` | unsupported/security | no in v1 | none |
| `INTERNAL_ERROR` | implementation | bounded retry | no secret disclosed |

## Mapping rules

- malformed input is rejected before authorization-sensitive state mutation;
- authentication and authorization errors reveal no credential validity;
- wrong PSK is distinguished only after an authorized transaction and ConnMan outcome;
- storage uncertainty is never silently reported as success and never causes automatic repeated destructive action;
- BlueZ/ConnMan/Agent dependency loss does not erase canonical provisioning data;
- a provisioning error never maps to an OTA firmware-health error.

## Logging and transport

Messages may include operation, transaction ID, state, generation, dependency name, and redacted reason. They must not include passphrase, raw write payload, pairing secret, rejected raw endpoint, or arbitrary contents of a corrupt store. D-Bus errors sent to a peer are stable and short; detailed diagnostics remain local and redacted.

## Recovery ownership

| Failure layer | Recovery owner |
|---|---|
| BLE/GATT/advertisement | provisioning daemon re-register/retry |
| pairing/window | user physical-presence workflow |
| canonical store | provisioning daemon/operator storage repair |
| ConnMan | provisioning daemon reconciliation and ConnMan service |
| runtime endpoint parse | Agent fallback plus provisioning repair |
| Agent control | OTA Agent; provisioning may retry only when policy allows |
| RAUC/A-B/firmware health | existing R4 OTA path only |
