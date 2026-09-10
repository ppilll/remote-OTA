# R5 ConnMan Integration

## Decision: persistent provisioning file

R5 selects Option B:

```text
canonical /userdata store
        |
        v
generated /var/lib/connman/edgeguard-provisioning.config
        |
        v
ConnMan 1.40
```

Option A (persist all `/var/lib/connman`) is rejected because it would carry rootfs/runtime/cache state across slots and blur ownership. Option C (credential-serving `net.connman.Agent`) is not selected because unattended boot/autoconnect and agent lifecycle were not target-proven. Option B is supported by direct target evidence: adding a provisioning `.config` changed `Immutable`, `Favorite`, and `AutoConnect` from false to true, and removing it changed them back to false.

`/var/lib/connman` remains rootfs-local and derived. Loss of that directory on full-rootfs replacement is repaired from `/userdata` and does not lose the canonical credential.

## Derived file

Path: `/var/lib/connman/edgeguard-provisioning.config`, root-owned mode `0600`. It contains exactly one managed service section and no comments containing user data.

Conceptual form:

```ini
[service_edgeguard]
Type=wifi
Name=<validated SSID>
Hidden=<true|false>
Security=psk
Passphrase=<validated passphrase>
```

The generator must use the exact ConnMan 1.40 key-file escaping accepted by the target. It never interpolates through a shell. Newline, carriage return, NUL, `[`/`]`, and key-file delimiter injection are rejected or escaped through a tested GLib key-file serializer. The validated target sample `[service_r5probe]`, `Type=wifi`, `Name=...`, `Security=none` proves the mechanism; WPA-PSK syntax and hidden-network behavior remain target-validation items.

## Startup reconciliation

After `/userdata` is mounted and ConnMan is or becomes available:

- valid `wifi.json`: atomically regenerate the derived file, observe provisioning, then request scan/connect through ConnMan D-Bus when required;
- absent credential: ensure the derived file is absent;
- corrupt credential: remove no unrelated ConnMan state, expose no credential, report `WIFI_CONFIG_INVALID`/`PERSISTENCE_READ_FAILED`, and remain unprovisioned;
- changed generation: replace the derived file atomically and apply once;
- ConnMan unavailable: retain canonical data, report `CONNMAN_UNAVAILABLE`, and retry with bounded backoff.

The daemon does not call `connmanctl`, `wpa_cli`, or shell commands as its product API.

## Apply and observation

Use system D-Bus to discover `net.connman.Manager`, Wi-Fi technology, and matching service. Match a service using ConnMan identity/properties derived from the validated SSID/security; never accept a BLE-supplied D-Bus path. Observe state transitions with a monotonic timeout.

Provisioning state sequence:

```text
UNPROVISIONED -> STORED -> APPLYING -> ASSOCIATING -> CONNECTED
                                                `-> FAILED_*
```

`CONNECTED` requires the target's ConnMan service to reach a connected/online state and obtain IP configuration. Optional OTA-server reachability is reported separately and is never firmware health.

## Replacement and failure

Only one apply transaction runs at once. A candidate credential replaces the derived file only after full validation and canonical commit. Deterministic authentication/not-found/timeout outcomes are classified separately. The rollback policy in `05_PROVISIONING_STORE_SPEC.md` preserves or restores the previous validated credential without exposing it.

No failure in this layer changes `agent-state.json`, invokes RAUC, alters A/B metadata, marks a slot, or interprets network failure as firmware-health failure.

## Forget/revoke

On `FORGET_WIFI`:

1. complete the canonical tombstone step;
2. atomically remove the derived `.config` and sync its directory;
3. do not call `Service.Remove` or `Service.Disconnect`: ConnMan 1.40 exposes no property proving that a merged SSID/security service or learned credential is exclusively provisioning-owned, so those calls could mutate unrelated state;
4. observe that the provisioned service no longer has the provisioning-owned immutable/autoconnect properties;
5. report success without affecting unrelated ConnMan services.

The target evidence's true-to-false transition after removing the fixed derived file is the acceptance model. A pre-removal service is considered attributable to that file only when `Immutable`, `Favorite`, and `AutoConnect` are all true under the same ConnMan unique bus owner; an SSID/security match or any one marker is insufficient. Observation may retain the exact object path for correlation, but R5 does not use it to delete or disconnect a service. Do not delete all of `/var/lib/connman`.

## Availability recovery

Track the D-Bus owner of `net.connman`. On loss, cancel pending method calls and mark application state unavailable without deleting the canonical store. On reappearance, rediscover objects and reconcile the current canonical generation. Retry uses bounded exponential backoff with jitter and a ceiling; it is not a tight loop or a board reboot requirement.
