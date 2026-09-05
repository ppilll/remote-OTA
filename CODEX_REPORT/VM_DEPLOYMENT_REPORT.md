# R2 VM Deployment Report

Environment:
```text
Ubuntu 20.04
Python 3.8.10
systemd 245
FastAPI/Uvicorn
TCP 8000
```

Interfaces:
```text
ens33: VM Internet
ens34: wired development/NFS; not valid R2 Wi-Fi evidence
ens38: R2 Wi-Fi-side interface
```

Validated target-session VM address:
```text
10.119.65.50
```

It is runtime/DHCP state and must not become an API constant.

The human test owner confirmed VM deployment/systemd/server validation passed. Detailed terminal/journal transcript was omitted from the final handoff request.

Result:
```text
HOST VERIFIED
```
