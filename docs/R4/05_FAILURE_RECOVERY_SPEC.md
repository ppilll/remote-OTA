# R4 Failure and Recovery Specification

## 1. General rule

Recovery behavior follows three classes:

1. **Replay-safe transient work:** wait/retry without A/B mutation.
2. **Destructive or uncertain work:** fail closed; do not replay automatically.
3. **Post-commit reporting debt:** retry reporting without undoing or replaying accepted firmware.

R4 adds evidence and narrow health/reporting deltas. It does not change these R3 semantics.

## 2. Failure matrix

| Failure point | Required durable/operational outcome | Automatic retry? | Forbidden consequence | Evidence status / R4 need |
|---|---|---|---|---|
| Wi-Fi/AP unavailable while idle/checking | Remain in retryable discovery path and continue long-running operation | Yes, according to existing polling/backoff | No install, mark-bad, rollback, or A/B mutation | CODE REVIEWED; TARGET campaign required |
| OTA server unavailable during manifest check | Return/wait in safe checking/idle behavior | Yes | No firmware-health failure | CODE REVIEWED; TARGET campaign required |
| Same release / no update | Return to `IDLE` | Later poll | No reinstall of current release | CODE REVIEWED; TARGET long-run evidence required |
| Manifest malformed/incompatible | Reject attempt safely with no install | Only as defined by existing discovery/error policy | No bypass of compatibility/identity checks | R3 behavior preserved; host regression |
| Insufficient preconditions/storage | Do not begin install | Only after condition is safely re-evaluated by existing policy | No partial destructive transition | R3 behavior preserved; host regression |
| Download transport interruption | Retain synchronized `.part`; resume only after exact Range validation | Yes | No blind append; no install of incomplete file | CODE REVIEWED / HOST VERIFIED; TARGET campaign required |
| Server ignores Range with `200` | Restart download from byte zero | Yes | Never append `200` body to partial content | CODE REVIEWED / HOST VERIFIED |
| Wrong `206 Content-Range` | Reject response safely | Existing retry/error policy | Never append mismatched offset | CODE REVIEWED / HOST VERIFIED |
| `416 Range Not Satisfiable` | Verify completed local content or restart safely as existing logic dictates | Conditional | Never assume arbitrary partial file is complete | CODE REVIEWED / HOST VERIFIED |
| Size/hash mismatch | Reject artifact before install | New attempt/download according to frozen policy | No RAUC install | HOST VERIFIED; preserve |
| RAUC bundle verification failure | Stop before installation | No destructive retry without a new valid attempt | No install/reboot | R3 behavior preserved |
| Persistence failure before install intent | Fail closed before install | No destructive retry | No `rauc install` | CODE REVIEWED / HOST VERIFIED |
| Agent restart in replay-safe download/check/report state | Resume/retry from durable data | Yes | No loss of attempt correlation | CODE REVIEWED / HOST VERIFIED; target evidence as planned |
| Agent restart in `INSTALLING` / install command timeout or ambiguous result | Durable error/uncertain outcome; require diagnosis | **No** automatic reinstall | Never invoke install again for same attempt | CODE REVIEWED / HOST VERIFIED |
| Reboot command boundary failure | Preserve durable reboot context and recover according to R3 | Only existing safe behavior | No assumption that reboot occurred without boot evidence | R3 behavior preserved |
| Boot falls back to previous slot before candidate acceptance | Detect fallback and recover/report according to existing R3 path | Existing R3 behavior | No mark-good of unbooted candidate | CODE REVIEWED; target evidence where included |
| Candidate release identity mismatch | Health/acceptance must fail closed | No mark-good retry that ignores mismatch | No mark-good | CODE REVIEWED |
| Candidate current slot mismatch | Fail guarded acceptance | No | No mark-good/mark-bad of an unrelated slot | CODE REVIEWED |
| Built-in health failure | Enter existing guarded rollback path | Not as health retry beyond defined check | No mark-good | CODE REVIEWED / HOST VERIFIED; TARGET campaign required |
| External Wi-Fi hook failure/timeout | Same as existing health failure: guarded mark-bad, durable rollback, reboot/fallback | Hook samples only within bounded 30 s proposal | No mark-good; no hook-owned slot mutation | New R4 delta; host + target verification |
| AP/DHCP/DNS/server unavailable during health window | Not a firmware-health failure | Network operation retries elsewhere | No mark-bad/rollback due solely to reachability | R4 decision; target campaign required |
| Persistence failure before rollback/reboot boundary | Fail closed and retain diagnosable state | No unsafe continuation | No unrecorded slot mutation/reboot | R3 behavior preserved |
| Success report rejected/unreachable after mark-good | Stay durably in `REPORT_SUCCESS`; retry delivery | Yes | No reinstall, repeat health, repeat mark-good, or rollback | CODE REVIEWED / HOST VERIFIED; TARGET campaign required |
| Agent legacy report meets extended server | Continue accepting exact old payload | N/A | No breaking migration | New R4 server test required |
| Malformed extended optional report field | Reject request clearly | Device remains in report retry debt | No reinterpretation or state rollback | New R4 server test required |

## 3. Download recovery requirements

The existing downloader is retained. R4 validation must prove on target that:

- an interrupted transfer leaves a usable partial file;
- the subsequent request uses the correct byte offset;
- the server response is validated before append;
- the final size and SHA-256 match the manifest;
- install occurs once only after full verification;
- no stale partial data crosses into a different artifact identity.

This package does not authorize a new resume format or alternate downloader.

## 4. Install uncertainty requirements

An install timeout means the caller lost certainty; it does not prove RAUC made no changes. Process termination of the launched CLI cannot safely establish the state of the RAUC service or storage.

Therefore:

```text
uncertain install outcome
  -> durable non-replay outcome
  -> no automatic reinstall for the same attempt
```

Any proposal to retry `rauc install` from `INSTALLING` violates the safety contract and requires a separate architecture review outside the current two workstreams.

## 5. Candidate health failure and rollback

The R4 external hook feeds the existing failure path. Required order is:

1. Verify the durable attempt still identifies the running candidate.
2. Observe built-in plus external health failure.
3. Do not execute mark-good.
4. Use the existing guarded Native A/B interface to mark only the expected current candidate bad.
5. Persist `ROLLBACK` before the reboot boundary as defined by R3.
6. Reboot/fall back.
7. After booting the prior slot, retain evidence correlating attempt ID, candidate release/build, slots, and failure reason.

The health hook must not perform steps 4–6 itself.

## 6. Report recovery after committed success

Required order is:

```text
health accepted
  -> guarded mark-good committed
  -> durable REPORT_SUCCESS
  -> report attempts until accepted
  -> IDLE
```

Network/server rejection after mark-good cannot move the attempt backward. The current entry state for attempt `c035795a-2a2e-4c23-8dce-8457fb91a579` is treated exactly this way.

Before the R4 candidate switches to extended reporting:

- the existing endpoint must accept the extended shape;
- legacy payloads must still return success;
- the R4 server instance used by the target must contain that compatibility change.

## 7. Endpoint failure policy

R4's lab endpoint is `http://10.119.65.50:8000`.

If the address is stable but the service is temporarily down, normal network/report retry semantics apply. If the address itself cannot be kept stable, this is an architecture-entry problem: record the limitation and revisit endpoint provisioning. Do not work around it by adding unreviewed fallback addresses or by treating endpoint loss as bad firmware.

## 8. Human intervention boundary

The final unattended campaigns prohibit engineer login or manual triggering during the measured lifecycle. Human intervention remains appropriate only after a terminal/uncertain safety outcome, or to configure the campaign environment before measurement.

Pre-campaign preparation is not permission to:

- delete Agent state to hide reporting debt;
- issue install, reboot, mark-good, or mark-bad manually;
- restart the Agent to force a desired state;
- manually POST the pending report;
- edit A/B metadata directly.

## 9. Required failure evidence

Every target failure campaign must retain, as applicable:

- release version and build ID before/after;
- attempt ID;
- durable state transitions and error code/reason;
- boot IDs and reboot boundary;
- current/primary slot and both-slot status;
- Agent process continuity/restart evidence;
- server requests, Range offsets, and report status;
- proof that forbidden duplicate operations did not occur;
- final recovery state and whether it is `IDLE`, `ROLLBACK`, or a deliberate terminal error.

