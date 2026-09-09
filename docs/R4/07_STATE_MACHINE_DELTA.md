# R4 State-Machine Delta

## 1. Decision summary

**ARCHITECTURE DECISION:** R4 adds no durable state and does not redesign transition ownership.

```text
durable-state schema delta: NONE expected
new state names:            NONE
main orchestration rewrite: NO
install retry change:       NO
rollback semantic change:   NO
```

R4 changes inputs/configuration around two existing phases:

- `HEALTH_CHECK` gains a configured local Wi-Fi hook.
- `REPORT_SUCCESS` may emit the Agent's already-existing extended payload after server compatibility is deployed.

## 2. Frozen R3 graph

```text
IDLE
  -> CHECK_NETWORK
  -> CHECK_UPDATE
  -> PRECHECK
  -> DOWNLOADING
  -> VERIFY_DOWNLOAD
  -> RAUC_VERIFY
  -> INSTALLING
  -> REBOOT_PENDING

reboot

BOOT_NEW_SLOT
  -> HEALTH_CHECK
       | PASS
       v
     MARK_GOOD
       -> REPORT_SUCCESS
       -> IDLE

       | FAIL
       v
     guarded mark-bad
       -> ROLLBACK
       -> reboot/fallback recovery
```

Existing terminal/error and boot-fallback paths remain as implemented in R3.

## 3. Exact R4 deltas

| Existing phase | R3 behavior | R4 delta | Durable schema impact |
|---|---|---|---|
| `IDLE` / `CHECK_NETWORK` / `CHECK_UPDATE` | Long-running poll and transient retry | No transition change; collect unattended evidence | None |
| `DOWNLOADING` | Safe `.part`/Range resume | No transition change; collect target interruption evidence | None |
| `VERIFY_DOWNLOAD` / `RAUC_VERIFY` | Verify before install | No change | None |
| `INSTALLING` | Persisted destructive boundary; not replay-safe | No change | None |
| `REBOOT_PENDING` / `BOOT_NEW_SLOT` | Reboot and candidate recovery | No change | None |
| `HEALTH_CHECK` | Built-in checks plus optional executable hook | Configure/install local Wi-Fi hook; proposed bounded stabilization happens inside hook execution | None |
| `MARK_GOOD` | Candidate release and slot guards, then accept | No change | None |
| `REPORT_SUCCESS` | Durable, retryable post-commit report; current production uses legacy shape | Server accepts optional extended shape, then R4 candidate selects existing `extended` mode | None |
| `ROLLBACK` | Guarded candidate rejection/fallback | No change; collect target evidence | None |

## 4. Health integration

Configuration target:

```ini
[health]
hook=/usr/libexec/edgeguard/edgeguard-health-check
timeout_sec=30
```

**PROPOSED VALUE:** The hook obtains three consecutive successful local samples, five seconds apart, within the 30-second call budget.

This loop belongs inside the bounded hook behavior or its existing invocation contract; it does not require a new durable stabilization state. If implementation evidence shows that the existing runner cannot safely enforce this contract, the workstream must document the issue before changing Agent orchestration.

Hook pass/fail maps directly to the existing `HEALTH_CHECK` result. It cannot transition or mutate state itself.

## 5. Reporting integration

The endpoint remains:

```text
POST /device/report
```

Migration order:

1. Extend server validation/logging so exact legacy reports still succeed and the existing Agent extended shape is optionally accepted.
2. Host-verify legacy compatibility, extended success, malformed-field rejection, and unrelated-extra-field rejection.
3. Deploy the compatible server at the R4 lab endpoint.
4. Only then use the R4 candidate configuration:

```ini
[reporting]
mode=extended
```

5. Verify target report correlation without changing `REPORT_SUCCESS` retry/commit ordering.

Extended reporting does not create a new state, a new endpoint, or a new delivery guarantee.

## 6. Compatibility requirements

An R4 build must continue to read an R3 durable file without migration and preserve every existing field meaning, including the current baseline example:

```json
{
  "schema_version": 1,
  "state": "REPORT_SUCCESS",
  "attempt_id": "c035795a-2a2e-4c23-8dce-8457fb91a579",
  "target_version": "1.2.4",
  "build_id": "rk3588-r3h-1.2.4-001",
  "previous_slot": "a",
  "expected_candidate_slot": "b",
  "reboot_context": "install"
}
```

The excerpt documents compatibility-relevant fields; it is not a replacement state fixture and does not claim that the live file remains unchanged after the supplied snapshot.

Forbidden compatibility breaks include:

- renumbering or reinterpreting R3 states;
- clearing unknown-but-valid R3 attempt data on startup;
- treating `REPORT_SUCCESS` as `IDLE` before report acceptance;
- treating `INSTALLING` as replay-safe;
- requiring new fields in old server payloads;
- changing release/slot guards to accommodate the hook.

## 7. Escalation gate

The default workstreams must not modify `main.c`, `state_machine.c`, or `persistence.c`.

If a failing, relevant test demonstrates that one of these files must change, implementation pauses and records:

- failing test and expected invariant;
- why configuration, hook logic, Buildroot integration, or server compatibility cannot solve it;
- proposed transition/schema impact;
- backward-compatibility and replay-safety analysis;
- target evidence needed after the change.

Only a separate approval may authorize that expanded delta.

