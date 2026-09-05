# EdgeGuard RK3588 BOOT-GATE 冻结包

冻结日期：2026-09-04  
目标板：正点原子 ATK-DLRK3588（Rockchip RK3588，AArch64，eMMC）  
SDK：`/home/liu2004/work/Linux_SDK/atk_dlrk3588_linux5.10`  
来源会话：`构建前证据闭环`（conversation id `6a997254-485c-83ea-9e1a-32a6994d3d8b`）

## 一句话状态

```text
Rockchip Native A/B Platform Baseline = TARGET VERIFIED
BG-NATIVE-AB-CONTROL                  = CLOSED
BOOT-GATE                             = HELD AT PRE-RAUC BOUNDARY
RAUC custom backend                   = NOT STARTED / NOT VERIFIED
BOOT-GATE overall                     = NOT CLOSED
```

这里的 `HELD AT PRE-RAUC BOUNDARY` 是本冻结包的规范状态。历史对话中出现过 `BOOT-GATE = OPEN`，它表达的是同一个事实：总门尚未关闭。后续不得把 Native A/B 已验证误写成整个 BOOT-GATE 已关闭。

## 已经完成

- 从本地 vendor SDK 源码确认 U-Boot proper 按当前 Rockchip A/B metadata 选择 `boot_a/boot_b` 和 `system_a/system_b`，并把所选 system 分区的 GPT PARTUUID 写入 kernel cmdline。
- 创建并使用保留 `alientek_rk3588_defconfig` 的最小 A/B U-Boot fragment。
- 验证 SDK A/B 配置、U-Boot/SPL 构建、Buildroot rootfs、firmware staging 和完整 `update.img` 生成。
- 将完整 A/B `update.img` 刷入真实开发板，确认 GPT 分区、A 槽启动和 rootfs 身份一致。
- 用 `edgeguard-rk-abctl` 完成 metadata 读取、CRC 检查、mark-good、set-primary、A/B 实际切换、retry 递减、retry 耗尽自动回退、坏槽恢复。
- 最终把设备恢复到 A 为 primary、A/B 均 confirmed-good 的稳定状态。

## 尚未完成

- RAUC 尚未集成。
- RAUC `bootloader=custom` backend ABI 尚未实现并验证。
- 尚未用真实 RAUC bundle 完成 inactive-slot 安装、切换、health confirmation、mark-good 和失败回退闭环。
- 现有记录没有独立证明 Loader/Maskrom 恢复路径实际进入并完成恢复刷写；不得标为 `HARDWARE VERIFIED`。

## 文件导航

| 文件 | 用途 |
|---|---|
| `01_BOOT_GATE_STATUS_FREEZE.md` | 规范状态、已关子门、总门退出条件 |
| `02_ARCHITECTURE_FREEZE.md` | 启动链、metadata、分区和职责边界 |
| `03_EVIDENCE_LEVELS_AND_MATRIX.md` | 六种证据等级及逐项矩阵 |
| `04_TARGET_NATIVE_AB_EVIDENCE.md` | 实板 A/B、retry、rollback 原始事实摘要 |
| `05_EDGEGUARD_RK_ABCTL_SEMANTICS.md` | 工具格式、命令语义和 RAUC 适配约束 |
| `06_BUILD_ARTIFACT_AND_SAFETY_BASELINE.md` | SDK/构建/产物基线及禁止事项 |
| `07_RAUC_NEXT_STAGE_HANDOFF.md` | 下一阶段范围、设计入口和验收顺序 |
| `08_COMMANDS_AND_CHECKS.md` | 可复用的只读检查与受控实验命令 |

## 给下一位 AI 的读取规则

1. 先读 `01`、`03`、`07`，再讨论任何实现。
2. 把本文档中的“已验证事实”和“下一阶段建议”分开；建议不是既成事实。
3. 不重新设计 boot metadata，不改 U-Boot A/B 架构，不切换到 `fw_setenv/BOOT_ORDER` 路线。
4. 不进行 RAUC 自动安装和自动重启，直到 backend ABI、slot topology、健康确认和恢复前提被逐项验证。
5. 新证据必须按 `03_EVIDENCE_LEVELS_AND_MATRIX.md` 的等级记录，不能用源码或配置替代实板结果。

