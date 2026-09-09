# R4 Automation Invariants

## 1. Purpose

These invariants are stronger than implementation preferences. Every code change, host test, SDK build, target campaign, and evidence review must preserve them.

Status meanings:

- **Satisfied in R3:** supported by the reviewed implementation and/or prior host evidence.
- **Target campaign required:** R4 still needs real-device evidence for the unattended claim.
- **New R4 delta:** implementation and verification are required.

## 2. Core safety invariants

| ID | Invariant | Current basis | R4 obligation |
|---|---|---|---|
| INV-01 | One attempt may invoke install at most once. | CODE REVIEWED / HOST VERIFIED | Preserve; target evidence must show no duplicate install. |
| INV-02 | An uncertain install outcome must never become an automatic reinstall of the same attempt. | CODE REVIEWED / HOST VERIFIED | Preserve exactly. |
| INV-03 | Destructive action occurs only after the corresponding durable state/intent is safely persisted. | CODE REVIEWED / HOST VERIFIED | Preserve persistence boundary. |
| INV-04 | Candidate mark-good requires the expected candidate slot and exact candidate release identity. | CODE REVIEWED | Preserve both guards. |
| INV-05 | Health failure must never reach mark-good. | CODE REVIEWED / HOST VERIFIED | Target rollback campaign required. |
| INV-06 | Mark-bad/rollback may target only the expected current candidate; safety guards must prevent both slots becoming unbootable. | CODE REVIEWED | Preserve Native A/B guards; target campaign required. |
| INV-07 | Firmware acceptance and external reporting are separate commits. | CODE REVIEWED / HOST VERIFIED | Preserve durable `REPORT_SUCCESS`. |
| INV-08 | Report retry after committed success must not replay install, reboot, health, mark-good, or rollback. | CODE REVIEWED / HOST VERIFIED | Target report-outage campaign required. |
| INV-09 | Server/AP/DNS/Internet outage alone must not mark a slot bad or trigger rollback. | CODE REVIEWED plus R4 decision | Verify in target campaigns. |
| INV-10 | Failure to establish safe durable state must fail closed before destructive action or reboot. | CODE REVIEWED / HOST VERIFIED | Preserve and regression-test. |
| INV-11 | The Agent must not directly write the misc partition or raw block devices; it uses established RAUC/A-B interfaces. | CODE REVIEWED path | Preserve; review R4 changes. |
| INV-12 | The init script manages Agent lifecycle only; it does not become an OTA state-machine executor. | CODE REVIEWED | Preserve. |
| INV-13 | Maintenance disablement is explicit: an existing `autostart-disabled` marker may prevent start, but unattended R4 campaigns require the marker absent. | FROZEN FACT / TARGET OBSERVED absent | Record marker state in every campaign preflight. |
| INV-14 | A same-version/no-update response returns to long-running idle operation without A/B mutation. | CODE REVIEWED | Target long-running discovery evidence required. |
| INV-15 | A partial download is appended only after exact Range validation; otherwise restart/reject safely. | CODE REVIEWED / HOST VERIFIED | Target interruption/resume campaign required. |
| INV-16 | Health is device-local firmware readiness, not an environmental connectivity referendum. | R4 ARCHITECTURE DECISION | Enforce in hook/tests. |
| INV-17 | Legacy report clients remain valid after the R4 server extension. | New R4 delta | Host compatibility test required. |
| INV-18 | Extended reports are enabled on a candidate only when the campaign server accepts them. | R4 ARCHITECTURE DECISION | Enforce integration gate. |
| INV-19 | R4 must preserve durable-state schema compatibility; existing R3 state files remain readable with unchanged meanings. | R4 ARCHITECTURE DECISION | Regression test/review required. |
| INV-20 | The Agent must continue polling after a successful attempt returns to `IDLE`, enabling a later release without process replacement or engineer login. | R4 objective / CODE REVIEWED behavior | Two-direction unattended campaign required. |

## 3. Durable-state invariants

### 3.1 `INSTALLING` is not replay-safe

`INSTALLING` records that install invocation is at or beyond the destructive boundary. On restart or ambiguous command outcome, the system must not infer that it is safe to call `rauc install` again. The correct outcome remains durable error/manual diagnosis according to the frozen R3 behavior.

Forbidden changes include:

- converting `INSTALLING` into an ordinary transient-retry state;
- erasing the attempt and returning to `IDLE` after uncertainty;
- starting a new install solely because the Agent process restarted;
- using elapsed time as proof that installation did not occur.

### 3.2 `REPORT_SUCCESS` is a debt, not failure of accepted firmware

Once candidate health and mark-good have succeeded, `REPORT_SUCCESS` is the remaining delivery obligation. A report outage may keep this state durable for an unbounded operational interval, subject to retry policy, without undoing accepted firmware.

The current entry baseline is an example:

```text
slot b = current + primary + confirmed-good
durable state = REPORT_SUCCESS
attempt = c035795a-2a2e-4c23-8dce-8457fb91a579
```

For that attempt, reinstall, repeat mark-good, or rollback due only to reporting failure would violate INV-01, INV-07, INV-08, and INV-09.

### 3.3 Persistence must precede externally visible transitions

If a state transition cannot be made durable, the Agent must not proceed into the associated install/reboot/slot-mutation action. R4 changes must not weaken file synchronization, atomic update, or error propagation assumptions established by R3.

## 4. Health invariants

The full acceptance chain remains:

```text
exact candidate release identity
+ expected current candidate slot
+ /userdata persistence
+ RAUC and boot-control runtime checks
+ local Wi-Fi control-plane hook
```

The hook itself:

- returns only pass/fail plus diagnostics;
- is bounded by the Agent health timeout;
- performs read-only/local observation;
- never calls mark-good, mark-bad, reboot, RAUC install, block writes, or configuration mutation;
- does not require `wlan0` to be up/associated;
- does not require the OTA server to be reachable.

## 5. Reporting invariants

The endpoint remains `POST /device/report`.

Legacy fields remain valid:

```text
device_id
current_version
status
timestamp
```

The server extension may accept these optional Agent extended fields:

```text
attempt_id
target_version
build_id
agent_state
error_code
timestamp_valid
uptime_ms
```

Unknown unrelated fields remain rejected. Optional-field validation must not silently reinterpret malformed data. Structured logging may enrich observability but must not change success/commit ordering on the device.

## 6. Endpoint invariant

For R4 laboratory validation, `http://10.119.65.50:8000` is an operational contract. A server-address failure is an environment/configuration condition and must not be reclassified as candidate firmware health failure.

Persistent endpoint override is **DEFERRED to R5** unless a stable lab IP cannot be guaranteed. That exception requires an explicit architecture revisit; it is not authorization to add ad hoc precedence rules during implementation.

## 7. Evidence invariant

The final verdict must state each claim at its achieved level:

- code inspection cannot become `HOST VERIFIED`;
- host fixtures cannot become `TARGET VERIFIED`;
- one target snapshot cannot become a completed campaign;
- a target campaign without retained attempt/release/A-B/report evidence cannot be declared verified.

