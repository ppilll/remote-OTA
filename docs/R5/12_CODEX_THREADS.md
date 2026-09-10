# R5 Codex Threads and Reading Packages

## Execution rule

Use exactly three sequential threads: Thread 1, Thread 2, Thread 3. Every path below is relative to the repository root into which this package is copied. Each thread has its own explicit list of real repository paths; no aggregate baseline alias is used.

The current repository contains the frozen R4 package at `docs/R4/01_FINAL_BASELINE.md` through `docs/R4/06_EVIDENCE_REFERENCES.md`. Those real files, the current OTA Agent, `edgeguard-rk-ab`, and Buildroot package files form the R4 baseline. R5 implementation work may update references to them but must not edit the frozen R4 evidence.

## Thread 1 — persistence, ConnMan, endpoint

### Reading document package

1. `AGENTS.md`
2. `docs/R5/00_README.md`
3. `docs/R5/01_IMPLEMENTATION_PLAN.md`
4. `docs/R5/02_ARCHITECTURE_CONTEXT.md`
5. `docs/R5/05_PROVISIONING_STORE_SPEC.md`
6. `docs/R5/06_CONNMAN_INTEGRATION.md`
7. `docs/R5/07_ENDPOINT_CONFIG_SPEC.md`
8. `docs/R5/09_CONCURRENCY_POLICY.md`
9. `docs/R5/10_ERROR_MODEL.md`
10. `docs/R5/11_TEST_PLAN.md`
11. `docs/R5/15_SECRET_HANDLING.md`
12. `docs/R3/CODEX_EXECUTION_PACKAGE/R3F_CODEX_MASTER.md`
13. `docs/BOOT-GATE/00_README.md`
14. `docs/BOOT-GATE/01_BOOT_GATE_STATUS_FREEZE.md`
15. `docs/BOOT-GATE/02_ARCHITECTURE_FREEZE.md`
16. `docs/BOOT-GATE/03_EVIDENCE_LEVELS_AND_MATRIX.md`
17. `docs/BOOT-GATE/04_TARGET_NATIVE_AB_EVIDENCE.md`
18. `docs/BOOT-GATE/05_EDGEGUARD_RK_ABCTL_SEMANTICS.md`
19. `RK3588_app/edgeguard-remote-ota-agent/include/edgeguard_ota/model.h`
20. `RK3588_app/edgeguard-remote-ota-agent/include/edgeguard_ota/config.h`
21. `RK3588_app/edgeguard-remote-ota-agent/include/edgeguard_ota/persistence.h`
22. `RK3588_app/edgeguard-remote-ota-agent/src/config.c`
23. `RK3588_app/edgeguard-remote-ota-agent/src/persistence.c`
24. `RK3588_app/edgeguard-remote-ota-agent/src/main.c`
25. `buildroot-external/package/edgeguard-remote-ota/agent.conf`
26. `buildroot-external/package/edgeguard-remote-ota/edgeguard-remote-ota.mk`
27. `buildroot-external/package/edgeguard-remote-ota/S99edgeguard-remote-ota`

### Scope and ownership

Own the provisioning model and backend files under:

```text
RK3588_app/edgeguard-provisioningd/include/edgeguard_provisioning/
    model.h
    store.h
    endpoint.h
    connman.h
RK3588_app/edgeguard-provisioningd/src/
    store.c
    endpoint.c
    connman.c
```

Thread 1 may add backend-focused test sources. It does not own BlueZ/GATT files, existing OTA Agent files, Buildroot wiring, or SysV scripts. End by freezing the public function signatures, data ownership, schemas, and error mappings for Threads 2/3.

## Thread 2 — BlueZ GATT, advertising, pairing/authorization, protocol

### Reading document package

1. `AGENTS.md`
2. `docs/R5/00_README.md`
3. `docs/R5/01_IMPLEMENTATION_PLAN.md`
4. `docs/R5/02_ARCHITECTURE_CONTEXT.md`
5. `docs/R5/03_GATT_SPEC.md`
6. `docs/R5/04_BLE_SECURITY_POLICY.md`
7. `docs/R5/05_PROVISIONING_STORE_SPEC.md`
8. `docs/R5/08_LOCAL_IPC_SPEC.md`
9. `docs/R5/09_CONCURRENCY_POLICY.md`
10. `docs/R5/10_ERROR_MODEL.md`
11. `docs/R5/11_TEST_PLAN.md`
12. `docs/R5/14_TARGET_VALIDATION_PLAN.md`
13. `docs/R5/15_SECRET_HANDLING.md`
14. `docs/R3/CODEX_EXECUTION_PACKAGE/R3F_CODEX_MASTER.md`
15. `docs/BOOT-GATE/00_README.md`
16. `docs/BOOT-GATE/01_BOOT_GATE_STATUS_FREEZE.md`
17. `docs/BOOT-GATE/02_ARCHITECTURE_FREEZE.md`
18. `docs/BOOT-GATE/03_EVIDENCE_LEVELS_AND_MATRIX.md`
19. `RK3588_app/edgeguard-remote-ota-agent/include/edgeguard_ota/model.h`
20. `RK3588_app/edgeguard-remote-ota-agent/include/edgeguard_ota/state_machine.h`
21. `RK3588_app/edgeguard-remote-ota-agent/include/edgeguard_ota/persistence.h`
22. `RK3588_app/edgeguard-remote-ota-agent/src/main.c`
23. Thread 1's completed public headers: `RK3588_app/edgeguard-provisioningd/include/edgeguard_provisioning/model.h`, `store.h`, `endpoint.h`, and `connman.h`

### Scope and ownership

Own `edgeguard-provisioningd` main, GDBus ObjectManager/GATT/advertising/agent adapters, input/window handling, protocol reassembly, authorization, result/status serialization, and their headers/tests. Do not alter Thread 1 backend implementations or the existing OTA Agent. Call only Thread 1's frozen public interfaces.

End by freezing the D-Bus object tree, UUID/property registry, authorization callback contract, status source expectations, and source list for Thread 3.

## Thread 3 — Agent IPC/runtime endpoint, Buildroot/SysV, integration

Thread 3 starts only after Thread 1 and Thread 2 changes are present.

### Reading document package

1. `AGENTS.md`
2. every file in `docs/R5/00_README.md` through `docs/R5/15_SECRET_HANDLING.md`
3. `docs/R3/CODEX_EXECUTION_PACKAGE/R3F_CODEX_MASTER.md`
4. `docs/BOOT-GATE/00_README.md`
5. `docs/BOOT-GATE/01_BOOT_GATE_STATUS_FREEZE.md`
6. `docs/BOOT-GATE/02_ARCHITECTURE_FREEZE.md`
7. `docs/BOOT-GATE/03_EVIDENCE_LEVELS_AND_MATRIX.md`
8. `docs/BOOT-GATE/04_TARGET_NATIVE_AB_EVIDENCE.md`
9. `docs/BOOT-GATE/05_EDGEGUARD_RK_ABCTL_SEMANTICS.md`
10. `docs/BOOT-GATE/06_BUILD_ARTIFACT_AND_SAFETY_BASELINE.md`
11. `docs/BOOT-GATE/07_RAUC_NEXT_STAGE_HANDOFF.md`
12. `docs/BOOT-GATE/08_COMMANDS_AND_CHECKS.md`
13. `RK3588_app/edgeguard-remote-ota-agent/include/edgeguard_ota/model.h`
14. `RK3588_app/edgeguard-remote-ota-agent/include/edgeguard_ota/config.h`
15. `RK3588_app/edgeguard-remote-ota-agent/include/edgeguard_ota/state_machine.h`
16. `RK3588_app/edgeguard-remote-ota-agent/include/edgeguard_ota/persistence.h`
17. `RK3588_app/edgeguard-remote-ota-agent/include/edgeguard_ota/rauc_adapter.h`
18. `RK3588_app/edgeguard-remote-ota-agent/src/main.c`
19. `RK3588_app/edgeguard-remote-ota-agent/src/config.c`
20. `RK3588_app/edgeguard-remote-ota-agent/src/state_machine.c`
21. `RK3588_app/edgeguard-remote-ota-agent/src/persistence.c`
22. `RK3588_app/edgeguard-remote-ota-agent/src/rauc_adapter.c`
23. `RK3588_app/edgeguard-remote-ota-agent/tests/test_core_source_contract.py`
24. `RK3588_app/edgeguard-rk-ab/edgeguard-rk-abctl.c`
25. `RK3588_app/edgeguard-rk-ab/edgeguard-rk-ab-backend`
26. `buildroot-external/Config.in`
27. `buildroot-external/external.mk`
28. `buildroot-external/package/edgeguard-remote-ota/Config.in`
29. `buildroot-external/package/edgeguard-remote-ota/edgeguard-remote-ota.mk`
30. `buildroot-external/package/edgeguard-remote-ota/S99edgeguard-remote-ota`
31. Thread 1 and Thread 2 completed `RK3588_app/edgeguard-provisioningd/` tree

### Scope and ownership

Own the smallest required OTA Agent IPC/runtime-endpoint changes, their headers/tests, `buildroot-external/package/edgeguard-provisioning/**`, top-level Buildroot wiring, provisioning SysV script, and final cross-component static consistency edits.

Do not redesign Thread 1/2 interfaces, the R4 state machine, RAUC adapter, or Native A/B. Documentation edits may only align names/signatures discovered during integration; any proposed decision change must be reported instead of silently implemented.

## Completion report for every thread

List changed files, source/static review performed, interfaces handed to the next thread, and remaining later-verification items. Finish with exactly the four status lines required by `AGENTS.md`.
