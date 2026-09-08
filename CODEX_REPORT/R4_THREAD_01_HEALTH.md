# EdgeGuard Remote OTA R4 — Thread 1 Health Acceptance / Buildroot Integration

## 1. Result and evidence level

Thread 1 source implementation is complete in this isolated repository. The new
hook logic and package wiring have host/static evidence only. No vendor SDK image
build or RK3588 runtime campaign was performed, so this report does not claim
`SDK/Buildroot VERIFIED`, `TARGET VERIFIED`, or `HARDWARE VERIFIED`.

No Agent state-machine, persistence, RAUC, Native A/B, download, rollback, or
reporting C source was changed.

## 2. Files changed

- `buildroot-external/package/edgeguard-remote-ota/edgeguard-health-check` (new)
- `buildroot-external/package/edgeguard-remote-ota/agent.conf`
- `buildroot-external/package/edgeguard-remote-ota/edgeguard-remote-ota.mk`
- `buildroot-external/package/edgeguard-remote-ota/post-build-check.sh`
- `buildroot-external/package/edgeguard-remote-ota/README.md`
- `RK3588_app/edgeguard-remote-ota-agent/tests/r4_thread1/test_health_hook.py` (new)
- `RK3588_app/edgeguard-remote-ota-agent/tests/thread3/test_package.py`
- `RK3588_app/edgeguard-remote-ota-agent/tests/test_merged_source_contract.py`
- `CODEX_REPORT/R4_THREAD_01_HEALTH.md` (this report)

`RK3588_app/edgeguard-remote-ota-agent/src/health.c` was inspected and left
unchanged because its existing hook runner already enforces an absolute executable
path, maps hook failure/timeout to `OTA_ERROR_HEALTH_FAILED`, and applies the
configured timeout without replacing built-in health checks.

## 3. Implemented behavior

The package now installs the hook as executable at:

```text
/usr/libexec/edgeguard/edgeguard-health-check
```

Each sample is read-only and succeeds only when all of these local observations
succeed:

1. `/sys/class/net/wlan0` exists.
2. `/proc/*/comm` exposes an exact `connmand` process name.
3. `/proc/*/comm` exposes an exact `wpa_supplicant` process name.
4. `/usr/bin/connmanctl technologies` succeeds and enumerates `Type = wifi`.

The hook does not inspect interface carrier/up state, Powered state, association,
IP/DHCP, DNS, Internet, AP, manifest, report endpoint, or OTA-server reachability.
It performs no ConnMan mutation, network-profile write, RAUC/slot command, reboot,
durable-state write, or block-device access.

The proposed stabilization policy is implemented inside the hook:

```text
sample 1: immediate
maximum samples: 6
interval: 5 seconds
success: first streak of 3 consecutive passing samples
failure: no 3-pass streak after the sixth sample
nominal final sample: 25 seconds after start
Agent runner budget: 30 seconds
```

A failed sample resets the streak. Final diagnostics use the stable stderr form:

```text
HEALTH_FAIL reason=<local_reason> successful_streak=<count>
```

`agent.conf` now configures:

```ini
[health]
hook=/usr/libexec/edgeguard/edgeguard-health-check
timeout_sec=30

[reporting]
mode=extended
```

Extended mode is an integration-gated R4 candidate setting. The backward-compatible
Thread 2 server must be host-verified and deployed at the configured laboratory
endpoint before this candidate begins a campaign.

The Buildroot recipe installs the hook with mode `0755`. The final post-build check
now rejects a missing/non-executable hook, a hook changed by a later overlay, or a
target Agent configuration that differs from the R4 candidate configuration.

## 4. Test source added or modified

The new `tests/r4_thread1/test_health_hook.py` executes controlled shell fixtures
without adding a production test bypass. It covers:

- POSIX shell syntax;
- three consecutive healthy local samples;
- Wi-Fi technology present while `Powered = False`;
- missing `wlan0`, `connmand`, and `wpa_supplicant`;
- failed ConnMan command and absent Wi-Fi technology;
- failed samples resetting the consecutive streak;
- non-consecutive successes not passing prematurely;
- five-second interval and six-sample bounds;
- absence of connectivity tests and mutation/destructive commands;
- Buildroot install path/mode text, hook/timeout configuration, extended mode, and
  post-build overlay checks.

Existing package and merged-source tests were updated for the R4 candidate config.
The merged shell-attribute check no longer invokes Git; on POSIX it reads the
filesystem executable mode directly, consistent with `AGENTS.md`.

## 5. Commands actually executed and results

Final successful checks:

```powershell
$env:SH='D:\Git\bin\sh.exe'
python RK3588_app/edgeguard-remote-ota-agent/tests/r4_thread1/test_health_hook.py
```

Result: 6/6 passed.

```powershell
python RK3588_app/edgeguard-remote-ota-agent/tests/thread3/test_package.py
```

Result: 7 passed; `test_final_target_check` skipped because the host is Windows and
does not provide POSIX target executable-mode semantics. The skipped case is not
reported as pass.

```powershell
python RK3588_app/edgeguard-remote-ota-agent/tests/test_core_source_contract.py
python RK3588_app/edgeguard-remote-ota-agent/tests/test_merged_source_contract.py
```

Result: core 7/7 passed; merged 6/6 passed.

```powershell
& 'D:\Git\bin\sh.exe' -n buildroot-external/package/edgeguard-remote-ota/edgeguard-health-check
& 'D:\Git\bin\sh.exe' -n buildroot-external/package/edgeguard-remote-ota/post-build-check.sh
```

Result: both shell syntax checks passed.

```powershell
python -m py_compile RK3588_app/edgeguard-remote-ota-agent/tests/r4_thread1/test_health_hook.py RK3588_app/edgeguard-remote-ota-agent/tests/thread3/test_package.py RK3588_app/edgeguard-remote-ota-agent/tests/test_merged_source_contract.py
```

Result: passed. Generated Python cache files from this check were removed afterward.

Compile runners attempted:

```powershell
python RK3588_app/edgeguard-remote-ota-agent/tests/thread3/run_tests.py
python RK3588_app/edgeguard-remote-ota-agent/tests/run_merged_tests.py
```

Result: both reported `SKIPPED / NOT HOST VERIFIED` because this Windows workspace
does not provide a POSIX C compiler and the required pkg-config development
packages. No Agent C compile result is claimed.

`tests/thread3/test_preinstall.py` also reported `SKIPPED / NOT HOST VERIFIED:
POSIX shell`; it was not counted as pass.

Early development runs of the new fixture suite exposed fixture-only Windows LF/path
issues, which were corrected before the final 6/6 run. An initial merged-contract
run also exposed stale R3 config assertions and its old Git-based mode inspection;
both test-source issues were corrected before the final 6/6 run.

## 6. Checks intentionally not executed here

- No real Agent health acceptance, mark-good, mark-bad, rollback, reboot, or report
  lifecycle was executed.
- No AP, ConnMan daemon, wpa_supplicant, or real `wlan0` behavior was exercised.
- No full R3 C integration/regression binary was compiled or run because the POSIX
  compiler/development dependencies are unavailable in this workspace.
- No vendor Buildroot package, root filesystem, image, or signed RAUC bundle was
  built.
- No target runtime or unattended OTA campaign was executed.

## 7. Required SDK/Buildroot validation

- Cross-compile the unchanged Agent C sources with the vendor toolchain and required
  GLib/JSON-GLib/libcurl/RAUC package dependencies.
- Run the full existing R3 C regression suites in the supported Ubuntu environment.
- Confirm the final image contains the hook at the fixed path with executable mode.
- Run the final post-build checker after all overlays and confirm the R4 config/hook
  comparisons pass.
- Confirm target image availability of `/bin/sh`, `grep`, `sleep`, `/proc`,
  `/sys/class/net/wlan0`, and `/usr/bin/connmanctl`.
- Confirm RAUC remains 1.5.1 and the existing system.conf, keyring, custom backend,
  pre-install handler, and S99 responsibility boundary remain intact.
- Confirm Thread 2's compatible server is deployed before using `mode=extended`.

## 8. Required RK3588 target validation

- Measure real boot-time availability and exact process names for `wlan0`,
  `connmand`, and `wpa_supplicant`.
- Confirm RAUC/Agent invocation of the script is killed as one process group at the
  30-second timeout and leaves no child behind when ConnMan blocks.
- Confirm `connmanctl technologies` on the target exposes the expected Wi-Fi type
  while disconnected or unpowered and that AP/server/DNS loss alone does not fail
  health.
- Freeze or revise the proposed 3-sample/5-second timing only from retained target
  measurements.
- Execute T-01/T-02 and failure campaigns T-05/T-07 with retained attempt, release,
  slot, health, mark-good/mark-bad, rollback, report, and process evidence.

## 9. Unresolved issues

No source-level blocker directly related to Thread 1 was found. Verification debt
remains: POSIX C regressions, vendor SDK/Buildroot integration, compatible server
deployment, and the specified RK3588 campaigns are all still pending.
