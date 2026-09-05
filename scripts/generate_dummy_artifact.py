#!/usr/bin/env python3
"""Generate the deterministic, non-installable R2 dummy artifact."""

import argparse
import hashlib
from pathlib import Path
from typing import Iterator


PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUTPUT = PROJECT_ROOT / "artifacts" / "versions" / "1.1.0" / "update.raucb"
DEFAULT_SIZE = 32 * 1024 * 1024
BLOCK_SIZE = 64 * 1024
PREFIX = b"EDGEGUARD-R2-DUMMY-NOT-A-RAUC-BUNDLE\n"


def deterministic_blocks(total_size: int) -> Iterator[bytes]:
    written = 0
    block_index = 0
    while written < total_size:
        seed = PREFIX + str(block_index).encode("ascii") + b"\n"
        repeats = (BLOCK_SIZE + len(seed) - 1) // len(seed)
        block = (seed * repeats)[: min(BLOCK_SIZE, total_size - written)]
        yield block
        written += len(block)
        block_index += 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--size", type=int, default=DEFAULT_SIZE, help="artifact size in bytes")
    args = parser.parse_args()
    if args.size <= 0:
        parser.error("--size must be positive")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256()
    with args.output.open("wb") as artifact:
        for block in deterministic_blocks(args.size):
            artifact.write(block)
            digest.update(block)

    print("dummy artifact: {0}".format(args.output))
    print("size: {0}".format(args.size))
    print("sha256: {0}".format(digest.hexdigest()))
    print("warning: dummy test data; not a valid RAUC bundle")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
