# R5 Secret Handling

## Secret owner

`edgeguard-provisioningd` is the Wi-Fi-secret owner. The OTA Agent, Agent state directory, release metadata, Server manifest, BLE advertisement, runtime status, operation result, tests/evidence, and logs never receive a passphrase.

The canonical plaintext credential is stored only in root-owned `/userdata/edgeguard-provisioning/wifi.json` mode `0600`. A second plaintext copy exists temporarily as the owned derived ConnMan provisioning file, also mode `0600`. Its lifecycle is strictly create/replace/revoke/reconstruct; it is not a second authority.

R5 claims filesystem permission protection only. Unless the target later proves encrypted userdata or a hardware-backed key, the package must not claim cryptographic protection at rest.

## Input and memory

- accept a secret only in an authorized complete `SET_WIFI` transaction;
- bound all fragments and the reassembled object before parsing;
- never echo the raw payload in D-Bus errors, IPC, read responses, or diagnostics;
- keep old/new credentials only as long as required for atomic replacement/recovery;
- clear mutable secret buffers before free where the compiler/runtime permits, while acknowledging copies inside GLib/JSON libraries may not be provably erased;
- disable core dumps for the daemon when supported by target policy;
- do not place credentials in command lines, environment variables, temporary directories, or shell scripts.

## Logging and observability

Allowed: transaction ID, operation, authorization result, provisioning generation, ConnMan service object path, state/result code, Agent state, monotonic duration, and optionally a keyed/non-reversible SSID label defined by later privacy policy.

Forbidden: passphrase, raw request bytes, complete `wifi.json`, derived `.config` contents, BlueZ link keys, pairing secrets, provisioning tokens, private keys, or credential-bearing environment/argv.

SSID is personal/network information even when not a credential. Default logs should redact it; if operational identity is needed, log a per-device keyed digest or a fixed `SSID_REDACTED` marker, not an unsalted global hash.

## Errors and crash paths

Validation errors identify the field/category, never the rejected value. D-Bus and ConnMan failures are normalized before publication. Debug mode follows the same redaction rules. Fatal errors must not dump parsed JSON or key files.

On BLE disconnect, timeout, window close, authorization loss, or daemon shutdown, discard all incomplete secret buffers. On store corruption, do not include file contents in messages. On ConnMan failure, do not log the generated key file.

## Evidence and tests

Use an unmistakable synthetic secret fixture and scan stdout/stderr, captured logs, result JSON, failure messages, generated non-secret reports, and source test snapshots for it. Protected input and the intentionally inspected private store fixture are the only exceptions. Target evidence records `password=REDACTED` and never captures `/var/lib/bluetooth/*/info` or full secret files.

## Revoke boundaries

`FORGET_WIFI` clears canonical and derived credential copies and observes ConnMan's provisioning-property revocation. It does not call ConnMan `Service.Remove`/`Service.Disconnect` because ConnMan 1.40 does not expose exclusive provenance for a merged service, and it does not delete endpoint override, Agent state, release identity, RAUC data, or A/B metadata. BLE bonds are not silently deleted by `FORGET_WIFI`; bond revocation is a separate local security action. Factory reset is not exposed in R5 v1.
