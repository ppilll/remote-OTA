# R4 Evidence References

## 1. 仓库内架构与规范

| 主题 | 权威引用 |
|---|---|
| R4 entry vocabulary/初始计划 | `docs/R4/00_README.md` |
| 架构与组件边界 | `docs/R4/02_ARCHITECTURE_CONTEXT.md` |
| 自动化安全不变量 | `docs/R4/03_AUTOMATION_INVARIANTS.md` |
| Health truth 与 hook 边界 | `docs/R4/04_HEALTH_POLICY.md` |
| Failure/recovery 语义 | `docs/R4/05_FAILURE_RECOVERY_SPEC.md` |
| 状态机 delta | `docs/R4/07_STATE_MACHINE_DELTA.md` |
| 原 R4 campaign/test matrix | `docs/R4/08_TEST_PLAN.md` |
| 工作流 ownership | `docs/R4/09_CODEX_THREADS.md` |
| R3 冻结技术交接 | `docs/R3/CODEX_EXECUTION_PACKAGE/02_R3_TECHNICAL_HANDOFF.md` |
| Native A/B 真实基线 | `docs/BOOT-GATE/04_TARGET_NATIVE_AB_EVIDENCE.md` |
| Native A/B control semantics | `docs/BOOT-GATE/05_EDGEGUARD_RK_ABCTL_SEMANTICS.md` |
| RAUC handoff | `docs/BOOT-GATE/07_RAUC_NEXT_STAGE_HANDOFF.md` |

## 2. 仓库内 implementation/report references

| 主题 | 引用 |
|---|---|
| R4 health hook implementation report | `CODEX_REPORT/R4_THREAD_01_HEALTH.md` |
| R4 extended reporting implementation report | `CODEX_REPORT/R4_THREAD_02_REPORTING.md` |
| R4 corrective host review | `CODEX_REPORT/R4_CORRECTIVE_REVIEW.md` |
| R3 merged Agent contract | `CODEX_REPORT/R3-F-INTEGRATION.md` |
| Server API baseline | `CODEX_REPORT/API_SPEC.md` |
| Existing evidence-level examples | `CODEX_REPORT/EVIDENCE_MATRIX.md`、`TARGET_VALIDATION_REPORT.md` |

注意：`CODEX_REPORT/EVIDENCE_MATRIX.md` 和 `TARGET_VALIDATION_REPORT.md` 主要记录早期 R2 evidence，不能被引用为 R4 RAUC/A-B target campaign 的证明。

## 3. Source paths for frozen contracts

```text
buildroot-external/package/edgeguard-remote-ota/agent.conf
buildroot-external/package/edgeguard-remote-ota/edgeguard-health-check
buildroot-external/package/edgeguard-remote-ota/edgeguard-remote-ota.mk
buildroot-external/package/edgeguard-remote-ota/post-build-check.sh
RK3588_app/edgeguard-remote-ota-agent/include/edgeguard_ota/model.h
RK3588_app/edgeguard-remote-ota-agent/src/state_machine.c
RK3588_app/edgeguard-remote-ota-agent/src/persistence.c
RK3588_app/edgeguard-remote-ota-agent/src/health.c
RK3588_app/edgeguard-remote-ota-agent/src/reporting.c
host_app/models.py
host_app/main.py
host_app/manifest_service.py
host_app/artifact_service.py
build-r3-candidate.sh
publish-r3-candidate.sh
deploy-host-app.sh
live_server_gate.sh
real_sdk_preflight.sh
```

这些 source 只能证明实现/配置内容；除非配合对应执行记录，不得单独形成 `HOST VERIFIED`、`SDK BUILD VERIFIED` 或 `TARGET VERIFIED`。

## 4. R4-4 外部 target transcript reference

R4 最终 target evidence 来自阶段负责人在 R4-4 中保存/提供的真实 RK3588 与 Server transcript：

```text
conversation title = R4-4
conversation id    = 6aa011ed-3460-83e9-92e5-7041cfc003d7
```

其中可直接 correlation 的 happy-path evidence：

```text
attempt_id       48f3bd95-70b0-4e12-b44c-7d593aada87a
candidate        1.2.5 / rk3588-r4-1.2.5-001
bundle           586181970 bytes
sha256           b88a407068b4022c21e8909a8e76257f58fec08550b3e12db8f4fdefd26e5583
boot before      c0b19120-7518-4887-8b53-52110850010e
boot after       1f9eea0b-105d-4f20-973f-d24c11881dc2
slot transition  b -> a
final A/B        current=a primary=a last_boot=a; both slots confirmed-good
state closure    REPORT_SUCCESS -> IDLE
```

R4-4 中阶段负责人还确认 reverse direction、rollback 等 R4 campaign 正常并据此签收 R4。若后续需要独立审计这些场景，必须读取对应保留的 target/server logs；不得把本冻结文档或对话 assistant summary 当作缺失的 raw target evidence。

## 5. 明确不是 target evidence 的材料

- Codex/ChatGPT 的建议、推理、摘要或“PASS”措辞；
- repository code review；
- Windows/POSIX/Ubuntu host test output；
- vendor SDK compile/build output；
- Server 从 `127.0.0.1` 发起的 publish closure；
- 没有 client/attempt/release correlation 的 Server request；
- 人工 `curl` bundle、人工 `rauc install`、人工 mark/reboot；
- 单一 `pidof`、单一 `rauc status` 或单一 state snapshot。

这些材料可以是证据链的一部分，但不能单独证明真实 autonomous target lifecycle。

## 6. 后续证据归档建议

每个 campaign 使用独立目录或记录，至少以以下键命名/索引：

```text
stage / scenario / timestamp
VERSION / BUILD_ID / bundle SHA256
attempt_id
target device id
initial and final boot_id
initial, candidate and final slots
server client IP and request window
```

不要归档 Wi-Fi PSK、token、private key 或完整 secret-bearing ConnMan settings。
