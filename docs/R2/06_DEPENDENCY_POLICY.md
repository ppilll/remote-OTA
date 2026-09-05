# R2 Dependency Policy — Python 3.8.10

## Hard constraint

The Ubuntu 20.04 VM uses Python 3.8.10.

Do not let pip silently select current package releases that require newer Python.

## Recommended explicit pins

Use the following baseline unless an actual Python-3.8 installation test proves an incompatibility and the change is documented:

```text
fastapi==0.124.4
starlette==0.44.0
uvicorn==0.33.0
pydantic==2.10.5
anyio==4.5.2
httpx==0.28.1
pytest==8.3.4
```

Transitive dependencies may be resolved by pip, but the final resolved versions must be recorded in `CODEX_REPORT/DEPENDENCY_RESOLUTION.md`.

## Why these pins exist

- FastAPI 0.125.0 dropped Python 3.8; 0.124.4 is the last Python-3.8-compatible FastAPI release line.
- Uvicorn 0.34.0 dropped Python 3.8; use 0.33.0.
- Starlette 0.45.0 dropped Python 3.8; use 0.44.0.
- Pydantic 2.11 dropped Python 3.8; use the 2.10 line.
- AnyIO 4.6.0 requires Python 3.9; use 4.5.2.
- pytest 8.4.0 requires Python 3.9; use the 8.3 line.
- HTTPX 0.28.1 supports Python 3.8.

## Important Range security constraint

Starlette added `FileResponse` Range support in 0.39.0.

A later security advisory identified a CPU-exhaustion issue in the multi-range parser for Starlette versions `>=0.39.0, <=0.49.0`. The upstream patch is in 0.49.1, but that release does not support Python 3.8.

Therefore R2 must not expose the old multi-range parser on untrusted Range input.

For R2:

- implement a small single-range parser in application code
- reject comma-separated multi-range syntax
- bound input parsing
- stream selected file regions manually
- test malicious/multi-range rejection

This is a documented Python-3.8 legacy constraint.

## Technical debt

Python 3.8 is end-of-life upstream and constrains the web stack to older dependency branches.

R2 may proceed on the isolated experimental LAN, but R4 production planning should include a supported Python/runtime upgrade before Internet-facing production deployment.
