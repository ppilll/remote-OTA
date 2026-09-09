# R4 Codex Workstream Plan

## 1. Decision

```text
Codex required:     YES
Number of threads:  2
Scope:              incremental and ownership-separated
```

This file is a work-allocation contract. It deliberately does **not** contain ready-to-run thread prompts.

## 2. Shared constraints

Both threads must read all files in `docs/R4/` before changing code and must preserve:

- the frozen R3 state machine and durable-state schema;
- same-attempt install-once and uncertain-install non-replay behavior;
- release/slot guards around mark-good and rollback;
- RAUC 1.5.1 and existing custom bootloader/backend architecture;
- Native Rockchip A/B semantics;
- existing manifest, artifact, and Range behavior;
- S99's narrow process-lifecycle role;
- the laboratory endpoint `http://10.119.65.50:8000` for R4;
- strict evidence classification with no target claims from host tests.

Default forbidden files for both threads:

```text
main.c
state_machine.c
persistence.c
```

If a relevant failing test appears to require one of these files, that thread stops at a written escalation finding. It must not make the change speculatively.

## 3. Thread 1 — Target health, Buildroot integration, and Agent-side tests

### Ownership

Thread 1 exclusively owns the target-side R4 delta:

- the new local health-hook source/script at the repository's appropriate package/overlay location;
- installation at `/usr/libexec/edgeguard/edgeguard-health-check`;
- the Buildroot package recipe/install changes for that hook;
- production Agent configuration, including `[health]` hook/timeout;
- R4 candidate selection of the existing Agent extended reporting mode;
- health-hook and Agent-side host tests;
- relevant documentation/test notes produced by implementation.

Thread 1 must determine actual repository paths from the checked-out tree. The installed target path is fixed; this document does not invent a source-tree path.

### Required behavior

- Keep all existing built-in health checks.
- Check only local `wlan0`, `connmand`, `wpa_supplicant`, and ConnMan Wi-Fi technology.
- Perform read-only observation and provide bounded diagnostics.
- Do not require AP association, IP, DNS, Internet, or OTA-server reachability.
- Do not run `connmanctl enable wifi` as a health test.
- Implement the **PROPOSED** three-consecutive-sample, five-second interval policy within the 30-second timeout.
- Do not add an Agent state for stabilization.
- Ensure hook failure uses the existing health-failure path and can never mark good.
- Preserve existing R3 orchestration regression results.

### Reporting configuration ownership

To avoid overlapping edits, only Thread 1 touches target `agent.conf` or its production template. It may prepare `mode=extended`, but that candidate configuration is integration-gated and must not be deployed against the legacy-only R2 server.

The combined R4 integration order must land/deploy Thread 2's compatible server before a target running Thread 1's extended configuration begins a campaign.

### Out of scope

- server models, handlers, or server tests;
- new endpoints or endpoint discovery;
- persistent runtime override/config precedence;
- changes to polling/downloader/install/rollback algorithms without an escalation finding;
- Native A/B, RAUC, U-Boot, GPT, kernel, BLE, fleet, or cloud changes.

### Handoff requirements

Thread 1 returns:

- files changed;
- precise architecture delta;
- tests added and executed with results;
- R3 regression results;
- Buildroot/package impact;
- proposed-value implementation details and any timing concern;
- items requiring SDK/Buildroot or target verification;
- explicit statement that no target verification is claimed.

## 4. Thread 2 — Backward-compatible server reporting

### Ownership

Thread 2 exclusively owns the server-side R4 delta:

- the existing device-report request model/schema;
- the existing `POST /device/report` handler as needed;
- structured server report logging;
- server unit/integration tests for legacy and extended compatibility;
- relevant server implementation/test notes.

Reviewed likely areas include `host_app/models.py`, `host_app/main.py`, and server tests, but the thread must confirm actual paths before editing.

### Required behavior

Preserve exact legacy acceptance for:

```text
device_id
current_version
status
timestamp
```

Add optional, type-validated support matching the Agent's existing extended mode:

```text
attempt_id
target_version
build_id
agent_state
error_code
timestamp_valid
uptime_ms
```

Extend accepted status values only to the set actually emitted by the existing Agent extended implementation. Do not invent a new status taxonomy.

Structured logs record, when supplied:

```text
attempt_id
target_version
build_id
agent_state
error_code
```

Exact legacy payloads and clients remain unchanged. Malformed optional fields and unrelated unknown fields remain rejected. The endpoint continues to return the established success response (expected by reviewed tests as `204`).

### Out of scope

- target `agent.conf` or Buildroot files;
- Agent C source or durable state;
- `GET /manifest.json` behavior;
- `GET /update.raucb`, full-download, or Range semantics;
- artifact publication, signing, or RAUC bundle changes;
- new report endpoint, database/fleet/cloud redesign, or endpoint provisioning.

### Handoff requirements

Thread 2 returns:

- files changed;
- exact legacy and extended accepted schemas;
- status values added and their direct Agent-code basis;
- tests added/executed with results;
- proof that legacy report, manifest, and Range regressions remain passing;
- example structured log fields with no secrets;
- server deployment requirement for the R4 campaign;
- explicit statement that no target verification is claimed.

## 5. Ownership matrix

| Area/file class | Thread 1 | Thread 2 |
|---|---:|---:|
| Health hook | Owner | No edit |
| Buildroot package/install | Owner | No edit |
| Target Agent configuration | Owner | No edit |
| Health/Agent host tests | Owner | No edit |
| Server report model | No edit | Owner |
| Server report handler/logging | No edit | Owner |
| Server compatibility tests | No edit | Owner |
| `main.c` | Escalation only | No edit |
| `state_machine.c` | Escalation only | No edit |
| `persistence.c` | Escalation only | No edit |
| Native A/B / RAUC / U-Boot / GPT | No edit | No edit |

## 6. Integration order and gates

The threads can develop independently because their write ownership does not overlap. Integration follows these gates:

1. Review both handoffs against the invariants.
2. Land and host-verify the backward-compatible server extension.
3. Deploy that server at `http://10.119.65.50:8000`.
4. Integrate/build the target candidate with the health hook and `mode=extended`.
5. Run combined host regressions and SDK/Buildroot verification.
6. Close the inherited `REPORT_SUCCESS` baseline naturally and establish `IDLE`.
7. Run target campaigns in `08_TEST_PLAN.md`.

Thread 1's extended configuration must never be used against an unextended server. Thread 2's server may be deployed before the target candidate because it remains legacy-compatible.

## 7. Merge and review checks

The integration reviewer must confirm:

- no overlapping file ownership occurred;
- no unapproved changes touched the three guarded Agent files;
- no durable-state schema/version change occurred;
- the hook is local-only and bounded;
- the server still rejects unrelated fields;
- legacy report tests still pass;
- manifest and Range behavior are unchanged;
- evidence statements are labeled correctly;
- target deployment/campaign work is still presented as pending until actually performed.

## 8. Conditions that stop a thread

A thread stops and reports rather than expanding scope if it discovers:

- the R3 implementation differs materially from the reviewed architecture;
- the hook cannot be integrated without a state-machine or persistence change;
- the Agent extended payload does not match the reviewed optional-field list;
- server compatibility would require a new endpoint or breaking legacy clients;
- the stable lab endpoint cannot be guaranteed;
- a frozen regression fails for reasons caused by the proposed R4 delta;
- Native A/B/RAUC/U-Boot changes appear necessary.

These findings require architecture review. They are not permission for a thread to improvise a redesign.

