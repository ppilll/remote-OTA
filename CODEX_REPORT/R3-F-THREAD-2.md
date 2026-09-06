# R3-F Thread 2 — Manifest / Version / HTTP / Reporting

Evidence level: **CODEX VERIFIED**

## Task scope

Implemented Thread 2 only, with docs/R3/CODEX_EXECUTION_PACKAGE/R3F_CODEX_MASTER.md
read first and treated as normative. Thread 1 model.h was present and consumed
directly. No competing shared model or shared state/error enum was introduced.

## Files changed

New files relative to RK3588_app/edgeguard-remote-ota-agent/:

- include/edgeguard_ota/manifest.h
- include/edgeguard_ota/version.h
- include/edgeguard_ota/compatibility.h
- include/edgeguard_ota/download.h
- include/edgeguard_ota/reporting.h
- src/manifest.c
- src/version.c
- src/compatibility.c
- src/download.c
- src/reporting.c
- tests/thread2/test_values.c
- tests/thread2/test_http.c
- tests/thread2/run_tests.py
- tests/thread2/README.md

Repository-level report: CODEX_REPORT/R3-F-THREAD-2.md (this file).
All 15 files are Thread 2 deliverables. Existing Thread 1 files were not edited.

## Architecture invariants preserved

- GNU11 C and frozen libc/POSIX, GLib, JSON-GLib, libcurl dependencies in Agent code.
- No Wi-Fi/network management, shell execution, RAUC command execution, block-device
  access, A/B metadata implementation or state-machine mutation in these modules.
- JSON-GLib remains the manifest/release/report data model; a bounded lexical guard
  rejects ambiguous/permissive syntax before conversion.
- Device equality and RAUC compatible equality remain independent. The RAUC helper
  is an equality gate AFTER successful signature/metadata inspection, not a replacement
  for that inspection or the final RAUC install verification.
- Mandatory never bypasses validation, device equality or version policy. No downgrade
  support was added; the RAUC equality helper has no mandatory bypass input.
- Current software identity and report current_version come from the local release.
- Realtime is telemetry only. Thread 1 supplies monotonic uptime. Extended
  timestamp_valid is always false; epoch UTC is a placeholder when realtime cannot
  supply the timestamp required by R2.
- SHA256 does not replace RAUC signatures. The downloader never installs a bundle.

## Implementation summary

### Manifest, release and version

Canonical uint32 MAJOR.MINOR.PATCH follows the shared OtaVersion field widths.
It rejects leading zeros, signs, prefixes, suffixes, missing components and overflow.
Comparison uses numeric tuple ordering without subtraction overflow.

All eight frozen manifest fields are required, with exact known JSON types, schema 1,
positive signed-64-bit-compatible size, bounded nonempty identity strings, a valid
version, lowercase 64-character SHA256 and a server-relative artifact path.
Unknown additive fields are ignored. Duplicate decoded keys, trailing/permissive JSON,
malformed Unicode escapes, NUL in known string fields and integer overflow are
rejected before lossy conversion. Input is bounded to 64 KiB and nesting to 64 levels.

Artifact paths reject schemes/authority overrides, traversal, backslashes, controls,
query/fragment syntax, encoded equivalents and double encoding that obscures checks.
Strict local release parsing/loading fails closed on missing, malformed, wrong-type,
overlong, nonregular or oversized input.

Precheck returns update=true, update=false for identical version/build, VERSION_COLLISION,
DOWNGRADE_REJECTED, or a typed validation/device error.

### HTTP and download

Manifest fetch uses libcurl easy with finite connect/request timeouts, bounded body,
HTTP(S)-only protocols, no redirect following or environment proxy, and identity
encoding. It distinguishes 200, 204 no active release, retryable 503 and other errors.
204 neither means the device is current nor overwrites the output manifest.

Headers gate every download body write:

| Condition | Implemented action |
| --- | --- |
| Missing/empty partial | Full GET |
| 0 < partial=N < expected | Range: bytes=N- |
| 206 | Append only with start=N, end=expected-1, total=expected |
| Wrong/missing/duplicate range | Error before writing; retain original partial |
| 200 after partial | Validate, truncate/fsync, write full response from zero |
| Partial larger than expected | Truncate/fsync; restart full GET |
| Partial exactly expected | Size/hash verification before network |
| 416, incomplete partial | Discard/fsync; at most one recovery full GET |
| Repeated 416 | Error; no unbounded loop |
| 416, complete partial | Pure gate selects verify; entry fast path normally already verifies |
| Timeout/reset/short transport/503 | Typed error/retryability; retain safe prefix |
| 404 or rejected status | Error without appending response body |
| Successful HTTP body with wrong size | DOWNLOAD_SIZE_MISMATCH |
| SHA256 mismatch, including complete partial | Truncate/fsync invalid data; DOWNLOAD_HASH_MISMATCH |

A 200 restart uses that same full response from byte zero and never appends it.
416 error bodies never enter the bundle. Content-Length and body bytes are bounded
against the manifest. Nonidentity Content-Encoding is rejected. Interim responses
and trailers cannot authorize an unvalidated append. Partial files are regular,
opened without symlink following, checked for a single hard link and locked.
Space checks include remaining bytes plus the configured reserve.

Final sequence: exact size, SHA256, fsync(file), atomic rename, fsync(parent).
An existing final artifact survives failures before rename. Directory-fsync failure
after rename returns failure with an explicitly uncertain durability outcome.

### Reporting

Uses Thread 1 OtaReport. Legacy emits exactly device_id, current_version, status,
timestamp. Default/zero legacy mode never emits extended fields/statuses.
Installation/reboot/health map to downloaded, completed success to idle, rollback to
error. Explicit extended mode adds attempt_id, target_version, build_id, agent_state,
error_code, timestamp_valid, uptime_ms and supports installing/reboot_pending/success/
rollback statuses. POST uses JSON-GLib and bounded libcurl transport with typed errors.

## Tests created

13 GLib test registrations across two C executables, with embedded deterministic
fixtures and raw HTTP scripts calling the actual production implementations:

- Version grammar/boundaries and pairwise numeric ordering.
- Required keys/wrong types, additive fields, syntax, duplicate/escaped keys, NUL,
  integer overflow, exact 64 KiB boundary, artifact paths and SHA256 syntax.
- Local release, equal build/no-update, collision, downgrade, mandatory invariance,
  device mismatch and independent RAUC mismatch.
- Pure Range decisions including bad start/end/total and both 416 branches.
- Real libcurl loopback scripts assert actual Range and final/partial byte contents
  for full GET, exact 206, ignored Range/200, oversized/complete partials, wrong/missing/
  duplicate ranges, 416 recovery/repetition, 404/503, timeout, TCP reset, truncated
  transport, short/oversized body, wrong Content-Length, corruption, encoding rejection,
  interim 100 and redirects.
- Two-call interrupted-download fixture asserts the second Range uses the newly
  retained byte count and only the verified bundle replaces the previous artifact.
- Disk reserve rejection, symlink/FIFO rejection and final artifact preservation.
- Manifest HTTP 200/204/503/404, invalid JSON and body limits.
- All report mappings, exact legacy/extended key counts, local version, injected
  monotonic time, realtime fallback, mode validation and real POST 204/503.

Fixtures use private temporary files and ephemeral IPv4 loopback ports. TCP reset
assertions allow either the original or extended prefix because delivery before RST
is nondeterministic, while requiring retryable failure and intact prefix bytes.
Timeout fixtures use a condition released when the client returns and a watchdog.

The optional POSIX Python runner follows existing repository test-tooling practice,
uses argv subprocess calls without a shell and adds no deployed runtime dependency.
It builds GNU11 with warnings as errors and frozen libraries; exit 77 means missing
prerequisites. See tests/thread2/README.md for invocation and detailed contracts.

## Commands actually executed

None. No shell, compiler/linker, git or test executable commands were executed.
No external processes or HTTP requests were run. Reads/writes/static inspections used
Node REPL filesystem APIs and apply_patch. One report-writing REPL attempt had a
JavaScript quoting error; the corrected write completed, with no shell fallback.

## Actual results

- Read the master and existing Thread 1 model/config/time interfaces.
- Byte comparison: all 18 pre-existing Agent headers, sources and tests unchanged.
- Static delimiter/literal inspection of 12 new C/header files found no unbalanced
  delimiters or unterminated literals. This is not a compiler result.
- All 19 public Thread 2 API declarations have source definitions.
- Referenced shared states and error codes resolve to Thread 1 model.h.
- No system/popen/execl calls found in the five new Agent sources.
- Created 13 C test registrations; no C test pass count is claimed.

## Tests skipped and why

**SKIPPED: all C compilation, linking and test execution.** The user prohibited shell
commands. Toolchain/library availability was not probed; no compiler absence is
asserted. Static source checks are the only completed verification. The runner,
HTTP scripts, real callback behavior and integration require execution during R3-G
and subsequent target validation stages.

## Known merge dependencies

1. Thread 1 model.h/config.h/time_source.h already exist and are consumed. Reporting
   links Thread 1 ota_time_telemetry/time_source.c. No substitute model or clock exists.
2. main.c retains its fail-closed weak ota_agent_services_init. The merged adapter must
   bind these APIs to load_release and phase callbacks. Main/state-machine ownership
   and cross-thread phase routing were not modified.
3. The orchestrator must hold the Agent lock, associate .part with persisted attempt
   metadata, discard stale partials on release changes, and choose retry timing.
   Thread 2 never mutates persistent state.
4. Thread 3 must wire successful RAUC signature/metadata inspection and the separate
   compatible gate before install, plus final RAUC enforcement.
5. Thread 3 build ownership must include all five sources/libraries. Default reporting
   configuration belongs to that owner; Thread 1's present config loader already
   selects legacy when the mode is absent.

## Open issues

- Compilation/runtime results remain unknown until suites are executed.
- Phase wiring/build integration remain necessary; the complete merged Agent is not
  claimed operational.
- Shorter-than-requested 206 subranges are rejected, not chained. Accepted ranges
  describe the whole remaining manifest tail.
- A complete corrupt partial is discarded and reported as an error, not automatically
  redownloaded in the same call; later recovery belongs to the orchestrator.
- Post-rename directory fsync failure must remain fail-closed even if final path exists.
- Actual disk exhaustion, injected fsync/rename faults and power interruption were
  not exercised; no durability claim beyond the implemented POSIX sequence is made.

Evidence level: **CODEX VERIFIED**
