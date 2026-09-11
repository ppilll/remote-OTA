# R5 Endpoint Configuration Specification

## Ownership and precedence

Only the OTA Server base URL is runtime-overridable in R5.

```text
valid /userdata/edgeguard-provisioning/runtime.json
    >
/etc/edgeguard-ota/agent.conf [server].base_url
```

The immutable file continues to provide `manifest_path`, `report_path`, timeouts, and all non-endpoint Agent configuration. There are no hidden fallback IPs. The target's frozen R4 laboratory endpoint is `http://192.168.77.1:8000`; the repository copy may contain another lab default and must not be treated as product identity.

The provisioning daemon owns and validates `runtime.json`. The OTA Agent may read this non-secret file but never reads `wifi.json`.

## Canonical base URL

Accepted schemes are `http` and syntactically valid `https`. R5 target success over HTTPS is not claimed until a trust-store/TLS campaign exists. The maximum serialized base URL length is 64 bytes. The initial 128-byte candidate was reduced after worst-case serialization with the real JSON serializers showed that RuntimeStatus could still exceed the ATT 512-byte hard bound. The 64-byte bound keeps DeviceInfo, RuntimeStatus, and OperationResult successful read values within 512 bytes with their required schema fields; the same endpoint bound is enforced by the provisioning request parser, canonical store, Agent cycle snapshot, and status/result serializers.

Requirements:

- absolute hierarchical URI with a non-empty host;
- optional decimal port in `1..65535`;
- DNS name, IPv4 literal, or bracketed IPv6 literal;
- no userinfo, password, query, fragment, control character, whitespace, NUL, percent-encoded control character, or IPv6 zone identifier;
- path must be empty or `/`; it canonicalizes to no trailing slash;
- scheme and DNS host canonicalize to lowercase; default ports may be removed;
- reject `file:`, `ftp:`, local filesystem forms, relative URLs, shell-like prefixes, and ambiguous parser results.

Parsing uses a URI parser with exact post-parse validation; no shell, `system()`, or string concatenation decides validity. The immutable default passes the same validator at Agent startup.

## Update semantics

`SET_ENDPOINT` is accepted only under `09_CONCURRENCY_POLICY.md`. Validate the complete candidate before writing. Commit `runtime.json` atomically as specified in `05_PROVISIONING_STORE_SPEC.md`. Invalid input or write failure leaves the previous effective endpoint untouched.

The operation result reports the new generation and canonical non-secret URL. It does not restart the Agent.

## Agent consumption boundary

The Agent loads its immutable config at startup. At each new `IDLE` poll/check boundary—after any wait or `CHECK_UPDATE_NOW` wake and before transition to `CHECK_NETWORK`—it performs a bounded read of `runtime.json`:

1. validate file metadata, schema, generation, and URL;
2. if valid, copy only `base_url` into a cycle-local `OtaConfig` snapshot;
3. if absent, use the immutable default;
4. if invalid/corrupt, use the immutable default and emit a redacted typed diagnostic;
5. keep the snapshot unchanged until the cycle returns to `IDLE`.

All manifest, artifact, and report URLs within that cycle derive from the same snapshot. Because endpoint writes are allowed only at safe states, an active OTA/report obligation cannot be redirected midway. Restart is not the endpoint-application mechanism.

## Identity and paths

Changing the endpoint does not change release version, build ID, artifact hash, candidate bytes, manifest schema, `manifest_path`, `report_path`, or relative `artifact_url` rules. It does not permit a BLE peer to inject a one-off bundle URL.

## Read status

RuntimeStatus may expose the canonical effective base URL because it is non-secret. If override parsing failed, report `effective_endpoint_source:"immutable_default"` plus `runtime_config_error:"ENDPOINT_CONFIG_INVALID"`; never echo the rejected raw value.
