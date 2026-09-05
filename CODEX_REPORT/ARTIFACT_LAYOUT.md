# R2 Artifact Layout

```text
artifacts/
└── versions/
    └── 1.1.0/
        └── update.raucb
```

Public endpoint:
```text
/update.raucb
```

Validated R2 dummy artifact:
```text
size: 33554432
sha256: 4489164df7478188c3fd968f2c8d77a6e2a0522ae051972f588495cf9e118823
```

The file is deterministic R2 test data, not a valid installable RAUC bundle.

Operational rule for the current implementation:
```text
finalize artifact -> start/restart server -> treat active artifact as immutable
```

Do not replace the active artifact in place while the service is running.
