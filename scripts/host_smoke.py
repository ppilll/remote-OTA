#!/usr/bin/env python3
"""Exercise a running R2 server using only the Python standard library."""

import argparse
import hashlib
import json
import tempfile
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import urljoin
from urllib.request import Request, urlopen


def fetch(request: Request):
    return urlopen(request, timeout=15)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default="http://127.0.0.1:8000/")
    parser.add_argument("--device-id", default="host-smoke")
    args = parser.parse_args()
    base_url = args.base_url.rstrip("/") + "/"

    try:
        with fetch(Request(urljoin(base_url, "manifest.json"))) as response:
            if response.status != 200:
                raise RuntimeError("manifest returned HTTP {0}".format(response.status))
            manifest = json.loads(response.read().decode("utf-8"))

        required = {
            "schema_version",
            "device_compatible",
            "version",
            "build_id",
            "artifact_url",
            "sha256",
            "size",
            "mandatory",
        }
        if set(manifest) != required:
            raise RuntimeError("manifest fields do not match the R2 schema")

        artifact_url = urljoin(base_url, manifest["artifact_url"].lstrip("/"))
        with tempfile.TemporaryDirectory(prefix="edgeguard-r2-smoke-") as temp_dir:
            download_path = Path(temp_dir) / "update.raucb"
            digest = hashlib.sha256()
            size = 0
            with fetch(Request(artifact_url)) as response, download_path.open("wb") as output:
                if response.status != 200:
                    raise RuntimeError("artifact returned HTTP {0}".format(response.status))
                if response.headers.get("Accept-Ranges") != "bytes":
                    raise RuntimeError("artifact response omitted Accept-Ranges: bytes")
                while True:
                    chunk = response.read(1024 * 1024)
                    if not chunk:
                        break
                    output.write(chunk)
                    digest.update(chunk)
                    size += len(chunk)
            if size != manifest["size"] or digest.hexdigest() != manifest["sha256"]:
                raise RuntimeError("downloaded artifact size or SHA256 does not match manifest")

        range_request = Request(artifact_url, headers={"Range": "bytes=0-1023"})
        with fetch(range_request) as response:
            range_body = response.read()
            if response.status != 206 or len(range_body) != 1024:
                raise RuntimeError("single-range smoke check failed")
            expected_content_range = "bytes 0-1023/{0}".format(manifest["size"])
            if response.headers.get("Content-Range") != expected_content_range:
                raise RuntimeError("unexpected Content-Range")

        report = json.dumps(
            {
                "device_id": args.device_id,
                "current_version": "1.0.0",
                "status": "downloaded",
                "timestamp": "2026-09-02T04:00:00Z",
            }
        ).encode("utf-8")
        report_request = Request(
            urljoin(base_url, "device/report"),
            data=report,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with fetch(report_request) as response:
            if response.status != 204 or response.read() != b"":
                raise RuntimeError("device report smoke check failed")
    except (HTTPError, URLError, OSError, ValueError, RuntimeError) as exc:
        print("HOST SMOKE FAILED: {0}".format(exc))
        return 1

    print("HOST SMOKE PASSED")
    print("manifest version: {0}".format(manifest["version"]))
    print("artifact size: {0}".format(manifest["size"]))
    print("artifact sha256: {0}".format(manifest["sha256"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
