# R2 Failure Matrix — Handoff

| Failure | R2 behavior/evidence | Later-stage responsibility |
|---|---|---|
| server unavailable | transport fails | Agent retry/backoff |
| malformed manifest | server schema/test coverage | Agent reject invalid metadata |
| missing artifact | server error path | Agent stop/retry/report |
| hash mismatch | comparison mechanism proven | Agent reject download |
| partial transfer | Range proven | Agent retry/resume policy |
| bad report | 400/422 validation | Agent report correctness |
| Wi-Fi unavailable | outside server | Agent/network handling |
| HTTP timeout/reset | transport failure | Agent timeout/retry |
| VM firewall | deployment issue | environment checks |
| wrong route | false-PASS risk | require route evidence |
| active artifact replaced in-place | cached metadata risk | keep immutable; restart service |

A completed HTTP transfer and a successful SHA256 check are separate states.
