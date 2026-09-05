# R2 Evidence Matrix

| Requirement | Expected | Observed | Evidence Level | Result |
|---|---|---|---|---|
| FastAPI server | implemented | repository implementation | CODEX VERIFIED | PASS |
| Host tests | pass | Codex suite pass | CODEX VERIFIED | PASS |
| Ubuntu deployment | runnable | human-confirmed passed | HOST VERIFIED | PASS |
| systemd | service works | human-confirmed passed | HOST VERIFIED | PASS |
| RK route | `wlan0` | `dev wlan0` | TARGET VERIFIED | PASS |
| Manifest | 200 valid JSON | 200 + full schema | TARGET VERIFIED | PASS |
| Range | 206 | 206 + correct headers | TARGET VERIFIED | PASS |
| Range size | 1024 | 1024 | TARGET VERIFIED | PASS |
| Full artifact | 33554432 | 33554432 | TARGET VERIFIED | PASS |
| SHA256 | match | exact match | TARGET VERIFIED | PASS |
| Device report | accepted | human-confirmed passed | TARGET VERIFIED (confirmation) | PASS |
| RAUC install | out of scope | not performed | N/A | NOT CLAIMED |
| A/B | out of scope | not performed | N/A | NOT CLAIMED |
| Production TLS/auth | out of scope | not implemented | N/A | NOT CLAIMED |

Final stage verdict:
```text
PASS
```
