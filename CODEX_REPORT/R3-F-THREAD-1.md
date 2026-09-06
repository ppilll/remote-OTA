# R3-F Thread 1 — Core / State / Persistence

Evidence level: **CODEX VERIFIED**

This evidence is limited to implementation review and the portable source checks
actually executed below. The C sources and C unit tests have **not** been compiled
or executed in this environment. This report does not establish completion of the
merged R3-F Agent or of R3-G/R3-H.

## Task scope

Read `docs/R3/CODEX_EXECUTION_PACKAGE/R3F_CODEX_MASTER.md` first and implemented only
Thread 1: shared contracts, strict INI config, persistent generated device UUID,
atomic JSON state, explicit state transitions, reboot recovery, monotonic time,
controlled reboot and the main orchestration skeleton. No other thread's files
were changed. The Agent source directory was absent at the start.

## Files changed

All paths below except this report are relative to
`RK3588_app/edgeguard-remote-ota-agent/`. All listed files are new.

| Area | Files |
| --- | --- |
| Shared model | `include/edgeguard_ota/model.h` |
| Config | `include/edgeguard_ota/config.h`, `src/config.c` |
| Identity | `include/edgeguard_ota/identity.h`, `src/identity.c` |
| Persistence | `include/edgeguard_ota/persistence.h`, `src/persistence.c` |
| State machine | `include/edgeguard_ota/state_machine.h`, `src/state_machine.c` |
| Time | `include/edgeguard_ota/time_source.h`, `src/time_source.c` |
| Reboot | `include/edgeguard_ota/reboot.h`, `src/reboot.c` |
| Orchestration | `src/main.c` |
| C tests | `tests/test_core.c`, `tests/test_orchestration.c` |
| Test tooling | `tests/run_core_tests.py`, `tests/test_core_source_contract.py` |
| Report | Repository-root `CODEX_REPORT/R3-F-THREAD-1.md` |

## Architecture invariants preserved

- GNU11 C with GLib, JSON-GLib and libc/POSIX. Python is used only for development
  test runners and source checks; it is not an Agent runtime dependency.
- No HTTP, download, RAUC adapter, health module, Buildroot, backend or Native
  metadata implementation was added. No misc/block-device access, shell command
  execution, network management or RAUC service recovery was introduced.
- The only external program executed by this core is `/sbin/reboot`, using fixed
  argv and an explicit environment through `execve`, after `sync()`.
- Slot identity is supplied only by the injected interface whose contract requires
  `/usr/bin/edgeguard-rk-abctl get-current`; no slot is inferred from devices.
- State transitions are centralized. Illegal edges and mid-attempt identity
  changes are rejected and logged before writing. Memory is published only after
  the proposed state has been persisted successfully.
- State writes use exclusive temporary file creation, a complete write loop,
  file fsync, rename and parent-directory fsync. The final file is never truncated.
  Any failed state write blocks further transitions/reboot in that process,
  including the uncertain outcome after a successful rename but failed parent fsync.
- Malformed existing state and identity are not replaced with first-boot defaults.
  Only an absent state file initializes IDLE; only an absent device ID is generated.
- Install failure enters terminal ERROR; process restart never replays install.

## Implementation summary

### Shared contracts and configuration

`model.h` defines the 15 frozen states, all required error codes, a supplemental
`TIME_SOURCE_FAILED` code, slots, reboot context, manifest/release/report data and
the complete persistent snapshot. Errors persist their stable textual code.
Strings are bounded owned arrays; version components use unsigned 32-bit bounds;
JSON sizes fit signed 64-bit integers. Infrastructure returns typed errors and
does not call the state machine.

Configuration uses GKeyFile. A precheck rejects duplicate sections/keys before
last-value-wins decoding. Unknown groups/keys, missing required values, malformed
numbers, excessive manifest limits and invalid paths/URLs fail with CONFIG_INVALID.
Only `health.hook` and `reporting.mode` are optional; their defaults are empty and
legacy. An explicitly empty reporting mode is invalid. HTTP and HTTPS authority
forms are recognized; no HTTPS trust/time architecture is implemented here.

Conservative accepted configuration: base URL is a bare authority without a
trailing slash, credentials, query or fragment; request paths are plain absolute
server paths without percent encoding, query or fragment. Filesystem paths are
absolute and reject empty/dot/dot-dot components. The RAUC executable is fixed to
`/usr/bin/rauc`. Device ID, partial and bundle paths must be the frozen filenames
under `state_dir`. The state directory's parent must already exist.

### Identity and persistence

UUIDv4 comes from `/proc/sys/kernel/random/uuid`; the source is injectable for tests.
Existing IDs must be canonical lowercase UUIDv4, optionally followed by one LF.
A per-ID advisory lock serializes creation; main holds an exclusive agent lock.
An existing malformed ID is preserved. Erasing userdata still changes identity.

Reads are bounded regular-file reads with final-component symlink refusal. JSON
load requires every field, exact known types, known states/errors/context, coherent
attempt metadata, opposite slots and valid boot UUID. Duplicate object members,
embedded NUL, integer overflow/float tokens, leading-zero numeric tokens and
excessive nesting fail closed. Unknown persistent fields are rejected for schema 1.
Manifest parsing remains the responsibility of Thread 2.

The atomic writer exposes four failure/ordering hooks for deterministic tests.
Normal completion removes the temporary name through rename; handled failures
clean up their own temporary file. Stale temporary files from a killed process
are ignored, never promoted to the primary state.

### State, recovery and orchestration

The explicit table implements the happy path, no-release/no-update branches,
health rollback and hard failures. Attempt metadata becomes immutable after
PRECHECK; recorded previous/candidate slots and pre-install boot ID stay immutable
until the attempt returns to IDLE. ERROR and ROLLBACK do not automatically start a
new attempt.

Recovery from REBOOT_PENDING compares the actual kernel boot ID:

- Same boot: remain REBOOT_PENDING; main can retry controlled reboot. Do not
  classify this process restart as fallback and do not reinstall.
- Changed boot: durably enter BOOT_NEW_SLOT, query rigorous current-slot identity,
  then enter HEALTH_CHECK for the candidate or ROLLBACK for the previous slot.
- Invalid/ambiguous identity: ERROR. If a recovery write fails, no subsequent slot
  callback or reboot is performed.
- A restart already in BOOT_NEW_SLOT rechecks identity. Other interrupted active
  phases enter ERROR conservatively instead of replaying potentially mutating work.

Main validates config, prepares storage, locks, initializes identity/state and
requires service binding plus validated local release identity before phases.
Before entering INSTALLING, it captures current/opposite slot and kernel boot ID
and durably saves them. Before MARK_GOOD action, it checks current slot again.
After successful mark-bad supplied by the health callback, it persists ROLLBACK
with health-failure context and requests controlled reboot. On startup with that
context, it queries the current slot: reboot only if still on the candidate, never
reboot the previous slot after fallback has been observed.

Polling deadlines use the injectable CLOCK_MONOTONIC abstraction. Realtime is
used only for best-effort UTC telemetry, always with `timestamp_valid=false` and
monotonic `uptime_ms`. Time values/deadlines are not persisted across boot.

## Tests created

`test_core.c`: eight GLib test groups with table/mutation subcases:

- Round-trip all 15 states and their persistent fields.
- Missing/wrong JSON fields and types, duplicate keys, invalid/overflow numeric
  tokens, truncation, inconsistent slots and symlink rejection; failed loads
  preserve output/file contents.
- Inject write/file-fsync/rename/parent-fsync failure; check durable old/new outcomes,
  unchanged in-memory state, blocked machine and suppressed reboot. Successful
  fake reboot observes all four persistence steps in order.
- Legal happy path, no-release/no-update edges, rejected transitions, terminal
  errors and rejection of mid-attempt metadata changes.
- Same-boot pending, candidate boot, fallback, ambiguous/failing slot source,
  invalid boot ID, BOOT_NEW_SLOT restart, interrupted install and failed recovery fsync.
- UUID creation/reuse, malformed ID preservation, UUID version/variant/case and
  failed persistence without returning a new ID.
- Valid configuration/default legacy mode and malformed/duplicate/unknown config,
  missing values, number bounds, paths, URL and reporting-mode mutations.
- Fake monotonic deadline boundaries, wall-clock jumps, overflow and clock failure.

`test_orchestration.c`: five additional deterministic groups use fake service
callbacks against the actual private main dispatcher: no reinstall after failure,
pre-mark-good current-slot guard, durable health rollback before fake reboot,
no-active-release return to IDLE, and persistence failure blocking reboot.

`run_core_tests.py` builds both C test executables and the main skeleton with
`-std=gnu11 -Wall -Wextra -Werror`, checks the safe usage-only entrypoint and runs
the tests, when a POSIX compiler plus GLib/JSON-GLib development packages exist.
It uses subprocess argv, without a shell, and returns 77 for unavailable prerequisites.

`test_core_source_contract.py` provides seven portable source checks: expected
owned files, source whitespace, forbidden shell/block-device interfaces, external
execution limited to fixed reboot argv, atomic-write source ordering, frozen
dependency/clock choices and shared states/error names. These are source checks,
not C runtime tests or proof of crash consistency.

## Commands actually executed and actual results

From the repository root, using PowerShell:

```text
Get-Content -Raw docs/R3/CODEX_EXECUTION_PACKAGE/R3F_CODEX_MASTER.md
rg --files -g AGENTS.md -g '*R3F*' -g '*THREAD*' -g '!vendor'
git status --short
rg --files RK3588_app/edgeguard-remote-ota-agent docs/R3/CODEX_EXECUTION_PACKAGE
rg --files RK3588_app buildroot-external
Get-Command gcc,clang,cc,pkg-config,python,wsl -ErrorAction SilentlyContinue
python RK3588_app/edgeguard-remote-ota-agent/tests/test_core_source_contract.py
python RK3588_app/edgeguard-remote-ota-agent/tests/run_core_tests.py
python -c "import subprocess,sys; p=subprocess.run([sys.executable,'RK3588_app/edgeguard-remote-ota-agent/tests/run_core_tests.py']); print('runner_exit_code='+str(p.returncode))"
git diff --check
```

Additional read-only directory/compiler probes checked common MSYS, LLVM, Git
and Anaconda binary locations. No usable C compiler or pkg-config was found.
No applicable AGENTS.md was found in the workspace or the checked ancestor paths.
The initial Agent-tree lookup failed because the directory did not yet exist.

Actual final portable check result: **7 tests passed**, exit 0.
An additional Python `ast.parse` check of both test-tool files passed. Final
`git status --short --untracked-files=all` and `git diff --name-only` inspection
showed exactly 18 new Agent files plus this report, with no tracked-file changes.

C runner result: **SKIPPED / NOT AVAILABLE: C compiler (CC or cc), pkg-config**.
The subprocess check confirmed **runner_exit_code=77**. Development package
availability was not established because the prerequisite probe stopped first.

`git diff --check` reported no tracked-file whitespace errors; these additions
were untracked, so their whitespace was checked by the portable test instead.

## Skipped tests and limitations

- All 13 C test groups, C compilation/linking, and the skeleton usage smoke:
  skipped because compiler/pkg-config were unavailable. No C test pass is claimed.
- No real reboot, power-cut/fsync durability campaign or filesystem crash test.
  The injection tests were authored but not executed here.
- No merged HTTP/download/RAUC/health/reporting integration test: those modules
  are outside this thread and were absent.
- No RK3588, Vendor SDK, Ubuntu integration build or real Buildroot cross-build
  was run. None of those environments was invoked.

## Known merge dependencies

- Threads 2/3 must include the shared `model.h` and align their public APIs with
  these contracts rather than introduce competing state/error definitions.
- An integration change must implement `ota_agent_services_init()` and populate
  `OtaAgentServices` in `state_machine.h`. The current weak implementation fails
  explicitly; it does not pretend absent modules succeeded.
- Bind `load_release` to strict local release JSON validation. Bind `phase` to
  manifest/version/compatibility, download, RAUC verify/install, health,
  mark-good/mark-bad and reporting. It receives immutable current state and returns
  a full proposed snapshot. It must not directly write state or request reboot.
- CHECK_UPDATE -> PRECHECK must supply all target metadata; main generates the
  attempt UUID. RAUC_VERIFY -> INSTALLING must pass verification first; main adds
  rigorously captured slot/boot context. INSTALLING action must return either
  REBOOT_PENDING on successful install or a typed failure, without internal install
  retries. HEALTH_CHECK -> ROLLBACK is valid only after successful mark-bad;
  failed mark-bad must instead return RAUC_MARK_BAD_FAILED.
- Bind the current-slot callback to the Thread 3 argv adapter. Preserve its
  ambiguity checks and repeat candidate checks inside health/RAUC operations
  where required. REPORT_SUCCESS -> IDLE follows completed reporting.
- Thread 3/integration must add the build/package/init/config/release delivery.
  This thread intentionally adds no production Makefile or Buildroot edits.
  Required core link flags come from `pkg-config glib-2.0 json-glib-1.0`.

## Open issues

1. C compile/runtime compatibility remains unverified until the supplied runner
   executes in an appropriate POSIX development environment; source checks cannot
   substitute for it. Review JSON-GLib parser callback behavior on the target version.
2. The merged service binding is deliberately absent; this is a main orchestration
   skeleton, not a functioning end-to-end OTA client on its own.
3. Interrupted active phases other than the two reboot recovery states fail closed.
   Automatic download resumption across Agent process restarts and an operator
   acknowledgement/new-attempt policy after terminal ERROR/ROLLBACK are not wired
   here. Download resume within the Thread 2 operation remains that module's scope.
4. Persistence assumes a trusted local storage directory hierarchy. Final path
   components reject symlinks, but this is not a general hostile-ancestor traversal
   sandbox. Runtime access should be restricted to the Agent administrator.
5. A crash may leave unused temporary files; there is no automatic promotion or
   cleanup of unknown stale files. The production reboot replaces the Agent via
   execve; exec failure is handled, but after successful exec, actual shutdown
   behavior and the reboot program's later exit require integration evidence.

Evidence level: **CODEX VERIFIED**
