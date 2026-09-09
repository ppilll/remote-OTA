# EdgeGuard Remote OTA — R4 Documentation Package

## 1. Purpose

This package is the architecture and verification contract for **R4 — Automatic Wi-Fi Remote OTA**.

R4 asks one question:

> Can the device complete the OTA lifecycle repeatedly, without an engineer logging in to the target, while preserving safety across network loss, process restart, reboot, health failure, and report failure?

R4 is an incremental hardening and evidence stage. It is not a redesign of the R0–R3/BOOT-GATE system.

## 2. Evidence vocabulary

Every normative claim in this package uses one of these classes.

| Label | Meaning |
|---|---|
| **FROZEN FACT** | Accepted R0–R3/BOOT-GATE result. R4 must preserve it unless new contradictory evidence is recorded. |
| **CODE REVIEWED** | Present in the reviewed GitHub R3 implementation. This is not target execution evidence. |
| **HOST VERIFIED** | Demonstrated by host-side tests or fixtures. This is not target verification. |
| **TARGET OBSERVED** | A point-in-time fact collected from the RK3588 target. It does not by itself prove a full campaign. |
| **TARGET VERIFIED** | Demonstrated on the real target by a bounded campaign with retained evidence. |
| **ARCHITECTURE DECISION** | R4 design choice approved by this package. |
| **PROPOSED VALUE** | Initial value to implement and measure; it remains tunable until target evidence freezes it. |
| **DEFERRED** | Intentionally outside R4. |

Absence of a `TARGET VERIFIED` label must never be upgraded by inference from code, host tests, or a single target snapshot.

## 3. Frozen entry position

- **FROZEN FACT — TARGET VERIFIED:** R3 real-device happy path passed.
- **FROZEN FACT — TARGET VERIFIED:** Native Rockchip A/B behavior passed the prior target campaign.
- **FROZEN FACT:** RAUC version remains 1.5.1 with the existing custom bootloader/backend architecture.
- **FROZEN FACT:** R4 is cleared to start.
- **CODE REVIEWED:** GitHub `main` was reviewed at commit `2be352dbccbaa31beeb0ee402e85b0defb15cef2` (`R3-验证通过完整版`).
- **ARCHITECTURE DECISION:** GitHub is the authoritative repository view for this work. No additional Ubuntu working-tree audit is required by this package.

## 4. Current target entry baseline

The following is the user-supplied live snapshot. It is a baseline, not an R4 campaign verdict.

| Item | Observed value | Evidence class |
|---|---|---|
| Release | `1.2.4` | TARGET OBSERVED |
| Build ID | `rk3588-r3h-1.2.4-001` | TARGET OBSERVED |
| Current slot | `b` | TARGET OBSERVED |
| Primary slot | `b` | TARGET OBSERVED |
| Slot A | `confirmed-good` | TARGET OBSERVED |
| Slot B | `confirmed-good` | TARGET OBSERVED |
| Agent | Running | TARGET OBSERVED |
| `autostart-disabled` | Absent | TARGET OBSERVED |
| Durable state | `REPORT_SUCCESS` | TARGET OBSERVED |
| Attempt ID | `c035795a-2a2e-4c23-8dce-8457fb91a579` | TARGET OBSERVED |
| `wlan0` | Present, administratively/runtime down | TARGET OBSERVED |
| Reason Wi-Fi is down | User intentionally did not connect Wi-Fi | TARGET OBSERVED context |
| `connmand` | Running | TARGET OBSERVED |
| `wpa_supplicant` | Running | TARGET OBSERVED |
| `/userdata` | Mounted read/write and writable | TARGET OBSERVED |
| Configured server | `http://10.119.65.50:8000` | TARGET OBSERVED |
| Reporting mode | `legacy` | TARGET OBSERVED |

Interpretation:

- The installed firmware is already accepted: slot `b` is primary and both slots are confirmed-good.
- `REPORT_SUCCESS` is a durable, unfulfilled external reporting obligation after committed success. It is not an install failure and not a health failure.
- The attempt must not be reinstalled, rolled back, or marked good again merely because reporting is pending.
- `wlan0` being down in this snapshot is expected because Wi-Fi was intentionally not connected. It must not be used as evidence that the firmware is unhealthy.
- A clean R4 campaign baseline requires the pending report obligation to complete and durable state to return to `IDLE`; that closure is not claimed by this package.

## 5. R4 architecture decisions

1. **Preserve R3 orchestration.** No state-machine rewrite and no durable-state schema change are planned.
2. **Preserve built-in health.** Add one bounded, local-only external health hook for the Wi-Fi control plane.
3. **Do not equate network availability with firmware health.** AP, DHCP, DNS, Internet, and OTA-server reachability are not hard firmware-health gates.
4. **Use the existing lab endpoint contract.** R4 uses `http://10.119.65.50:8000` as a stable laboratory address.
5. **Defer persistent endpoint override.** It belongs to R5 unless the stable lab IP cannot be guaranteed.
6. **Extend reporting additively.** The server must accept both legacy and optional extended report fields before the R4 candidate enables Agent extended mode.
7. **Use two ownership-separated Codex workstreams.** One owns target health/Buildroot/tests; one owns backward-compatible server reporting.

## 6. Package map

| File | Purpose |
|---|---|
| `02_ARCHITECTURE_CONTEXT.md` | Frozen platform, component boundaries, reviewed R3 behavior, and R4 deltas |
| `03_AUTOMATION_INVARIANTS.md` | Safety properties that all implementation and campaigns must preserve |
| `04_HEALTH_POLICY.md` | Built-in plus external health contract and Wi-Fi semantics |
| `05_FAILURE_RECOVERY_SPEC.md` | Required behavior for every relevant interruption/failure class |
| `07_STATE_MACHINE_DELTA.md` | Exact delta against the frozen R3 state machine |
| `08_TEST_PLAN.md` | Host, SDK/Buildroot, and real-target campaign matrix |
| `09_CODEX_THREADS.md` | Ownership split, integration gates, and handoff requirements; not executable prompts |

## 7. Scope boundaries

R4 does not redesign or add:

- RTL8733BU bring-up or ConnMan architecture;
- R2 manifest, artifact, or HTTP Range architecture;
- GPT layout, U-Boot A/B algorithm, or Rockchip `AvbABData` format;
- RAUC 1.5.1 custom bootloader architecture;
- the R3 Agent state machine from scratch;
- BLE provisioning, fleet/cloud control, watchdog campaigns, or power-cut campaigns;
- persistent runtime endpoint provisioning, unless the stable lab IP contract proves impossible.

## 8. Document precedence and change control

When documents appear to conflict, apply this order:

1. Frozen R0–R3/BOOT-GATE facts and retained target evidence.
2. Safety invariants in `03_AUTOMATION_INVARIANTS.md`.
3. Architecture decisions in this package.
4. Proposed values, which may be tuned only with recorded evidence.

Any implementation discovery that appears to require changes to `main.c`, `state_machine.c`, `persistence.c`, the durable-state schema, Native A/B semantics, or RAUC architecture must stop at a documented finding. It is not implicitly authorized by this package.

## 9. Current R4 verdict

```text
R4 entry: CLEARED
R4 implementation: NOT YET VERIFIED
R4 host verification: NOT YET RECORDED FOR THE DELTA
R4 SDK/Buildroot verification: NOT YET RECORDED
R4 target campaigns: NOT YET VERIFIED
Current reporting debt: OPEN (durable REPORT_SUCCESS)
```

