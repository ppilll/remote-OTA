# R5 BLE Security Policy

## Threat boundary

The attacker is a nearby BLE peer able to discover, connect, replay traffic, race a legitimate peer, or send malformed/oversized writes. Filesystem root compromise and cryptographic protection of an unencrypted `/userdata` partition are outside R5's guarantees.

## Pairing decision

The target exposes BlueZ 5.77 `AgentManager1`, but no display or keyboard is established. Register an application agent with `NoInputNoOutput`. This provides an encrypted link after pairing but must not be described as MITM-authenticated pairing.

Physical presence supplies the additional local authorization factor. The input source is selected by device name `adc-keys`; `/dev/input/event7` is never hard-coded. The exact keycode and hold duration are deployment configuration and remain blocked on target validation. A missing, ambiguous, or unvalidated mapping fails closed: the window stays shut.

## Provisioning window

- duration: 120 seconds, measured with a monotonic clock;
- opening event: validated long-press from the configured `adc-keys` input;
- default boot state: closed, not pairable, not advertising;
- while open: pairable and provisioning advertisement enabled;
- close conditions: timeout, explicit local close, daemon shutdown, input-source loss, or successful sensitive operation if policy is configured one-shot;
- on every window epoch change (open, reopen, or close): discard prior transactions, active request binding, and cached OperationResult; on close also stop advertising, set pairable false, revoke in-memory provisioning authorization, and reject new sensitive writes;
- wall-clock changes never extend the window.

The window may be reopened only by a new physical-presence event. Being unprovisioned does not by itself open an unlimited advertising window.

Only one peer is provisioning-authorized per window. The first eligible bonded/encrypted peer that completes application authorization becomes the window owner; pairability is then disabled and other peers' sensitive writes are rejected. An existing bond is only an input to this selection, never permanent authorization. Closing the window clears the owner. OperationResult is owner-only encrypted read because BlueZ 5.77 does not expose peer identity to application-side notification callbacks.

## Advertising policy

Register through `LEAdvertisingManager1.RegisterAdvertisement`. Advertise only during the open window. Include a stable local name such as `EdgeGuard Setup` and the provisioning service UUID. Do not include device secrets, SSID, passphrase, endpoint token, pairing material, OTA attempt ID, or detailed operational state.

On BlueZ disappearance/reappearance, rebuild the GATT and advertisement registrations. Re-registration does not open a closed window or make the adapter pairable.

## Authorization predicate

A sensitive write is allowed only when all predicates are true at fragment acceptance and again at commit:

```text
encrypted link
AND Paired=true
AND bonded peer identity is stable
AND provisioning window is open
AND peer was admitted during the current window
AND operation is allowed by the concurrency policy
```

`Connected=true`, `Paired=true`, or `Trusted=true` alone is never sufficient. The daemon's current-window authorization is volatile and is cleared on window close, BlueZ ownership loss, or daemon restart. BlueZ `Trusted` may be managed for bond lifecycle but is not the application authorization database.

DeviceInfo is the only unpaired read and contains no secrets. RuntimeStatus and OperationResult require an encrypted link. All writes use `encrypt-write` plus `authorize`. The daemon accepts characteristic and Agent method calls only from the current unique `org.bluez` bus owner, and binds commit authorization to the same window and peer connection/security epochs captured by the accepted encrypted write. Do not change to `encrypt-authenticated-write` unless later target evidence proves compatible authenticated pairing behavior and the policy is formally revised.

## Pairing and bond rules

- reject new pairing requests outside an open window;
- admit at most one provisioning peer at a time;
- bind a transaction to the BlueZ device object, not a caller-provided address;
- do not silently authorize every historical bond when a new window opens;
- `FORGET_WIFI` does not delete BLE bonds;
- bond revoke/factory reset is not exposed in R5 v1 GATT;
- stale or missing device properties fail closed.

## Security states

| Peer state | DeviceInfo | RuntimeStatus | Sensitive write |
|---|---:|---:|---:|
| Unpaired | allow | deny | deny |
| Paired/bonded, window closed | allow | allow on encrypted link | deny |
| Paired/bonded, window open, not admitted | allow | allow on encrypted link | deny |
| Paired/bonded, admitted in current window | allow | allow on encrypted link | allow if concurrency permits |

## Negative requirements

No credential readback, public write, always-on pairability, permanent authorization inferred from bond state, arbitrary shell, BLE bundle transfer, direct OTA/RAUC/boot-control operation, or secret-bearing logging is permitted.
