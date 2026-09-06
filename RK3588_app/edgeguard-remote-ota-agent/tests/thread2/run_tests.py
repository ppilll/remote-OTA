"""Optional R3-G runner. Never invokes a shell. Not executed during Thread 2.
Requires POSIX GNU11, pkg-config, GLib, JSON-GLib and libcurl development files.
Exit 77 means required build dependencies are unavailable, not a passing test.
Python is test tooling only; the deployed Agent remains C with frozen libraries.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import sys
import tempfile


def main():
    root = Path(__file__).resolve().parents[2]
    compiler = shutil.which(os.environ.get("CC", "cc"))
    pkg = shutil.which("pkg-config")
    if not compiler or not pkg or os.name != "posix":
        print("SKIPPED / NOT AVAILABLE: POSIX C compiler and pkg-config required")
        return 77
    probe = subprocess.run([pkg, "--cflags", "--libs", "glib-2.0", "gthread-2.0",
                            "json-glib-1.0", "libcurl"], capture_output=True, text=True)
    if probe.returncode:
        print("SKIPPED / NOT AVAILABLE: GLib / JSON-GLib / libcurl development packages")
        print(probe.stderr)
        return 77
    sources = [root / "src" / (name + ".c") for name in
               ("version", "manifest", "compatibility", "download", "reporting", "time_source")]
    flags = ["-std=gnu11", "-D_GNU_SOURCE", "-D_FILE_OFFSET_BITS=64",
             "-Wall", "-Wextra", "-Werror", "-g", "-I", str(root / "include")]
    with tempfile.TemporaryDirectory(prefix="ota-thread2-build-") as temporary:
        for name in ("test_values", "test_http"):
            binary = str(Path(temporary) / name)
            subprocess.run([compiler, *flags, str(Path(__file__).parent / (name + ".c")),
                            *(str(p) for p in sources), *shlex.split(probe.stdout),
                            "-o", binary], check=True)
            subprocess.run([binary], check=True, timeout=120)
    return 0


if __name__ == "__main__":
    sys.exit(main())
