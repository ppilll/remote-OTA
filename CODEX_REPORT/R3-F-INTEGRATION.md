# R3-F integrated F1-F7 closure report

Date: 2026-09-06

Evidence level: **CODEX VERIFIED** for the portable source, packaging, shell, and
byte-contract checks listed below. This report does not claim host, SDK, target,
or hardware verification.

## Revision and diff summary

- Base SHA: `05d82fd5544b715615ae90e66f5f7bb273b49205`
- Final HEAD SHA: `05d82fd5544b715615ae90e66f5f7bb273b49205`
- No commit was created; the integrated implementation remains in the working tree.
- Tracked diff before this report: 30 paths, 559 insertions, 171 deletions.
- New implementation files before this report: 8 paths, 951 lines.
- This report is one additional new path.
- `edgeguard-rk-ab-backend` was byte-checked as exact LF/no-CR. Its working bytes
  are identical to the LF blob at HEAD; the new `.gitattributes` rule prevents a
  future checkout from converting it to CRLF.

## Exact files changed

Added:

- `.gitattributes`
- `CODEX_REPORT/R3-F-INTEGRATION.md`
- `RK3588_app/edgeguard-remote-ota-agent/include/edgeguard_ota/artifact.h`
- `RK3588_app/edgeguard-remote-ota-agent/include/edgeguard_ota/services.h`
- `RK3588_app/edgeguard-remote-ota-agent/src/artifact.c`
- `RK3588_app/edgeguard-remote-ota-agent/src/services.c`
- `RK3588_app/edgeguard-remote-ota-agent/tests/run_merged_tests.py`
- `RK3588_app/edgeguard-remote-ota-agent/tests/test_integration.c`
- `RK3588_app/edgeguard-remote-ota-agent/tests/test_merged_source_contract.py`

Modified:

- `RK3588_app/edgeguard-remote-ota-agent/include/edgeguard_ota/download.h`
- `RK3588_app/edgeguard-remote-ota-agent/include/edgeguard_ota/manifest.h`
- `RK3588_app/edgeguard-remote-ota-agent/include/edgeguard_ota/model.h`
- `RK3588_app/edgeguard-remote-ota-agent/include/edgeguard_ota/rauc_adapter.h`
- `RK3588_app/edgeguard-remote-ota-agent/include/edgeguard_ota/state_machine.h`
- `RK3588_app/edgeguard-remote-ota-agent/src/download.c`
- `RK3588_app/edgeguard-remote-ota-agent/src/main.c`
- `RK3588_app/edgeguard-remote-ota-agent/src/manifest.c`
- `RK3588_app/edgeguard-remote-ota-agent/src/persistence.c`
- `RK3588_app/edgeguard-remote-ota-agent/src/rauc_adapter.c`
- `RK3588_app/edgeguard-remote-ota-agent/src/state_machine.c`
- `RK3588_app/edgeguard-remote-ota-agent/tests/run_core_tests.py`
- `RK3588_app/edgeguard-remote-ota-agent/tests/test_core.c`
- `RK3588_app/edgeguard-remote-ota-agent/tests/test_orchestration.c`
- `RK3588_app/edgeguard-remote-ota-agent/tests/thread2/run_tests.py`
- `RK3588_app/edgeguard-remote-ota-agent/tests/thread2/test_http.c`
- `RK3588_app/edgeguard-remote-ota-agent/tests/thread2/test_values.c`
- `RK3588_app/edgeguard-remote-ota-agent/tests/thread3/fake_command.c`
- `RK3588_app/edgeguard-remote-ota-agent/tests/thread3/run_tests.py`
- `RK3588_app/edgeguard-remote-ota-agent/tests/thread3/test_package.py`
- `RK3588_app/edgeguard-remote-ota-agent/tests/thread3/test_preinstall.py`
- `RK3588_app/edgeguard-remote-ota-agent/tests/thread3/test_rauc_health.c`
- `RK3588_app/edgeguard-rk-ab/edgeguard-rk-ab-preinstall`
- `build-rauc-bundle.sh`
- `buildroot-external/package/edgeguard-remote-ota/Config.in`
- `buildroot-external/package/edgeguard-remote-ota/README.md`
- `buildroot-external/package/edgeguard-remote-ota/agent.conf`
- `buildroot-external/package/edgeguard-remote-ota/edgeguard-remote-ota.mk`
- `buildroot-external/package/edgeguard-remote-ota/post-build-check.sh` (Git mode `100755`)
- `buildroot-external/package/edgeguard-remote-ota/prepare-release.py`

Inspected and byte-normalized without a content delta because HEAD already stores LF:

- `RK3588_app/edgeguard-rk-ab/edgeguard-rk-ab-backend`

Inspected and unchanged:

- `buildroot-external/package/edgeguard-remote-ota/S99edgeguard-remote-ota`

## F1-F7 closure mapping

### F1 - production service binding

`src/services.c` provides the strong production `ota_agent_services_init()` and
binds the existing release, compatibility/version, manifest, download, reporting,
RAUC, health, current-slot, monotonic-time, and reboot implementations. The weak
fallback in `main.c` is compiled only with the explicit
`OTA_TEST_WEAK_SERVICES` test macro. Buildroot links `services.c` and `artifact.c`;
omitting `services.c` from a production link leaves the strong symbol unresolved.

### F2 - RAUC 1.5.1 pre-install argv ABI

The pre-install handler accepts either no arguments or exactly one empty
argument. It rejects one non-empty argument and every 2-or-more argument shape.
The active-slot, opposite-group, target cardinality, duplicate, numeric-index,
canonical-index, missing-variable, and ambiguous-identity checks remain fail-closed.

### F3 - LF/shebang regression prevention

Target/backend/helper scripts were checked as LF with no carriage returns.
`.gitattributes` has targeted LF rules for known executable and source-text types
and explicit binary rules for firmware, filesystem, bundle, compiled Python, and
archive types. It does not use a blanket text rule. The final-target checker now
requires the backend, pre-install gate, and SysV script to begin with the exact
`#!/bin/sh\n` bytes and contain no `\r`; executable mode alone is insufficient.
`post-build-check.sh` is mode `100755` in the Git index.

### F4 - complete release identity chain

RAUC shell-format output is parsed as data only. The parser requires exactly one
strictly quoted, bounded, valid value for each of `RAUC_MF_COMPATIBLE`,
`RAUC_MF_VERSION`, and `RAUC_MF_BUILD`; missing, duplicate, malformed, control-byte,
and unquoted required assignments fail. Before INSTALLING, signed compatibility is
matched to the local release and signed version/build to the durable attempt. On
the candidate slot, the local device compatibility, RAUC profile compatibility,
version, and build are matched to the durable attempt before health can advance to
MARK_GOOD. No shell output is sourced or evaluated.

### F5 - retry/resume orchestration

Phase callbacks return typed `ADVANCE`, `RETRY`, or `HARD_FAILURE` results.
Retryability is supplied by the lower HTTP module and is allowed only in replay-safe
states; INSTALLING is never generic-retryable. Safe states survive restart.
Interrupted INSTALLING becomes durable `ERROR/RAUC_INSTALL_FAILED` and cannot invoke
install again. New attempts receive durable identity before PRECHECK discards any
old `.part`; same-attempt DOWNLOADING recovery retains and resumes it. A published
bundle found after a crash is revalidated by durable size/SHA256 and reused only
when valid. The production bundle request timeout default is 600 seconds, independent
of retry/resume behavior.

### F6 - terminal and post-mark-good reporting

ERROR and ROLLBACK now reach reporting. Their durable root error remains unchanged
across report failures and successes, and successful telemetry does not reset the
terminal attempt. Retryable terminal transport failures repeat only reporting.
REPORT_SUCCESS is durable: failures leave it pending, and a later successful 2xx
report alone advances it to IDLE. It cannot reinstall, mark again, mark bad, roll
back, or reboot. Health rollback marks bad before durable ROLLBACK intent and reboot;
after fallback to the prior known-good slot, the agent reports without rebooting
that slot. Reporting remains `mode=legacy`.

### F7 - one artifact path contract

Manifest and persistence validation call the single validator in `src/artifact.c`.
It accepts only absolute origin-relative paths and rejects authority forms,
query/fragment, traversal, backslash, literal spaces/control bytes, percent encoding,
and colon. The merged test covers manifest parse -> durable attempt construction ->
save -> load for a valid path and rejects invalid paths in both admission layers,
including `/releases/update%20file.raucb` at manifest validation.

## Tests actually executed

Successful checks, exact exit code 0:

- `python RK3588_app/edgeguard-remote-ota-agent/tests/test_core_source_contract.py`
  - 7/7 passed.
- `python RK3588_app/edgeguard-remote-ota-agent/tests/test_merged_source_contract.py`
  - 6/6 passed.
- `python RK3588_app/edgeguard-remote-ota-agent/tests/thread3/test_package.py`
  - native Windows: 7 passed, 1 POSIX-mode test skipped; command exit 0.
- WSL `/usr/bin/python3 .../tests/thread3/test_package.py`
  - 8/8 passed, including final-target executable modes and byte checks; exit 0.
- WSL `TEST_SH=/usr/bin/sh /usr/bin/python3 .../tests/thread3/test_preinstall.py`
  - 5/5 passed; exit 0.
- WSL `/usr/bin/bash -n build-rauc-bundle.sh`
  - exit 0.
- WSL `/usr/bin/sh -n` for backend, pre-install, S99, and post-build-check scripts
  - exit 0.
- In-memory Python syntax compilation for 8 changed Python files
  - exit 0; no `.pyc` output was requested.
- Exact shell-byte scan for backend, pre-install, S99, post-build-check, and bundle helper
  - exit 0.
- `git diff --check HEAD`
  - exit 0.

Tests skipped or blocked:

- Native pre-install runner: exit 77, `SKIPPED / NOT HOST VERIFIED: POSIX shell`.
  It was then run successfully under WSL as listed above.
- Core C runner: exit 77, `SKIPPED / NOT HOST VERIFIED` because the native host
  has no C compiler or pkg-config.
- Thread 2 C runner: exit 77, `SKIPPED / NOT HOST VERIFIED` for the same prerequisites.
- Thread 3 C runner: exit 77, `SKIPPED / NOT HOST VERIFIED` for the same prerequisites.
- Merged C runner: exit 77, `SKIPPED / NOT HOST VERIFIED` for the same prerequisites.
- WSL exposes `/usr/bin/cc` but lacks pkg-config and the GLib, JSON-GLib, and libcurl
  development headers/libraries, so it also could not compile or run the C suites.
- Repository-wide `python -m pytest`: exit 4 while loading `tests/conftest.py`
  because the system Python lacks `fastapi`.
- Repository-wide `.venv/Scripts/python.exe -m pytest`: exit 4 while loading
  `tests/conftest.py` because `app.config` cannot be imported from the mojibake
  Unicode working path. These host-app tests are outside the R3-F C merge and no
  host-app source was changed.

## Remaining blocker and external gate

There is no remaining source-level blocker identified by the checks available in
this environment. Runtime/toolchain evidence is still missing: **Ubuntu GNU11
`-Wall -Wextra -Werror` production compile/link and the merged C runtime tests remain
for the external host gate.** SDK, staged-target, RAUC 1.5.1 service behavior, and
RK3588 hardware validation also remain external activities.

This report does not claim HOST VERIFIED, SDK BUILD VERIFIED, TARGET VERIFIED, or
HARDWARE VERIFIED.
