# Thread 2 test sources

These are later host/build evidence inputs. They were added for the protocol,
authorization, and static source contracts, but Thread 2 does not compile or
execute them.

- `test_protocol.c` covers framing, fragmentation, duplicate fragments,
  conflicting overlap, timeout, completed replay, and conflicting ID reuse.
- `test_security.c` covers closed default, encrypted/bonded fail-closed checks,
  single-window ownership, close cleanup, and BlueZ-owner loss.
- `test_thread2_contract.py` freezes the UUID/object registry, security flags,
  protocol bounds, fixed read-only slot command, and forbidden OTA/shell calls.

Later host verification should link the C tests with GLib/GIO/JSON-GLib and the
Thread 1 model/error implementation, then separately exercise GDBus against a
mock BlueZ ObjectManager. Target behavior remains governed by
`docs/R5/14_TARGET_VALIDATION_PLAN.md`.
