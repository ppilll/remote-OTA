# R2 Known Issues

## KI-01 Python 3.8 legacy runtime
R2 is constrained to older compatible FastAPI/Starlette/Uvicorn versions. Production should move to a supported runtime.

## KI-02 Active artifact immutability
Manifest metadata is cached at startup. Same-size in-place replacement can leave cached SHA256 stale.

R2 mitigation:
```text
do not mutate active artifact
restart server after changing release content
```

## KI-03 Manifest 204 contract cleanup
Older design material discussed a no-active-release HTTP 204 path, but the actual R2 implementation expects a configured active release at startup. Future Agent code must not depend on that 204 path without a server change.

## KI-04 Dummy `.raucb`
The R2 artifact is not an installable/signed RAUC bundle.

## KI-05 HTTP only
No production TLS/auth/PKI is validated.

## KI-06 Evidence archival
Final route/manifest/range/full/SHA transcript is present in stage context. Detailed VM and report raw logs were confirmed passed but omitted from the final handoff input.
