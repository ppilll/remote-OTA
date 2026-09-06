"""Portable source-contract checks, NOT execution/compilation of the C code."""
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCES = {p.name: p.read_text(encoding="utf-8") for p in (ROOT / "src").glob("*.c")}
HEADERS = {p.name: p.read_text(encoding="utf-8")
           for p in (ROOT / "include/edgeguard_ota").glob("*.h")}
OWNED_SOURCE = {name + ".c" for name in
                ("main", "config", "identity", "persistence", "state_machine", "time_source", "reboot")}


class SourceContract(unittest.TestCase):
    def test_owned_source_whitespace(self):
        for path in [*(ROOT / "src").glob("*.c"), *(ROOT / "include/edgeguard_ota").glob("*.h"),
                     *(ROOT / "tests").glob("*.c"), *(ROOT / "tests").glob("*.py")]:
            if path.parent.name == "src" and path.name not in OWNED_SOURCE:
                continue
            for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                self.assertEqual(line, line.rstrip(), f"{path.name}:{number}")

    def test_thread_one_sources_present(self):
        self.assertTrue(OWNED_SOURCE <= SOURCES.keys())
        for name in ("model", "config", "identity", "persistence", "state_machine", "time_source", "reboot"):
            self.assertIn(name + ".h", HEADERS)

    def test_no_shell_or_block_device_access_in_core(self):
        source = "\n".join(SOURCES[name] for name in OWNED_SOURCE)
        for forbidden in (r"\bsystem\s*\(", r"\bpopen\s*\(", r"/bin/(?:ba)?sh",
                          r"/dev/mmcblk", r"/dev/disk/", r"[\"']/dev/.*misc",
                          r"--no-verify", r"write-slot"):
            self.assertIsNone(re.search(forbidden, source), forbidden)

    def test_only_reboot_executes_a_program(self):
        for name in OWNED_SOURCE - {"reboot.c"}:
            self.assertIsNone(re.search(r"\b(?:exec\w*|posix_spawn\w*|g_spawn\w*)\s*\(", SOURCES[name]))
        self.assertIn('char *const argv[] = {"/sbin/reboot", NULL}', SOURCES["reboot.c"])
        self.assertIn("execve(argv[0], argv, environment)", SOURCES["reboot.c"])

    def test_atomic_write_source_order(self):
        source = SOURCES["persistence.c"]
        source = source[source.index("gboolean ota_atomic_write("):source.index("static const char *state_names")]
        positions = [source.index(token) for token in (
            "O_WRONLY | O_CREAT | O_EXCL", "write(fd,", "fsync(fd)",
            "renameat(dir, temporary, dir, base)", "fsync(dir)")]
        self.assertEqual(positions, sorted(positions))
        self.assertNotIn("O_TRUNC", source)

    def test_clock_and_dependencies(self):
        self.assertIn("CLOCK_MONOTONIC", SOURCES["time_source.c"])
        self.assertIn("report->timestamp_valid = FALSE", SOURCES["time_source.c"])
        self.assertIn("g_key_file_load_from_data", SOURCES["config.c"])
        self.assertIn("json-glib/json-glib.h", SOURCES["persistence.c"])
        self.assertIn('"/proc/sys/kernel/random/uuid"', SOURCES["identity.c"])
        for name in OWNED_SOURCE - {"time_source.c"}:
            self.assertNotIn("CLOCK_REALTIME", SOURCES[name])

    def test_frozen_states_and_error_codes(self):
        required = """IDLE CHECK_NETWORK CHECK_UPDATE PRECHECK DOWNLOADING VERIFY_DOWNLOAD
            RAUC_VERIFY INSTALLING REBOOT_PENDING BOOT_NEW_SLOT HEALTH_CHECK MARK_GOOD
            REPORT_SUCCESS ROLLBACK ERROR""".split()
        for state in required:
            self.assertIn("OTA_STATE_" + state, HEADERS["model.h"])
        required_errors = """NONE CONFIG_INVALID IDENTITY_INVALID IDENTITY_AMBIGUOUS
            LOCAL_RELEASE_INVALID MANIFEST_HTTP MANIFEST_INVALID DEVICE_COMPAT_MISMATCH
            VERSION_MALFORMED VERSION_COLLISION DOWNGRADE_REJECTED DOWNLOAD_HTTP
            DOWNLOAD_RANGE_MISMATCH DOWNLOAD_DISK_SPACE DOWNLOAD_SIZE_MISMATCH
            DOWNLOAD_HASH_MISMATCH RAUC_VERIFY_FAILED RAUC_COMPAT_MISMATCH
            RAUC_INSTALL_FAILED RAUC_MARK_GOOD_FAILED RAUC_MARK_BAD_FAILED HEALTH_FAILED
            REBOOT_CONTEXT_INVALID REBOOT_FAILED PERSISTENCE_FAILED REPORT_FAILED
            ILLEGAL_TRANSITION""".split()
        for code in required_errors:
            self.assertIn("X(" + code + ")", HEADERS["model.h"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
