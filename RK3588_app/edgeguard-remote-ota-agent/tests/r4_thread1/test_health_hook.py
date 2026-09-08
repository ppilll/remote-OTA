"""Host tests for the R4 local-only Wi-Fi health hook and package wiring."""
from pathlib import Path
import configparser
import os
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[4]
PACKAGE = ROOT / "buildroot-external/package/edgeguard-remote-ota"
HOOK = PACKAGE / "edgeguard-health-check"


def write_lf(path, text):
    with Path(path).open("w", encoding="ascii", newline="\n") as stream:
        stream.write(text)


def shell_path(path):
    path = Path(path).resolve()
    if os.name != "nt":
        return str(path)
    drive, tail = os.path.splitdrive(str(path))
    return "/" + drive[0].lower() + tail.replace("\\", "/")


class HealthHookTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        requested = os.environ.get("SH")
        candidates = [requested] if requested else []
        candidates.extend([shutil.which("sh"), r"D:\Git\bin\sh.exe"])
        cls.shell = next((value for value in candidates if value and Path(value).is_file()), None)
        if cls.shell is None:
            raise unittest.SkipTest("POSIX sh is required to execute the hook fixtures")

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="edgeguard-r4-health-")
        self.directory = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def fixture(self, *, wlan=True, connmand=True, supplicant=True, sequence=("pass",)):
        fixture_dir = Path(tempfile.mkdtemp(prefix="case-", dir=self.directory))
        sys_wlan = fixture_dir / "sys/class/net/wlan0"
        proc = fixture_dir / "proc"
        commands = fixture_dir / "commands"
        commands.mkdir(parents=True)
        proc.mkdir()
        if wlan:
            sys_wlan.mkdir(parents=True)
        if connmand:
            path = proc / "101/comm"
            path.parent.mkdir()
            write_lf(path, "connmand\n")
        if supplicant:
            path = proc / "102/comm"
            path.parent.mkdir()
            write_lf(path, "wpa_supplicant\n")

        sequence_file = commands / "sequence"
        sequence_file.write_text("\n".join(sequence) + "\n", encoding="ascii")
        count_file = commands / "count"
        call_log = commands / "calls"
        sleep_log = commands / "sleeps"
        connmanctl = commands / "connmanctl"
        write_lf(connmanctl,
            "#!/bin/sh\n"
            f"count_file='{shell_path(count_file)}'\n"
            f"sequence_file='{shell_path(sequence_file)}'\n"
            f"call_log='{shell_path(call_log)}'\n"
            "count=0\n"
            "[ ! -f \"$count_file\" ] || count=$(cat \"$count_file\")\n"
            "count=$((count + 1))\n"
            "printf '%s\\n' \"$count\" > \"$count_file\"\n"
            "printf '%s\\n' \"$*\" >> \"$call_log\"\n"
            "result=$(sed -n \"${count}p\" \"$sequence_file\")\n"
            "[ \"$result\" != fail ] || exit 1\n"
            "if [ \"$result\" = pass ]; then\n"
            "    printf '/net/connman/technology/wifi\\n  Type = wifi\\n  Powered = False\\n'\n"
            "else\n"
            "    printf '/net/connman/technology/ethernet\\n  Type = ethernet\\n'\n"
            "fi\n",
        )
        sleeper = commands / "sleep"
        write_lf(sleeper,
            "#!/bin/sh\n"
            f"printf '%s\\n' \"$1\" >> '{shell_path(sleep_log)}'\n",
        )
        connmanctl.chmod(0o755)
        sleeper.chmod(0o755)

        source = HOOK.read_text(encoding="utf-8")
        replacements = {
            "/sys/class/net/wlan0": shell_path(sys_wlan),
            "/proc/[0-9]*/comm": shell_path(proc) + "/[0-9]*/comm",
            "/usr/bin/connmanctl": shell_path(connmanctl),
            'sleep "$SAMPLE_INTERVAL_SEC"':
                f"'{shell_path(sleeper)}' \"$SAMPLE_INTERVAL_SEC\"",
        }
        for original, replacement in replacements.items():
            self.assertIn(original, source)
            source = source.replace(original, replacement)
        script = fixture_dir / "health-check"
        write_lf(script, source)
        script.chmod(0o755)
        return script, call_log, sleep_log

    def run_fixture(self, **kwargs):
        script, calls, sleeps = self.fixture(**kwargs)
        result = subprocess.run(
            [self.shell, shell_path(script)], capture_output=True, text=True, timeout=10
        )
        call_lines = calls.read_text(encoding="ascii").splitlines() if calls.exists() else []
        sleep_lines = sleeps.read_text(encoding="ascii").splitlines() if sleeps.exists() else []
        return result, call_lines, sleep_lines

    def test_posix_syntax(self):
        subprocess.run([self.shell, "-n", shell_path(HOOK)], check=True)

    def test_three_healthy_local_samples_pass_while_wifi_is_unpowered(self):
        result, calls, sleeps = self.run_fixture(sequence=("pass", "pass", "pass"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls, ["technologies"] * 3)
        self.assertEqual(sleeps, ["5"] * 2)

    def test_each_required_local_observation_fails(self):
        cases = [
            ({"wlan": False}, "wlan0_missing"),
            ({"connmand": False}, "connmand_missing"),
            ({"supplicant": False}, "wpa_supplicant_missing"),
            ({"sequence": ("fail",) * 6}, "connman_wifi_unavailable"),
            ({"sequence": ("no-wifi",) * 6}, "connman_wifi_unavailable"),
        ]
        for arguments, reason in cases:
            with self.subTest(reason=reason, arguments=arguments):
                result, _, sleeps = self.run_fixture(**arguments)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("HEALTH_FAIL reason=" + reason, result.stderr)
                self.assertEqual(sleeps, ["5"] * 5)

    def test_failures_reset_the_consecutive_success_streak(self):
        result, calls, sleeps = self.run_fixture(
            sequence=("fail", "fail", "pass", "pass", "pass")
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(calls), 5)
        self.assertEqual(sleeps, ["5"] * 4)

        result, calls, sleeps = self.run_fixture(
            sequence=("pass", "fail", "pass", "fail", "pass", "pass")
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("successful_streak=2", result.stderr)
        self.assertEqual(len(calls), 6)
        self.assertEqual(sleeps, ["5"] * 5)

    def test_hook_contains_no_connectivity_or_mutation_commands(self):
        source = HOOK.read_text(encoding="utf-8")
        for forbidden in (
            "connmanctl enable", "connmanctl connect", "curl ", "wget ", "ping ",
            "mark-good", "mark-bad", "rauc ", "reboot", "/dev/mmc", "/userdata",
            "kill ", "&\n",
        ):
            self.assertNotIn(forbidden, source)

    def test_r4_candidate_package_wiring(self):
        config = configparser.ConfigParser(interpolation=None, strict=True)
        config.read(PACKAGE / "agent.conf", encoding="utf-8")
        self.assertEqual(
            config["health"]["hook"], "/usr/libexec/edgeguard/edgeguard-health-check"
        )
        self.assertEqual(config["health"]["timeout_sec"], "30")
        self.assertEqual(config["reporting"]["mode"], "extended")
        makefile = (PACKAGE / "edgeguard-remote-ota.mk").read_text(encoding="utf-8")
        self.assertIn("$(INSTALL) -D -m 0755 $(EDGEGUARD_REMOTE_OTA_PACKAGE_DIR)/edgeguard-health-check", makefile)
        self.assertIn("$(TARGET_DIR)/usr/libexec/edgeguard/edgeguard-health-check", makefile)
        postcheck = (PACKAGE / "post-build-check.sh").read_text(encoding="utf-8")
        self.assertIn('cmp -s "$here/edgeguard-health-check" "$target_hook"', postcheck)
        self.assertIn('cmp -s "$here/agent.conf" "$target_config"', postcheck)


if __name__ == "__main__":
    unittest.main(verbosity=2)
