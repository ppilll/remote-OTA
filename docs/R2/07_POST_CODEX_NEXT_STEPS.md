# What To Do After Codex Finishes

Codex completion is not the R2 stage completion.

Use the following order.

---

# Gate 1 — Review Codex output

Inspect:

```text
CODEX_REPORT/SUMMARY.md
CODEX_REPORT/FILES_CHANGED.md
CODEX_REPORT/HOST_TEST_RESULTS.md
CODEX_REPORT/DEPENDENCY_RESOLUTION.md
CODEX_REPORT/OPEN_ISSUES.md
```

Reject the Codex result if it claims target/hardware evidence that Codex could not obtain.

Expected result:

```text
CODEX VERIFIED
HOST VERIFIED
```

or a clearly documented host failure.

---

# Gate 2 — Deploy to Ubuntu VM

On Ubuntu 20.04 / Python 3.8.10:

1. copy/clone server repository
2. create virtual environment
3. install pinned requirements
4. generate/place dummy artifact at:
   `artifacts/versions/1.1.0/update.raucb`
5. run pytest
6. run local server manually
7. run localhost smoke test
8. install/start systemd service
9. inspect journald
10. verify listener on TCP 8000

Capture evidence for `VM_DEPLOYMENT_REPORT.md`.

---

# Gate 3 — Freeze current Wi-Fi server address

Immediately before target validation:

```text
ip -4 addr show ens38
```

Record the current address.

The planning-time `10.119.65.50/24` is evidence of the architecture, not a permanent server configuration.

---

# Gate 4 — Prove the intended network path

Do not allow the wired NFS/development route to create a false PASS.

During Wi-Fi validation, disconnect/disable the wired development path as appropriate.

VM evidence:

```text
ip route get <RK3588_WIFI_IP>
```

Expected:

```text
dev ens38
```

RK3588 evidence:

```text
ip route get <VM_ENS38_IP>
```

Expected:

```text
dev wlan0
```

Capture both.

---

# Gate 5 — RK3588 API validation

Perform, in order:

1. GET manifest
2. inspect status/body
3. single-range artifact GET
4. full artifact GET
5. SHA256 comparison against manifest
6. POST device report
7. inspect VM journal for the target client/report

This proves the R2 HTTP server/device transport contract over the real R1 Wi-Fi path.

It does not prove OTA installation.

---

# Gate 6 — Failure tests on VM/LAN

Exercise selected failures without modifying BSP:

- stop server -> target connection failure
- request missing path -> 404
- temporarily remove artifact -> expected manifest/artifact error behavior
- malformed report -> 400/422
- invalid Range -> 400/416
- optional firewall block/recovery test
- Wi-Fi disconnect/recovery if operationally safe

Record observed behavior rather than expected behavior only.

---

# Gate 7 — Build R2 evidence package

Prepare:

```text
R2_STAGE_REPORT.md
ARCHITECTURE_DELTA.md
API_SPEC.md
TEST_REPORT.md
VM_DEPLOYMENT_REPORT.md
TARGET_VALIDATION_REPORT.md
CODEX_REPORT/
KNOWN_ISSUES.md
NEXT_STAGE_NOTES.md
```

The final requirement matrix must separate:

- expected
- observed
- evidence
- evidence level
- result

---

# Gate 8 — R2 verdict

Only after VM and target validation:

## PASS

Use only if all required R2 server/API/network-integrity/report tests pass on the intended Wi-Fi path.

## CONDITIONAL PASS

Use when the core R2 goal is proven but a bounded, explicitly non-blocking issue remains.

## FAIL

Use when a required R2 capability cannot be demonstrated, including incorrect route/interface evidence, artifact integrity failure, report failure, or deployment failure.

---

# After R2 PASS

Return the evidence package to the Project Controller.

The next stage may then begin R3 Remote OTA Agent architecture/implementation planning.

Do not let Codex silently continue into R3.
