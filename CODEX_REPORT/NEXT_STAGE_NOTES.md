# Next Stage Notes

R2 is closed:
```text
PASS
```

Do not keep expanding the server merely because more production features are possible.

If the project controller defines the next stage as Remote OTA Agent work, start with:
1. Agent boundary/state machine
2. server-origin configuration
3. manifest parser
4. compatibility gate
5. version policy
6. artifact download
7. SHA256 verification
8. report mapping
9. retry/backoff
10. process/service lifecycle
11. target evidence plan

Freeze early:
- `device_id` source/persistence
- local compatibility identity
- version semantics
- download staging path
- storage/free-space behavior
- restart vs Range resume
- report timing/status mapping

Carry forward:
- ConnMan owns networking
- no second DHCP client
- no speculative BSP changes
- host tests != target tests
- SHA256 != RAUC signature
- R2 dummy artifact is not installable
