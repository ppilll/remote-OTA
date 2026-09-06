#!/usr/bin/env python3
"""Build-machine-only identity validation; no Python installed on the target."""
import argparse
import configparser
import json
import os
from pathlib import Path
import re
import stat
import sys
import tempfile

HERE = Path(__file__).resolve().parent
FIELDS = {"schema_version", "device_compatible", "rauc_compatible", "version", "build_id"}


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON field: " + key)
        result[key] = value
    return result


def read_release(path):
    path = Path(path)
    if not path.is_absolute() or not path.is_file() or path.stat().st_size > 65536:
        raise ValueError("release input must be an absolute regular file of at most 64 KiB")
    value = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_object)
    if not isinstance(value, dict) or set(value) != FIELDS:
        raise ValueError("release requires exactly the five schema-1 fields")
    if type(value["schema_version"]) is not int or value["schema_version"] != 1:
        raise ValueError("schema_version must be integer 1")
    for key in FIELDS - {"schema_version"}:
        text = value[key]
        if (not isinstance(text, str) or not text or len(text.encode("utf-8")) >= 256
                or any(ord(c) < 32 or ord(c) == 127 for c in text)
                or re.search(r"@[A-Z_]+@", text)):
            raise ValueError("invalid or placeholder release field: " + key)
    if value["device_compatible"] != "atk-dlrk3588":
        raise ValueError("device_compatible does not match this board package")
    if value["rauc_compatible"] != "EdgeGuard-ATK-DLRK3588-RK3588":
        raise ValueError("rauc_compatible does not match production system.conf")
    version = value["version"]
    if not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", version):
        raise ValueError("version must be canonical MAJOR.MINOR.PATCH")
    if any(int(component) > 4294967295 for component in version.split(".")):
        raise ValueError("version component exceeds uint32")
    return value


def write_release(value, output):
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n",
                                         dir=str(output.parent), delete=False) as stream:
            temporary = stream.name
            json.dump(value, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, str(output))
    finally:
        if temporary and os.path.exists(temporary):
            os.unlink(temporary)


def profile(path):
    parser = configparser.ConfigParser(interpolation=None, strict=True)
    with Path(path).open(encoding="utf-8") as stream:
        parser.read_file(stream)
    if parser.defaults():
        raise ValueError("RAUC profile must not use DEFAULT overrides")
    return {section: dict(parser[section]) for section in parser.sections()}


def check_target(target, expected=None):
    target = Path(target)
    actual = read_release(target / "etc/edgeguard-ota/release.json")
    if expected is not None and actual != expected:
        raise ValueError("target release differs from build input")
    if profile(target / "etc/rauc/system.conf") != profile(HERE / "system.conf"):
        raise ValueError("target RAUC profile differs from frozen production profile (overlay override?)")
    gate = target / "usr/libexec/rauc/edgeguard-rk-ab-preinstall"
    source = HERE.parents[2] / "RK3588_app/edgeguard-rk-ab/edgeguard-rk-ab-preinstall"
    # HERE.parents[2] is the repository root.
    if gate.read_bytes() != source.read_bytes():
        raise ValueError("target pre-install safety gate was replaced")
    for relative in ("usr/bin/edgeguard-remote-ota-agent", "usr/bin/rauc",
                     "usr/bin/edgeguard-rk-abctl", "usr/libexec/rauc/edgeguard-rk-ab-backend",
                     "usr/libexec/rauc/edgeguard-rk-ab-preinstall",
                     "etc/init.d/S99edgeguard-remote-ota"):
        path = target / relative
        if not path.is_file() or not path.stat().st_mode & (stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH):
            raise ValueError("missing executable: " + relative)
    for relative in ("etc/rauc/keyring.pem", "etc/edgeguard-ota/agent.conf"):
        path = target / relative
        if not path.is_file() or not path.stat().st_size:
            raise ValueError("missing provisioned file: " + relative)
    # Certificate cryptographic validity remains RAUC's responsibility.


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--output")
    mode.add_argument("--check-target")
    args = parser.parse_args()
    try:
        expected = read_release(args.input) if args.input else None
        if args.output:
            if expected is None:
                raise ValueError("an explicit --input release identity is required")
            write_release(expected, args.output)
        else:
            check_target(args.check_target, expected)
    except (OSError, ValueError, UnicodeError, configparser.Error) as error:
        print("edgeguard-remote-ota: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
