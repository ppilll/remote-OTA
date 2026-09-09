# R5 Codex Execution Package

## Purpose

This package is the implementation source of truth for EdgeGuard Remote OTA R5: BLE provisioning, persistent Wi-Fi and endpoint configuration, and a narrow local control/status bridge to the existing R4 OTA Agent.

R5 adds a provisioning plane; it does not redesign the OTA data plane or the Rockchip Native A/B path.

## Frozen starting evidence

| Item | Confirmed baseline |
|---|---|
| Target release | `1.2.5` |
| Target build ID | `rk3588-r4-1.2.5-001` |
| OTA Agent | running; final durable state `IDLE` |
| A/B | current/primary/last_boot = `a/a/a`; A and B confirmed-good |
| BlueZ | 5.77; `GattManager1`, `LEAdvertisingManager1`, and `AgentManager1` present |
| ConnMan | 1.40 |
| Persistence | `/userdata` persists; `/var/lib/connman` is rootfs-local |
| ConnMan provisioning evidence | `.config` creation changed `Immutable/Favorite/AutoConnect` from false to true; removal changed them back to false |
| Physical input | an input device named `adc-keys` exists; its product keycode mapping is not frozen |

The target observations above authorize the design decisions in this package. They do not claim that the R5 implementation has been built or target-validated.

## Repository-baseline note

The repository snapshot examined for this package contains `docs/R3/`, `docs/BOOT-GATE/`, the OTA Agent sources, and the Rockchip A/B sources, but no `docs/R4/` directory. The thread reading lists therefore cite only paths that actually exist: the current OTA Agent/Buildroot files are the R4 implementation baseline, while the confirmed R4 target identity above is recorded as evidence. Do not invent `docs/R4/*` paths.

## Document map

| File | Purpose |
|---|---|
| `01_IMPLEMENTATION_PLAN.md` | ordering, interfaces, acceptance boundary |
| `02_ARCHITECTURE_CONTEXT.md` | component ownership and frozen contracts |
| `03_GATT_SPEC.md` | UUIDs, wire framing, operations, notifications |
| `04_BLE_SECURITY_POLICY.md` | pairing, authorization, window, advertising |
| `05_PROVISIONING_STORE_SPEC.md` | canonical persistent schema and transactions |
| `06_CONNMAN_INTEGRATION.md` | selected persistence option and apply/revoke flow |
| `07_ENDPOINT_CONFIG_SPEC.md` | precedence, validation, Agent refresh semantics |
| `08_LOCAL_IPC_SPEC.md` | Agent socket and read-only status source |
| `09_CONCURRENCY_POLICY.md` | state-by-operation matrix |
| `10_ERROR_MODEL.md` | stable errors and recovery classes |
| `11_TEST_PLAN.md` | later host/SDK/target verification plan |
| `12_CODEX_THREADS.md` | thread scope, ownership, and exact reading lists |
| `13_BUILDROOT_INTEGRATION.md` | package, dependencies, SysV lifecycle |
| `14_TARGET_VALIDATION_PLAN.md` | target campaigns and evidence rules |
| `15_SECRET_HANDLING.md` | secret ownership, redaction, storage limits |

## Execution order

Run Thread 1, then Thread 2, then Thread 3. Thread 3 is the integration owner and starts only after Thread 1 and Thread 2 changes are present in its working tree.

Each thread follows the repository-root `AGENTS.md`. Compilation, tests, hashes, GitHub checks, commits, pushes, and pull requests are outside the Codex thread completion condition. The test and target-validation documents define later evidence work; they do not authorize implementation threads to claim those evidence levels.

## R5 scope boundary

In scope: WPA/WPA2-PSK provisioning, persistent endpoint override, BLE status, `CHECK_UPDATE_NOW`, ConnMan provisioning-file integration, BlueZ lifecycle recovery, and Buildroot/SysV wiring.

Out of scope: BLE bundle transfer, direct RAUC or boot-control commands, arbitrary URLs per request, arbitrary shell, factory-reset GATT command, 802.1X, WEP, production PKI/TLS rollout, R4 state-machine redesign, and R6 fault-injection campaigns.
