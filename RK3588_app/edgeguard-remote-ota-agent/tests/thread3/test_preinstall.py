"""Execute a copy of the real safety script with ONLY its fixed ABCTL path replaced.

No production environment bypass is added. TEST_SH may select a POSIX shell;
Windows Git sh is supported if supplied. No RAUC, devices or metadata are touched.
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[4]
SHELL = os.environ.get("TEST_SH") or shutil.which("sh")


@unittest.skipUnless(SHELL, "POSIX shell unavailable")
class PreinstallTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ota-preinstall-")
        self.directory = Path(self.temporary.name)
        self.fake = self.directory / "abctl"
        self.fake.write_text('#!/bin/sh\n[ "$#" -eq 1 ] && [ "$1" = get-current ] || exit 99\n'
                             'printf "%s" "$TEST_CURRENT"\nexit "${TEST_EXIT:-0}"\n', encoding="utf-8")
        self.fake.chmod(0o755)
        original = (ROOT / "RK3588_app/edgeguard-rk-ab/edgeguard-rk-ab-preinstall").read_text(encoding="utf-8")
        literal = "ABCTL=/usr/bin/edgeguard-rk-abctl"
        self.assertEqual(original.count(literal), 1)
        # Quote a trusted temporary path for the static script, never target data.
        quoted = "'" + self.fake.as_posix().replace("'", "'\\''") + "'"
        self.script = self.directory / "preinstall"
        self.script.write_text(original.replace(literal, "ABCTL=" + quoted), encoding="utf-8")

    def tearDown(self):
        self.temporary.cleanup()

    def gate(self, current, slots, names, exit_code=0):
        env = {key: value for key, value in os.environ.items() if not key.startswith("RAUC_")}
        env.update(TEST_CURRENT=current, TEST_EXIT=str(exit_code))
        if slots is not None:
            env["RAUC_TARGET_SLOTS"] = slots
        env.update({"RAUC_SLOT_NAME_" + key: value for key, value in names.items()})
        return subprocess.run([SHELL, str(self.script)], env=env, cwd=str(self.directory),
                              capture_output=True, text=True, timeout=5)

    def test_opposite_groups_and_order(self):
        for current, suffix in (("a\n", "1"), ("b\n", "0"), ("a", "1"), ("b", "0")):
            for slots in ("1 4", "4 1", "\t4\n1 "):
                result = self.gate(current, slots, {"1": "rootfs." + suffix, "4": "boot." + suffix})
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, "")

    def test_bad_identity(self):
        for current in ("", "unknown", "A", "a\nb\n", "a\n\n", " a", "a\r\n"):
            self.assertNotEqual(self.gate(current, "1 2", {"1": "rootfs.1", "2": "boot.1"}).returncode, 0)
        self.assertNotEqual(self.gate("a\n", "1 2", {"1": "rootfs.1", "2": "boot.1"}, 7).returncode, 0)

    def test_bad_target_indices(self):
        for slots in (None, "", "1", "1 2 3", "1 1", "01 2", "-1 2", "a 2", "1; 2",
                      "$(touch sentinel) 2", "* 2", "rootfs.1 boot.1"):
            result = self.gate("a\n", slots, {"1": "rootfs.1", "2": "boot.1"})
            self.assertNotEqual(result.returncode, 0, repr(slots))
        self.assertFalse((self.directory / "sentinel").exists())

    def test_bad_groups_or_names(self):
        for names in ({}, {"1": "rootfs.1"}, {"1": "rootfs.0", "2": "boot.0"},
                      {"1": "rootfs.1", "2": "boot.0"}, {"1": "rootfs.1", "2": "rootfs.1"},
                      {"1": "boot.1", "2": "boot.1"}, {"1": "recovery.1", "2": "boot.1"},
                      {"1": "rootfs.1\n", "2": "boot.1"}, {"1": "rootfs.1", "2": "boot.1\nextra"},
                      {"1": "$(touch sentinel)", "2": "boot.1"}):
            self.assertNotEqual(self.gate("a\n", "1 2", names).returncode, 0, repr(names))
        self.assertFalse((self.directory / "sentinel").exists())


if __name__ == "__main__":
    if not SHELL:
        print("SKIPPED / NOT AVAILABLE: POSIX shell")
        sys.exit(77)
    unittest.main(verbosity=2)
