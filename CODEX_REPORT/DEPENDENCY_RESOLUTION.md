# Codex-host dependency resolution

The direct versions in `requirements.txt` match `docs/R2/06_DEPENDENCY_POLICY.md`. Installation completed successfully in a workspace virtual environment using Python 3.8.5. This is evidence that the selected packages installed on the Codex host; the exact resolution must be repeated and recorded on Ubuntu 20.04 / Python 3.8.10.

Observed resolved environment:

```text
annotated-doc==0.0.4
annotated-types==0.7.0
anyio==4.5.2
certifi==2026.7.22
click==8.1.8
colorama==0.4.6
exceptiongroup==1.3.1
fastapi==0.124.4
h11==0.16.0
httpcore==1.0.9
httpx==0.28.1
idna==3.15
iniconfig==2.1.0
packaging==26.2
pluggy==1.5.0
pydantic==2.10.5
pydantic-core==2.27.2
pytest==8.3.4
sniffio==1.3.1
starlette==0.44.0
tomli==2.4.1
typing-extensions==4.13.2
uvicorn==0.33.0
```

The test suite exercises the application-owned bounded single-range path and rejects comma-separated ranges. It does not route untrusted range input through Starlette's legacy multi-range `FileResponse` parser.
