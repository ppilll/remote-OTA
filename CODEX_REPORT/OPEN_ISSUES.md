# Open issues and unexecuted gates

## Required before `HOST VERIFIED`

- Run dependency installation, the full pytest suite, and the smoke script on the Ubuntu 20.04 VM's required Python 3.8.10. The Codex host provided Python 3.8.5 on Windows.
- Install and exercise `deploy/edgeguard-ota.service.template` with systemd 245. Record service status, journal output, and the TCP 8000 listener. No systemd command was executed by Codex.
- Record the final dependency resolution from the Ubuntu VM; the resolution in `DEPENDENCY_RESOLUTION.md` is from the Codex-host virtual environment.

## Required separate target gate

- Discover the current DHCP-derived VM `ens38` address at validation time; no address is frozen or hard-coded.
- Execute `deploy/RK3588_TARGET_VALIDATION.md` manually, including VM route evidence through `ens38` and RK3588 route evidence through `wlan0` while preventing the wired/NFS development path from producing false evidence.
- Record actual target manifest, range, full-download SHA256, report, and VM journal observations. Codex did not execute or mark any target command complete.

## Stage limitations, not hidden defects

- Python 3.8 is end-of-life and constrains the dependency set. A supported runtime upgrade remains production-stage technical debt.
- R2 experimental-LAN HTTP has no production authentication, TLS, signing, or PKI. These are explicitly outside R2 scope.
- The dummy `update.raucb` is deterministic data only; it is not installable and does not validate RAUC or artifact authenticity.

No server-core defect was found by the Thread 2 host tests.
