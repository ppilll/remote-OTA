# R3-F Thread 3 — RAUC / Health / Buildroot / SysV

Evidence level: **CODEX VERIFIED**

## Task scope

Read docs/R3/CODEX_EXECUTION_PACKAGE/R3F_CODEX_MASTER.md first and implemented
Thread 3 only. Existing Thread 1/2 files and reports were present but untracked;
they were read for contracts and were not edited. No applicable AGENTS.md was
found in the repository or checked ancestors. No subagents were used.

## Files changed

New Agent files under RK3588_app/edgeguard-remote-ota-agent/:

- include/edgeguard_ota/rauc_adapter.h and health.h
- src/rauc_adapter.c and health.c
- tests/thread3/fake_command.c, test_rauc_health.c, run_tests.py,
  test_preinstall.py, test_package.py and README.md

New substrate file:

- RK3588_app/edgeguard-rk-ab/edgeguard-rk-ab-preinstall

New files under buildroot-external/package/edgeguard-remote-ota/:

- Config.in, edgeguard-remote-ota.mk, S99edgeguard-remote-ota
- agent.conf, system.conf, release.json.in
- prepare-release.py, post-build-check.sh, README.md

Modified buildroot-external/Config.in to reference the new package. external.mk
already includes package/*/*.mk and needed no change. This report is the only
new Thread 3 report: CODEX_REPORT/R3-F-THREAD-3.md.

## Architecture invariants preserved

- Shared OtaError/OtaSlot/OtaRelease contracts come from Thread 1 model.h. No
  competing state/error definitions, state machine or Native metadata code.
- No changes to edgeguard-rk-abctl, custom backend, their package, or frozen R3-C
  overlay. The new package installs the separate production profile and gate.
- GNU11, GLib and POSIX implementation. Buildroot links existing frozen curl,
  GLib and JSON-GLib dependencies. Python is build/test tooling only.
- Agent subprocesses use explicit posix_spawn argv and a controlled environment;
  no command-string interpreter, PATH executable search or shell-output evaluation.
- No direct block writes, forbidden RAUC operations, service restart, Wi-Fi or
  network-stack management. Install calls once and returns RAUC_INSTALL_FAILED
  on failure. No same-attempt reinstall or hidden recovery.
- Health infrastructure returns errors and never persists state or requests reboot.
  Native boot-state ownership remains in the existing RAUC/backend/abctl chain.

## Implementation summary

### RAUC adapter

Allows precisely info --output-format=shell <absolute-bundle>, install
<absolute-bundle>, status, status mark-good and status mark-bad. Bundle paths
are single argv elements; relative paths/options are rejected. Defaults are
/usr/bin/rauc and /usr/bin/edgeguard-rk-abctl. The injected runner is a test seam.

Verification requires successful info execution, exactly one valid
RAUC_MF_COMPATIBLE assignment and exact equality to local release.rauc_compatible.
The parser decodes RAUC single-quoted data, including escaped apostrophes, and
never evaluates expressions. Missing/duplicate/malformed values fail verification;
a valid different compatible returns RAUC_COMPAT_MISMATCH. Final install still
performs RAUC's signature/compatible enforcement.

The command runner captures stdout with a 256 KiB bound, rejects NUL/oversized
output, inherits stderr for diagnostics, and checks exit/signal status. It drains
oversized output without interrupting an operation just because a log grew large.
Monotonic deadlines are 30 seconds for queries and one hour for install by default.
Timeout cleanup affects the invoked command's process group only; it never signals
the long-lived RAUC service. A failed/timed-out install has an uncertain service-side
outcome and returns an error, with no retry or service restart.

Current identity accepts only a or b, optionally one LF, from get-current.
Both mark APIs re-read identity and require the caller's expected candidate
immediately before invoking RAUC. Identity ambiguity blocks mutation. RAUC mark
failures return RAUC_MARK_GOOD_FAILED / RAUC_MARK_BAD_FAILED respectively.

### Health and pre-install gate

Health checks expected current identity; /userdata directory creation/write/fsync
of a private temporary probe; executable RAUC/abctl/backend; readable regular RAUC
config/keyring; successful rauc status; then an optional absolute executable hook
with no additional argv. External health operations use the configured timeout
per command. Probe files are removed. Failure returns HEALTH_FAILED.

The caller must use guarded mark-good after healthy. After unhealthy, guarded
mark-bad must succeed before persisting ROLLBACK and invoking Thread 1 reboot.
Neither a failed mark-bad nor identity ambiguity establishes rollback was armed.

The production POSIX shell substrate handler has a fixed abctl path, obtains
get-current, and treats RAUC_TARGET_SLOTS as exactly two distinct canonical numeric
environment indices. It obtains RAUC_SLOT_NAME_N through printenv with a validated
name, disables globbing, resets IFS, and does not use eval. The target set must be
exactly rootfs.1 + boot.1 when current=a, or rootfs.0 + boot.0 when current=b.
Sentinels preserve trailing newlines so ambiguous identity/slot names fail closed.
Every rejection exits nonzero; the handler performs no metadata or device writes.

### Buildroot, SysV and release

Mirrors the existing local generic-package style using TARGET_CC, target flags,
explicit sources and target-sysroot pkg-config. Dependencies include the unchanged
edgeguard-rk-ab package. Installs the Agent, pre-install handler, static symmetric
RAUC profile, default INI, validated release and SysV S99 script.

Default config matches Thread 1's accepted keys and the normative values, including
legacy reports and an empty optional health hook. No test keyring is shipped.
S99 requires mounted writable /userdata, creates the private state directory,
uses start-stop-daemon with executable/PID matching, and has bounded TERM shutdown
without forced kill or automatic respawn. A one-second startup liveness check
detects immediate failure; it is not candidate health acceptance.

An explicit build-machine release JSON input is required. prepare-release.py
rejects missing/extra/duplicate fields, incorrect types, invalid uint32 versions,
board compatibility mismatch, control characters and unresolved placeholders.
It writes JSON with correct escaping to the package build directory. There is no
fallback identity or server-derived version. Rebuild after changing the input.

Board integration must make post-build-check.sh executable on its Linux checkout
and append it LAST to BR2_ROOTFS_POST_BUILD_SCRIPT. It validates the staged release,
production profile, exact gate, executable presence/modes and provisioned-file
presence after overlays. This required board registration is documented rather
than rewriting an absent vendor board configuration. The old R3-C readonly overlay
must not replace the new profile. Optional --input comparison detects release
replacement against the original build input. Key cryptographic validity remains
RAUC's responsibility.

## Tests created

- Eight Python package/release/source cases, one requiring POSIX file modes.
- Four executable shell test groups covering both opposite groups, ordering,
  whitespace, active/mixed/extra/duplicate/missing/unexpected targets, malformed
  indices, identity errors, trailing-newline ambiguity and injection-like data.
- Five C/GLib groups: exact argv and typed failures/no reinstall; compatible data
  parsing; guarded marks; built-in/optional health; actual POSIX runner against
  the fake executable (exit, spawn, signal, timeout and output failures).
- Shell syntax checks for the gate, S99 script and post-build checker.

## Commands actually executed

Read-only inspection included Get-Content of the master, existing interfaces,
Thread 1/2 reports, current Agent main/config, existing local package/backend and
R3-C profile; rg --files and rg text searches; git status --short;
git diff --stat; git diff --check; Get-Command compiler/pkg-config/python/shell/git
probes and Test-Path/Get-Item probes of common compiler/shell locations.

Actual test commands from the repository root (PowerShell):

```text
python --version
python -B RK3588_app/edgeguard-remote-ota-agent/tests/thread3/test_package.py
$env:TEST_SH = 'D:/Git/bin/sh.exe'
python -B RK3588_app/edgeguard-remote-ota-agent/tests/thread3/test_preinstall.py
python -B RK3588_app/edgeguard-remote-ota-agent/tests/thread3/run_tests.py
& D:/Git/bin/sh.exe -n RK3588_app/edgeguard-rk-ab/edgeguard-rk-ab-preinstall
& D:/Git/bin/sh.exe -n buildroot-external/package/edgeguard-remote-ota/S99edgeguard-remote-ota
& D:/Git/bin/sh.exe -n buildroot-external/package/edgeguard-remote-ota/post-build-check.sh
git diff --check
git status --short --untracked-files=all
git diff --name-only
git diff -- RK3588_app/edgeguard-rk-ab/edgeguard-rk-abctl.c RK3588_app/edgeguard-rk-ab/edgeguard-rk-ab-backend buildroot-external/package/edgeguard-rk-ab
```

Additional Python -B -c invocations used subprocess.run with an argv list to
repeat the C prerequisite probe and assert returncode == 77, and ast.parse over
the three new Python test tools plus prepare-release.py.

## Actual results

- Python 3.8.5. Package suite: 7 passed, 1 explicitly skipped, exit 0.
- Production handler copy with fake abctl, executed through Git's POSIX shell:
  all 4 test groups passed, exit 0. No production ABCTL override was added.
- All three shell syntax checks completed without diagnostics.
- C runner printed SKIPPED / NOT AVAILABLE: POSIX C compiler and/or pkg-config.
  A subprocess check confirmed runner_exit_code=77. C compilation and the five
  C runtime groups have not run.
- Python syntax parsing passed for all four new Python files.
- git diff --check found no tracked whitespace error. Git warned of its configured
  future LF-to-CRLF conversion for Config.in. Portable checks verified LF text and
  final newlines in the added package, gate and C implementations.
- The only existing tracked file changed is buildroot-external/Config.in. All
  substrate Native metadata/backend tracked files remain unchanged.

## Tests skipped and why

- C compiler/linker and GLib C runtime tests: compiler/pkg-config not available
  in the current environment. No compilation/runtime pass is claimed.
- Full staged-target executable-mode test: skipped on Windows because it needs
  POSIX executable file-mode semantics.
- No actual Buildroot cross-build, Vendor SDK, Ubuntu integration environment,
  RAUC daemon/bundle, real reboot, RK3588, eMMC, UART or hardware was used.
- No real SysV service execution, daemon ordering, certificate verification,
  filesystem fault/power-cut, or on-target hook timeout campaign was performed.

## Known merge dependencies

1. Thread 1 model.h is consumed directly. Its main.c still has a weak fail-closed
   ota_agent_services_init. This thread did not rewrite its phase dispatcher.
   The packaged linked sources therefore remain non-operational for OTA until
   the merged service adapter is supplied. No end-to-end Agent success is claimed.
2. Bind current_slot to ota_rauc_current_slot; bind verify/install/status/marks
   and health in the phase adapter. Use persisted expected_candidate_slot, not a
   guessed slot. A HEALTH_CHECK -> ROLLBACK transition requires successful guarded
   mark-bad first. Thread 1 owns durable state/reboot; Thread 2 owns release loading,
   manifest/device/version/download/report behavior. No circular callbacks added.
3. When a new merged service source is added, include it in the package's explicit
   source list. All currently present Thread 1/2/3 sources are listed and checked.
4. Supply release input, keyring, actual server endpoint, BusyBox applets,
   RAUC/D-Bus/userdata startup ordering and final post-build script registration
   in the vendor board configuration during R3-G integration.

## Open issues

- C compilation and runtime behavior still require the authored C tests to run.
- Actual RAUC 1.5.1 quoting/environment ABI and Buildroot/Kconfig/pkg-config support
  must be confirmed in the real integration environment. Shell fakes alone do not
  establish the handler's invocation timing before real image writes.
- BusyBox background startup may discard stderr. Foreground Agent execution is
  documented for integration diagnostics; no production log-rotation design was
  added to this thread.
- CLI timeout/failure cannot prove the service stopped its install. Operator
  investigation and existing orchestration ERROR handling remain necessary.
- Post-build checker registration and executable permission are explicit board
  integration steps. No supplied vendor configuration existed to update here.
- Complete merged Agent operation and later R3-G/R3-H completion are not established.

Evidence level: **CODEX VERIFIED**
