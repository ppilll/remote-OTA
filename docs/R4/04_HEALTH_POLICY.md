# R4 Health Policy

## 1. Decision

**ARCHITECTURE DECISION:** R4 preserves the R3 built-in acceptance checks and adds one local-only external Wi-Fi health hook.

```text
R4 acceptance
  = existing built-in health and orchestration guards
  + external local Wi-Fi control-plane hook
```

The hook is not a replacement for built-in health, a connectivity test, or an independent slot-management program.

## 2. Existing acceptance chain

**CODE REVIEWED:** Built-in `health.c` checks the expected current slot, writable/persistent `/userdata`, required RAUC and boot-control executables/configuration, and successful RAUC status operation. The external hook facility already exists and is optional in R3.

**CODE REVIEWED:** Candidate release identity is guarded in orchestration/service logic both before health acceptance and again before mark-good. Required identity includes:

- `device_compatible`;
- `rauc_compatible`;
- version;
- build ID.

Therefore the complete pre-R4 acceptance chain is broader than `health.c` alone:

```text
booted release identity matches the durable attempt
+ current slot is the expected candidate
+ /userdata persistence is operational
+ RAUC/boot-control runtime is operational
+ optional platform hook passes
```

R4 must preserve all of this.

## 3. External hook contract

### 3.1 Installed path

**ARCHITECTURE DECISION:**

```text
/usr/libexec/edgeguard/edgeguard-health-check
```

The Buildroot package installs the executable and configures the existing health hook setting to reference it.

### 3.2 Required observations

One sample passes only when all four conditions are true:

1. `/sys/class/net/wlan0` exists.
2. The `connmand` process is present.
3. The `wpa_supplicant` process is present.
4. ConnMan's local control plane exposes Wi-Fi technology.

“Exposes Wi-Fi technology” means the service/control API can enumerate a Wi-Fi technology object. It does not require the technology to be powered, connected, associated, or configured with an access point.

### 3.3 Explicit non-requirements

The hook must not require:

- `wlan0` link state `UP`;
- Wi-Fi power to be enabled by policy;
- a saved network profile;
- AP association;
- DHCP or an IPv4 address;
- DNS resolution;
- Internet reachability;
- OTA-server reachability;
- a successful manifest fetch or report POST.

These depend on environment, credentials, deliberate configuration, or server availability. None is sufficient evidence that the candidate firmware is bad.

The current baseline—`wlan0` present but down because Wi-Fi was intentionally not connected, with `connmand` and `wpa_supplicant` running—is a concrete reason for this distinction.

### 3.4 Side-effect restrictions

The hook must not:

- run `connmanctl enable wifi` or otherwise change ConnMan state;
- create, edit, or delete network profiles;
- mark a slot good or bad;
- select a primary slot;
- invoke install;
- reboot or stop services;
- write boot metadata, misc, or block devices;
- clear or rewrite Agent durable state;
- contact an AP, Internet endpoint, or OTA server as its health truth.

The hook observes and reports. The existing Agent state machine owns the consequence of pass/fail.

## 4. Stabilization policy

**PROPOSED VALUE:**

```text
successful samples required: 3 consecutive
sample interval:             5 seconds
total hook/health timeout:   30 seconds
```

Initial semantics:

- sample immediately when the hook starts;
- a failed sample breaks the consecutive-success streak;
- continue at five-second intervals while the 30-second budget permits;
- return success as soon as three consecutive samples pass;
- return failure if the required streak cannot be completed within the budget;
- never continue in the background after the caller's timeout.

These numbers are not a product SLA. Target timing evidence may justify adjustment, but any change must remain bounded and must be recorded as a proposed-value revision before the final R4 campaign.

## 5. Outcome ownership

### Hook pass

A hook pass contributes one result to the existing `HEALTH_CHECK`. It does not mark the slot good itself. The existing candidate identity and slot guards still apply before `MARK_GOOD`.

### Hook failure or timeout

A hook failure/timeout is an existing health failure. The frozen R3 path is used:

```text
HEALTH_CHECK failure
  -> guarded mark-bad of expected current candidate
  -> durable ROLLBACK
  -> reboot/fallback according to existing orchestration
```

No new durable state is introduced.

### Server or AP failure

Failure to reach an AP or `http://10.119.65.50:8000` is not a hook failure and cannot, by itself, cause mark-bad or rollback. Discovery/report logic remains retryable under the R3 state machine.

## 6. Auto-connect is a separate operational requirement

R4 unattended OTA requires the device to regain usable Wi-Fi under the campaign's documented network configuration. That is verified separately from firmware acceptance.

An auto-connect campaign may demonstrate that a saved profile and normal service startup regain network access without engineer action. Failure may block OTA operations and must be diagnosed, but it does not automatically prove the installed slot is unhealthy. Credential absence, AP outage, intentional Wi-Fi disablement, or lab-network changes remain environmental/configuration causes.

## 7. Diagnostics

**CODE REVIEWED:** The current command runner captures hook standard output, while hook standard error is visible through the Agent's error path. Therefore the initial hook should provide a concise, stable failure reason on standard error.

Recommended diagnostic shape, not a new wire protocol:

```text
HEALTH_FAIL reason=<local_reason>
```

Suggested local reasons include missing `wlan0`, missing `connmand`, missing `wpa_supplicant`, and unavailable ConnMan Wi-Fi technology. Diagnostics must not include secrets or saved network credentials.

## 8. Verification classification

| Claim | Required evidence |
|---|---|
| Hook logic and failure modes | HOST VERIFIED |
| Hook included/executable/configured in target image | SDK/Buildroot verified plus image inspection |
| Passing local Wi-Fi stack reaches mark-good | TARGET VERIFIED |
| Deterministic hook failure reaches rollback and never mark-good | TARGET VERIFIED |
| AP/server outage does not reject otherwise healthy firmware | TARGET VERIFIED |
| Proposed timing fits candidate boot behavior | TARGET measurement, then freeze or revise |

No host-only result may be presented as proof of target health acceptance.

