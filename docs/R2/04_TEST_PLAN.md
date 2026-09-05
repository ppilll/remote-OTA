# R2 Test Plan

## Evidence rule

Host/server tests and RK3588 target validation are separate gates.

A passing Codex/pytest suite does not prove RK3588 behavior.

Allowed evidence labels:

- CODEX VERIFIED
- HOST VERIFIED
- TARGET VERIFIED
- HARDWARE VERIFIED

Codex can produce only the first two.

---

# A. HOST TESTS

Run on the Codex environment and/or Ubuntu VM with Python 3.8.10.

## A1. Dependency compatibility

Verify:

```text
python3 --version
```

Expected:

```text
Python 3.8.10
```

Install the pinned R2 requirements and record the resolved package versions.

Result: HOST VERIFIED only after actual execution on the Ubuntu VM.

## A2. Manifest schema

Test:

- status 200
- required fields exist
- `schema_version == 1`
- compatible matches configured value
- version matches active release
- `artifact_url == "/update.raucb"`
- SHA256 is 64 lowercase hex characters
- `size` is positive integer
- `mandatory` is boolean

## A3. Manifest artifact integrity

For the active dummy artifact:

1. independently calculate SHA256
2. independently obtain byte size
3. request `/manifest.json`
4. compare manifest SHA256 and size

Both must match.

## A4. Full artifact download

Request `/update.raucb` without Range.

Expect:

- 200
- correct Content-Length
- `Accept-Ranges: bytes`
- downloaded bytes exactly equal source artifact
- downloaded SHA256 equals manifest SHA256

## A5. Single range

Test at minimum:

- `bytes=0-1023`
- a middle range
- a final-byte range
- a valid suffix range if implemented

Expect:

- 206
- exact byte payload
- exact Content-Range
- exact Content-Length

## A6. Range negative cases

Test:

- start beyond EOF -> 416
- inverted range -> 416 or documented 400, consistently
- malformed number -> 400
- wrong unit -> 400
- comma-separated multi-range -> 400
- very long/malicious-looking Range string -> quick bounded rejection

The test must demonstrate that R2 does not use the vulnerable legacy multi-range path.

## A7. Artifact missing

Remove/rename artifact in an isolated fixture.

Expected:

- manifest does not advertise a valid release; 503 for configured-but-inconsistent release
- artifact endpoint returns 404

Restore the fixture afterward.

## A8. Permission/read error

Inject or fixture a read failure.

Expected:

- no stack trace returned to client
- server logs error reason
- response is a defined server error

## A9. Device report valid

POST a valid report.

Expect:

- 204
- structured log contains device_id and status
- no secret fields are logged

## A10. Device report invalid

Test:

- malformed JSON -> 400
- missing device_id -> 422
- empty device_id -> 422
- unknown status -> 422
- invalid timestamp -> 422

## A11. Concurrency

Issue concurrent manifest/artifact requests.

R2 host acceptance target:

- at least 10 concurrent requests
- no corrupt responses
- no mismatched checksums
- no unhandled exception

This is an MVP stability test, not a production load benchmark.

## A12. Logging

Verify logs include, where applicable:

- timestamp
- client IP
- method/path
- status code
- device_id
- artifact access
- error reason

Verify logs do not contain:

- Wi-Fi password
- token/secret material
- arbitrary complete request body

---

# B. VM DEPLOYMENT TEST

Run on Ubuntu 20.04.

## B1. systemd

Verify:

```text
systemctl daemon-reload
systemctl start edgeguard-ota
systemctl status edgeguard-ota
```

Expected:

- active/running
- listener on TCP 8000

## B2. local smoke

On VM:

```text
curl http://127.0.0.1:8000/manifest.json
curl -o /tmp/r2-update.raucb http://127.0.0.1:8000/update.raucb
```

Compare SHA256.

## B3. ens38 listener reachability

Record:

```text
ip -4 addr show ens38
ss -ltnp
```

Use the current `ens38` IPv4 address. The planning-time address `10.119.65.50` is not assumed permanent.

---

# C. TARGET TEST — RK3588

Must be performed after Codex/host work.

## C0. Path isolation

Do not use `ens34`/NFS as evidence.

Prefer physically disconnecting the wired development path as already practiced for Wi-Fi validation.

## C1. VM route evidence

Record on VM:

```text
ip -4 addr show ens38
ip route get <RK3588_WIFI_IP>
```

Expected egress/interface for RK3588 Wi-Fi address:

```text
dev ens38
```

## C2. RK3588 route evidence

Record on RK3588:

```text
ip addr show wlan0
ip route get <VM_ENS38_IP>
```

Expected:

```text
dev wlan0
```

This route/interface evidence is mandatory to prevent false PASS through the development interface.

## C3. Manifest

From RK3588:

```text
curl -v http://<VM_ENS38_IP>:8000/manifest.json
```

Capture:

- request
- HTTP status
- response body
- VM server log

## C4. Range

From RK3588:

```text
curl -v -r 0-1023 -o /tmp/r2-range.bin \
  http://<VM_ENS38_IP>:8000/update.raucb
```

Verify 206 and byte count.

## C5. Full artifact integrity

From RK3588:

```text
curl -o /tmp/update.raucb \
  http://<VM_ENS38_IP>:8000/update.raucb

sha256sum /tmp/update.raucb
```

Compare to manifest SHA256.

## C6. Device report transport

Send a static R2 validation report with curl.

Success proves the HTTP/report API path only.

It does not prove a device daemon or OTA Agent.

## C7. Evidence label

If C1-C6 pass:

`TARGET VERIFIED` for:

- manifest retrieval
- artifact transfer
- range behavior
- SHA256 transfer integrity
- report POST
- Wi-Fi route/interface path

Do **not** claim:

- RK3588 OTA PASS
- RAUC PASS
- A/B PASS
- rollback PASS
