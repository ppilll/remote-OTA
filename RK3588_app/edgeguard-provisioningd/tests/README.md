# Thread 2 test sources

These are later host/build evidence inputs. They were added for the protocol,
authorization, and static source contracts, but Thread 2 does not compile or
execute them.

- `test_protocol.c` covers framing, fragmentation, duplicate fragments,
  conflicting overlap, timeout, completed replay, conflicting ID reuse, and
  behavior-level scoped Agent request identity relations.
- `test_security.c` covers closed default, encrypted/bonded fail-closed checks,
  single-window ownership, commit epochs, disconnect/security-property
  continuity, close/reopen cleanup, and BlueZ-owner loss.
- `test_thread2_contract.py` freezes the UUID/object registry, security flags,
  protocol bounds, fixed read-only slot command, and forbidden OTA/shell calls.
- `test_r5_review_regressions.py` covers continuous ConnMan owner
  reconciliation and revoke ownership, read-only GATT surfaces, BlueZ
  owner/epoch gates, scoped IPC request IDs, endpoint bounds/result schema,
  and the real R4 documentation references.

Later host verification should link the C tests with GLib/GIO/JSON-GLib and the
Thread 1 model/error implementation, then separately exercise GDBus against a
mock BlueZ ObjectManager. Target behavior remains governed by
`docs/R5/14_TARGET_VALIDATION_PLAN.md`.
