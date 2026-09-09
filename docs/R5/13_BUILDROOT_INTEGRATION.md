# R5 Buildroot and SysV Integration

## Package boundary

Add a separate package:

```text
buildroot-external/package/edgeguard-provisioning/
├── Config.in
├── edgeguard-provisioning.mk
└── S98edgeguard-provisioning
```

Source lives at `RK3588_app/edgeguard-provisioningd/`. Do not fold it into `edgeguard-remote-ota` or change ownership of `S99edgeguard-remote-ota`.

## Dependencies

The daemon design requires C, GLib/GIO/GDBus, JSON-GLib, system D-Bus, BlueZ, ConnMan, and Linux input APIs. The examined external tree already uses Buildroot dependency names `libglib2` and `json-glib`; exact vendor symbols for BlueZ, ConnMan, and D-Bus must be located in the real vendor SDK before writing `select` lines. Do not invent `BR2_*` symbols from memory and do not claim an SDK build.

The package should normally depend on the packages that install the required runtime libraries/services rather than embedding or shelling out to their CLIs. `bluetoothctl`, `connmanctl`, and `wpa_cli` are diagnostics, not product dependencies.

## Installed files

| Path | Mode | Owner | Purpose |
|---|---:|---|---|
| `/usr/bin/edgeguard-provisioningd` | `0755` | root:root | daemon |
| `/etc/init.d/S98edgeguard-provisioning` | `0755` | root:root | SysV service |
| optional `/etc/edgeguard-provisioning/daemon.conf` | `0644` | root:root | non-secret keycode/hold/window policy |
| `/userdata/edgeguard-provisioning/` | `0700` | root:root | created only after confirmed mount |
| `/var/lib/connman/edgeguard-provisioning.config` | `0600` | root:root | runtime-derived, not shipped in image |

No credential, runtime override, generated ConnMan profile, bond, or target evidence is baked into the rootfs.

## Config.in wiring

Add the new package source to `buildroot-external/Config.in` under the existing EdgeGuard menu. `external.mk` already includes package makefiles via wildcard and should not need a special include. Preserve the existing package entries.

## SysV lifecycle

Use `S98edgeguard-provisioning`, before the existing `S99edgeguard-remote-ota`, after normal D-Bus/BlueZ/ConnMan scripts. Ordering numbers are startup hints, not readiness guarantees.

Start behavior:

1. verify executable and non-secret config;
2. verify `/userdata` is a mounted writable filesystem, not merely a directory on rootfs;
3. create/chmod the provisioning directory;
4. start one daemon instance with a PID file and restrictive umask;
5. do not use a fixed long sleep to claim service readiness.

The daemon itself tracks D-Bus and the owners of `org.bluez` and `net.connman`, registering/reconciling when they appear. A late service or owner restart must not require a board reboot.

Stop behavior requests graceful termination, closes the window, unregisters advertisement/GATT/agent when possible, clears in-memory secrets, and removes its runtime PID state. It must not stop ConnMan, BlueZ, the OTA Agent, RAUC, or force-kill an active OTA operation.

## Agent package delta

Thread 3 adds only the IPC/runtime-endpoint sources needed by `edgeguard-remote-ota-agent`, updates the explicit C source list in `edgeguard-remote-ota.mk`, and preserves immutable configuration installation, release metadata ownership, RAUC files, and `S99edgeguard-remote-ota` safety behavior.

## Static package review

Confirm paths, modes, source list, dependency names found in the actual tree, absence of secrets, absence of duplicate init ownership, and no rootfs overlay that overwrites runtime provisioning data. This is source/static review, not a build or installed-image claim.
