# RK3588 target-validation command sheet

Status: **NOT EXECUTED BY CODEX**. Every checkbox below is for a human target-validation session. Record actual commands, timestamps, outputs, and logs in the project evidence package; do not treat this sheet as observed evidence.

Define these values from current observations, not from a planning-time address:

```bash
VM_ENS38_IP='<current VM ens38 IPv4 address>'
RK3588_WIFI_IP='<current RK3588 wlan0 IPv4 address>'
```

## 1. Isolate and prove the intended path

- [ ] Disconnect or disable the wired development/NFS path as operationally appropriate.
- [ ] On the VM, record:

```bash
ip -4 addr show ens38
ip route get "$RK3588_WIFI_IP"
```

- [ ] Confirm the route output selects `dev ens38`.
- [ ] On the RK3588, record:

```bash
ip addr show wlan0
ip route get "$VM_ENS38_IP"
```

- [ ] Confirm the route output selects `dev wlan0`.

Evidence through `ens34` or the wired NFS path does not satisfy the R2 Wi-Fi target gate.

## 2. Manifest retrieval

- [ ] On RK3588, capture verbose request, status, headers, and body:

```bash
curl -v "http://${VM_ENS38_IP}:8000/manifest.json"
```

- [ ] On the VM, capture the corresponding service log:

```bash
journalctl -u edgeguard-ota.service --since '-5 minutes' --no-pager
```

## 3. Single-range artifact transfer

- [ ] On RK3588:

```bash
curl -v -r 0-1023 -o /tmp/r2-range.bin \
  "http://${VM_ENS38_IP}:8000/update.raucb"
wc -c /tmp/r2-range.bin
```

- [ ] Record HTTP 206, `Content-Range`, and exactly 1024 downloaded bytes.

## 4. Full artifact integrity

- [ ] On RK3588:

```bash
curl -o /tmp/update.raucb "http://${VM_ENS38_IP}:8000/update.raucb"
wc -c /tmp/update.raucb
sha256sum /tmp/update.raucb
```

- [ ] Compare observed byte count and SHA256 with the manifest values. SHA256 here is transfer-integrity metadata, not artifact authenticity.

## 5. Device report transport

- [ ] On RK3588, send a static validation event:

```bash
curl -v -X POST "http://${VM_ENS38_IP}:8000/device/report" \
  -H 'Content-Type: application/json' \
  --data '{"device_id":"rk3588-001","current_version":"1.0.0","status":"downloaded","timestamp":"2026-09-02T04:00:00Z"}'
```

- [ ] Record HTTP 204 and the corresponding structured VM journal event.

## 6. Required interpretation boundary

If all checks pass, a human evidence owner may label only the tested API transport, range transfer, SHA256 integrity, report POST, and `wlan0`/`ens38` path according to project policy. These commands do not validate an OTA Agent, RAUC installation, A/B slots, boot control, rollback, or hardware OTA success.
