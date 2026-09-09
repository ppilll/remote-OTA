# R5 Implementation Plan

## Delivery shape

R5 adds one independent daemon, `edgeguard-provisioningd`, and makes two narrow changes to the existing OTA Agent:

1. a root-only local `CHECK_UPDATE_NOW` socket;
2. validated runtime endpoint refresh at each new `IDLE` poll/check boundary.

The provisioning daemon owns BLE, provisioning transactions, Wi-Fi secrets, the canonical persistent store, ConnMan provisioning, authorization, and presentation of read-only status. The OTA Agent continues to own all OTA decisions and RAUC/Native A/B orchestration.

## Dependency sequence

### Thread 1 — data and network backend

- define public provisioning model/store/endpoint/ConnMan APIs;
- implement atomic `/userdata/edgeguard-provisioning/` storage;
- generate and revoke the derived ConnMan `.config`;
- validate endpoint overrides;
- expose no BlueZ surface and make no OTA Agent changes.

Thread 1 freezes the public headers consumed by Threads 2 and 3.

### Thread 2 — BLE product surface

- implement `edgeguard-provisioningd` main loop and GDBus objects;
- register GATT application, advertisement, and pairing agent;
- implement physical-presence window and per-peer authorization;
- implement framed protocol, transactions, result notifications, and status serialization;
- call Thread 1 APIs without changing Thread 1 backend semantics.

### Thread 3 — integration

- add the OTA Agent Unix-domain control socket;
- make the Agent refresh the runtime endpoint only at the frozen safe boundary;
- implement state-aware mutation gating and read-only state consumption;
- add the Buildroot package, SysV service, package wiring, and merged static/test artifacts;
- reconcile documentation with the final public interfaces without redesigning them.

## Interface freeze gates

Thread 1 must report the persistent schemas, endpoint validator contract, ConnMan apply/revoke API, and error ownership. Thread 2 must report the D-Bus object tree, UUID/property table, protocol framing, and authorization callbacks. Thread 3 consumes those interfaces and owns only necessary integration edits.

No thread changes the OTA state names, persistent OTA schema v1, Server API, RAUC backend semantics, release identity, or Native A/B state rules.

## Implementation acceptance

For a Codex thread, acceptance means:

- all assigned files are present and internally consistent;
- inputs are bounded and errors typed;
- secrets are not logged or copied into Agent state;
- source/static review finds no cross-thread ownership violation;
- the completion report lists changed files, exposed interfaces, and remaining verification work;
- the four no-claim lines required by `AGENTS.md` are present.

Build, test, SDK, target, hash, GitHub, and commit status are deliberately not Codex completion gates.

## Later evidence gates

Host tests, SDK builds, and target campaigns are separate post-implementation activities described in `11_TEST_PLAN.md` and `14_TARGET_VALIDATION_PLAN.md`. Only those activities may promote an R5 claim to their respective evidence level.
