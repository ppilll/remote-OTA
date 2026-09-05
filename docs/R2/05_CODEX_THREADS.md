# R2 Codex Thread Plan

## Decision

Use **2 implementation threads**.

Do not create a large parallel fan-out.

Reasoning:

- API models and server handlers are tightly coupled.
- Tests depend on the endpoint contract.
- Deployment/docs are mostly independent once the server entry point is frozen.
- Two threads keep file overlap low and make review manageable.

A final integration pass is performed in the parent/main Codex session. It is not a third implementation thread.

---

# Thread 1 — Server Core

## Ownership

Primary ownership:

```text
app/**
requirements.txt
artifact metadata implementation
server startup/config
```

Thread 1 owns the API behavior contract in code.

## Responsibilities

- enforce Python 3.8.10-compatible dependencies
- create FastAPI application
- create Pydantic request/response models
- calculate artifact SHA256/size at startup
- implement `/manifest.json`
- implement safe single-range `/update.raucb`
- implement `/device/report`
- structured logging
- basic local smoke checks

## Explicit restrictions

- no OTA Agent
- no RAUC
- no A/B
- no target claims
- do not use the vulnerable legacy Starlette multi-range parser
- no DB/cloud/auth scope expansion

## Handoff condition to Thread 2

Thread 1 must first stabilize:

- module layout
- app import path
- endpoint paths
- report model
- manifest model
- dependency file

Then Thread 2 may build tests/deployment around that contract.

---

# Thread 2 — Tests, Tools, Deployment, Docs

## Ownership

Primary ownership:

```text
tests/**
scripts/**
deploy/**
README.md
CODEX_REPORT/**
```

Thread 2 should not edit `app/**` in parallel.

If a test reveals a server defect, report the defect back to Thread 1 or wait until Thread 1 has stopped editing before applying a coordinated fix.

## Responsibilities

- pytest/API tests
- manifest/hash/size checks
- single-range positive/negative tests
- malformed/multi-range rejection tests
- device report validation tests
- concurrent request test
- dummy artifact generation script
- host smoke script
- systemd service template/guide
- target validation command sheet
- evidence/report templates

## Restrictions

- no invented target results
- no marking target commands as executed
- no BSP modifications
- no unrelated refactors

---

# Final integration pass — Parent Codex session

After both threads complete:

1. inspect diffs for file overlap/conflicts
2. run installation under Python 3.8.10
3. run full pytest suite
4. run local HTTP smoke test
5. verify manifest SHA/size against artifact
6. verify range tests
7. inspect logs
8. produce `CODEX_REPORT/`
9. list unresolved issues without hiding failures

## Final Codex verdict

Codex may conclude:

- HOST READY FOR VM VALIDATION
- HOST VERIFIED

It must not conclude R2 stage PASS until VM deployment and RK3588 target validation are performed outside Codex.
