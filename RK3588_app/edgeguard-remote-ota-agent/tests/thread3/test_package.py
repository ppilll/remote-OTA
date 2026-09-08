"""Portable source/release tests. These do not compile or execute Agent C code."""
from pathlib import Path
import configparser
import importlib.util
import json
import os
import re
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[4]
PACKAGE = ROOT / "buildroot-external/package/edgeguard-remote-ota"
AGENT = ROOT / "RK3588_app/edgeguard-remote-ota-agent"
spec = importlib.util.spec_from_file_location("prepare_release", PACKAGE / "prepare-release.py")
release_tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release_tool)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ota-package-")
        self.directory = Path(self.temporary.name)
        self.input = self.directory / "release.json"
        self.value = {"schema_version": 1, "device_compatible": "atk-dlrk3588",
                      "rauc_compatible": "EdgeGuard-ATK-DLRK3588-RK3588",
                      "version": "1.2.3", "build_id": 'build-"quoted"-\\-研发'}

    def tearDown(self):
        self.temporary.cleanup()

    def put(self, value):
        self.input.write_text(json.dumps(value), encoding="utf-8")
        return release_tool.read_release(self.input)

    def test_release_roundtrip(self):
        self.assertEqual(self.put(self.value), self.value)
        output = self.directory / "out/release.json"
        release_tool.write_release(self.value, output)
        self.assertEqual(release_tool.read_release(output), self.value)
        self.assertEqual(list(output.parent.iterdir()), [output])
        for version in ("0.0.0", "4294967295.0.1"):
            self.value["version"] = version
            self.assertEqual(self.put(self.value), self.value)

    def test_release_wrong_types_missing_extra(self):
        for key in self.value:
            bad = dict(self.value)
            del bad[key]
            with self.subTest(missing=key), self.assertRaises(ValueError):
                self.put(bad)
            for value in (None, True, [], {}, 1.5):
                bad = dict(self.value, **{key: value})
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    self.put(bad)
        with self.assertRaises(ValueError):
            self.put(dict(self.value, unknown="x"))

    def test_release_versions_and_identity(self):
        for version in ("v1.2.3", "01.2.3", "1.2", "1.2.3-beta", "1.2.3+foo",
                        "1..3", "1.2.-3", "1.2.4294967296", "", "@VERSION@"):
            with self.subTest(version=version), self.assertRaises(ValueError):
                self.put(dict(self.value, version=version))
        for key, value in (("build_id", "@BUILD_ID@"), ("build_id", "x" * 256),
                           ("build_id", "x\ny"), ("build_id", "x\0y"),
                           ("device_compatible", "wrong"), ("rauc_compatible", "wrong")):
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                self.put(dict(self.value, **{key: value}))

    def test_release_malformed_file(self):
        for data in ('{"schema_version":1,"schema_version":1}', "[]", "null", "{", "x" * 65537):
            self.input.write_text(data, encoding="utf-8")
            with self.subTest(data=data[:60]), self.assertRaises(ValueError):
                release_tool.read_release(self.input)
        with self.assertRaises(ValueError):
            release_tool.read_release("relative.json")
        with self.assertRaises(ValueError):
            release_tool.read_release(self.directory)
        with self.assertRaises(ValueError):
            release_tool.read_release(PACKAGE / "release.json.in")

    def test_production_profile(self):
        profile = release_tool.profile(PACKAGE / "system.conf")
        self.assertEqual(profile["handlers"]["pre-install"],
                         "/usr/libexec/rauc/edgeguard-rk-ab-preinstall")
        self.assertEqual(profile["system"]["compatible"], self.value["rauc_compatible"])
        self.assertEqual(profile["keyring"]["check-purpose"], "codesign")
        for index, slot in enumerate("ab"):
            rootfs, boot = profile["slot.rootfs." + str(index)], profile["slot.boot." + str(index)]
            self.assertNotIn("readonly", rootfs)
            self.assertNotIn("readonly", boot)
            self.assertEqual(rootfs["device"], "/dev/disk/by-partlabel/system_" + slot)
            self.assertEqual(rootfs["bootname"], slot)
            self.assertEqual(boot["device"], "/dev/disk/by-partlabel/boot_" + slot)
            self.assertEqual(boot["parent"], "rootfs." + str(index))

    def test_default_config_and_build_sources(self):
        config = configparser.ConfigParser(interpolation=None)
        config.read(str(PACKAGE / "agent.conf"), encoding="utf-8")
        self.assertEqual(config["reporting"]["mode"], "extended")
        self.assertEqual(config["health"]["hook"],
                         "/usr/libexec/edgeguard/edgeguard-health-check")
        self.assertEqual(config["health"]["timeout_sec"], "30")
        self.assertEqual(config["rauc"]["binary"], "/usr/bin/rauc")
        self.assertEqual(config["server"]["request_timeout_sec"], "600")
        source = (AGENT / "src/config.c").read_text(encoding="utf-8")
        expected = set(re.findall(r'\{"([a-z_]+)", "([a-z_]+)"\}', source))
        actual = {(section, key) for section in config.sections() for key in config[section]}
        self.assertEqual(actual, expected)
        makefile = (PACKAGE / "edgeguard-remote-ota.mk").read_text(encoding="utf-8")
        sources = set(re.findall(r"\$\(@D\)/src/([a-z_]+\.c)", makefile))
        self.assertEqual(sources, {p.name for p in (AGENT / "src").glob("*.c")})
        self.assertIn("EDGEGUARD_REMOTE_OTA_SITE_METHOD = local", makefile)
        self.assertIn("EDGEGUARD_REMOTE_OTA_INSTALL_INIT_SYSV", makefile)
        self.assertIn("PKG_CONFIG_SYSROOT_DIR", makefile)
        self.assertIn("/usr/libexec/edgeguard/edgeguard-health-check", makefile)
        self.assertIn("package/edgeguard-remote-ota/Config.in",
                      (ROOT / "buildroot-external/Config.in").read_text(encoding="utf-8"))

    def test_owned_sources_and_shell_gate(self):
        for name in ("rauc_adapter.c", "health.c"):
            source = (AGENT / "src" / name).read_text(encoding="utf-8")
            self.assertNotRegex(source, r"\b(system|popen|execl|execvp|posix_spawnp)\s*\(")
            for prohibited in ("write-slot", "mark-active", "--no-verify", "/bin/sh", "/dev/mmcblk"):
                self.assertNotIn(prohibited, source)
        gate = (ROOT / "RK3588_app/edgeguard-rk-ab/edgeguard-rk-ab-preinstall").read_text(encoding="utf-8")
        self.assertIn('ABCTL=/usr/bin/edgeguard-rk-abctl', gate)
        self.assertIn('"$ABCTL" get-current', gate)
        self.assertIn('printenv "RAUC_SLOT_NAME_$index"', gate)
        self.assertNotRegex(gate, r"(?m)^\s*(eval|source|dd)\b")
        self.assertNotIn("RAUC_SERVICE", gate)
        for path in [*PACKAGE.iterdir(), AGENT / "src/rauc_adapter.c", AGENT / "src/health.c",
                     ROOT / "RK3588_app/edgeguard-rk-ab/edgeguard-rk-ab-preinstall"]:
            if path.is_file():
                data = path.read_bytes()
                self.assertNotIn(b"\r", data, str(path))
                self.assertTrue(data.endswith(b"\n"), str(path))

    @unittest.skipUnless(os.name == "posix", "target executable modes require a POSIX filesystem")
    def test_final_target_check(self):
        target = self.directory / "target"
        files = ("usr/bin/edgeguard-remote-ota-agent", "usr/bin/rauc", "usr/bin/edgeguard-rk-abctl",
                 "usr/libexec/rauc/edgeguard-rk-ab-backend", "etc/init.d/S99edgeguard-remote-ota",
                 "usr/libexec/rauc/edgeguard-rk-ab-preinstall", "etc/rauc/keyring.pem",
                 "etc/edgeguard-ota/agent.conf", "etc/rauc/system.conf", "etc/edgeguard-ota/release.json")
        for name in files:
            path = target / name
            path.parent.mkdir(parents=True, exist_ok=True)
            if name in ("usr/libexec/rauc/edgeguard-rk-ab-backend",
                        "usr/libexec/rauc/edgeguard-rk-ab-preinstall",
                        "etc/init.d/S99edgeguard-remote-ota"):
                path.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            else:
                path.write_text("fixture", encoding="utf-8")
            path.chmod(0o755)
        gate = target / "usr/libexec/rauc/edgeguard-rk-ab-preinstall"
        shutil.copyfile(ROOT / "RK3588_app/edgeguard-rk-ab/edgeguard-rk-ab-preinstall", gate)
        shutil.copyfile(PACKAGE / "system.conf", target / "etc/rauc/system.conf")
        release_tool.write_release(self.value, target / "etc/edgeguard-ota/release.json")
        release_tool.check_target(target, self.value)
        with self.assertRaises(ValueError):
            release_tool.check_target(target, dict(self.value, build_id="different"))
        profile = target / "etc/rauc/system.conf"
        profile.write_text(profile.read_text() + "readonly=true\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            release_tool.check_target(target, self.value)
        shutil.copyfile(PACKAGE / "system.conf", profile)
        gate.write_text("replaced", encoding="utf-8")
        with self.assertRaises(ValueError):
            release_tool.check_target(target, self.value)
        shutil.copyfile(ROOT / "RK3588_app/edgeguard-rk-ab/edgeguard-rk-ab-preinstall", gate)
        backend = target / "usr/libexec/rauc/edgeguard-rk-ab-backend"
        backend.write_bytes(b"#!/bin/sh\r\nexit 0\r\n")
        with self.assertRaises(ValueError):
            release_tool.check_target(target, self.value)


if __name__ == "__main__":
    unittest.main(verbosity=2)
