# Codex host-work summary

## Verdict

**CODEX VERIFIED — HOST READY FOR UBUNTU VM VALIDATION**

The Thread 2-owned R2 tests, tools, deployment assets, repository README, deterministic dummy artifact, and host evidence were created and exercised in the Codex environment. No `app/**` server-core file was changed.

This is not `HOST VERIFIED` under the R2 gate because the available Codex host was Windows with Python 3.8.5, not the required Ubuntu 20.04 VM with Python 3.8.10. It is not target or hardware evidence.

## Executed evidence

- Pinned dependencies installed successfully into workspace `.venv`.
- `python -m pytest -q`: 32 tests passed in 0.94 seconds on the final run.
- A real Uvicorn listener on `127.0.0.1:8000` was exercised by `scripts/host_smoke.py`.
- Smoke result: `HOST SMOKE PASSED`.
- Repository artifact observed size: 33,554,432 bytes.
- Independently observed artifact SHA256: `4489164df7478188c3fd968f2c8d77a6e2a0522ae051972f588495cf9e118823`.
- Syntax compilation with `python -m compileall -q app tests scripts` completed successfully.

Detailed commands, scope, and limitations are in `HOST_TEST_RESULTS.md`. Unresolved environment gates are in `OPEN_ISSUES.md`.

## Evidence boundary

No Ubuntu systemd, VM `ens38`, RK3588 `wlan0`, OTA Agent, RAUC, A/B slot, boot control, rollback, or hardware validation was performed or claimed.
