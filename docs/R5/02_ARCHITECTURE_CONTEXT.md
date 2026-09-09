# R5 Architecture Context

## Frozen architecture plane

```text
BLE peer
   |
BlueZ 5.77 (system D-Bus)
   |
edgeguard-provisioningd
   |-- authorization + 120 s physical-presence window
   |-- GATT protocol and transaction manager
   |-- canonical store: /userdata/edgeguard-provisioning/
   |-- ConnMan 1.40 integration
   `-- read-only Agent status + narrow local control
             |                         |
             v                         v
/var/lib/connman/edgeguard-       existing R4 OTA Agent
provisioning.config (derived)          |
             |                         v
          ConnMan                    RAUC
             |                         |
            Wi-Fi                      v
                                Rockchip Native A/B
```

## Ownership

| Component | Exclusive responsibility |
|---|---|
| Provisioning daemon | Wi-Fi credentials, persistent runtime override, BLE protocol/security, ConnMan apply/revoke |
| OTA Agent | manifest/version/compatibility gates, download, RAUC orchestration, durable OTA state, reporting |
| RAUC | signed bundle verification and inactive-slot installation |
| `edgeguard-rk-ab` | RAUC-to-Rockchip A/B translation |
| U-Boot/SPL | boot selection, retry, fallback |
| `/userdata/edgeguard-provisioning/` | canonical provisioning authority across rootfs replacement |
| `/var/lib/connman/` | rootfs-local ConnMan runtime/derived state, not canonical authority |

## R4 contracts preserved

- Agent never writes block devices or `misc` directly.
- Provisioning never invokes `rauc install`, mark-good, mark-bad, set-primary, reboot, or raw boot control.
- Same `attempt_id` destructive-install count remains at most one.
- Uncertain install outcome never causes reinstall.
- Mark-good remains gated by expected slot, release identity, built-in health, and external health hook.
- A report failure after committed mark-good causes reporting retry only.
- Network/provisioning failure is not firmware-health failure.
- The durable OTA state schema and names remain unchanged.
- Server API remains `GET /manifest.json`, `GET /update.raucb`, and `POST /device/report` with manifest schema v1.

## R5 architectural decisions

1. Provisioning is a separate C daemon using GLib/GIO/GDBus and JSON-GLib. This matches the existing runtime while preserving the secret/OTA boundary.
2. ConnMan persistence uses Option B: canonical independent store, generated provisioning `.config`, rootfs-local derived copy.
3. The endpoint override is owned by provisioning and consumed by the Agent; `/etc/edgeguard-ota/agent.conf` remains the immutable fallback.
4. Agent control uses a Unix-domain socket, while status is read from the existing read-only `agent-state.json` and other read-only system sources.
5. BLE sensitive writes require link encryption, a paired/bonded peer, the physical-presence window, and application authorization. `NoInputNoOutput` pairing is not described as MITM-authenticated.
6. Advertising and pairability are off by default and enabled only during the provisioning window.
7. R5 v1 does not provide a factory-reset command. `FORGET_WIFI` is narrowly scoped to the provisioning credential and derived ConnMan profile.

## Evidence boundary

BlueZ/ConnMan capability and the ConnMan `.config` property transition are already target-observed. Product GATT behavior, exact `adc-keys` keycode, WPA-PSK end-to-end provisioning, persistence across reboot/A-B, and Agent endpoint consumption remain target-validation work.
