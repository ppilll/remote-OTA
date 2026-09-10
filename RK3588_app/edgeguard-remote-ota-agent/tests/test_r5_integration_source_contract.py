"""R5 Thread 3 source/static contracts; this file does not build target code."""
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parents[1]
CONTROL = (ROOT / "src/control.c").read_text(encoding="utf-8")
RUNTIME = (ROOT / "src/runtime_endpoint.c").read_text(encoding="utf-8")
MAIN = (ROOT / "src/main.c").read_text(encoding="utf-8")
OTA_MK = (REPO / "buildroot-external/package/edgeguard-remote-ota/edgeguard-remote-ota.mk").read_text(encoding="utf-8")
PROVISIONING = REPO / "buildroot-external/package/edgeguard-provisioning"


class R5IntegrationSourceContract(unittest.TestCase):
    def test_control_socket_is_narrow_and_root_only(self):
        self.assertIn('"/run/edgeguard-remote-ota/control.sock"',
                      (ROOT / "include/edgeguard_ota/control.h").read_text(encoding="utf-8"))
        for required in ("SO_PEERCRED", "credentials.uid != 0", "CONTROL_MAX_JSON 1024u",
                         '"CHECK_UPDATE_NOW"', "json_object_get_size(object) == 3",
                         "listen(control->listener, 3)"):
            self.assertIn(required, CONTROL)
        for forbidden in ("rauc install", "mark-good", "mark-bad", "/dev/mmcblk", "system(", "popen("):
            self.assertNotIn(forbidden, CONTROL)

    def test_idle_wakeup_and_cycle_snapshot_are_explicit(self):
        self.assertIn("ota_control_idle_wait", MAIN)
        self.assertIn("ota_runtime_endpoint_refresh", MAIN)
        self.assertRegex(MAIN, re.compile(
            r"ota_control_idle_wait\(.*?refresh_cycle_config\(.*?OTA_STATE_CHECK_NETWORK",
            re.DOTALL))
        self.assertRegex(MAIN, re.compile(
            r"static gboolean refresh_cycle_config\(.*?ota_runtime_endpoint_refresh\(",
            re.DOTALL))
        self.assertIn("services->phase(services->user, &cycle_config", MAIN)

    def test_runtime_reader_cannot_open_wifi_secret(self):
        self.assertIn('"/userdata/edgeguard-provisioning/runtime.json"',
                      (ROOT / "include/edgeguard_ota/runtime_endpoint.h").read_text(encoding="utf-8"))
        self.assertNotIn("wifi.json", RUNTIME)
        for required in ("O_NOFOLLOW", "st.st_uid != 0", "(st.st_mode & 0777) != 0600",
                         "EGP_STORE_MAX_BYTES", "egp_runtime_parse_json"):
            self.assertIn(required, RUNTIME)

    def test_buildroot_source_lists_and_modes(self):
        for source in ("src/control.c", "src/runtime_endpoint.c", "src/endpoint.c"):
            self.assertIn(source, OTA_MK)
        package_mk = (PROVISIONING / "edgeguard-provisioning.mk").read_text(encoding="utf-8")
        frozen_sources = [
            "main.c", "store.c", "endpoint.c", "connman.c", "protocol.c",
            "security.c", "status.c", "operations.c", "input.c", "bluez.c",
        ]
        for source in frozen_sources:
            self.assertEqual(package_mk.count("/src/" + source), 1)
        self.assertIn("-m 0755", package_mk)
        self.assertIn("S98edgeguard-provisioning", package_mk)
        self.assertIn("daemon.conf", package_mk)


if __name__ == "__main__":
    unittest.main(verbosity=2)
