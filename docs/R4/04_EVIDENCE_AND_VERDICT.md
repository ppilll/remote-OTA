# R4 Evidence 与最终 Verdict

## 1. Acceptance-criteria 调整

R4 原始计划把“full-rootfs OTA reboot 后自动恢复旧 Wi-Fi profile”当作 unattended campaign 的必要条件。真实 candidate 证明完整 rootfs replacement 不携带旧槽 `/var/lib/connman` 中的运行期 credential，因此阶段负责人批准以下 scope change：

```text
REMOVED FROM R4:
- Wi-Fi credential persistence across full-rootfs replacement
- automatic reconnection of an AP whose saved profile existed only in the previous rootfs

DEFERRED TO:
- provisioning
- persistent runtime configuration

REVISED R4 NETWORK ASSUMPTION:
- transport must be available when download/report delivery is required
- post-update network reprovisioning may be required
- credential absence is not firmware-health failure
- OTA/A-B/rollback correctness is evaluated independently from credential provisioning
```

该项在修订后的 R4 matrix 中为 `N/A / DEFERRED`，不是失败项，也不是用 waiver 隐藏 blocker。

## 2. Evidence matrix

| 结论 | 等级 | 冻结状态 | 依据/限制 |
|---|---|---|---|
| R4 health hook source/package integration | HOST VERIFIED | PASS | Linux/host suite + package/static checks；不替代 target |
| Server legacy + extended report compatibility | HOST VERIFIED | PASS | Ubuntu Server pytest 与实际 204/structured correlation 验证 |
| Manifest/artifact/Range regression | HOST VERIFIED | PASS | 既有 Server suite 与部署检查；不重复已关闭 tests |
| Agent POSIX/Linux core/merged tests | HOST VERIFIED | PASS | 正确 Linux 环境记录；早期 Windows skipped 仍保持历史事实 |
| Same-attempt install uncertainty non-reinstall | HOST VERIFIED | PASS | host integration；未通过危险 target kill 扩大结论 |
| R4 `1.2.5` vendor SDK/full image/signed bundle | SDK BUILD VERIFIED | PASS | exact version/build/artifact identity build record |
| `1.2.4/B -> 1.2.5/A` autonomous install/reboot/accept | TARGET VERIFIED | PASS | attempt、前后 boot ID、release、slot、RAUC/Native A/B correlation 完整 |
| A slot health/mark-good and confirmed-good | TARGET VERIFIED | PASS | RAUC 与 `edgeguard-rk-abctl` 最终状态一致 |
| Post-mark-good report debt survives outage | TARGET VERIFIED within observed campaign | PASS | `REPORT_SUCCESS` 保持；无 reinstall/rollback；恢复后 `IDLE` |
| Reverse-direction、server outage、download resume、deterministic health rollback 等 campaign | PROJECT ACCEPTED from target campaign completion | PASS | 阶段负责人确认真实 target campaign 正常；每个 `TARGET VERIFIED` 声明仍以其保留原始日志为边界，本包不以一句总结或 Codex 输出替代日志 |
| Wi-Fi credential persistence/autoconnect across new rootfs | DEFERRED / N/A | 不属于 R4 verdict | 已移交 provisioning/persistent runtime config |
| Fleet、TLS/PKI、BLE provisioning、delta OTA、power-cut campaign | DEFERRED | 不影响 R4 PASS | R5/R6 或更后阶段 |

## 3. 已审定 happy-path target evidence

```text
initial:
  release = 1.2.4 / rk3588-r3h-1.2.4-001
  current/primary = b
  boot_id = c0b19120-7518-4887-8b53-52110850010e

attempt:
  id = 48f3bd95-70b0-4e12-b44c-7d593aada87a
  candidate = 1.2.5 / rk3588-r4-1.2.5-001
  expected candidate slot = a
  bundle size = 586181970
  bundle sha256 = b88a407068b4022c21e8909a8e76257f58fec08550b3e12db8f4fdefd26e5583

after reboot:
  boot_id = 1f9eea0b-105d-4f20-973f-d24c11881dc2
  release = 1.2.5 / rk3588-r4-1.2.5-001
  current/primary/last_boot = a/a/a
  slot a = confirmed-good, priority 15, tries 0, successful 1
  slot b = confirmed-good, priority 14, tries 0, successful 1
  state = REPORT_SUCCESS -> IDLE
```

该链路支持：

```text
automatic discovery/download/install/reboot
candidate boot and durable recovery
health acceptance and mark-good
Native A/B final metadata
post-commit report persistence and eventual completion
```

## 4. 最终 verdict

```text
R4 FINAL VERDICT = PASS
ACCEPTANCE BASE = REVISED R4 CRITERIA DATED 2026-09-09
WIFI CREDENTIAL PERSISTENCE/AUTOCONNECT = N/A FOR R4, DEFERRED
```

此 verdict 的含义：

- R4 的 Remote OTA orchestration、RAUC/A-B integration、durable recovery、health acceptance/rollback 与 reporting separation 可作为后续阶段输入；
- 不代表 Wi-Fi secret provisioning 已完成；
- 不代表所有可能的电源故障、fleet scale、安全或生产网络场景已验证；
- 不授权重构已通过的 Native A/B、RAUC 1.5.1 或 Agent state machine。

## 5. 后续 evidence 引用最低要求

任何新的 target campaign 至少保留：

```text
VERSION / BUILD_ID / bundle SHA256
attempt_id
initial/current/expected slot
boot_id before/after
manifest and artifact request correlation
Range/Content-Range when relevant
RAUC verify/install result
Native A/B state before/after reboot
durable Agent state transitions and root error
health / mark-good or mark-bad order
report request/result
final release + current/primary + both-slot state
proof of no forbidden duplicate destructive action
```

没有这些 correlation 时，只能降级为 `TARGET OBSERVED` 或 `operator-reported`，不能由文档作者补写成 `TARGET VERIFIED`。

