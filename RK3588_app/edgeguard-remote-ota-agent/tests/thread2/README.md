# Thread 2 deterministic tests

Evidence ceiling: CODEX VERIFIED. The C suites were created but not executed in the
Codex session: the user prohibited shell commands. Compilation and runtime results
are SKIPPED; source inspection does not substitute for those results.

Optional later POSIX execution from any directory:

    python3 RK3588_app/edgeguard-remote-ota-agent/tests/thread2/run_tests.py

The runner uses argv subprocess calls with no shell, temporary build outputs and
exit 77 when dependencies are unavailable. Python is development tooling only,
matching the existing Core test convention; no target runtime dependency is added.
It links the five Thread 2 implementations and Thread 1 time_source.c/model.h.

## Fixtures and assertions

- test_values.c embeds the valid abc-bundle manifest and local release fixtures.
  It calls production functions for canonical uint32 versions (including overflow),
  tuple ordering, every required key's missing/wrong type, future additive fields,
  strict JSON syntax, duplicate decoded keys, escaped NUL, int64 limits, the 64 KiB
  boundary, artifact path rejection, hash syntax, local release and both compatibility
  gates. Mandatory never bypasses the Agent/version gate. RAUC comparison is separate.
- The pure response table tests exact 206, missing/wrong start/end/total, malformed
  ranges, 200 after partial, both 416 decisions and oversized input.
- test_http.c starts a raw IPv4 loopback server on an ephemeral port. Scripts assert
  actual Range headers and feed the real libcurl callbacks; files use private temp
  directories. It checks byte content, final artifact preservation on failure,
  verified promotion, and partial removal/truncation as appropriate.
- HTTP scripts cover absent/empty/oversized partial, 206 append, ignored Range/200,
  missing/wrong/duplicate range, 416 with a body, repeated 416 bounded termination,
  404, 503, timeout, TCP reset, short Content-Length transfer, short close-delimited
  response, wrong Content-Length, oversized chunked response, corruption on full
  GET/resume, encoded response rejection, interim 100 and redirect rejection.
- Complete valid/corrupt partials are exercised without a network listener, plus
  disk-reserve rejection, symlink and FIFO rejection. No actual disk exhaustion or
  power-loss durability claim is made.
- Report tests assert all states, exactly four legacy or eleven extended keys,
  local current_version, untrusted realtime fallback, monotonic uptime and explicit
  mode rejection. POST fixtures parse the real wire JSON and test 204/503 responses.
- Manifest HTTP tests distinguish 200/204/503/404, invalid JSON and bounded body size;
  204 leaves the output manifest unchanged and returns available=false.

Timeout fixtures wait on a condition released when the client returns (one-second
request timeout); a five-second watchdog prevents a stalled fixture from hanging.
A TCP reset can arrive before the last sent byte is consumed, so that assertion
accepts either original prefix or extended prefix and requires retryable failure.
All scripts use finite requests; no external server, network configuration, RAUC or
board access is involved. These suites still need compilation and execution in R3-G.

## Integration contract

- Call ota_manifest_fetch after validated config. TRUE with available=false means
  no active server release, independently of the locally installed version.
- ota_release_load is the strict local-identity loader for the Thread 1 adapter.
- ota_compatibility_precheck covers local/manifest validity, device equality and
  version/build policy. ota_compatibility_rauc is the independent equality gate
  after successful RAUC signature/metadata inspection owned by Thread 3.
- Call ota_download_bundle while the Agent lock is held and persisted attempt
  metadata matches the .part file. On a new release, the orchestrator must remove
  stale partial data. A final SHA256 check protects against a mismatched old prefix,
  but persistence remains the authority for attempt ownership.
- A range tail must be start=N, end=expected-1, total=expected. Shorter subranges
  are rejected; no automatic multi-segment HTTP download is introduced.
- No HTTP redirects or environment proxies are used. HTTP(S) requests stay on the
  configured origin, content decoding is disabled, and bundle content encoding
  must be absent or identity. Config must have passed ota_config_load.
- Network failures return retryability and keep a safe prefix; retries belong to
  the orchestrator. HTTP 416 gets at most one full recovery GET per call. A valid
  200 after a Range request supplies the restart body directly from byte zero.
- Download success includes size + SHA256, fsync(file), rename and fsync(directory).
  The RAUC_VERIFY phase is still required; SHA256 is not signature verification.
  If directory fsync fails after rename, failure is returned even though the final
  pathname may now exist. The orchestrator must fail closed.
- Report building consumes Thread 1's injectable clock. In legacy mode, installation,
  reboot and health phases map to downloaded; completed success maps to idle and
  rollback maps to error. Extended mode exposes installing/reboot_pending/success/
  rollback. No extra fields are sent by default.
- This task does not supply ota_agent_services_init/phase routing, edit main.c,
  configuration defaults, Buildroot or Thread 3 adapters. Merge ownership must wire
  these APIs into the existing fail-closed service adapter and include sources in
  the build. Thread 1 model.h already exists; no competing shared model was created.
