# R4 Architecture Context

## 1. System identity

| Area | Frozen value | Evidence class |
|---|---|---|
| Project | EdgeGuard Remote OTA | FROZEN FACT |
| Board | 正点原子 ATK-DLRK3588 | FROZEN FACT |
| SoC / architecture | Rockchip RK3588 / AArch64 | FROZEN FACT |
| Storage | eMMC | FROZEN FACT |
| Build system | Rockchip / 正点原子 vendor Buildroot 2021.11 family | FROZEN FACT |
| Kernel | Linux 5.10.209 family | FROZEN FACT |
| U-Boot | Rockchip vendor U-Boot 2017.09 family | FROZEN FACT |
| OTA installer | RAUC 1.5.1 | FROZEN FACT |
| Transport | Wi-Fi | FROZEN FACT |
| Future provisioning/control | BLE | DEFERRED |

## 2. Frozen architecture

The verified R3 system consists of these responsibility boundaries:

| Component | Responsibility | R4 treatment |
|---|---|---|
| OTA Agent | Long-running discovery, decision, download, verification, install orchestration, reboot recovery, health, mark-good/rollback, and reporting | Preserve; incremental configuration/hook integration only |
| R2 server | Manifest, RAUC bundle delivery including Range behavior, and device report endpoint | Preserve core behavior; add backward-compatible optional report fields |
| RAUC 1.5.1 | Bundle verification and installation | Preserve |
| Native Rockchip A/B layer | Read/update Rockchip `AvbABData` through the established control/backend tools | Preserve |
| U-Boot / GPT / kernel | Boot selection and platform runtime | Preserve |
| Buildroot integration | Build/install Agent, configuration, release identity, RAUC configuration, init script, and dependencies | Add the external health hook and R4 candidate configuration |
| S99 init script | Validate `/userdata`, honor maintenance marker, and manage Agent process lifecycle | Preserve responsibility boundary |
| `/userdata` durable state | Persist attempt identity and recovery position across process restart/reboot | Preserve schema and semantics |

## 3. Reviewed R3 Agent structure

**CODE REVIEWED:** The production Agent is a C implementation with modules for main/orchestration, services, configuration, manifest handling, compatibility, download, persistence, RAUC adapter, health, reboot, reporting, and state machine.

**CODE REVIEWED:** The Buildroot package compiles and installs the Agent and its configuration with dependencies including libcurl, GLib, JSON-GLib, RAUC, and the EdgeGuard Rockchip A/B tooling.

**CODE REVIEWED:** S99 verifies that `/userdata` is mounted and writable, honors `/userdata/edgeguard-remote-ota/autostart-disabled`, and manages only the Agent process. It does not perform health acceptance, mark-good, rollback, OTA retry, or force-kill RAUC.

## 4. Frozen R3 lifecycle

```text
IDLE
  -> CHECK_NETWORK
  -> CHECK_UPDATE
  -> PRECHECK
  -> DOWNLOADING
  -> VERIFY_DOWNLOAD
  -> RAUC_VERIFY
  -> INSTALLING
  -> REBOOT_PENDING

reboot

BOOT_NEW_SLOT
  -> HEALTH_CHECK
  -> MARK_GOOD
  -> REPORT_SUCCESS
  -> IDLE
```

Existing recovery branches include:

- `HEALTH_CHECK -> ROLLBACK` on failed candidate health;
- boot-fallback recovery;
- durable `ERROR`/`ROLLBACK` outcomes where appropriate;
- replay-safe retry for transient network/report work;
- no replay of an uncertain install.

This lifecycle is a **FROZEN FACT** for R4. `07_STATE_MACHINE_DELTA.md` defines the narrow delta.

## 5. Existing R3 behavior relevant to R4

### 5.1 Long-running polling

**CODE REVIEWED:** `IDLE` does not terminate the Agent. Startup performs an update check, and later checks occur according to `poll_interval_sec`. No update or the same release returns to `IDLE`. Transient discovery/network failures are retried without converting them into A/B changes.

### 5.2 Download and resume

**CODE REVIEWED / HOST VERIFIED:** The downloader uses a `.part` file and safe HTTP Range rules:

- a valid `206` with the exact expected offset may append;
- a `200` response to a Range request restarts from zero rather than appending;
- an incorrect `Content-Range` is rejected;
- `416` is handled by verifying or safely restarting according to local state;
- transport interruption retains and synchronizes the partial file for resume.

R4 needs real-target campaign evidence; it does not need a downloader rewrite.

### 5.3 Destructive-operation boundary

**CODE REVIEWED / HOST VERIFIED:** The transition from `RAUC_VERIFY` persists `INSTALLING` before a later iteration calls `rauc install`. `INSTALLING` is deliberately not replay-safe. If execution is interrupted or the result becomes uncertain, recovery does not automatically run a second install for the same attempt.

### 5.4 Acceptance and reporting separation

**CODE REVIEWED / HOST VERIFIED:** Successful candidate acceptance is committed before success reporting. `REPORT_SUCCESS` is durable and retryable. Report retry must not replay install, health, or mark-good.

The current target snapshot illustrates this separation: slot `b` is confirmed-good and primary while the durable state remains `REPORT_SUCCESS`.

## 6. Current target baseline

**TARGET OBSERVED — R4 entry baseline:**

```text
release:             1.2.4
build_id:            rk3588-r3h-1.2.4-001
current slot:        b
primary slot:        b
slot a:              confirmed-good
slot b:              confirmed-good
Agent:               running
autostart-disabled:  absent
durable state:       REPORT_SUCCESS
attempt_id:          c035795a-2a2e-4c23-8dce-8457fb91a579
wlan0:               present, down
Wi-Fi context:       intentionally not connected
connmand:            running
wpa_supplicant:      running
/userdata:           mounted read/write and writable
server base URL:     http://10.119.65.50:8000
reporting mode:      legacy
```

This proves the snapshot values only. It does not claim that the pending report has since completed or that any R4 campaign has passed.

## 7. R4 architecture delta

### 7.1 Health acceptance

**ARCHITECTURE DECISION:** Preserve all built-in R3 health and release/slot guards. Add a local-only executable hook installed as:

```text
/usr/libexec/edgeguard/edgeguard-health-check
```

The hook checks:

- `wlan0` exists;
- `connmand` exists/runs;
- `wpa_supplicant` exists/runs;
- ConnMan exposes Wi-Fi technology through its local control plane.

It does not require AP association, DHCP, DNS, Internet, or OTA-server reachability. It must not enable Wi-Fi, mutate configuration, mark slots, reboot, or write boot metadata.

**PROPOSED VALUE:** Require three consecutive successful samples, five seconds apart, within the existing 30-second health timeout budget.

### 7.2 Laboratory endpoint

**ARCHITECTURE DECISION:** R4 uses the current configured contract:

```text
http://10.119.65.50:8000
```

The laboratory must keep this address stable for the campaign. The endpoint is configuration, not an Agent binary constant.

**DEFERRED:** Persistent runtime endpoint override/provisioning moves to R5 unless the stable laboratory address cannot be guaranteed. If stability is not guaranteed, R4 must record the limitation and revisit this decision before claiming unattended validation.

### 7.3 Report correlation

**CODE REVIEWED:** The Agent supports `legacy` and `extended` reporting modes. Extended mode carries correlation fields such as attempt, target release/build, Agent state, error code, uptime, and timestamp validity.

**CODE REVIEWED:** The current R2 server rejects extra fields and currently accepts only the legacy report schema/status set. Therefore enabling Agent extended mode alone would break reporting.

**ARCHITECTURE DECISION:** Extend the existing `POST /device/report` contract additively:

- exact legacy payloads remain accepted;
- the Agent's existing extended fields become optional and type-validated;
- unrelated unknown fields remain rejected;
- structured logs include correlation fields when present;
- manifest, artifact, and Range behavior remain unchanged.

The R4 candidate may enable `mode=extended` only after server compatibility is host verified and the compatible server is the campaign endpoint.

## 8. Explicit non-deltas

R4 does not add durable states, change the state schema, redesign automatic polling, relax install-once safety, change rollback guards, change the A/B backend, or make server reachability a reason to reject firmware.

