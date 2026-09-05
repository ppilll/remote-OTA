# Ubuntu 20.04 VM deployment guide

This guide deploys only the EdgeGuard R2 FastAPI server. It does not install an OTA Agent, RAUC, A/B logic, boot control, rollback, or a health check.

## 1. Prepare the application

Run on the Ubuntu 20.04 VM and verify that `python3 --version` reports Python 3.8.10 before recording VM evidence:

```bash
sudo useradd --system --home /opt/edgeguard-ota --shell /usr/sbin/nologin edgeguard-ota
sudo mkdir -p /opt/edgeguard-ota
sudo cp -a app artifacts scripts tests deploy requirements.txt /opt/edgeguard-ota/
sudo chown -R root:root /opt/edgeguard-ota
cd /opt/edgeguard-ota
sudo python3 -m venv .venv
sudo .venv/bin/python -m pip install --upgrade 'pip<25.1'
sudo .venv/bin/python -m pip install -r requirements.txt
```

If the artifact was not copied, generate the deterministic dummy data:

```bash
sudo .venv/bin/python scripts/generate_dummy_artifact.py
sudo chown -R root:root artifacts
```

The generated file is test data and is not a valid RAUC bundle.

## 2. Test before service installation

```bash
.venv/bin/python -m pytest -q
.venv/bin/uvicorn app.main:app --host 127.0.0.1 --port 8000
```

In a second VM shell:

```bash
.venv/bin/python scripts/host_smoke.py --base-url http://127.0.0.1:8000
```

Record the actual output. Do not copy expected commands into an evidence file as though they ran.

## 3. Install the systemd unit

Review `deploy/edgeguard-ota.service.template`. Its fixed `/opt/edgeguard-ota` path must match the installation above.

```bash
sudo cp deploy/edgeguard-ota.service.template /etc/systemd/system/edgeguard-ota.service
sudo systemctl daemon-reload
sudo systemctl enable --now edgeguard-ota.service
systemctl status --no-pager edgeguard-ota.service
journalctl -u edgeguard-ota.service --no-pager -n 100
ss -ltnp | grep ':8000'
```

The service listens on all VM interfaces, but application metadata contains only the relative `/update.raucb` URL. No DHCP-derived `ens38` address is hard-coded.

## 4. Identify the current Wi-Fi-facing address

Immediately before RK3588 validation:

```bash
ip -4 addr show ens38
```

Use the address observed at that time. Do not assume a planning-time address is still valid. Follow [RK3588_TARGET_VALIDATION.md](RK3588_TARGET_VALIDATION.md) for the separate, manually executed target gate.

## Service operations

```bash
sudo systemctl restart edgeguard-ota.service
sudo systemctl stop edgeguard-ota.service
journalctl -u edgeguard-ota.service -f
```

R2 intentionally uses HTTP on an isolated experimental LAN. Production authentication, TLS, signing, PKI, and supported-runtime migration remain outside this stage.
