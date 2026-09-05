# Architecture Delta — R1 to R2

R1 proved the RK3588 Wi-Fi HTTP data path.

R2 adds:
```text
Ubuntu VM
  FastAPI OTA Server
    +-- GET /manifest.json
    +-- GET /update.raucb
    +-- POST /device/report
```

Artifact layout:
```text
artifacts/
└── versions/
    └── 1.1.0/
        └── update.raucb
```

Inherited rules:
- ConnMan remains network owner.
- Do not start a second DHCP client.
- Do not create a custom wpa_supplicant manager.
- `/manifest.json` is an EdgeGuard server manifest, not a RAUC manifest.
- SHA256 is transfer/integrity metadata, not artifact authenticity.
- Buildroot/kernel/U-Boot remain unchanged in R2.

Deferred:
Agent, RAUC, A/B, boot control, rollback, health check, TLS/PKI, auth, fleet/cloud, BLE.
