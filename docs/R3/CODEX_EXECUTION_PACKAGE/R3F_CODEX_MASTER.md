# EdgeGuard Remote OTA — R3-F Codex Master Execution Package

## 0. Stage context

Project:

```text
EdgeGuard Remote OTA
```

Repository:

```text
remote-OTA
```

Work only inside the existing repository.

Current stage:

```text
R3-F — Codex Agent Implementation
```

Previous stages are frozen:

```text
R3-A PASS
R3-B PASS
R3-C PASS
R3-D PASS
R3-E PASS / architecture frozen
```

Do NOT reopen R3-A/B/C/D architecture research.

R3-D has already proven on a real RK3588 target:

```text
real RAUC 1.5.1 bundle install
inactive-slot installation
candidate pending state
real reboot
new-slot boot
mark-good
confirmed-good
retry consumption
mark-bad
automatic rollback/fallback
```

Codex has NO access to:

```text
real RK3588 target
real eMMC
UART
Vendor SDK build environment
Ubuntu integration/build environment
real Buildroot cross-build
```

Therefore Codex MUST NOT claim:

```text
HOST VERIFIED
SDK BUILD VERIFIED
TARGET VERIFIED
HARDWARE VERIFIED
```

Maximum evidence level produced by these tasks:

```text
CODEX VERIFIED
```

If tests cannot run because compiler/libraries/toolchain are unavailable:

```text
report SKIPPED / NOT AVAILABLE
do not fake results
do not change architecture merely to make sandbox tests runnable
```

---

# 1. Frozen architecture

Final architecture must remain:

```text
edgeguard-remote-ota-agent
        |
        v
RAUC 1.5.1 CLI/service
        |
        v
edgeguard-rk-ab-backend
        |
        v
edgeguard-rk-abctl
        |
        v
Rockchip AvbABData
        |
        v
U-Boot/SPL
```

Ownership:

```text
Agent:
OTA orchestration only

RAUC:
bundle signature verification
RAUC compatible verification
slot topology
image installation

RAUC custom backend:
RAUC boot-state semantics
→ Rockchip Native A/B translation

edgeguard-rk-abctl:
only Linux-side Native metadata reader/writer
current-slot identity
CRC
locking
mutation
fail-closed behavior

U-Boot/SPL:
actual boot selection
tries decrement
retry/fallback
```

The Agent MUST NEVER:

```text
write misc
open misc for mutation
write /dev/mmcblk*
write /dev/disk/by-partlabel/*
dd images
implement A/B metadata itself
guess current slot
mark-good an inactive slot
use rauc write-slot
use --no-verify
manage Wi-Fi
start wpa_supplicant
run DHCP
take over ConnMan
```

---

# 2. Target baseline

Target:

```text
ATK-DLRK3588
RK3588
AArch64
eMMC
Linux 5.10
BusyBox / SysV
ConnMan 1.40
wpa_supplicant 2.10
RAUC 1.5.1
```

Native partition groups:

```text
A:
boot_a
system_a

B:
boot_b
system_b
```

Current-slot identity is owned by:

```text
/usr/bin/edgeguard-rk-abctl get-current
```

That command already enforces:

```text
android_slotsufix
+
root=PARTUUID
+
/dev/disk/by-partuuid mapping
+
actual mounted root block-device identity
```

Any conflict is fail-closed.

The Agent MUST consume this interface rather than duplicate it.

---

# 3. Production RAUC profile

R3-D found that dynamically changing `readonly=true` in a running RAUC service is unsuitable for production orchestration.

Production R3 architecture therefore uses:

```text
static symmetric writable A/B RAUC slots
+
pre-install target safety handler
```

Production system.conf direction:

```ini
[system]
compatible=EdgeGuard-ATK-DLRK3588-RK3588
bootloader=custom
statusfile=/userdata/edgeguard-remote-ota/rauc-status.raucs

[keyring]
path=/etc/rauc/keyring.pem
check-purpose=codesign

[handlers]
bootloader-custom-backend=/usr/libexec/rauc/edgeguard-rk-ab-backend
pre-install=/usr/libexec/rauc/edgeguard-rk-ab-preinstall

[slot.rootfs.0]
device=/dev/disk/by-partlabel/system_a
type=ext4
bootname=a

[slot.boot.0]
device=/dev/disk/by-partlabel/boot_a
type=raw
parent=rootfs.0

[slot.rootfs.1]
device=/dev/disk/by-partlabel/system_b
type=ext4
bootname=b

[slot.boot.1]
device=/dev/disk/by-partlabel/boot_b
type=raw
parent=rootfs.1
```

Do NOT dynamically rewrite system.conf from the Agent.

The pre-install handler is substrate code, not Agent logic.

It must:

```text
1. execute edgeguard-rk-abctl get-current
2. fail if current cannot be rigorously determined
3. inspect RAUC_TARGET_SLOTS / RAUC_SLOT_NAME_N
4. require exactly the opposite install group:
   current=a -> rootfs.1 + boot.1
   current=b -> rootfs.0 + boot.0
5. reject any unexpected target
6. exit non-zero before image writes on failure
```

It must not write metadata or block devices.

---

# 4. Agent implementation language

Frozen:

```text
C
GNU11
```

Use existing target libraries:

```text
libcurl
GLib
JSON-GLib
libc / POSIX
```

Do not add Python, C++, Qt, Boost, libuuid or another large runtime unless explicitly requested later.

Use:

```text
GKeyFile
```

for INI configuration.

Use:

```text
JSON-GLib
```

for manifest, persistent state and reports.

Use GLib checksum APIs or equivalent existing dependency for SHA256.

No shell command composition.

Forbidden:

```c
system(...)
popen(...)
execl("/bin/sh", "sh", "-c", ...)
```

External tools must be executed with explicit argv using `fork/execve`, `posix_spawn`, or a small testable command-runner abstraction.

---

# 5. HTTP architecture

Frozen HTTP client:

```text
libcurl easy API
```

R3 transport:

```text
HTTP experimental LAN
```

Agent does not manage the network stack.

Server reachability is determined by application-level HTTP operations.

Frozen manifest:

```json
{
  "schema_version": 1,
  "device_compatible": "atk-dlrk3588",
  "version": "1.1.0",
  "build_id": "...",
  "artifact_url": "/update.raucb",
  "sha256": "<64 lowercase hex>",
  "size": 0,
  "mandatory": false
}
```

Manifest response semantics:

```text
200 -> manifest available
204 -> server has no active release
503 -> retryable server inconsistency
other HTTP/network failure -> error
```

IMPORTANT:

```text
204 != device already current
```

Manifest parser requirements:

```text
schema_version == 1
device_compatible non-empty string
version valid
build_id non-empty string
artifact_url valid
sha256 exactly 64 lowercase hexadecimal chars
size > 0
mandatory boolean
known field with wrong type -> reject
unknown future fields -> ignore
```

Manifest maximum size:

```text
64 KiB
```

`artifact_url` must be a server-relative path.

Reject:

```text
scheme://
host override
..
control characters
empty path
```

---

# 6. Download resume semantics

Persistent files:

```text
/userdata/edgeguard-remote-ota/update.raucb.part
/userdata/edgeguard-remote-ota/update.raucb
```

Frozen algorithm:

```text
.part absent
→ full GET

.part size=N, 0 < N < expected
→ Range: bytes=N-

append ONLY when:
HTTP 206
AND
Content-Range begins exactly at N

HTTP 200 after Range request
→ truncate partial
→ restart from byte 0
→ NEVER append full response

partial > expected
→ discard/truncate and restart

partial == expected
→ perform size + SHA256 verification

HTTP 416:
  if partial == expected -> verify it
  otherwise -> discard/restart

wrong Content-Range
→ reject; never append

connection reset / timeout / retryable server failure
→ retain valid partial file

complete file size mismatch
→ error

SHA256 mismatch
→ discard invalid complete/partial data
→ error
```

Final sequence:

```text
download
→ expected size
→ SHA256
→ fsync
→ atomic rename .part -> update.raucb
→ RAUC_VERIFY
→ RAUC install
```

SHA256 does NOT replace RAUC signature verification.

---

# 7. RAUC Agent interface

Frozen:

```text
RAUC CLI argv interface
```

Not direct D-Bus in R3.

Binary:

```text
/usr/bin/rauc
```

Allowed operations:

Signature / metadata inspection:

```text
rauc info --output-format=shell <bundle>
```

Do not execute/eval the shell-formatted output.

Parse required keys as data.

Installation:

```text
rauc install <bundle>
```

Success confirmation:

```text
rauc status mark-good
```

Health-failure rejection of current booted candidate:

```text
rauc status mark-bad
```

Status smoke:

```text
rauc status
```

Forbidden Agent operations:

```text
rauc write-slot
rauc status mark-active
--no-verify
direct block write
```

RAUC_VERIFY must require:

```text
rauc info exit 0
AND
bundle RAUC compatible ==
local release RAUC compatible
```

RAUC install performs the final RAUC compatible/signature enforcement again.

R3-D observed a RAUC 1.5.1 failed-install condition where a failed raw-slot operation could leave the target device busy in the long-lived service.

Therefore:

```text
RAUC_INSTALL_FAILED
→ ERROR
→ no automatic same-attempt rauc install retry
```

The Agent must not kill/restart RAUC service as hidden recovery behavior in R3.

---

# 8. Compatibility model

There are TWO independent compatibility gates.

Agent gate:

```text
server manifest.device_compatible
==
local release.device_compatible
```

Example:

```text
atk-dlrk3588
```

RAUC gate:

```text
bundle RAUC compatible
==
local release.rauc_compatible
```

Example:

```text
EdgeGuard-ATK-DLRK3588-RK3588
```

Both must pass.

`mandatory=true` must never bypass:

```text
manifest validation
device compatible mismatch
RAUC compatible mismatch
signature failure
version malformed
downgrade policy
```

---

# 9. Local release identity

Path:

```text
/etc/edgeguard-ota/release.json
```

Schema:

```json
{
  "schema_version": 1,
  "device_compatible": "atk-dlrk3588",
  "rauc_compatible": "EdgeGuard-ATK-DLRK3588-RK3588",
  "version": "1.1.0",
  "build_id": "..."
}
```

Agent MUST NOT infer the currently installed software version from the Server.

Invalid/missing local release identity is fail-closed.

---

# 10. Version policy

Frozen grammar:

```text
MAJOR.MINOR.PATCH
```

All components are unsigned decimal integers.

Reject:

```text
v1.2.3
1.2
1.2.3-beta
1.2.3+foo
01.2.3
overflow
empty component
non-digit characters
```

`0` is valid.

Comparison is numeric tuple comparison:

```text
(MAJOR, MINOR, PATCH)
```

Rules:

```text
remote > local
→ update allowed

remote == local AND build_id == local build_id
→ no update

remote == local AND build_id != local build_id
→ VERSION_COLLISION
→ reject

remote < local
→ DOWNGRADE_REJECTED
```

R3 does not support downgrade.

`mandatory=true` does not override this policy.

---

# 11. Device identity

Frozen R3 source:

```text
persistent generated UUID
```

Path:

```text
/userdata/edgeguard-remote-ota/device-id
```

On first boot:

```text
missing
→ generate UUIDv4 from a strong kernel/system source
→ atomic persistent write
→ fsync
```

Suggested no-extra-dependency source:

```text
/proc/sys/kernel/random/uuid
```

On subsequent boots:

```text
strictly validate existing UUID
→ reuse unchanged
```

Never use:

```text
DHCP IP
BLE random address
current network interface address
```

If device-id storage is malformed:

```text
fail closed
```

Do not silently replace an existing malformed ID.

Known limitation:

```text
factory erase of userdata changes generated device identity
```

Do not invent a factory identifier.

---

# 12. Time model

RTC battery is known to be unreliable.

Frozen:

```text
state deadlines
timeouts
retry durations
attempt duration
→ CLOCK_MONOTONIC
```

Wall clock:

```text
telemetry only
not update-decision authority
```

R2 report requires a timestamp.

Generate a best-effort UTC timestamp from realtime but do NOT use it for:

```text
version ordering
slot decisions
rollback decisions
update eligibility
```

Extended reporting should carry:

```text
timestamp_valid=false
uptime_ms=<monotonic uptime>
```

until a later stage freezes a trusted time-synchronization source.

---

# 13. Persistent Agent state

Directory:

```text
/userdata/edgeguard-remote-ota/
```

Primary state file:

```text
agent-state.json
```

At minimum persist:

```text
schema_version
state
attempt_id
target_version
build_id
artifact_url
expected_size
expected_sha256
last_error
previous_slot
expected_candidate_slot
boot_id_before
reboot_context
```

Example:

```json
{
  "schema_version": 1,
  "state": "REBOOT_PENDING",
  "attempt_id": "...",
  "target_version": "1.2.0",
  "build_id": "...",
  "artifact_url": "/update.raucb",
  "expected_size": 123456789,
  "expected_sha256": "...",
  "previous_slot": "b",
  "expected_candidate_slot": "a",
  "boot_id_before": "...",
  "last_error": {
    "code": "",
    "message": ""
  }
}
```

State write sequence MUST be:

```text
write temporary file
→ flush/write complete
→ fsync(temp file)
→ rename(temp, final)
→ fsync(parent directory)
```

Forbidden:

```text
truncate final state file then rewrite
```

Malformed persistent state:

```text
fail closed
```

Do not silently reset to IDLE.

`REBOOT_PENDING` persistence is a critical R3 requirement.

---

# 14. State machine

Required states:

```text
IDLE
CHECK_NETWORK
CHECK_UPDATE
PRECHECK
DOWNLOADING
VERIFY_DOWNLOAD
RAUC_VERIFY
INSTALLING
REBOOT_PENDING
BOOT_NEW_SLOT
HEALTH_CHECK
MARK_GOOD
REPORT_SUCCESS
ROLLBACK
ERROR
```

Implement an explicit transition table or centralized transition function.

Do NOT implement the Agent as scattered state-changing if/else statements.

Illegal transition:

```text
reject
log
do not mutate persistent state as though it succeeded
```

Happy path:

```text
IDLE
→ CHECK_NETWORK
→ CHECK_UPDATE
→ PRECHECK
→ DOWNLOADING
→ VERIFY_DOWNLOAD
→ RAUC_VERIFY
→ INSTALLING
→ REBOOT_PENDING
→ reboot
→ BOOT_NEW_SLOT
→ HEALTH_CHECK
→ MARK_GOOD
→ REPORT_SUCCESS
→ IDLE
```

Important branches:

```text
CHECK_UPDATE + HTTP 204
→ IDLE

PRECHECK + remote == local
→ IDLE

any hard validation failure
→ ERROR

INSTALLING + RAUC failure
→ ERROR
(no same-attempt reinstall)

startup with persisted REBOOT_PENDING
→ BOOT_NEW_SLOT

BOOT_NEW_SLOT:
current == expected_candidate
→ HEALTH_CHECK

current == previous_slot
→ ROLLBACK

anything else
→ ERROR
```

`REBOOT_PENDING` MUST be persisted and fsynced before reboot is requested.

---

# 15. Reboot context

Before RAUC install persist/remember:

```text
previous_slot
expected_candidate_slot
boot_id_before
```

Expected candidate is always the opposite of the rigorously verified current slot.

After successful install:

```text
state = REBOOT_PENDING
persist + fsync
→ request controlled reboot
```

On next process startup:

```text
if persisted state == REBOOT_PENDING
→ BOOT_NEW_SLOT
```

Call:

```text
/usr/bin/edgeguard-rk-abctl get-current
```

Then:

```text
current == expected_candidate
→ HEALTH_CHECK

current == previous_slot
→ rollback/fallback observed

other / ambiguous
→ ERROR
```

Never reinstall merely because the process restarted.

---

# 16. Health policy

Built-in health checks:

```text
edgeguard-rk-abctl get-current succeeds
current == expected_candidate

/userdata exists and is writable

/usr/bin/rauc executable
/usr/bin/edgeguard-rk-abctl executable
/usr/libexec/rauc/edgeguard-rk-ab-backend executable
/etc/rauc/system.conf readable
/etc/rauc/keyring.pem readable

rauc status exits successfully
```

Optional configured health hook:

```text
absolute executable path
no shell
no untrusted argv construction

exit 0 -> healthy
non-zero -> unhealthy
```

Healthy:

```text
HEALTH_CHECK
→ MARK_GOOD
→ rauc status mark-good
```

Before mark-good, rigorously confirm that current still equals expected candidate.

Unhealthy:

```text
rauc status mark-bad
→ persist ROLLBACK
→ controlled reboot
```

If mark-bad fails:

```text
ERROR
do not claim rollback was armed
```

---

# 17. Reboot abstraction

Production reboot path:

```text
persist state
fsync
sync()
exec /sbin/reboot with explicit argv
```

Do not use shell.

Do not reboot if critical persistence failed.

Design reboot as an injectable/testable abstraction so host/fake tests can replace the real reboot operation.

---

# 18. Reporting

Existing R2 report:

```json
{
  "device_id": "...",
  "current_version": "...",
  "status": "...",
  "timestamp": "..."
}
```

Existing coarse statuses:

```text
idle
checking
downloading
downloaded
error
```

Internal R3 state MUST remain richer than this.

Default wire mode for compatibility:

```text
legacy
```

Suggested mapping:

```text
IDLE -> idle

CHECK_NETWORK
CHECK_UPDATE
PRECHECK
-> checking

DOWNLOADING
-> downloading

VERIFY_DOWNLOAD
RAUC_VERIFY
-> downloaded

ERROR
-> error
```

Implement internal extended report model supporting future additive fields:

```text
attempt_id
target_version
build_id
agent_state
error_code
timestamp_valid
uptime_ms
```

and future statuses:

```text
installing
reboot_pending
success
rollback
```

Do not assume the existing R2 server accepts extended fields.

Default configuration must use:

```text
reporting.mode=legacy
```

---

# 19. Error model

At minimum:

```text
NONE

CONFIG_INVALID

IDENTITY_INVALID
IDENTITY_AMBIGUOUS

LOCAL_RELEASE_INVALID

MANIFEST_HTTP
MANIFEST_INVALID

DEVICE_COMPAT_MISMATCH

VERSION_MALFORMED
VERSION_COLLISION
DOWNGRADE_REJECTED

DOWNLOAD_HTTP
DOWNLOAD_RANGE_MISMATCH
DOWNLOAD_DISK_SPACE
DOWNLOAD_SIZE_MISMATCH
DOWNLOAD_HASH_MISMATCH

RAUC_VERIFY_FAILED
RAUC_COMPAT_MISMATCH
RAUC_INSTALL_FAILED
RAUC_MARK_GOOD_FAILED
RAUC_MARK_BAD_FAILED

HEALTH_FAILED

REBOOT_CONTEXT_INVALID
REBOOT_FAILED

PERSISTENCE_FAILED

REPORT_FAILED

ILLEGAL_TRANSITION
```

Errors must have:

```text
stable enum/code
human-readable diagnostic
```

Do not expose internal shell strings as architecture-level error codes.

---

# 20. Configuration

Path:

```text
/etc/edgeguard-ota/agent.conf
```

INI direction:

```ini
[server]
base_url=http://192.168.1.100:8000
manifest_path=/manifest.json
report_path=/device/report
poll_interval_sec=300
connect_timeout_sec=5
request_timeout_sec=30

[agent]
state_dir=/userdata/edgeguard-remote-ota
release_file=/etc/edgeguard-ota/release.json
max_manifest_bytes=65536

[download]
part_file=/userdata/edgeguard-remote-ota/update.raucb.part
bundle_file=/userdata/edgeguard-remote-ota/update.raucb
reserve_bytes=67108864

[rauc]
binary=/usr/bin/rauc

[identity]
device_id_file=/userdata/edgeguard-remote-ota/device-id

[health]
hook=
timeout_sec=30

[reporting]
mode=legacy
```

R3 accepts HTTP LAN URLs.

Unknown configuration section/key:

```text
CONFIG_INVALID
```

Missing required values:

```text
CONFIG_INVALID
```

Do not silently accept misspelled security-relevant options.

---

# 21. Source tree

Expected Agent tree:

```text
RK3588_app/edgeguard-remote-ota-agent/
├── include/
│   └── edgeguard_ota/
│       ├── model.h
│       ├── config.h
│       ├── identity.h
│       ├── manifest.h
│       ├── version.h
│       ├── compatibility.h
│       ├── download.h
│       ├── persistence.h
│       ├── rauc_adapter.h
│       ├── state_machine.h
│       ├── reporting.h
│       ├── time_source.h
│       ├── reboot.h
│       └── health.h
├── src/
│   ├── main.c
│   ├── config.c
│   ├── identity.c
│   ├── manifest.c
│   ├── version.c
│   ├── compatibility.c
│   ├── download.c
│   ├── persistence.c
│   ├── rauc_adapter.c
│   ├── state_machine.c
│   ├── reporting.c
│   ├── time_source.c
│   ├── reboot.c
│   └── health.c
└── tests/
```

`model.h` is owned by Thread 1 and contains shared enums/data contracts.

Other threads must not create competing definitions of shared Agent states/errors.

---

# 22. Thread ownership

## Thread 1 — Core

Owns:

```text
include/edgeguard_ota/model.h
include/edgeguard_ota/config.h
include/edgeguard_ota/identity.h
include/edgeguard_ota/persistence.h
include/edgeguard_ota/state_machine.h
include/edgeguard_ota/time_source.h
include/edgeguard_ota/reboot.h

src/main.c
src/config.c
src/identity.c
src/persistence.c
src/state_machine.c
src/time_source.c
src/reboot.c

tests related to these modules

CODEX_REPORT/R3-F-THREAD-1.md
```

Must not edit Thread 2/3 owned files.

---

## Thread 2 — Manifest / Version / HTTP

Owns:

```text
include/edgeguard_ota/manifest.h
include/edgeguard_ota/version.h
include/edgeguard_ota/compatibility.h
include/edgeguard_ota/download.h
include/edgeguard_ota/reporting.h

src/manifest.c
src/version.c
src/compatibility.c
src/download.c
src/reporting.c

tests related to these modules

CODEX_REPORT/R3-F-THREAD-2.md
```

Use shared types from Thread 1 `model.h`.

If it is absent in the isolated branch, code against the contracts in this master package and note the merge dependency.

Do not create a competing `model.h`.

---

## Thread 3 — RAUC / Health / Buildroot Integration

Owns:

```text
include/edgeguard_ota/rauc_adapter.h
include/edgeguard_ota/health.h

src/rauc_adapter.c
src/health.c

RK3588_app/edgeguard-rk-ab/edgeguard-rk-ab-preinstall

buildroot-external/package/edgeguard-remote-ota/*
necessary buildroot-external Config.in/external.mk references

SysV init script
default agent.conf
release.json integration/template mechanism
Fake RAUC/fake command fixtures

tests related to these modules

CODEX_REPORT/R3-F-THREAD-3.md
```

Mirror the existing `edgeguard-rk-ab` Buildroot local-package style.

Do not modify Native metadata semantics in `edgeguard-rk-abctl`.

Do not modify custom backend semantics unless required solely to install the already-frozen files; architectural changes are forbidden.

---

# 23. Cross-thread rules

Threads are isolated.

Therefore:

```text
do not assume another thread's unmerged files physically exist
do not rewrite another thread's files
code against the interfaces in this master package
report missing cross-thread dependencies explicitly
```

Avoid circular module dependencies.

Preferred dependency direction:

```text
main/state_machine
    |
    +--> config
    +--> identity
    +--> persistence
    +--> manifest/version/compatibility
    +--> download
    +--> rauc_adapter
    +--> health
    +--> reporting
    +--> time_source
    +--> reboot
```

Infrastructure modules should not call the state machine directly.

Return typed results/errors to the orchestrator.

---

# 24. Testing rules

Codex environment is not authoritative for target behavior.

Create deterministic tests/fakes where useful.

Especially cover:

```text
version comparison
malformed versions
manifest validation
unknown additive manifest keys
wrong field types

download resume decision:
206 exact
206 wrong Content-Range
200 after partial
416
partial > expected
partial == expected
hash mismatch

atomic persistent-state serialization
state-machine legal/illegal transitions
REBOOT_PENDING recovery

RAUC argv construction
RAUC non-zero exit
mark-good/mark-bad argv
no shell use

pre-install target-group gate:
current a -> only rootfs.1 + boot.1
current b -> only rootfs.0 + boot.0
unexpected/ambiguous -> reject
```

If compiler or development packages are unavailable:

```text
still create tests
report that execution was unavailable
```

Never report fake tests as real target validation.

---

# 25. Required report from every thread

Create exactly one thread report:

```text
CODEX_REPORT/R3-F-THREAD-<N>.md
```

Include:

```text
Task scope

Files changed

Architecture invariants preserved

Implementation summary

Tests created

Commands actually executed

Actual results

Tests skipped and why

Known merge dependencies

Open issues

Evidence level:
CODEX VERIFIED
```

Do not write:

```text
TARGET VERIFIED
HOST VERIFIED
SDK BUILD VERIFIED
HARDWARE VERIFIED
```

---

# 26. Out of scope

Do NOT implement/research:

```text
new GPT layout
new U-Boot retry algorithm
standard RAUC U-Boot BOOT_ORDER
direct Rockchip metadata access from Agent
new Wi-Fi manager
HTTPS PKI/time architecture
R4 fully unattended acceptance campaign
R6 chaos/power-cut campaign
server rewrite
new OTA bundle format
direct RAUC D-Bus Agent implementation
downgrade support
factory device identity provisioning
```

Do not recreate deprecated:

```text
edgeguard-rk-ab-write
```

---

# 27. R3-F completion definition

Codex implementation is complete only when merged repository contains:

```text
modular C Agent source
explicit state machine
persistent REBOOT_PENDING
strict manifest/version/compatibility logic
correct Range resume implementation
libcurl integration
RAUC argv adapter
health + mark-good/mark-bad path
reboot abstraction
reporting implementation
device identity persistence
Buildroot package skeleton/integration
SysV startup integration
production RAUC pre-install safety gate
fake/test infrastructure
thread reports
```

R3-F completion does NOT imply target verification.

After Codex:

```text
R3-F
→ human evidence/diff review
→ R3-G Ubuntu/VM/Buildroot build and integration
→ R3-H RK3588 real Agent validation
```

Do not claim R3-G or R3-H completion.