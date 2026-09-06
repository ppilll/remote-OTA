# R3-F Thread 3 package integration

Evidence level: CODEX VERIFIED. These are local-package files, not a completed
Buildroot/SDK or board validation. Current main.c has a weak fail-closed
ota_agent_services_init; merged phase/service bindings remain an integration task.

This mirrors edgeguard-rk-ab: SITE_METHOD=local, TARGET_CC, GNU11, generic-package
and explicit target installs. external.mk already discovers every package/*.mk.
Dependencies are RAUC, edgeguard-rk-ab, libcurl, GLib and JSON-GLib. Python is a
build-machine dependency only. No Agent runtime or metadata dependencies were added.

1. Enable the existing RAUC 1.5.1 service/custom-backend support, then
   BR2_PACKAGE_EDGEGUARD_REMOTE_OTA=y, in the BusyBox/SysV board configuration.
   Keep D-Bus/RAUC startup and userdata mounting before S99. Enable BusyBox
   start-stop-daemon (including -b/-m/-t), printenv, sleep and standard file applets.
2. Copy release.json.in to a board/release input outside the package source. Replace
   @VERSION@ and @BUILD_ID@ with the identity of the image being built. Set
   BR2_PACKAGE_EDGEGUARD_REMOTE_OTA_RELEASE_FILE to its absolute build-machine path.
   Missing input/placeholders/malformed identity fail the build; no sample identity
   is installed. The version uses canonical uint32 MAJOR.MINOR.PATCH. Publish this
   same version/build_id in the manifest after producing the corresponding bundle.
3. Provision /etc/rauc/keyring.pem through your trusted board inputs. This package
   deliberately contains no test keyring and creates no trust key automatically.
   Set the LAN server endpoint in agent.conf through board inputs as needed.
4. On the Linux build checkout, make post-build-check.sh executable and append
   its absolute path LAST to BR2_ROOTFS_POST_BUILD_SCRIPT.
   It checks the final target after overlays: production profile, exact safety gate,
   executable files, trust-file presence, and valid release. Do not apply the old
   r3-c RAUC overlay: its readonly slots would replace the production profile.
   Package install hooks cannot detect later overlay changes. The post-build check
   is a required board integration step, not silently registered globally.
5. Build/rebuild the local package after source or release input changes. Buildroot
   does not track external JSON file contents as package rebuild dependencies.
   For comparison with the original identity input, additionally run the host
   prepare-release.py --input /absolute/release.json --check-target /absolute/target.
   Do not overwrite release.json later in a board script.

Installed files:

- /usr/bin/edgeguard-remote-ota-agent
- /usr/libexec/rauc/edgeguard-rk-ab-preinstall (from the substrate source directory)
- /etc/rauc/system.conf (symmetric writable slots plus pre-install handler)
- /etc/edgeguard-ota/agent.conf (legacy reporting, empty optional hook)
- /etc/edgeguard-ota/release.json (validated input)
- /etc/init.d/S99edgeguard-remote-ota (SysV hook only)

The dependent edgeguard-rk-ab package owns abctl/backend installation unchanged.
S99 requires /userdata to be mounted and writable, creates the private state
directory, launches the foreground Agent with --config, and uses an executable +
PID match for status/stop. Stop requests TERM and waits at most 35 seconds; it never
force-kills or restarts RAUC. BusyBox background startup may discard Agent stderr;
run the Agent foreground during integration to inspect typed diagnostics. There is
no automatic respawn/reinstall. Initial liveness is not candidate health acceptance.

The adapter uses 30-second query and one-hour install deadlines by default. Health
uses its configured timeout per external command, including the optional argv[0]
hook. A CLI timeout/failure may leave service-side install outcome uncertain; the
orchestrator must enter ERROR, never retry the same attempt or restart RAUC.
Mark-good/mark-bad APIs require the persisted expected candidate and re-read
get-current. Health itself never marks, persists or reboots. After unhealthy,
the caller may use guarded mark-bad, persist ROLLBACK only on success, then reboot
through Thread 1. An identity ambiguity blocks both mark operations.

Remaining R3-G work includes actual compiler/library compatibility, merged service
binding, vendor Buildroot/Kconfig and pkg-config sysroot behavior, BusyBox options,
post-build registration, service ordering and keyring provisioning. Actual RAUC
1.5.1 environment/output behavior, RK3588 slot safety and reboot outcomes require
the later real integration stages. Fakes do not establish these results.
