# Thread 3 verification

Evidence level: CODEX VERIFIED. Run from the repository root:

```text
python -B RK3588_app/edgeguard-remote-ota-agent/tests/thread3/test_package.py
python -B RK3588_app/edgeguard-remote-ota-agent/tests/thread3/test_preinstall.py
python -B RK3588_app/edgeguard-remote-ota-agent/tests/thread3/run_tests.py
```

test_package.py exercises the actual build-machine release validator/writer and
source/config/package contracts. The full staged-rootfs check needs POSIX file
modes and is explicitly skipped on Windows. Other cases are portable.

test_preinstall.py runs a copy of the production shell handler, replacing ONLY
the literal fixed ABCTL path with a private fake executable. TEST_SH selects the
test shell if it is not on PATH (e.g. D:/Git/bin/sh.exe in PowerShell). It covers
both current slots, ordering/whitespace, identity failure/ambiguity, absent and
extra targets, duplicate indices/names, mixed/current groups, missing environment
variables, unexpected names and shell-like input. It adds no production path or
environment override. Missing shell returns 77.

run_tests.py requires a POSIX compiler, pkg-config and GLib headers/libraries.
It compiles the actual adapter/health sources and the C fixtures using GNU11,
-Wall -Wextra -Werror. Missing prerequisites return 77. The five GLib test groups
cover exact argv (including metacharacter paths), metadata data parsing, nonzero
results, no repeated install, guarded marks, built-in and optional health checks,
and real process-runner behavior against fake_command.c (spawn failure, signal,
timeout, NUL/oversized stdout). There are no actual RAUC/service/reboot calls.

fake_command.c only accepts the allowlisted RAUC/current-slot argv or a no-argument
health hook. Its executable-adjacent .mode/.info/.slot files control deterministic
output/failure, and .log records length-delimited argv. The test copies it to a
private temporary directory and removes those files afterward. Python and these
fixtures are never installed in the target image.

Integration must bind the existing OtaAgentServices phase/current-slot callbacks;
tests of these modules do not make the current weak main.c binding operational.
Run the Thread 1/2 suites and actual build/RAUC/board campaigns in their appropriate
later environments. No such validation is implied by these fakes.
