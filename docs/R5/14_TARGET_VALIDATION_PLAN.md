# R5 Target Validation Plan

## Status before execution

Already observed: BlueZ 5.77 and required manager interfaces; ConnMan 1.40; `/userdata` persistent; `/var/lib/connman` rootfs-local; provisioning-file property transition false→true→false; `adc-keys` device name; R4 target `1.2.5` / `rk3588-r4-1.2.5-001` with healthy A/B and Agent `IDLE`.

Not yet verified: product daemon, pairing/authorization path, exact keycode/hold mapping, WPA-PSK generated profile, real GATT protocol, reboot/A-B persistence, runtime endpoint consumption, and critical-state rejection.

Every campaign records target release/build, current/primary/last_boot slot, monotonic time, provisioning transaction ID, peer context, authorization/window state, persistent generation/schema, ConnMan service/state, effective endpoint, Agent state, result, and redacted error. Never record a passphrase, link key, raw secret payload, or private token.

## Campaign 1 — physical presence and BLE product surface

1. Map the exact `adc-keys` keycode and safe hold duration with read-only input observation; update deployment config, not source constants.
2. Prove closed default: not pairable, not provisioning-advertising, sensitive write rejected.
3. Open the 120-second window and prove advertisement name/service UUID, phone/PC discovery, connection, service and all characteristics.
4. Prove unpaired write rejection; pair with the registered agent; observe bond/encrypted state; prove only the authorized peer in the open window can write.
5. Close/expire the window and prove writes and partial transactions are rejected/discarded.
6. Restart `bluetoothd` in a safe state and prove owner recovery, GATT re-registration, and policy-correct advertising without board reboot.

## Campaign 2 — Wi-Fi provisioning and reboot

1. Select a named WPA/WPA2-PSK lab AP and record the PSK only in the operator's protected input.
2. Send `SET_WIFI` only over the product GATT path.
3. Observe atomic store generation, derived ConnMan profile, `Immutable/Favorite/AutoConnect=true`, association, address DHCP/IP/route, and optional server reachability.
4. Do not use manual secret entry, `connmanctl connect`, `wpa_cli`, or manual profile copying as a repair.
5. Reboot without re-entering the credential. Prove canonical load, derived-file reconstruction, automatic reconnection, IP, and reachability.
6. Replace with a known alternate credential and verify success/rollback semantics; test wrong PSK without losing the prior working configuration.
7. Run `FORGET_WIFI`; prove canonical and owned derived profile are gone, property state revokes, and Agent/A-B state is unchanged.

## Campaign 3 — full-rootfs A/B persistence

Starting from the confirmed-good R4 baseline, provision Wi-Fi, prove connectivity, publish a valid signed candidate through the existing release path, and allow the existing R4 Agent→RAUC→Native A/B path to perform the update.

After automatic boot into the candidate, prove `/userdata` retained the canonical generation, the rootfs-local ConnMan file was reconstructed, Wi-Fi reconnected without credential entry, the Agent completed its existing health/mark-good/report obligations, and A/B state is correct. No manual RAUC install, slot mutation, Agent-state deletion, profile copy, or Wi-Fi secret entry is allowed in the measurement window.

## Campaign 4 — endpoint configuration

1. Read the current effective endpoint and source.
2. GATT-write another valid lab base URL; prove atomic generation update and no Agent restart.
3. Trigger or wait for a new normal check; prove the Agent uses the new endpoint for manifest/report resolution.
4. Reboot and repeat; then cross one full-rootfs A/B update and repeat.
5. Write invalid scheme/syntax/control-character/oversized cases; prove rejection and retention of the prior effective endpoint.
6. Corrupt a copy only in a controlled campaign; prove immutable-default fallback and redacted diagnostics.

## Campaign 5 — status/control/concurrency

- compare BLE release/build, Agent state, attempt ID, error, and slot with authoritative read-only sources;
- send `CHECK_UPDATE_NOW` in `IDLE`; prove the existing check path and normal no-update return;
- prove there is no direct RAUC, reboot, bundle URL, or boot-control action;
- for every matrix row, host-verify the decision; on target, safely sample busy/critical states such as `DOWNLOADING` and `INSTALLING` only within an approved OTA campaign;
- prove mutations and duplicate checks return busy/rejected without changing persistent provisioning or disturbing the attempt;
- disconnect mid-fragment and mid-notification; prove only complete committed transactions survive.

## Verdict boundary

R5 target PASS requires product GATT/advertising, authorized provisioning, reboot persistence, full-rootfs A/B Wi-Fi and endpoint persistence, accurate status, safe check control, negative security cases, and unchanged R4 invariants. Existing interface inventory and `.config` probe alone are architecture evidence, not R5 product PASS.
