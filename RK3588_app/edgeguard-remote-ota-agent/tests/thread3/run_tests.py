"""Build Thread 3 C tests with a POSIX compiler/GLib; exit 77 if unavailable."""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import sys
import tempfile


def main():
    compiler = shutil.which(os.environ.get("CC", "cc"))
    pkg = shutil.which("pkg-config")
    if not compiler or not pkg:
        print("SKIPPED / NOT HOST VERIFIED: POSIX C compiler and/or pkg-config")
        return 77
    if os.name != "posix":
        print("SKIPPED / NOT HOST VERIFIED: POSIX process APIs required")
        return 77
    flags = subprocess.run([pkg, "--cflags", "--libs", "glib-2.0"], capture_output=True, text=True)
    if flags.returncode:
        print("SKIPPED / NOT HOST VERIFIED: GLib development package")
        return 77
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="ota-thread3-build-") as temporary:
        fake = str(Path(temporary) / "fake-command")
        test = str(Path(temporary) / "test-rauc-health")
        common = [compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-g"]
        subprocess.run([*common, str(root / "tests/thread3/fake_command.c"), "-o", fake], check=True)
        subprocess.run([*common, "-I", str(root / "include"),
                        str(root / "tests/thread3/test_rauc_health.c"),
                        str(root / "src/rauc_adapter.c"), str(root / "src/health.c"),
                        *shlex.split(flags.stdout), "-o", test], check=True)
        subprocess.run([test, fake], check=True, timeout=30)
    return 0


if __name__ == "__main__":
    sys.exit(main())
