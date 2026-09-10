# R5 OTA and Provisioning Concurrency Policy

## Rule

R5 v1 does not stage deferred mutations. A mutable provisioning operation either commits in an eligible state or returns a typed busy/rejected error with no persistent change. Status reads remain available in every state.

Eligibility for a new BLE mutation is decided from a bounded, read-only snapshot of the existing Agent durable state immediately before commit and checked again before external apply. Missing, corrupt, changing, or ambiguous Agent state fails closed for that new mutation. Recovery reconciliation of previously committed canonical state is governed by the explicit exception below.

## Matrix

| Frozen Agent state | Status read | `SET_WIFI` / `FORGET_WIFI` | `SET_ENDPOINT` | `CHECK_UPDATE_NOW` | Factory reset |
|---|---:|---:|---:|---:|---:|
| `IDLE` | allow | allow | allow | allow | unsupported/reject |
| `CHECK_NETWORK` | allow | reject busy | reject busy | reject busy | unsupported/reject |
| `CHECK_UPDATE` | allow | reject busy | reject busy | reject busy | unsupported/reject |
| `PRECHECK` | allow | reject busy | reject busy | reject busy | unsupported/reject |
| `DOWNLOADING` | allow | reject busy | reject busy | reject busy | unsupported/reject |
| `VERIFY_DOWNLOAD` | allow | reject busy | reject busy | reject busy | unsupported/reject |
| `RAUC_VERIFY` | allow | reject critical | reject critical | reject busy | unsupported/reject |
| `INSTALLING` | allow | reject critical | reject critical | reject busy | unsupported/reject |
| `REBOOT_PENDING` | allow | reject critical | reject critical | reject busy | unsupported/reject |
| `BOOT_NEW_SLOT` | allow | reject critical | reject critical | reject busy | unsupported/reject |
| `HEALTH_CHECK` | allow | reject critical | reject critical | reject busy | unsupported/reject |
| `MARK_GOOD` | allow | reject critical | reject critical | reject busy | unsupported/reject |
| `REPORT_SUCCESS` | allow | reject critical | reject critical | reject busy | unsupported/reject |
| `ROLLBACK` | allow | reject critical | reject critical | reject busy | unsupported/reject |
| `ERROR` | allow | reject ambiguous | reject ambiguous | reject | unsupported/reject |
| unavailable/corrupt | partial/unavailable | reject fail-closed | reject fail-closed | IPC unavailable/reject | unsupported/reject |

The matrix governs new BLE mutations. The conservative `REPORT_SUCCESS` rule preserves the committed firmware/report obligation by rejecting new changes. Separately, startup/recovery reconciliation of an already committed canonical Wi-Fi generation into rootfs-local ConnMan derived state is allowed in every frozen Agent state, including `BOOT_NEW_SLOT`, `HEALTH_CHECK`, `MARK_GOOD`, and `REPORT_SUCCESS`. That recovery has no Agent-state, OTA-FSM, RAUC, reboot, or slot-mutation authority. It does not relax the stable-`IDLE` gate for a new `SET_WIFI`, `FORGET_WIFI`, or `SET_ENDPOINT` commit.

## Race handling

The state check and persistent commit are serialized against other provisioning mutations, but they cannot lock the Agent state machine. Therefore:

- before committing, read state and require stable `IDLE` across two observations or use a read-only Agent status query if Thread 3 can provide one without widening IPC v1;
- `CHECK_UPDATE_NOW` is not accepted while a mutation transaction owns the commit lock;
- after a Wi-Fi or endpoint commit begins, the daemon blocks new check requests until apply/result publication completes;
- if Agent state changes away from `IDLE` before commit, abort with no write;
- if it changes after a completed atomic commit, finish reconciliation without touching OTA state and report `PROVISIONING_RACE`; do not roll back an OTA action or kill/restart the Agent.
- startup/recovery reconciliation uses the serialized ConnMan worker but no Agent-state eligibility predicate; retryable failures use continuous capped backoff and do not wait for a daemon/board restart.

Thread 3 may implement a minimal in-process Agent `IDLE` snapshot helper, but it must not add a new durable state or provisioning-owned lock inside RAUC lifecycle code.

## Critical invariants

- provisioning never interrupts or restarts the Agent;
- provisioning never deletes or edits `agent-state.json`;
- provisioning failure never marks firmware unhealthy;
- an OTA check request never bypasses manifest/version/compatibility/hash/RAUC gates;
- same-attempt destructive install count remains at most one;
- read-only status is best effort and not a second state machine.

## Disconnect and process failure

BLE disconnect discards uncommitted fragment/staging buffers. A committed atomic store remains committed. Daemon crash recovery reconciles only the canonical provisioning generation. Agent crash/restart follows existing R4 recovery semantics and is not controlled by provisioning.
