# R5 Test Plan

## Evidence rule

This file is a later verification specification, not a Codex thread completion requirement. Implementation threads may add test sources and perform source/static review, but must not require or claim compilation, host verification, SDK build verification, target verification, hash verification, or GitHub status.

Use a known fixture passphrase such as `R5-Secret-Fixture-Do-Not-Log` only in isolated test memory. After every test, scan captured output and artifacts for the exact fixture and fail if it appears outside the intentionally protected input/store fixture.

## Host unit tests

### Store and endpoint

- valid v1 Wi-Fi and runtime parsing;
- empty/oversized SSID; invalid security; short/non-ASCII/invalid 64-hex PSK;
- duplicate/unknown JSON fields, invalid UTF-8, NUL, oversized file, symlink/non-regular file, wrong mode;
- same-directory atomic replacement, short-write/fsync/rename failures, restart recovery, tombstone completion;
- corrupt/unknown schema behavior and explicit migration fixture;
- valid HTTP/HTTPS syntax; missing host; bad port; userinfo; path/query/fragment; control characters; unsupported schemes; oversized URL;
- invalid endpoint preserves the old effective value; corrupt override falls back to immutable default.

### ConnMan adapter

- deterministic key-file generation and escaping;
- no shell construction;
- create/replace/remove only the owned derived path;
- mocked D-Bus service discovery, association transitions, timeout, authentication failure, owner loss/reappearance, and backoff;
- unrelated ConnMan service data is never deleted;
- canonical credential survives derived-file loss.

### BLE protocol and security

- ObjectManager hierarchy and UUID/property registry;
- valid single/multi-fragment writes at several MTUs;
- gap, overlap conflict, exact duplicate, inconsistent header, all-zero ID, oversized total, invalid UTF-8, timeout, disconnect, global/per-peer limits;
- completed transaction replay and conflicting duplicate ID;
- unpaired, encrypted-but-unbonded, bonded but window-closed, authorized/window-open, revoked bond, and wrong peer result access;
- window monotonic timeout, input loss, close cleanup, BlueZ owner loss/re-registration;
- advertising content contains only approved name and service UUID.

### IPC and Agent integration

- socket mode, peer-credential rejection, length bounds, unknown fields/commands, timeouts, client limits, duplicate request IDs;
- `CHECK_UPDATE_NOW` wakes only the idle poll and follows existing state transitions;
- busy/critical states and duplicate requests return busy without creating an unbounded queue;
- endpoint override reloads immediately before each `IDLE` cycle and remains fixed through that cycle;
- corrupt override uses immutable default; no Wi-Fi file is opened by the Agent;
- every frozen Agent state is exercised against the concurrency policy in `09_CONCURRENCY_POLICY.md`;
- source-contract checks assert no provisioning call to RAUC, block devices, `misc`, boot-control write verbs, shell, or Agent-state writes.

## Static integration review

- public headers have one owner and no incompatible duplicate definitions;
- every protocol/store field has a bound;
- every secret buffer has deterministic cleanup where practical;
- logs and errors accept structured, redacted product facts rather than raw payloads or credentials;
- D-Bus and socket callbacks cannot bypass commit-time authorization and concurrency rechecks;
- Buildroot package dependencies and installed modes match `13_BUILDROOT_INTEGRATION.md`;
- SysV service handles absent `/userdata`, late services, and clean termination;
- R4 state enumeration, persistent schema, Server API, RAUC adapter, and A/B code remain unchanged except the documented narrow Agent integration.

## SDK/build verification plan

When a separate authorized evidence task is authorized, verify actual vendor symbols/packages with repository searches, build the new package and final image, inspect installed files/modes/dependencies, and record the SDK revision and output. This package makes no such claim.

## Target campaign link

Target cases, ordering, forbidden interventions, and evidence rules are stated in `14_TARGET_VALIDATION_PLAN.md`. Host mocks cannot promote BLE, ConnMan, reboot, or A/B persistence to target-verified.
