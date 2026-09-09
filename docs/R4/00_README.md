# EdgeGuard Remote OTA R4 最终冻结包

冻结日期：2026-09-09  
适用平台：ATK-DLRK3588 / RK3588  
R4 最终项目结论：`PASS`（以本包记录的修订后 acceptance criteria 为准）

## 1. 本包用途

本包是 R4 退出与 R5/R6 进入的权威交接入口。它不重新设计 R0–R4 架构，也不替代既有过程文档；它把已经冻结的实现、真实阶段基线、问题处置、证据边界和后续不可回退 contracts 汇总到一个可直接使用的入口。

本包重点回答：

- R5/R6 必须继承什么，不能重构什么；
- target、server、SDK、RAUC、Native A/B、Agent 和 release 从什么状态继续；
- R4 中实际发生了什么问题，根因和处置是什么；
- 哪些结论来自 host、SDK 或真实 target，哪些不能相互替代；
- Wi-Fi credential persistence/autoconnect 为什么从 R4 移出，以及后续由谁负责。

## 2. 文档优先级

出现冲突时按以下顺序处理：

1. 本 `R4_FREEZE_PACKAGE` 对 R4 最终状态、修订后 acceptance criteria、R5/R6 交接边界的说明；
2. `docs/R4/03_AUTOMATION_INVARIANTS.md` 的安全不变量；
3. `docs/R4/02_ARCHITECTURE_CONTEXT.md`、`04_HEALTH_POLICY.md`、`05_FAILURE_RECOVERY_SPEC.md`、`07_STATE_MACHINE_DELTA.md` 的冻结架构与行为定义；
4. R0–R3 与 `docs/BOOT-GATE` 已冻结事实；
5. 过程报告与早期 proposed values。

`docs/R4/00_README.md` 至 `09_CODEX_THREADS.md` 形成于 R4 进入/实施期。其中的 `NOT YET VERIFIED`、旧 target snapshot、旧实验室地址和 Wi-Fi autoconnect 入场条件是历史时点信息；本包只在“最终状态与 acceptance criteria”上取代这些内容，不撤销其架构、安全和实现说明。

## 3. 包内容

| 文件 | 用途 |
|---|---|
| `01_FINAL_BASELINE.md` | 当前 target/server/SDK/RAUC/A-B/Agent/release 起点 |
| `02_FROZEN_CONTRACTS.md` | 架构、接口、状态机、证据等级和不可回退边界 |
| `03_PROBLEMS_AND_DECISIONS.md` | R4 实际问题、根因、解决方案、排除与延期项 |
| `04_EVIDENCE_AND_VERDICT.md` | 证据矩阵、acceptance-criteria 调整与最终 verdict |
| `05_R5_R6_HANDOFF.md` | R5/R6 的进入条件、工作边界和 handoff notes |
| `06_EVIDENCE_REFERENCES.md` | 仓库内外证据索引及引用规则 |

建议顺序：`01` → `02` → `03` → `04` → `05`；审计或定位原始依据时再读 `06`。

## 4. 一句话冻结结论

```text
R4 FINAL VERDICT = PASS

条件：
- 使用修订后的 R4 acceptance criteria；
- Wi-Fi credential persistence/autoconnect across full-rootfs replacement = N/A for R4；
- 该能力转交 provisioning / persistent runtime configuration；
- OTA、RAUC、Native A/B、durable recovery、health/mark-good、rollback 和 post-commit reporting contracts 保持冻结。
```

## 5. 证据纪律

- Codex/ChatGPT 的分析、总结或成功退出码本身不是 target evidence。
- Host tests 只能形成 `HOST VERIFIED`。
- Vendor SDK 构建与最终镜像检查只能形成 `SDK BUILD VERIFIED`。
- 只有带 release/build、attempt、boot ID、slot、durable state、RAUC/Native A/B 和 server correlation 的真实 RK3588 记录，才能形成对应范围的 `TARGET VERIFIED`。
- 项目 verdict 可以由阶段负责人按 acceptance criteria 签收；这不允许把缺失的原始 target 日志伪造为已审阅证据。

