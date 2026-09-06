"""Build/run the merged GNU11 integration suite; unavailable prerequisites exit 77."""
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
    pkg = shutil.which("pkg-config")
    if not compiler or not pkg or os.name != "posix":
        print("SKIPPED / NOT HOST VERIFIED: POSIX C compiler and pkg-config required")
        return 77
    probe = subprocess.run([pkg, "--cflags", "--libs", "glib-2.0", "gthread-2.0",
                            "json-glib-1.0", "libcurl"], capture_output=True, text=True)
    if probe.returncode:
        print("SKIPPED / NOT HOST VERIFIED: GLib / JSON-GLib / libcurl development packages")
        print(probe.stderr.strip())
        return 77
    all_sources = sorted((root / "src").glob("*.c"))
    sources = [path for path in all_sources if path.name != "main.c"]
    flags = ["-std=gnu11", "-D_FILE_OFFSET_BITS=64",
             "-Wall", "-Wextra", "-Werror", "-g", "-I", str(root / "include")]
    with tempfile.TemporaryDirectory(prefix="ota-merged-build-") as temporary:
        agent = str(Path(temporary) / "edgeguard-remote-ota-agent")
        subprocess.run([compiler, *flags, *(str(path) for path in all_sources),
                        *shlex.split(probe.stdout), "-o", agent], check=True)
        binary = str(Path(temporary) / "test-integration")
        subprocess.run([compiler, *flags, str(root / "tests/test_integration.c"),
                        *(str(path) for path in sources), *shlex.split(probe.stdout),
                        "-o", binary], check=True)
        subprocess.run([binary], check=True, timeout=120)
    return 0


if __name__ == "__main__":
    sys.exit(main())
