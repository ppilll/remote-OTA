"""Build/run Thread 1 C unit tests only; unavailable toolchain exits 77.

Usage: python tests/run_core_tests.py
Needs a POSIX C toolchain, pkg-config, GLib and JSON-GLib development packages.
No board, SDK, RAUC daemon, networking, shell command or real reboot is used.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import sys
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    compiler = shutil.which(os.environ.get("CC", "cc"))
    pkg_config = shutil.which("pkg-config")
    missing = []
    if compiler is None:
        missing.append("C compiler (CC or cc)")
    if pkg_config is None:
        missing.append("pkg-config")
    if missing:
        print("SKIPPED / NOT HOST VERIFIED: " + ", ".join(missing))
        return 77
    packages = ["glib-2.0", "json-glib-1.0"]
    probe = subprocess.run([pkg_config, "--cflags", "--libs", *packages],
                           capture_output=True, text=True, check=False)
    if probe.returncode:
        print("SKIPPED / NOT HOST VERIFIED: GLib / JSON-GLib development packages")
        print(probe.stderr.strip())
        return 77
    sources = [root / "src" / (name + ".c") for name in
               ("artifact", "config", "identity", "persistence", "state_machine", "time_source", "reboot")]
    flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-g", "-I", str(root / "include")]
    libraries = shlex.split(probe.stdout)
    with tempfile.TemporaryDirectory(prefix="ota-core-build-") as temporary:
        executable = str(Path(temporary) / "test-core")
        command = [compiler, *flags, str(root / "tests/test_core.c"),
                   *(str(p) for p in sources), *libraries, "-o", executable]
        print("Building GNU11 core tests", flush=True)
        subprocess.run(command, check=True)
        # Also compile/link the skeleton and verify its safe usage-only path.
        skeleton = str(Path(temporary) / "agent-skeleton")
        subprocess.run([compiler, *flags, "-DOTA_TEST_WEAK_SERVICES", str(root / "src/main.c"),
                        *(str(p) for p in sources), *libraries, "-o", skeleton], check=True)
        usage = subprocess.run([skeleton, "--invalid-option"], capture_output=True, text=True)
        if usage.returncode != 2 or "Usage:" not in usage.stderr:
            raise RuntimeError("Skeleton usage check failed")
        subprocess.run([executable], check=True)
        orchestration = str(Path(temporary) / "test-orchestration")
        subprocess.run([compiler, *flags, str(root / "tests/test_orchestration.c"),
                        *(str(p) for p in sources), *libraries, "-o", orchestration], check=True)
        subprocess.run([orchestration], check=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
