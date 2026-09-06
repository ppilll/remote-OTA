"""Portable checks for merged F1-F7 source/build contracts, not C runtime proof."""
from pathlib import Path
import re
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[3]
AGENT = ROOT / "RK3588_app/edgeguard-remote-ota-agent"
PACKAGE = ROOT / "buildroot-external/package/edgeguard-remote-ota"


class MergedSourceContract(unittest.TestCase):
    def test_strong_service_binding_is_in_production_link(self):
        services = (AGENT / "src/services.c").read_text(encoding="utf-8")
        main = (AGENT / "src/main.c").read_text(encoding="utf-8")
        makefile = (PACKAGE / "edgeguard-remote-ota.mk").read_text(encoding="utf-8")
        self.assertIn("gboolean ota_agent_services_init(", services)
        self.assertIn("$(@D)/src/main.c $(@D)/src/services.c", makefile)
        self.assertRegex(main, r"#ifdef OTA_TEST_WEAK_SERVICES\s+__attribute__\(\(weak\)\)")

    def test_single_artifact_validator_and_conservative_contract(self):
        definitions = []
        for path in (AGENT / "src").glob("*.c"):
            if re.search(r"gboolean\s+ota_artifact_path_valid\s*\(", path.read_text(encoding="utf-8")):
                definitions.append(path.name)
        self.assertEqual(definitions, ["artifact.c"])
        self.assertIn("ota_artifact_path_valid(s->artifact_url)",
                      (AGENT / "src/persistence.c").read_text(encoding="utf-8"))
        artifact = (AGENT / "src/artifact.c").read_text(encoding="utf-8")
        for rejected in ('"\\\\%:?# "', 'strstr(path, "..")', "path[1] == '/'"):
            self.assertIn(rejected, artifact)

    def test_production_profile_and_timeout(self):
        config = (PACKAGE / "agent.conf").read_text(encoding="utf-8")
        profile = (PACKAGE / "system.conf").read_text(encoding="utf-8")
        self.assertIn("request_timeout_sec=600", config)
        self.assertIn("mode=legacy", config)
        self.assertNotIn("readonly", profile)

    def test_shell_bytes_and_attributes(self):
        paths = [ROOT / "RK3588_app/edgeguard-rk-ab/edgeguard-rk-ab-backend",
                 ROOT / "RK3588_app/edgeguard-rk-ab/edgeguard-rk-ab-preinstall",
                 PACKAGE / "S99edgeguard-remote-ota", PACKAGE / "post-build-check.sh",
                 ROOT / "build-rauc-bundle.sh"]
        for path in paths:
            data = path.read_bytes()
            self.assertNotIn(b"\r", data, str(path))
            self.assertTrue(data.startswith(b"#!"), str(path))
            self.assertIn(b"\n", data[:64], str(path))
        attributes = (ROOT / ".gitattributes").read_text(encoding="utf-8")
        for suffix in ("*.raucb -text", "*.img -text", "*.ext4 -text"):
            self.assertIn(suffix, attributes)
        mode = subprocess.run(["git", "ls-files", "-s", "--",
                               "buildroot-external/package/edgeguard-remote-ota/post-build-check.sh"],
                              cwd=str(ROOT), capture_output=True, text=True, check=True).stdout
        self.assertTrue(mode.startswith("100755 "), mode)

    def test_bundle_helper_uses_production_profile_and_data_parser(self):
        helper = (ROOT / "build-rauc-bundle.sh").read_text(encoding="utf-8")
        self.assertNotIn("$HOME/work/EG_OTA", helper)
        self.assertNotIn("board/r3-c", helper)
        self.assertIn("buildroot-external/package/edgeguard-remote-ota/system.conf", helper)
        self.assertIn("--output-format=shell", helper)
        self.assertIn("RAUC_MF_VERSION", helper)
        self.assertIn("RAUC_MF_BUILD", helper)
        self.assertIn("RAUC_IMAGE_CLASS_", helper)
        self.assertIsNone(re.search(r"(?m)^\s*(?:source|eval)\s", helper))

    def test_merged_matrix_is_registered(self):
        source = (AGENT / "tests/test_integration.c").read_text(encoding="utf-8")
        self.assertEqual(len(re.findall(r"(?m)^\s*case\s+\d+:", source)), 36)
        for name in ("happy-reboot-pending", "install-once", "post-good-report-retry",
                     "persistence-faults", "artifact-path-roundtrip"):
            self.assertIn(name, source)


if __name__ == "__main__":
    unittest.main(verbosity=2)
