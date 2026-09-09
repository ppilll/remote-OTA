# R4 Verification and Test Plan

## 1. Goal and verdict discipline

R4 passes only when the integrated candidate completes repeated unattended OTA lifecycles and the defined failure campaigns on the real RK3588 target without violating the automation invariants.

Verification layers are independent:

| Layer | What it can prove | What it cannot prove |
|---|---|---|
| Code review | Intended paths, boundaries, and absence/presence of implementation | Runtime behavior |
| Host tests | Deterministic logic and compatibility in fixtures | Target hardware/boot/network behavior |
| SDK/Buildroot verification | Package integration and artifact construction | Real target lifecycle |
| Target campaign | Real-device behavior for the exact candidate/environment | General fleet/product behavior outside campaign bounds |

Results must be reported at their actual layer. A passing host suite is not `TARGET VERIFIED`.

## 2. Entry conditions

Before implementation verification:

- GitHub R3 baseline and frozen R0–R3/BOOT-GATE facts remain the reference.
- No unapproved changes exist to Native A/B, U-Boot, GPT, RAUC architecture, or durable-state semantics.
- The two workstreams comply with `09_CODEX_THREADS.md` ownership.

Before the first measured target campaign:

- the pending R3 `REPORT_SUCCESS` obligation has naturally completed and durable state is `IDLE`;
- it was not closed by deleting state, manual report, reinstall, rollback, repeated mark-good, or forced Agent restart;
- `autostart-disabled` is absent;
- `/userdata` is mounted and writable;
- the long-running Agent is enabled;
- the lab endpoint is stably available at `http://10.119.65.50:8000` when a campaign requires it;
- the compatible R4 server has passed host tests and is deployed;
- the R4 candidate contains the hook and uses extended reporting only after that server gate;
- campaign releases/build IDs are distinct and their direction/order is documented.

The user-supplied entry snapshot is retained as `TARGET OBSERVED`, not silently replaced by the clean precondition:

```text
1.2.4 / rk3588-r3h-1.2.4-001
current=b, primary=b, both slots confirmed-good
Agent running, autostart-disabled absent
REPORT_SUCCESS / c035795a-2a2e-4c23-8dce-8457fb91a579
wlan0 present but down by intentional non-connection
connmand and wpa_supplicant running
```

## 3. Host verification — health workstream

| Test ID | Scenario | Expected result |
|---|---|---|
| H-H01 | `wlan0`, `connmand`, `wpa_supplicant`, and ConnMan Wi-Fi technology all present for three samples | Hook exits success within budget |
| H-H02 | `wlan0` absent | Hook fails with bounded local diagnostic |
| H-H03 | `connmand` absent | Hook fails |
| H-H04 | `wpa_supplicant` absent | Hook fails |
| H-H05 | ConnMan cannot expose Wi-Fi technology | Hook fails |
| H-H06 | Wi-Fi technology exists but link is down/not associated | Hook may pass; link connectivity is not a hard gate |
| H-H07 | First/second sample fails, later samples produce three consecutive passes within budget | Hook passes only after the required consecutive streak |
| H-H08 | Successful samples are non-consecutive | Hook does not pass prematurely |
| H-H09 | Three-pass streak cannot complete within 30 seconds | Hook fails/times out cleanly; no background child remains |
| H-H10 | AP/server/DNS unavailable while local stack is healthy | Hook result is unaffected |
| H-H11 | Inspect side effects | No ConnMan mutation, network-profile write, slot command, reboot, RAUC install, state deletion, or block write |
| H-H12 | Existing Agent treats hook failure/timeout as health failure | Existing path never reaches mark-good |
| H-H13 | Existing built-in health fails while hook would pass | Overall health fails; hook cannot override built-in checks |
| H-H14 | R3 durable state fixture is loaded by R4 Agent | State and attempt fields retain the same meanings |

Timing values in H-H07–H-H09 are **PROPOSED** until target measurement freezes them.

## 4. Host verification — server reporting workstream

| Test ID | Scenario | Expected result |
|---|---|---|
| H-S01 | Exact existing legacy report | `204`; behavior remains compatible |
| H-S02 | Existing Agent extended report with all supported optional fields | `204` |
| H-S03 | Extended report with a valid subset of optional fields | Accepted according to optional schema |
| H-S04 | Wrong type/range for each optional field | Rejected; no coercion that loses meaning |
| H-S05 | Unknown unrelated extra field | Rejected, preserving strict schema behavior |
| H-S06 | Existing legacy status values | Still accepted |
| H-S07 | Status values actually emitted by Agent extended mode | Accepted exactly as required; no speculative status expansion |
| H-S08 | Structured log for an extended report | Contains attempt ID, target version, build ID, Agent state, and error code when supplied |
| H-S09 | Structured log for a legacy report | Remains valid without requiring optional fields |
| H-S10 | Existing manifest tests | Unchanged and passing |
| H-S11 | Existing artifact/full-download and Range tests | Unchanged and passing |
| H-S12 | Existing R2 report tests | Unchanged except additive expectations and passing |

The endpoint remains `POST /device/report`. No new endpoint is part of a passing result.

## 5. Host regression — frozen orchestration

Run the established R3 fixtures and retain results for at least:

- HTTP 503/transient retry;
- interrupted download and resume;
- restart while downloading;
- `200` Range fallback restart;
- valid/invalid `206` offset handling;
- `416` handling;
- size/hash mismatch;
- install timeout/uncertain outcome;
- same-attempt install once;
- health success and guarded mark-good;
- health failure, mark-bad, and durable rollback;
- post-good report retry;
- report retry without install/health/mark-good replay;
- persistence failures that block destructive action.

No regression result authorizes changes to the frozen architecture merely to simplify a test.

## 6. SDK/Buildroot integration verification

The integrated build must prove:

1. The hook is included at `/usr/libexec/edgeguard/edgeguard-health-check` and is executable.
2. Production configuration points `[health].hook` to that path and retains `timeout_sec=30` unless a proposed-value revision was approved.
3. R4 candidate reporting configuration is `extended` only in an integration where the compatible server is already available.
4. RAUC remains 1.5.1 and existing `system.conf`, keyring, custom backend, and bundle/signing architecture remain intact.
5. S99 retains its existing narrow responsibilities.
6. Release identity is internally consistent across manifest, image, bundle metadata, and `/etc/edgeguard-ota/release.json`.
7. No direct Agent write to raw block/misc paths is introduced.
8. The bundle and image build complete using the existing project pipeline.

Record this layer as `SDK/Buildroot VERIFIED`; do not call it target verification.

## 7. Target campaign matrix

All measured campaigns prohibit engineer login or manual lifecycle triggers between campaign start and the defined end state. Environment setup and evidence collectors may be prepared beforehand.

### T-00 — Baseline and reporting-debt closure

Purpose: begin R4 from a coherent idle system without hiding the inherited obligation.

Pass criteria:

- the existing Agent delivers the pending success report when network/server availability returns;
- attempt `c035795a-2a2e-4c23-8dce-8457fb91a579` is not reinstalled, rolled back, or marked good again;
- durable state reaches `IDLE` naturally;
- accepted slot `b` remains current/primary/confirmed-good.

Classification: target evidence for baseline cleanup, not yet an R4 candidate campaign.

### T-01 — Unattended A-to-B happy path

Starting from accepted slot A and a clean `IDLE`, publish a newer compatible candidate for B before measurement begins.

Pass criteria:

- the already-running Agent discovers the release on its poll schedule;
- download/verify/install occur without target login;
- install is invoked once;
- reboot reaches expected slot B and exact candidate identity;
- built-in health and local Wi-Fi hook pass;
- candidate is marked good exactly once;
- extended success report is accepted and correlated;
- durable state returns to `IDLE`;
- Agent remains running for future polls.

### T-02 — Same-service unattended B-to-A happy path

Without replacing/restarting the long-running Agent after T-01, publish the next compatible release targeting the inactive slot.

Pass criteria match T-01, ending on accepted slot A. This is the principal proof that success is not a one-shot flow.

### T-03 — Server outage during discovery

Make the campaign server temporarily unavailable before/during a poll, then restore it.

Pass criteria:

- Agent continues running and retries;
- no A/B metadata changes while only discovery is unavailable;
- no mark-bad or rollback occurs;
- after restoration, the release can be discovered through normal polling.

### T-04 — Interrupted download and resume

Interrupt artifact delivery after a meaningful partial transfer, then restore service with correct Range behavior.

Pass criteria:

- `.part` persists;
- request resumes from exact safe offset;
- response is validated before append;
- final size/hash match;
- install occurs once only after verification;
- lifecycle reaches the normal accepted/report/`IDLE` end state.

### T-05 — Deterministic candidate health failure rollback

Use a candidate/test condition that deterministically makes one required local hook observation fail without corrupting A/B metadata or depending on external AP/server loss.

Pass criteria:

- expected candidate boots;
- hook fails within the bounded policy;
- mark-good is never called;
- only the expected current candidate is rejected through guarded interfaces;
- durable rollback precedes reboot as defined by R3;
- previous confirmed-good slot boots;
- evidence correlates attempt/release/build/failure/slots;
- system does not reinstall the failed attempt automatically.

### T-06 — Report outage after committed success

Allow health and mark-good to commit, while making `POST /device/report` unavailable or transiently unsuccessful; later restore it.

Pass criteria:

- candidate remains current/primary/confirmed-good;
- durable state remains `REPORT_SUCCESS` while delivery is pending;
- retries carry stable attempt/release/build correlation;
- no install, reboot, health, mark-good, mark-bad, or rollback is replayed;
- restoration allows report success and transition to `IDLE`;
- long-running polling continues afterward.

### T-07 — Local health independent of reachability

With the local Wi-Fi control plane healthy but no AP/server reachability during the health decision, demonstrate that reachability alone is not used by the hook.

Pass criteria:

- hook outcome reflects only the four local checks;
- no rollback is attributed solely to AP/DHCP/DNS/server absence;
- the operational inability to report/check updates remains visible as retry debt, not firmware rejection.

### T-08 — Optional auto-connect operational campaign

With a documented saved Wi-Fi profile and AP available before measurement, reboot through a normal OTA cycle.

Pass criteria:

- normal network services regain connectivity without target login;
- OTA/report operations resume automatically.

This campaign is operational evidence. Its failure requires diagnosis and may block the unattended verdict, but it is not automatically a health-hook failure.

## 8. Evidence bundle per campaign

Retain a coherent bundle containing:

- campaign ID, start/end timestamps, and operator-declared fault window;
- source and candidate version/build ID;
- attempt ID from device and server;
- manifest identity and bundle SHA-256/size;
- boot ID before/after each reboot;
- durable state sequence;
- Agent lifecycle/log evidence;
- server access/structured report evidence;
- current/primary slot and both-slot status before/after;
- RAUC status relevant to install/acceptance;
- health failure/pass reason and timing;
- proof of install count and mark-good/mark-bad count;
- final state and explicit evidence classification.

Secrets, Wi-Fi credentials, and private key material must not be captured.

## 9. Exit criteria

R4 may be declared `PASS` only when:

- both workstream host suites pass;
- frozen R3 orchestration regressions pass;
- SDK/Buildroot integration is verified;
- T-01 and T-02 prove repeated two-direction unattended lifecycle;
- T-03 through T-07 pass with retained evidence;
- any auto-connect dependency for the declared lab setup is resolved or explicitly bounded;
- no invariant violation or unapproved architecture delta remains;
- the final matrix distinguishes `HOST VERIFIED`, `SDK/Buildroot VERIFIED`, and `TARGET VERIFIED` claims.

Use `CONDITIONAL PASS` if the core lifecycle is demonstrated but a declared laboratory dependency—such as stable endpoint or saved-profile auto-connect—remains a material limitation. Use `FAIL` for any safety invariant violation.

